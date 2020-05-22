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
- The `struct ctlist_entry` which holds a child process's information so its parent can keep track of it

#### wait

`wait` pauses the execution of the current thread until the child thread with the given PID returns. If the PID is -1, wait until any child process returns. `wait` returns the process ID of the child process that terminated, and copies the process's exit status to the memory location specified by the arguments to `wait`.

The primary data structures of interest for `wait`:

- The child thread table for the process calling `wait`
- The condvar for the `ctlist_entry` corresponding to the child process with the given PID
- The exit status of the process at that `ctlist_entry`

#### exit

`exit` halts the execution of the thread and frees its resources. Before the thread exits, it saves its exit status in its parent's `ctlist` entry, if the parent has not already exited.

The primary data structures which will be modified when calling `exit` are:

- The child thread table for the process calling `exit`
- The files in the `fdtable` for the exiting process must be closed
- If the parent process has not exited:
  - Write the exit status to the parent's `ctlist` entry

## In-depth Analysis and Implementation

### Bookkeeping

- `include/kernel/proc.h` provides the definitions for `struct proc` and the process calls which will need to be made
- `include/kernel/thread/h` provides the definitions for `struct thread` and the thread creation, scheduling, and cleanup calls
- `include/kernel/list.h` defines the `List` type and functions, in case these need to be used
- `include/kernel/sched.h` defines scheduling functions
- `include/kernel/synch.h` defines synchronization structures and functions, which will be useful for implementing locks and waiting based on condition variables (condvar and spinlocks)

### Key Data Structures

**Global exit lock:** `struct spinlock exit_lock`

Need a global exit lock which prevents race conditions when multiple threads attempt to exit simultaneously.

- This prevents the need for separate spinlocks for each child thread, and reduces the overhead of allocating, initializing, acquiring, releasing, and freeing the spinlock every time a process is forked and exits
- `exit_lock` should be initialized as part of `proc_sys_init()`
- This lock should only be held when an exiting thread:
  - Checks the `parent_live` value, and if `1`, write return value to its `ctlist_entry` of its parent's `ctlist`
  - Checks each entry `ctle` in its `ctlist`, and if `ctle->status == STATUS_ALIVE`, then writes `parent_live = 0` for that child, and always frees the ctlist entry
- After this is released, it can worry about freeing its own memory

**Global wait condition variable:** `struct condvar wait_var`

Need a global condition variable to signal that a child thread has exited

- This prevents race conditions concerning which thread needs to free a child-specific the condition variable
  - If there is a child-specific condition variable, then that condition variable must be freed by either the child or the parent
  - The only way to guarantee that there is not a null pointer exception caused by the parent calling `condvar_wait` on the condvar after the child freed it, by the child calling `condvar_signal` after the parent has freed it, or by one of them calling free after the other has freed it is if the (waiting or signalling) and freeing happens while the global exit lock is held, but this prevents the signalling from working at all, so this is inviable
- When a process exits, it first acquires the exit lock, then checks if `parent_live == 1` and if so, writes exit status, then releases the exit lock and broadcasts this condition variable
  - Again, the signal must happen while a spinlock is not held
  - Waiting parents should be waiting in a while loop, `while (ctle->status == STATUS_ALIVE) { condvar_wait(ctle->wait_var) }`, so that they immediately continue to wait until the child they are waiting for exits
- A global variable is not as efficient as child-specific condition variables because if there are many waiting threads, they will all wake whenever another thread exits, but this is preferable to the race conditions regarding how and when to free a child- or parent-specific lock or condvar, since signalling can't happen while exit lock is held

#### struct proc

Several additions are necessary to `struct proc`:

**Parent PID:** stored in `pid_t ppid`

- This allows a child thread to keep track of its parent process

**Parent status:** stored in `bool parent_live`

- `1` if parent is live (has not exited)
- `0` if parent has exited
- If a thread exits, it first goes to all non-exited children and sets this value to `0` for each of them
- When a thread exits, it checks if this value is `1`, and if so, writes its exit status to `proc->status`

**Exit status:** stored in `sysret_t *status`

- This is equivalent to `&(ctle->status)` where `ctle` is the `struct ctlist_entry` for the given thread in its parent's `ctlist`
- If the parent exits, the `struct ctlist_entry` will be freed, but if the child checks `parent_live` and finds it to be false, it will not attempt to access this memory location
  - The global exit lock must be held by the parent in order to change the child's `parent_live` value, and by the child in order to check that value, so race conditions should not occur here

**Child thread list**: stored in `List ctlist`

```c
struct ctlist_entry {
    pid_t pid;
    struct thread *thread;
    Node node;
    sysret_t status;
}
```

#### fdtable

Make the fdtable struct stored directly in the proc struct, so that copying the proc struct results in copying the fdtable as well.

- Need to change all file descriptor related calls in `kernel/proc.c` to use `&(proc->fdtable)->table` instead of `proc->fdtable->table`
- Need to have the child process call `fs_reopen_file` on each open file

### General notes on memory and locks

- The process is responsible for cleaning up its own memory on exit
  - This includes the entries of the process's `ctlist`
- A `ctlist_entry` for a given child process is freed by the parent after their call to `wait` returns, or when the parent calls `exit`
- If a thread forks many children and does not wait on them, then the `ctlist` entries will sit allocated and with exit statuses stored until the thread calls `exit`
- When a process exits, it must first acquire the global `exit_lock`, and then check `parent_live` and write its exit status if so, and then go through every entry of its `ctlist` and for every entry (let be denoted `ctle`):
  - If `ctle->status == STATUS_ALIVE`, then update the child's `parent_live`value to `0`
  - Free the memory of the `ctlist_entry`
  - The thread pointer itself will have already been freed by the child process, or will be freed when the child exits
