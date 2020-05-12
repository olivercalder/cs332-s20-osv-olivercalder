# Lab 3 Design Doc: Multiprocessing

## Overview

The goal of this project is to implement the core system process-management calls, as well as the underlying process functions which handle the creation/destruction of and interaction between threads for a given process. A secondary goal is to implement inter-process communication through pipes, allowing input and output from multiple processes to be redirected to/from one another.

### Major Parts

#### fork

`fork` creates a new process identical to the current process, and creates a new thread to run that process. The parent process adds the new child thread to its list of threads, and the new child process holds the PID of its parent. `fork` returns twice: it returns the process ID of the new child process to the parent process, and it returns `0` to the child thread.

The primary data structures which will be modified when calling `fork` are:
- a new `struct proc` for the child
- a new `struct thread` for the thread, containing the new `struct proc`
- a duplicate trap frame for the child thread
- the new entry if the calling thread's child thread table (`cttable`) is allocated to the child thread, and the child PID and `struct thread` are stored there
- child thread calls `sleeplock_acquire` on the lack for that entry of the table
  - This allows the parent process to wait on the child process by calling `sleeplock_acquire` on that lock, which will only return once the child process has released the lock by exiting

#### wait

`wait` pauses the execution of the current thread until the child thread with the given PID returns. If the PID is -1, wait until any child process returns. `wait` returns the process ID of the child process that terminated, and copies the process's exit status to the memory location specified by the arguments to `wait`.

The primary data structures of interest for `wait`:
- The child thread table for the process calling `wait`
- The sleeplock for the `cttable` entry corresponding to the child process with the given PID
- The return status of the process at that `cttable` entry

#### exit

`exit` halts the execution of the thread and frees its resources. Before the thread exits, it saves its exit status in its parent's `cttable` entry, if the parent has not already exited.

The primary data structures which will be modified when calling `exit` are:
- The child thread table for the process calling `exit`, which should be nearly all freed
- If the parent process has not exited:
  - Write the exit status to the parent's `cttable` entry
- If the parent process has exited:
  - Free the sleeplock

## In-depth Analysis and Implementation

### Bookkeeping

- `include/kernel/proc.h` provides the definitions for `struct proc` and the process calls which will need to be made
- `include/kernel/thread/h` provides the definitions for `struct thread` and the thread creation, scheduling, and cleanup calls
- `include/kernel/list.h` defines the `List` type and functions, in case these need to be used
- `include/kernel/sched.h` defines scheduling functions
- `include/kernel/synch.h` defines synchronization structures and functions, which will be useful for implementing waiting based on condition variables (sleeplocks)

### Key Data Structures

#### struct proc

Several additions are necessary to `struct proc`:

**Parent PID**: stored in `pid_t ppid`
- This allows a child thread to learn about or interact with its parent process

**Parent status**: stored in `bool parent_live`
- `1` if parent is live (has not exited)
- `0` if parent has exited
- If a parent exits, it first goes to all non-exited children and sets this value to `0` for each of them
- When a thread exits, it checks if this value is `1`, and if so, writes its exit status to the 

**Parent cttable entry**: stored in `struct cttable_ele *parent_entry`
- This allows a process to quickly find its entry in its parent's child thread table
- The structure of both the the table entries and the table itself are defined below

**Child thread table**: stored in `struct cttable *cttable` (to replace `List threads`, which only stores `struct thread`s)
```c
struct cttable_ele {
    bool allocated;         // Identifies whether this slot of the table is currently allocated (1) or available (0)
    pid_t cttable_pid;      // Stores a copy of the process's PID in case the thread struct is reclaimed before the parent has a chance to check its PID
    struct thread *thread;  // The pointer to the actual thread struct
    struct sleeplock *lock; // The lock which prohibits the a process from being able to access a child thread's return value until it is finished
    sysret_t status;        // The exit status of the child process, which is undefined until the sleeplock is released by the 
}

struct cttable {
    struct cttable_ele table[MAX_CHILDREN]; // MAX_CHILDREN = 256
    int capacity;       // = MAX_CHILDREN
    int count;          // the current number of child threads in the table
    int first_full;     // provides a starting point for where to search through child PIDs
    int first_empty;    // provides the index of the first available slot in the table
}
```
- There should be no lock needed on the `struct cttable` as a whole, since only the process which owns the cttable will ever modify its overall form
  - Individual child threads will only ever modify their specific entry in the table, but not the table as a whole
