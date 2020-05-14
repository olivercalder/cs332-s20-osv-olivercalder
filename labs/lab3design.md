# Lab 3 Design Doc: Multiprocessing

## Overview

The goal of this project is to implement the core system process-management calls, as well as the underlying process functions which handle the creation/destruction of and interaction between threads for a given process. A secondary goal is to implement inter-process communication through pipes, allowing input and output from multiple processes to be redirected to/from one another.

### Major Parts

#### fork

`fork` creates a new process identical to the current process, and creates a new thread to run that process. The parent process adds the new child thread to its list of threads, and the new child process holds the PID of its parent. `fork` returns twice: it returns the process ID of the new child process to the parent process, and it returns `0` to the child thread.

The primary data structures which will be modified when calling `fork` are:

- A new `struct proc` for the child
- A new `struct thread` for the thread, containing the new `struct proc`
- A duplicate trap frame for the child thread (with modifications to allow fork to return different value to the parent and child?)
- The new entry if the calling thread's child thread list (`ctlist`) is allocated for the child thread, and the child PID and `struct thread` are stored there
- Child thread calls `sleeplock_acquire` on the lack for that entry of the table
  - This allows the parent process to wait on the child process by calling `sleeplock_acquire` on that lock, which will only return once the child process has released the lock by exiting
- A new `fdtable` must be allocated for the child process, and then populated with files by calling `sys_open` on every file in the parent's `fdtable`

#### wait

`wait` pauses the execution of the current thread until the child thread with the given PID returns. If the PID is -1, wait until any child process returns. `wait` returns the process ID of the child process that terminated, and copies the process's exit status to the memory location specified by the arguments to `wait`.

The primary data structures of interest for `wait`:

- The child thread table for the process calling `wait`
- The sleeplock for the `cttable` entry corresponding to the child process with the given PID
- The return status of the process at that `cttable` entry

#### exit

`exit` halts the execution of the thread and frees its resources. Before the thread exits, it saves its exit status in its parent's `ctlist` entry, if the parent has not already exited.

The primary data structures which will be modified when calling `exit` are:

- The child thread table for the process calling `exit`, which should be nearly all freed
- The files in the `fdtable` for the exiting process must be closed
- If the parent process has not exited:
  - Write the exit status to the parent's `cttable` entry
- If the parent process has exited:
  - Free the sleeplock and the spinlock

## In-depth Analysis and Implementation

### Bookkeeping

- `include/kernel/proc.h` provides the definitions for `struct proc` and the process calls which will need to be made
- `include/kernel/thread/h` provides the definitions for `struct thread` and the thread creation, scheduling, and cleanup calls
- `include/kernel/list.h` defines the `List` type and functions, in case these need to be used
- `include/kernel/sched.h` defines scheduling functions
- `include/kernel/synch.h` defines synchronization structures and functions, which will be useful for implementing waiting based on condition variables (sleeplocks and spinlocks)

### Key Data Structures

#### struct proc

Several additions are necessary to `struct proc`:

**Parent PID**: stored in `pid_t ppid`

- This allows a child thread to learn about or interact with its parent process

**Parent status**: stored in `bool parent_live`

