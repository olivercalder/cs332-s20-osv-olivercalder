# Lab 3: Multiprocessing

## Important deadlines
- Design on `fork`, `wait`, `exit` due 5/11/2020 (Monday) at 9:00pm
- Lab Due 5/22/2020 (Friday) at 9:00pm.

## Introduction
This lab adds process management to osv. 
You will add support for the following UNIX system calls:

- `fork`: creates a copy of the current process, returning from the system call in each
context (but with different return values). 
- `wait`: allows a process to pause until a child process finishes executing.
- `exit`: terminates the current process and releases kernel resources given to the process.
- (OPTIONAL) `pipe`: implement basic inter-process communication through pipes, 
which will allow transfer of data from one process to another.
- (OPTIONAL) Extend `spawn` with argument passing, which
requires you to properly set up the user stack.

### Keeping track of processes
For lab 3, there is a provided a process table which tracks all processes and a lock
guarding this process table.

`ptable_dump` (in `proc.c`) is a debugging function that iterates 
over the process table and show every process's name and pid. 
Feel free to modify this function to dump more information 
if you extend the process struct.

### Partners
You can complete this lab with a partner if you wish. 
To work with a partner, one member of your group needs to send me an email by Friday 5/8 with
- The name of each partner
- Which of your two osv repositories you will use for this lab. 
I will then add the other person as a collaborator on that repo.

You can clone your partner's repo with
```
git clone git@github.com:Carleton-College-CS/cs332-s20-osv-PARTNERS-GIT-USERNAME.git
git remote add upstream git@github.com:Carleton-College-CS/osv-s20.git
```

### Writing design docs
For labs 3 and 4, I will ask you to write a small design document.
**Lab 3 will be time consuming and difficult**, there are many design decisions to be made.
- Part 1: Design for `fork`, `wait`, `exit` is due on 5/11.
- You will be grouped with 2 or 3 other students for a round of peer review on your 
design documents.
    - Peer review will be conducted over Slack. 
    Each peer review group will have a dedicated Slack channel.
    - Your feedback for your peer review group members is due 5/15.
    - Substantive participation in peer review will be part of your grade for the lab.
- You do not need to submit a design doc for either optional part, 
though writing one will be helpful if you attempt them.  

Follow the guidelines on [how to write a design document](designdoc.md). 
We have provided a [design doc template for lab 3](lab3design.md)
When finished with your design doc, tag your repo with:
```
git tag lab3_design
```

Don't forget to push the tags!
```
git push origin master --tags
```

For reference, the staff solution for this lab has made changes to
- `include/kernel/proc.h` 
- `kernel/proc.c`
- `kernel/syscall.c`

For the optional part 2:
- `include/kernel/fs.h`
- `include/kernel/pipe.h` (newly created file)
- `kernel/pipe.c` (newly created file)

## Configuration
To pull new changes, second lab tests and description, run the command
```
git pull upstream master
```
and merge.

**After the merge, double check that your code still passes lab 2 tests.**


## Part #1: Implement the `fork`, `wait`, and `exit` system calls
These system calls work together as a unit to support multiprocessing.
You can start by implementing `fork`, but one of the first tests will
have the new process call process `exit`.

osv `fork` duplicates the state of the user-level application. 
The system call `fork` returns twice:
1. Once in the parent, with the return value of the process
ID (pid) of the child
2. Once in the child, with a return value of 0

The open files should be shared between both the parent and the child, e.g.
calling read in the parent should advance the offset in both the parent
and the child. In short, a child process inherits a parent's open file table. 
The child process should have its own address space, meaning 

- any changes in the 
child's memory after `fork` is not visible to the parent
- any changes in the parent's memory after
`fork` is not visible to the child

### Preparation
To help prepare to do your implementation, complete these two exercises. 
You do not need to submit anything for them.

#### Exercise 1
Describe the relationship between 
- `kernel/sched.c:sched_ready`
- `kernel/sched.c:sched_sched`
- `arch/x86_64/kernel/cpu.c:cpu_switch_thread`

#### Exercise 2
Describe how you can set up the child process to return to where `fork` is called.
Think about 
- how the parent returns to user space
- how is the child process similar and 
different from the parent process

### Implementation
Implement `proc_fork` in `kernel/proc.c`. 
Once a new process is created, osv will run it concurrently via the
process scheduler. A hardware device generates a timer interrupt on
fixed intervals. If another process/thread is READY, the scheduler will switch
to it, essentially causing the current process to yield the CPU. 

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

## Synchronization between parent and child process 

`exit` takes in a status, which is the exit status of the process.
This needs to be saved somewhere, since its parent might ask for it in `wait`.
Note that `STATUS_ALIVE` (`kernel/proc.h`) is a reserved exit status value that will not be 
used by any process, so you can safely use `STATUS_ALIVE` as an initial value for exit status.

`exit` also needs to release kernel resources given to the process. 
On process creation, the kernel gives each process 
- an address space
- a kernel thread
- a process struct

All of these need to be cleaned up, plus any information tracked
by the process struct (open file table).