- The sleeplock is stored as a pointer so that the parent will be able to exit and be reclaimed without interfering with the ability of the child process to release the spinlock successfully
- The `first_full` and `first_empty` indices allow the search process to be quicker by not needing to search through many non-viable entries in the table


### General notes on memory and locks

- The process is responsible for cleaning up its own memory on exit
  - This includes the process's `cttable`
- sleeplocks are allocated on a child-by-child basis and freed after use
- A sleeplock for a given child process is freed by the parent after their call to `wait` returns
- If the parent exits before the child process exits, then it is the responsibility of the child to free its own sleeplock
- If a process forks many children and does not wait on them, then the `cttable` entries will sit allocated and with exit statuses stored
- When a process exits, it must go through every entry of its `cttable` and for every entry:
  - Check if the entry is terminated by calling `sleeplock_try_acquire`
  - If `sleeplock_try_acquire` returns `ERR_OK`, then free the memory of the sleeplock
  - The thread pointers themselves will have already been freed by the child processes
  - If the child process has not terminated, then do not free the sleeplock or the thread pointers, but once done, free the enture `cttable`, thus freeing the memory of all the pointers, but not the structs they point to
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
- Parent must copy its memory to the child via `kernel/mm/vm.c:as_copy_as` 
  - Any changes in the child's memory after `fork` is not visible to the parent
  - Any changes in the parent's memory after `fork` is not visible to the child
- All the opened files must be duplicated in the new process (not as simple as a memory copy)
- Create a new thread to run the process
  - The name of the child thread is equal to the name of the parent thread + `" child"`
  - The priority for the new thread should be `0` in order to enable interrupts
- Allocates the first open slot (let be denoted `i`) in the calling process's child thread table to the child thread
  - Sets `allocated` to `1`
  - Sets `cctable_pid` to the child process's PID
  - Sets the `thread` pointer to the child's thread struct
  - Allocates a new sleeplock using `kmalloc`, sets the lock pointer to it
- Set `cttable->first_full = MIN(cttable->first_full, i)`
- Set `cttable->first_empty = (i + 1) % cttable->capacity`
- Set `cttable->count += 1`;
- Set up the child thread's `proc struct`
  - Set `ppid` to the parent's PID
  - Set `parent_live = 1`
  - Set `parent_entry` to the memory address of the `struct cttable_ele` where the child thread will be stored in its parent's `cttable`
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
- Checks the `cttable` for the given child PID
- Keeps looking through the table until either the corresponding PID is found or a number of allocated entries equal to `cctable->count` have been found
  - If `cttable->count == 0`, then return `ERR_CHILD` immediately
- If the given PID is found, then call `sleeplock_acquire` on the lock for that table entry (let the index be denoted `i`)
  - Once the sleeplock is acquired, then the child process must have finished
  - Extract the exit status from the struct and save it to be returned later
  - Set the `allocated` status to `0`
  - Release and then free the sleeplock
  - Set `cttable->first_empty = MIN(cttable->first_empty, i)`
  - If `i == cttable->first_full`, then set `cttable->first_full = (i + 1) % cttable->capacity`
  - Set `cttable->count -= 1`
  - Return the PID of the child
- If the given PID is `-1`, then loop through the entries of the `cttable`, trying `sleeplock_try_acquire` on each until one of them returns true
  - Apply the same steps
  - Return the PID of the child for which `sleeplock_try_acquire` returned true