- `1` if parent is live (has not exited)
- `0` if parent has exited
- If a parent exits, it first goes to all non-exited children and sets this value to `0` for each of them
- When a thread exits, it checks if this value is `1`, and if so, writes its exit status to `ctlist_entry->status` of the `ctlist` entry where it is stored (in its parent's `ctlist`, which is a list of `ctlist_entry`s, which are defined below)

**Parent ctlist entry**: stored in `struct ctlist_entry *ctlist_entry`

- This allows a process to quickly find the ctlist struct in which it is stored (in its parent's ctlist) so it can store its exit status, release its sleeplock, or acquire its spinlock
- The structure of the `struct ctlist_entry` is defined below

**Child thread list**: stored in `List ctlist`

```c
struct ctlist_entry {
    pid_t pid;
    struct thread *thread;
    struct sleeplock *ct_lock;
    struct spinlock *exit_lock;
    Node node;
    sysret_t status;
}
```

- Use `list_entry(<node>, struct ctlist_entry, node)` to get a pointer to the struct itself
- The sleeplock is stored as a pointer so that the parent will be able to exit and be reclaimed without interfering with the ability of the child process to release the spinlock successfully
  - May be beneficial to have the `ctlist_entry` outlive the parent process, but that adds complexity as then it could be the parent's or the child's job to free the `ctlist_entry`. Either way, this would allow for the sleeplock to be part of the struct, rather than a pointer
- Similarly, exit lock is stored as a pointer so that whichever process exits last will be able to acquire it, verify that it is the last relevant process alive, and then release and free it
- The exit lock prohibits a parent and child from exiting simultaneously, thus running into problems around writing data to the `ctlist_entry` (defined below) and deciding who frees the sleeplock
- When a thread exits, it must acquire its own `ctlist_entry->exit_lock` while it checks its parent's status and writes the exit status there if needed, and then releases the exit lock
  - The thread then iterate through each child thread in its `ctlist`, acquire each of their exit locks in turn, change their `parent_live` value, then release the lock and go on freeing the `ctlist_entry`

#### fdtable

Several changes are needed to `fdtable` in the `struct proc`:

- Need to store the pathname, flags, and mode so that the file can be reopened by a child process, if needed

**File descriptor table entry**: stored in `struct fdtable_entry *table[PROC_MAX_FILE]`

```c
struct fdtable_entry {
    struct file *file;
    char *pathname;
    int flags;
    int mode;
}
```

- Need to change `kernel/syscall.c:alloc_fd` and `kernel/proc.c:proc_alloc_fd` to take pathname, flags, and mode as parameters
- Initializes the table with `NULL` values (by luck, this is already implemented)
- Need to change `kernel/proc.c:proc_alloc_fd` to allocate memory for the fdtable entry whenever a new file descriptor is opened
  - Also need to allocate memory for `char *pathname`, probably by using `strlen` and `strcpy`
- Need to change `kernel/proc.c:proc_remove_fd` to free the memory of `char *pathname` and the table entry as a whole

### General notes on memory and locks

- The process is responsible for cleaning up its own memory on exit
  - This includes the entries of the process's `ctlist`
- sleeplocks and spinlocks are allocated on a child-by-child basis and freed after use
- A sleeplock for a given child process is freed by the parent after their call to `wait` returns
- If the parent exits before the child process exits, then it is the responsibility of the child to free its own sleeplock
- Similarly, it is responsibility of the last remaining relevant process to free the spinlock
- If a process forks many children and does not wait on them, then the `ctlist` entries will sit allocated and with exit statuses stored
- When a process exits, it must go through every entry of its `ctlist` and for every entry:
  - Acquire the exit lock for that `ctlist_entry`
  - Check if the entry is terminated by calling `sleeplock_try_acquire`
  - If `sleeplock_try_acquire` returns `ERR_OK`, then release the exit lock and free the memory of the sleeplock and the spinlock
  - The thread pointers themselves will have already been freed by the child processes
  - If the child process has not terminated, then update the child's `parent_live` value, release the exit lock, and do not free the locks or the thread pointers
  - Free the `ctlist_entry`
- Since the only process which may call `wait` using a given process's PID is that process's direct parent, then there is no need to have another thread inherit the child if that child's parent exits before the child does

### System functions

#### fork

**Description:**

```c
/*
 * Creates a new process as a copy of the current process, with
 * the same open file descriptors.
 *
 * Return:
 * PID of child - returned in the parent process.
 * 0 - returned in the newly created child process.
 * ERR_NOMEM on error: kernel lacks space to create new process
 */
sysret_t
sys_fork(void *arg);
```

**Behavior:**

- A new process needs to be created through `kernel/proc.c:proc_init`
- Set up the child thread's `proc struct`
  - Set `ppid` to the parent's PID
  - Set `parent_live = 1`
  - Set `ctlist_entry` to the memory address of the `struct ctlist_entry` which is added to its parent's `ctlist`
- Parent must copy its memory to the child via `kernel/mm/vm.c:as_copy_as` 
  - Any changes in the child's memory after `fork` is not visible to the parent
  - Any changes in the parent's memory after `fork` is not visible to the child
- All the opened files must be duplicated in the new process (not as simple as a memory copy)
  - Go through every entry of the parent's `fdtable`, and for each one, have the child process call `sys_open` with the proper pathname, flags, and mode (all now saved in the `fdtable`)
- Create a new thread to run the process
  - Do this via `thread_create` and `thread_start_context`
  - `sys_spawn` provides a great template for what `sys_fork` should do
  - The name of the child thread is equal to the name of the parent thread + `" child"`
  - The priority for the new thread should be `0` in order to enable interrupts
- Create a new `ctlist_entry` for the new thread (let it be denoted `ctle`)
  - Set `ctle->thread` to the child's thread struct
  - Allocate a new sleeplock using `kmalloc`, set `ctle->ct_lock` to it
  - Allocate a new spinlock using `kmalloc`, set `ctle->exit_lock` to it
  - Set `ctle->pid` to the child process's PID
  - Add the entry to the parent process's `ctlist` using `list_append(ctlist, ctle->node)`
- Duplicate current thread (parent process)'s trapframe in the new thread (child process)
- Have the child process acquire the sleeplock before the fork returns
  - This may be difficult...
- Set up trapframe to return 0 in the child via `kernel/trap.c:tf_set_return`, while returning the child's pid in the parent

#### wait

**Description:**

```c
/*
 * Corresponds to int wait(int pid, int *wstatus);
 *
 * Suspend execution until a child process terminates.
 * Wait for child with pid `pid` to terminate, if pid is -1, wait for any child process.
 * If wstatus is not NULL, store the exit status of the child in wstatus.
 * A parent can only wait for the same child once.
 *
 * Return:
 * PID of the child process that terminated.
 * ERR_FAULT - Address of wstatus is invalid.
 * ERR_CHILD - The caller does not have a child with the specified pid, or have already waited
 *             for the child.
 */
sysret_t
sys_wait(void *arg);
```

Corresponding process function:

```c
/*
 * Wait for a process to change state. If pid is ANY_CHILD, wait for any child process.
 * If wstatus is not NULL, store the the exit status of the child in wstatus.
 *
 * Return:
 * pid of the child process that changes state.
 * ERR_CHILD - The caller does not have a child with the specified pid.
 */
int proc_wait(pid_t pid, int* status);
```

**Behavior:**

- Get the first element with `list_begin(ctlist)`, let it be denoted `currnode`
- Get the `ctlist_entry` associated with `currnode` by calling `list_entry(currnode, struct ctlist_entry, node)`, let it be denoted `ctle`
- Keeps looking through the list (using `currnode = list_next(currnode)` and getting `ctle` for each) until either the corresponding PID is found or `currnode = list_end(ctlist)`
  - If `list_empty(ctlist)` then return `ERR_CHILD` immediately
  - If the given PID is not found in the table (and is not `-1`), then return `ERR_CHILD`
- If the given PID is found, then call `sleeplock_acquire(ctle->ct_lock)`
  - Once the sleeplock is acquired, then the child process must have finished
  - Extract the exit status from the struct and save it to be returned later
  - Release and then free the sleeplock
  - Remove `ctle` from the `ctlist` using `list_remove(currnode)`
  - Free `ctle`
  - Return the PID of the child
- If the given PID is `-1`, then loop through the entries of the `ctlist`, trying `sleeplock_try_acquire` on each until one of them returns true
  - Apply the same steps
  - Return the PID of the child for which `sleeplock_try_acquire` returned true
  - The exit lock is only necessary if `sleeplock_try_acquire` returns false and we want to modify that entry anyway, but in this case, we just move on, so no need to acquire it

#### exit

**Description**

```c
/*
 * Corresponds to void exit(int status);
 *
 * Halts program and reclaims resources consumed by program.
 * The process will exit with the given status.
 * Should never return.
 */
sysret_t
sys_exit(void *arg);
```

Corresponding process function:

```c
/* Exit a process with a status */
void proc_exit(int status);
```

**Behavior:**

- Go through every entry of the process's `fdtable` and for every file descriptor, call `sys_close(fd)`
- Go through every entry of the process's `ctlist` and for every entry `ctle`:
  - Acquire the exit lock using `spinlock_acquire(ctle->exit_lock)`
  - Check if the entry is terminated by calling `sleeplock_try_acquire(ctle->ct_lock)`
  - If `sleeplock_try_acquire` returns `ERR_OK`, then release the exit lock and free the memory of the sleeplock and the spinlock
  - The thread pointers themselves will have already been freed by the child processes
  - If `sleeplock_try_acquire` returns `ERR_LOCK_BUSY`, then the child process has not terminated, then do not free the sleeplock or the thread pointers, but do set the child's `ctle->thread->proc->parent_live = 0`, and then release the exit lock
  - Remove `ctle->ct_node` from `ctlist` using `list_remove(ctle->ct_node)`
  - Free `ctle` (Note that this does not free the thread or the locks, which have been or will be freed by the child)
- Acquire the process's own `exit_lock` by calling `spinlock_acquire(proc->ctlist_entry->exit_lock)`
- If the process's `parent_live` value equals `1`, then set the process's `ctlist_entry->status` to the exit status of the process, then release the spinlock
- If the process's `parent_live` value equals `0`, then release the exit lock and free the sleeplock and the spinlock
- Free the process's data
  - The process's address space
  - The process's proc struct
  - The process's thread struct
  - `proc_exit`, `thread_exit`, and `thread_cleanup` will be of use

### Other functions

Will likely need functions for `validate_pid`, `get_pid`, `alloc_pid`, and `remove_pid`, though the latter two might be contained within `fork` and `wait`. These functions should also likely exist both in `sys_*` and `proc_*` form, though this remains to be seen.

## Risk Analysis

### Edge cases

- What happens if the parent checks the `ctlist` via `wait` before the child has a chance to be added there, thus returning `ERR_CHILD`?
  - Answer: This cannot occur, since the child process is added to the `ctlist` of the parent before the `fork` call returns for either the parent or the child.
- What if the parent waits before the child exits?
  - Answer: The parent will call `sleeplock_acquire` and wait for the child to exit, then the parent's call to `wait` will return.
- What if the parent waits after the child exits?
  - Answer: The parent will find the PID in its `ctlist`, call `sleeplock_acquire`, and immediately acquire the sleeplock, indicating that the child process has finished. It may then carry out the usual `wait` cleanup and exit status behavior.
- What if the parent exits without waiting for the child?
  - Answer: The parent changes the child's proc struct `parent_live` value to `0`, thus indicating that the child should not attempt to set an exit status for its parent when it eventually returns, and that the child is in charge of freeing its own sleeplock in addition to all the other data associated with the child. The parent frees all of its own data, and the residual data (essentially just the sleeplock) for all other terminated child processes, leaving only the child thread pointer and sleeplock pointers un-freed. The child is orphaned, and when it exits, itdoes not report its exit status to any other process, and instead frees all its data and exits.
- What if the parent calls exit, calls `sleeplock_try_acquire`, sees that the child is still running, then proceeds to free the `ctlist`, but before doing so, the child exits and tries to write things to the partially deceased parent or free the spinlock before the ?
  - Answer: Depending on the scheduler, null pointer exceptions might occur. To be safe, I've added a spinlock to the `ctlist` struct which both the parent and child try to grab when exiting, so only one can exit at a time, and both must hold the spinlock while checking/changing the `parent_live` value or writing exit status to a parent. Like the sleeplock, the spinlock is freed by whichever process exits last.

### Unanswered questions

- How do I deschedule a thread which calls wait
  - I only see functions in `include/kernel/sched.h` and `kernel/sched.c` for `sched_start`, `sched_start_ap` (what does that do?), `sched_ready`, and `sched_sched`, none of which put the thread in a waiting state
  - It seems as though scheduling and setting thread states should not be handled outside of these purpose-built functions
- How can a new child process acquire a sleeplock before returning from the `fork` call?
- Can the parent change its children's proc values, such as `parent_live`, given that the memory is separate?
  - Does there need to be some independent mutually-accessible memory region shared by the parent and child where they can exchange information? (I think the `ctlist_entry` satisfies this)
- Can a process free itself?
  - If not, probably need to work out a different way for grandparent threads to inherit child threads when the parent exits, and for those grandparent threads to then clean up the child threads when they exit

### Staging of Work

First, I will implement the changes to the structs in `include/kernel/proc.h`. Then, I will write the `proc_*` functions which will serve as the backing for the `sys_*` functions. Then, I will write the `sys_*` functions, which will provide validity checking for the inputs before calling `proc_*`. If I have time, I will then work on implementing `sys_pipe` and `sys_spawn`.

### Time Estimation:

- Struct changes:
  - Best case: 0.25 hour
  - Average case: 0.5 hour
  - Worst case: 3 hours (if a mutually-accessible external memory location is necessary to exchange data between the parent and child processes or if `List` implementation goes horribly wrong)
- `proc_fork`:
  - Best case: 1 hours
  - Average case: 2 hours
  - Worst case: 4 hours (if child processes can't acquire locks before they return from `fork`)
- `proc_wait`:
  - Best case: 1 hour
  - Average case: 2 hours
  - Worst case: 4 hours (if descheduling threads is difficult or parent and child processes cannot directly modify each other's data)
- `proc_exit`:
  - Best case: 1 hour
  - Average case: 2 hours
  - Worst case: 4 hours (if parent and child processes cannot directly modify each other's data or if another process needs to inherit orphaned children)
- `validate_pid` and `get_pid`:
  - Best case: 0.25 hour
  - Average case: 0.5 hour
  - Worst case: 2 hours (if `List` implementation goes horribly wrong)
- `sys_fork`:
  - Best case: 0.5 hour
  - Average case: 1 hour
  - Worst case: 1.5 hours
- `sys_wait`:
  - Best case: 0.5 hour
  - Average case: 1 hour
  - Worst case: 1.5 hours
- `sys_exit`:
  - Best case: 0.5 hour
  - Average case: 1 hour
  - Worst case: 1.5 hours
- Edge cases and error handling:
  - Best case: 0.5 hour
  - Average case: 1 hour
  - Worst case: 2 hours