- Since the only process which may wait using a given process's PID is that process's direct parent, then there is no need to have another thread inherit the child if that child's parent exits before the child does

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

Corresponding process function:

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
proc_fork();
```

**Behavior:**

- A new process needs to be created through `kernel/proc.c:proc_init`
- Parent must copy its memory to the child via `kernel/mm/vm.c:as_copy_as` 
  - Any changes in the child's memory after `fork` is not visible to the parent
  - Any changes in the parent's memory after `fork` is not visible to the child
- Duplicate current thread (parent process)'s trapframe in the new thread (child process)
- Create a new thread to run the process
  - Do this via `thread_create` and `thread_start_context`
  - `sys_spawn` provides a great template for what `sys_fork` should do
  - The name of the child thread is equal to the name of the parent thread
  - The priority for the new thread should be `DEFAULT_PRI`
- Create a new `ctlist_entry` for the new thread (let it be denoted `ctle`)
  - Set `ctle->thread` to the child's thread struct
  - Set `ctle->pid` to the child process's PID
  - Add the entry to the parent process's `ctlist` using `list_append(ctlist, ctle->node)`
- Set up the child thread's `proc struct`
  - Set `ppid` to the parent's PID
  - Set `parent_live = 1`
  - Set `status = &new_ctlist->status`
  - Set `*status = STATUS_ALIVE`
- All the opened files must be duplicated in the new process (not as simple as a memory copy)
  - Go through every entry of the parent's `fdtable`, and for each one, have the child process call `fs_reopen_file`
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
- If the given PID is found, then `while (ctle->status == STATUS_ALIVE) { condvar_wait(wait_var) }`
  - Once this loop ends, the child process will have finished and written its value to `ctle->status`
  - Write the value of `ctle->status` to `*wstatus`
  - Remove `ctle` from the `ctlist` using `list_remove(currnode)`
  - Free `ctle`
  - Return the PID of the child
- If the given PID is `-1`, then loop through the entries of the `ctlist`, checking if `ctle->status != STATUS_ALIVE` until one of them returns true
  - Apply the same steps as above
  - Return the PID of the child for which `ctle->status != STATUS_ALIVE` returned true
- The exit lock is not necessary in the wait call, since it is only acquired by exiting threads or threads interacting with separate process data
  - One concern could be if a parent checks the status and is going to call `condvar_wait`, but the child thread signals before it gets the chance to do so
  - Since the wait is in a loop and should be preempted by the kernel at some point, this should be okay

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

- Acquire the global `exit_lock`
- Check the `parent_live` value:
  - If `parent_live == 1`, then write exit status to `*status`
  - If `parent_live == 0`, then do nothing -- The parent will have already freed the ctlist entry which had stored the exiting thread
- Go through every entry of the process's `ctlist` and for every entry `ctle`:
  - If `ctle->status == STATUS_ALIVE`, then set `ctle->thread->proc->parent_live = 0`
  - Always: Remove `ctle->node` from `ctlist` using `list_remove(ctle->node)`
  - Always: Free `ctle` -- Note that this does not free the thread, which has been freed already or will be freed by the child when it exits
- Go through every entry of the process's `fdtable` and for every file descriptor, call `sys_close(fd)`
- Free the process's data
  - The process's address space
  - The process's proc struct
  - The process's thread struct
  - `proc_exit`, `thread_exit`, and `thread_cleanup` will be of use

### Other functions

Will likely need functions for `validate_pid`, `get_pid`, `remove_pid`, though the latter two might be contained within `fork` and `wait`. These functions should also likely exist both in `sys_*` and `proc_*` form, though this remains to be seen.

## Risk Analysis

### Edge cases

- What happens if the parent checks the `ctlist` via `wait` before the child has a chance to be added there, thus returning `ERR_CHILD`?
  - Answer: This cannot occur, since the child process is added to the `ctlist` of the parent before the `fork` call returns for either the parent or the child.
- What if the parent waits before the child exits?
  - Answer: The parent will find the `ctlist_entry` for the given child and wait for `while (ctle->status == STATUS_ALIVE) { condvar_wait(wait_var) }` to finish, after which time the child must have exited
- What if the parent waits after the child exits?
  - Answer: The parent will find the PID in its `ctlist`, run `while (ctle->status == STATUS_ALIVE) { condvar_wait(wait_var) }` which will immediately finish (since child has already set status to something else, and the parent can then extract the status and carry out the usual cleanup
- What if the parent exits without waiting for the child?
  - Answer: The parent changes the child's proc struct `parent_live` value to `0`, thus indicating that the child should not attempt to set an exit status for its parent when it eventually returns. The parent frees all of its own data, and the residual data for all other child processes, leaving only the child thread pointers allocated. The child is orphaned, and when it exits, it does not report its exit status to any other process, and instead frees all its data and exits.

### Unanswered questions

- How do I deschedule a thread which calls wait
  - I only see functions in `include/kernel/sched.h` and `kernel/sched.c` for `sched_start`, `sched_start_ap` (what does that do?), `sched_ready`, and `sched_sched`, none of which put the thread in a waiting state
  - It seems as though scheduling and setting thread states should not be handled outside of these purpose-built functions
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