osv takes care of cleaning up the process's address space (`proc_exit`) and its 
thread for you (`thread_exit`, `thread_cleanup`), but you still need to clean up the rest 
and communicate your exit status back to the parent process. This communication is tricky. 
Let's say you decide to store the exit status in struct A (can be 
process struct or something else), you cannot free this struct until your parent has 
seen it.
One way to handle this problem is by relying on someone else (eg. your parent) to free it
after it has seen the exit status. 

The `wait` system call interacts with `fork` and `exit`. 
It is called by the parent process to wait
for a child. Note that a parent can 
- create multiple child processes
- wait on a specific child or 
any child to exit, and get its exit status.

You need to be careful with synchronization here. The
parent may call `wait` before any child calls `exit`.  In that case, the `wait`
should stall (e.g., use a condition variable) until the child waiting on exits. 
Note that the parent need not call `wait`; it can exit without waiting
for the child. The child's data structures must still be reclaimed
in this case when the child does eventually exit.

There are various ways that you can implement all of this, so you should
think through the various cases and design an implementation *before* you start to code. 
If you choose to let the parent clean up its exited children.
you will need some way to reclaim the children's data when the parent exits first. 
(Note that in osv as in UNIX, the initial(`user/init.c`) process never exits, 
meaning an exiting process could "hand off" its children to this process.)

In short, cases you need to think through are:
  - parent waits before child exits
  - parent waits after child exits
  - parent exits without waiting for child

### Implementation
Implement `exit` and `wait`. This should include the system calls (`sys_exit` and `sys_wait`),
and the underlying functions in `proc.c`, `proc_exit` and `proc_wait`.

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

/* Exit a process with a status */
void proc_exit(int);

/*
 * Wait for a process to change state. If pid is ANY_CHILD, wait for any child process.
 * If wstatus is not NULL, store the the exit status of the child in wstatus.
 *
 * Return:
 * pid of the child process that changes state.
 * ERR_CHILD - The caller does not have a child with the specified pid.
 */
int proc_wait(pid_t, int* status);
```

## OPTIONAL Part 2: Pipes

A pipe is a sequential communication 
channel between two endpoints, supporting writes on one end, and 
reads on the other. Reads and writes are asynchronous and buffered, 
up to some internally defined size. A read will block until there are 
bytes to read (it may return a partial read); a write will block if there is
no more room in the internal buffer. Pipes are a simple way to support interprocess 
communication, especially between processes started by the shell, with the system
calls you just implemented. 

### Implementation
Add support for pipes. In terms of implementation, pipes can use a bounded buffer 
described in Chapter 5; the pipe does not specify how large the bounded buffer needs to be,
but you may use 512 bytes as the buffer size. In addition to the buffer, you will also need 
to track metadata about the pipe. You should be able to write any number of bytes to a pipe. 
Concurrent operations can happen in any order, but each operation completes as an atomic unit. 

Since pipes support concurrency, your pipe implementation will
need to use spinlocks and condition variables.  Note that with
`dup` you may also have multiple readers and writers on the same pipe.

The `pipe` system call creates a pipe (a holding area for written
data) and opens two file descriptors, one for reading and one for writing.
A write to a pipe with no open read descriptors will return an error.
A read from a pipe with no open write descriptors will return any
remaining buffered data, and then 0 to indicate EOF.

```c
/*
 * Corresponds to int pipe(int* fds);
 * 
 * Creates a pipe and two open file descriptors. The file descriptors
 * are written to the array at fds, with fds[0] the read end of the 
 * pipe and fds[1] as the write end of the pipe.
 * 
 * Return:
 * ERR_OK on success
 * ERR_INVAL if fds address is invalid
 * ERR_NOMEM if no 2 available new file descriptors
 */
sysret_t
sys_pipe(void *arg);
```

## OPTIONAL Part 3: Setting up stack for user program

We have provided a simple version of `proc_spawn` that only sets up a minimal stack for user process.
You need to extend `spawn` to properly set up the stack to pass arguments to a new process.

Let's walk through an example where a new process is spawned with `spawn("cat README smallfile")`:
- `sys_spawn` first parses the string "cat README smallfile" into an array of strings separated by
whitespace ended with a `NULL` entry
- then passes the array to `proc_spawn` and `stack_setup`.

You will use this array to complete `stack_setup` in `kernel/proc.c`. You will only set up one page of
stack. When setting up the user stack, we need to be careful. Note that every user program in
osv has the same definition of main.

``` C
int
main(int argc, char *argv[])
```

Note that
- `argv` is a pointer to an array of strings
- `argc` is the length of the array of strings

In the previous example, this means you have to copy `cat`, `README`
and `smallfile` to the user stack. They will be the data `argv` points to. 

Next, you need to set up the `argv` array. Word-aligned accesses are faster than unaligned accesses, 
so for best performance round the stack pointer down to a multiple of 8 before setting up `argv`. To set up `argv`, you need to create an array on the user stack where the 
- 0th index points to `cat`
- 1st index points to `README`
- 2nd index points to `smallfile` 
- and 3rd index element is `NULL`.

Then, you need to push 
- `argv` (the user stack address of `argv[0]`)
- `argc` (length of `argv`)
- and a fake "return address"

Although the entry function will never return, its stack frame must
have the same structure as any other. According to x86\_64 calling convention, `argc` and `argv`
should be set to `%rsi` and `%rdi`, this is taken care of in `tf_proc`. `stackptr` is a kernel address referring to the user stack page. Before you can copy things 
onto the stack, you need to first allocate space on the stack by subtracting the number of bytes
you will push, and then copy the content. To set up the new `argv`, you need to translate the address
of the newly pushed argument from kernel virtual address to user accessible address, you can use
`USTACK_ADDR()` to do so.

### Implementation
Implement `stack_setup`.

```c
/*
 * argv is an array of strings; argv[0] is the name of the 
 * file; argv[1] is the first argument; argv[n] is NULL signalling the
 * end of the arguments. Store final stackpointer at *ret_stackptr.
 */