- If the given PID is not found in the table (and is not `-1`), then return `ERR_CHILD`

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
- Go through every entry of the process's `cttable` and for every entry:
  - Check if the entry is terminated by calling `sleeplock_try_acquire`
  - If `sleeplock_try_acquire` returns `ERR_OK`, then free the memory of the sleeplock
  - The thread pointers themselves will have already been freed by the child processes
  - If the child process has not terminated, then do not free the sleeplock or the thread pointers, but do set the child's `thread->proc->parent_live = 0`
- Once all entries have been checked, free the enture `cttable`, thus freeing the memory of all the pointers, but not the structs they point to
- If the process's `parent_live` value equals `1`, then set the process's `parent_entry->status` to the exit status of the process
- If the process's `parent_live` value equals `0`, then free the sleeplock
- Free the process's data
  - The process's address space
  - The process's proc struct
  - The process's thread struct

### Other functions

Will likely need functions for `validate_pid`, `get_pid`, `alloc_pid`, and `remove_pid`, though the latter two might be contained within `fork` and `wait`. These functions should also likely exist both in `sys_*` and `proc_*` form, though this remains to be seen.

## Risk Analysis

### Edge cases

- What happens if the parent checks the thread list via `wait` before the child has a chance to be added there, thus returning `ERR_CHILD`?
  - Answer: This cannot occur, since the child process is added to the `threads` list of the parent before the `fork` call returns for either the parent or the child.
- What if the parent waits before the child exits?
  - Answer: The parent will call `sleeplock_acquire` and wait for the child to exit, then the parent's call to `wait` will return.
- What if the parent waits after the child exits?
  - Answer: The parent will find the PID in its `cttable`, call `sleeplock_acquire`, and immediately acquire the sleeplock, indicating that the child process has finished. It may then carry out the usual `wait` cleanup and exit status behavior.
- What if the parent exits without waiting for the child?
  - Answer: The parent changes the child's proc struct `parent_live` value to `0`, thus indicating that the child should not attempt to set an exit status for its parent when it eventually returns, and that the child is in charge of freeing its own sleeplock in addition to all the other data associated with the child. The parent frees all of its own data, and the residual data (essentially just the sleeplock) for all other terminated child processes, leaving only the child thread pointer and sleeplock pointers un-freed. The child is orphaned, and when it exits, itdoes not report its exit status to any other process, and instead frees all its data and exits.

### Unanswered questions

- How do I deschedule a thread which calls wait
  - I only see functions in `include/kernel/sched.h` and `kernel/sched.c` for `sched_start`, `sched_start_ap` (what does that do?), `sched_ready`, and `sched_sched`, none of which put the thread in a waiting state
  - It seems as though scheduling and setting thread states should not be handled outside of these purpose-built functions
- How can a new child process acquire a sleeplock before returning from the `fork` call?
- Is a child process able to modify the `cttable` of its parent, given that the memory is separate?
  - Likewise, can the parent change its children's proc values, such as `parent_live`?
  - Does there need to be some independent mutually-accessible memory region shared by the parent and child where the 
- Can a process free itself?
  - If not, probably need to work out a different way for grandparent threads to inherit child threads when the parent exits, and for those grandparent threads to then clean up the child threads when they exit

### Staging of Work

First, I will implement the changes to the structs in `include/kernel/proc.h`. Then, I will write the `proc_*` functions which will serve as the backing for the `sys_*` functions. Then, I will write the `sys_*` functions, which will provide validity checking for the inputs before calling `proc_*`. If I have time, I will then work on implementing `sys_pipe` and `sys_spawn`.

### Time Estimation:

- Struct changes:
  - Best case: 0.25 hour
  - Average case: 0.5 hour
  - Worst case: 3 hours (if a mutually-accessible external memory location is necessary to exchange data between the parent and child processes or if I have to use some form of `List`)
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
  - Worst case: 2 hours (if I have to use a `List`)
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