err_t stack_setup(struct proc *p, char **argv, vaddr_t* ret_stackptr);
```

#### Stack layout
Call `as_dump` with the new process's address space and its stack pointer to observe the stack contents.

This is NOT the only possible stack format.

Here is our solution code's stack layout for `ls`:
```
dumping memregion 0xFFFFFF7FFFFFEFD0 to 0xFFFFFF7FFFFFF000
vaddr: 0xFFFFFF7FFFFFEFD0 | data (hex):  0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFD8 | data (hex):  0x1 0x0 0x0 0x0 0x0 0x0 0x0 0x0  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFE0 | data (hex):  0xE8 0xEF 0xFF 0xFF 0x7F 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFE8 | data (hex):  0xFD 0xEF 0xFF 0xFF 0x7F 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFF0 | data (hex):  0x0 0x0 0x0 0x0 0x0 0x0 0x0 0x0  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFF8 | data (hex):  0x0 0x0 0x0 0x0 0x0 0x6C 0x73 0x0  | data (ascii): .....ls.|

```
And for `cat README smallfile`:
```
dumping memregion 0xFFFFFF7FFFFFEFB0 to 0xFFFFFF7FFFFFF000
vaddr: 0xFFFFFF7FFFFFEFB0 | data (hex):  0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFB8 | data (hex):  0x3 0x0 0x0 0x0 0x0 0x0 0x0 0x0  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFC0 | data (hex):  0xC8 0xEF 0xFF 0xFF 0x7F 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFC8 | data (hex):  0xFC 0xEF 0xFF 0xFF 0x7F 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFD0 | data (hex):  0xF5 0xEF 0xFF 0xFF 0x7F 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFD8 | data (hex):  0xEB 0xEF 0xFF 0xFF 0x7F 0xFF 0xFF 0xFF  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFE0 | data (hex):  0x0 0x0 0x0 0x0 0x0 0x0 0x0 0x0  | data (ascii): ........|
vaddr: 0xFFFFFF7FFFFFEFE8 | data (hex):  0x0 0x0 0x0 0x73 0x6D 0x61 0x6C 0x6C  | data (ascii): ...small|
vaddr: 0xFFFFFF7FFFFFEFF0 | data (hex):  0x66 0x69 0x6C 0x65 0x0 0x52 0x45 0x41  | data (ascii): file.REA|
vaddr: 0xFFFFFF7FFFFFEFF8 | data (hex):  0x44 0x4D 0x45 0x0 0x63 0x61 0x74 0x0  | data (ascii): DME.cat.|
```
An explanation of the above memory dump: 
```
Address               	Name	         Data	              Type  
0xFFFFFF7FFFFFEFFC  argv[0]content    cat\0               char[4]          <--|
0xFFFFFF7FFFFFEFF5  argv[1]content    README\0            char[7]       <--|  |
0xFFFFFF7FFFFFEFEB  argv[2]content    smallfile\0         char[10]   <--|  |  |
0xFFFFFF7FFFFFEFE8  word alignment    0                   uint8_t       |  |  |
0xFFFFFF7FFFFFEFE0  argv[3]           NULL                char*         |  |  |
0xFFFFFF7FFFFFEFD8  argv[2]           0xFFFFFF7FFFFFEFEB  char*     -----  |  |
0xFFFFFF7FFFFFEFD0  argv[1]           0xFFFFFF7FFFFFEFF5  char*     --------  |
0xFFFFFF7FFFFFEFC8  argv[0]           0xFFFFFF7FFFFFEFFC  char*     -----------   <--|
0xFFFFFF7FFFFFEFC0  argv              0xFFFFFF7FFFFFEFC8  char**    ------------------ 
0xFFFFFF7FFFFFEFB8  argc              3                   int
0xFFFFFF7FFFFFEFB0  return address    -1                  void*
```
In this example, the stack pointer would be initialized to `0xFFFFFF7FFFFFEFB0`.

## Testing
After you implement the system calls described above, test lab 3 code by either running 
individual tests in osv shell, or run `python3 test.py 3`.

## Handin
When you're finished, create a `end_lab3` git tag as described above.
