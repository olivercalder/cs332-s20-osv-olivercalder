# osv Overview

The overarching goal of osv (the experimental kernel) is to help you
understand operating system implementation at an advanced level.
The baseline code for osv provides a complete, but limited,
working operating system, capable of running a single program
(loaded from disk) that can receive and print characters to a serial port
(keyboard and display). It also contains basic synchronization,
process time slicing, memory management, and a file system.

The lab assignments will task you with adding basic UNIX-like functionality to
this baseline in a series of three projects: (i) implement file system
calls including managing file descriptors, (ii) support a user-level shell
by implementing UNIX fork, wait, and pipe, (iii) add sbrk, dynamic stack growth,
and copy-on-write semantics. Except for the first lab,
labs will require your code to carefully consider data structure
synchronization. Prior to these three projects, you have a lab focused on 
exploring osv, particularly the properties of the osv file system.

## Baseline Code Walkthrough

We next walk through the code and main data structures.

### Directory structure

arch — architecture dependent code for kernel and user. You shouldn't have to make any changes here,
        but if you want to understand how OS interacts with hardware, this is the place.

include — header files for all architecture independent code. Provides detailed documentation on OS
           subsystem interfaces (virtual memory, file system) -- they are contracts that each OS component provides.

kernel — the operating system kernel code which implements the above contracts.

user — user applications that run on osv. This will be updated during the term with test cases for each lab.

lib — library code shared by kernel and user, some are for user programs only.

labs — lab descriptions. These are updated during the term.

tools — utility programs for building osv.

### Booting

The first "program" that gets executed as an x86 PC boots is the BIOS. The BIOS, after performing hardware initialization,
loads the bootloader from a fixed location on disk into a fixed
location in memory and jumps to it.  The bootloader
in turn loads the operating system (osv) kernel from a predefined location on
disk into a fixed location in memory and jumps to it.

When compiled with `x86_64` flag, osv uses `arch/x86_64` folder for architecture
dependent code. The x86 is backwardly compatible with earlier operating systems
(e.g., 16 bit or 32 bit), so when it boots it starts out in a more
primitive mode.  Part of the task of the bootloader is to shift the
processor to 64 bit mode.
gdb is not set up to be able to debug both 32 and 64 bit mode simultaneously,
so we start debugging at the beginning of osv, rather than from the
beginning of the bootloader.

For reference, the bootloader code is `bootasm.S`, `bootmain.c`, and `multiboot.h`.
You can safely ignore these files.


### Initialization

`kernel/main.c` — initialization code

In function `main`, the kernel needs to do several things as it starts up.
Note that the ordering of the function calls in `main` very much matters — for example, we need the ability to allocate 
memory before we can build the kernel page table, etc.
We list files in the order they are referenced by `main`.

The first step is to figure out how much physical memory the machine has, and then
to make that memory available for dynamic allocation. (Different
machines are configured with different amounts of physical memory, but the kernel
code is the same regardless.) This is done by `vm_init`, which internally invokes functions in the following files:

`arch/x86_64/kernel/mm/pmem.c` — architecture specific code to read machine registers to determine
the range of physical addresses (DRAM) that are available to the OS and those used by the IO devices.

`kernel/mm/pmem.c` — kernel code to manage the available physical memory. It first uses a simple first-fit allocator,
and later switches to a more sophisticated buddy allocator. Allocates physical memory in multiples of page size (4096).

`arch/x86_64/kernel/mm/vpmap.c` — kernel code for manipulating page tables.
Use protection bits in page table entries to ensure user code cannot access
kernel memory, even though it is "mapped" into the user address space.

Once physical memory allocator is initialized, the kernel can set up the
kernel page table, and maps all physical memory into the kernel address space.
`memory.md` describes the memory layout in more detail. For now, you should understand that in osv,
both user code and kernel code runs with translated (virtual) addresses.

Natively, the kernel has access to all physical memory, so it could just
run using physical addresses.  However, by mapping we allow the kernel
to access both user addresses and kernel addresses at the same time:
user code/data is in low virtual addresses, while kernel code/data
has high virtual addresses.  While user address mappings can be complex and partial
(e.g., for virtual memory paging), kernel address mapping is simple:
we simply add a (very large) constant offset to every physical address to get the mapped virtual address.

Note that this can be confusing! A user virtual address and a kernel
virtual address can refer to the same physical memory location, and it
is easy to forget that kernel memory can refer to either its location
in physical memory or its remapped kernel virtual address.

`kernel/mm/kmalloc.c` — kernel virtual memory allocator.

`kernel/mm/vm.c` — virtual memory management. Adds a level of
indirection so virtual memory actions can be architecture independent.
We'll have more to say about `vm.c` later.

`arch_init` invokes functions in the following files:

`arch/x86_64/kernel/mp.c, lapic.c` — code to setup multiprocessor execution. osv runs on 2 CPUs.

`arch/x86_64/kernel/pic.c, ioapic.c` — IO interrupts

`arch/x86_64/kernel/mm/vm.c:seginit` — initializes the per-CPU segment table.

`arch/x86_64/kernel/trap.c` — code to handle interrupts, exceptions, and system calls.
Initially, we just need to set up the interrupt table — the set of handlers for where the hardware
should jump to on different types of interrupts and system calls.

Again, ordering matters — we need to set up the interrupt table before we enable interrupts!

You will need to understand the trap code in detail, so we'll describe
that shortly.

`kernel/thread.c` — the kernel threading system. Threads are the unit of execution.

`kernel/synch.c` — synchronization primitives (locks and condition variables)

`kernel/proc.c` — code to manage the process' life cycle. Each process has its own address space and metadata.

`kernel/trap.c` — machine independent part of trap handling, where trap handlers are registered and invoked.

`kernel/console.c, kernel/drivers/uart.c` — Console, uart driver. The specific initialization code depends on the device
(e.g., the specific model number of the serial port). Understanding these device code therefore requires reading device manuals,
something you can skip for now.

However, note that `kprintf` assumes that the console and uart are initialized!
So don't try to print anything before this point.  (You can of course
use gdb to set a breakpoint to examine the contents of memory.)

`kernel/sched.c` — kernel scheduler. Sets the current thread as this CPU's idle thread,
turns on interrupt for this CPU and starts scheduling. The current thread (idle thread initially)
schedules the next thread to run.

`arch/x86_64/kernel/swtch.S` — code to context switch between threads.

At the end of `main`, a new thread is created to run `kernel_init` which initializes the storage system and
starts up the second CPU.

`kernel/bdev.c` — Machine independent block device interface.

`kernel/drivers/ide.c` —  device specific code for the (IDE) disk device.

`kenrel/fs/fs.c` — a virtual file system (VFS) interface that defines operations a file system needs to support. Also defines general file system operations that other parts of the kernel can use.

`kernel/fs/sfs/sfs.c` — a simple file system implementation that implements the VFS layer. This is the default file system in osv.

`kernel/proc.c:proc_spawn` — called after all initialization is done. Sets up the first
process by copying the application code/data from a file on disk into memory.
It also sets up the page table so that the hardware will translate
user addresses to their corresponding physical addresses.
We don't immediately jump to to the user program, however.
Rather, we mark it as "schedulable" by calling `thread_start_context()`. The user process will get scheduled later on a timer interrupt.

### Traps

The osv code for handling traps: interrupts, exceptions, and system
calls is spread over several files. You will find this discussion easier
to follow if you have already done the assigned readings for week 2.

`arch/x86_64/kernel/vectors.S` — the interrupt table (gets generated when you run `make`).
More precisely, `arch/x86_64/kernel/trap.c:idt_init` sets up a gate for each entry point in `vectors.S`.
The gate specifies what to do on each type of interrupt, e.g., including whether to take
the interrupt in kernel mode or not.

Some trap handlers push a value onto the stack, and some don't. This is
because the x86 puts different information on the kernel/interrupt stack
for different types of interrupt (e.g., a page fault pushes the failing
virtual address, while a timer interrupt does not push anything).
For those that don't have a value, we push one to ensure that
the format of the stack is the same regardless of how we get there.

`arch/x86_64/kernel/trapasm.S:alltraps` — generic assembly code to
handle traps, that is, interrupts, system calls, and exceptions.
Because we could get here on an interrupt, we need to
be able to restore the entire state of the interrupted process.  So we first
save all the registers onto the stack, move a pointer to the stack into
a register holding the first argument to a procedure, and then
call into the C procedure `arch/x86_64/kernel/trap.c:trap`.

When we return in `trapasm.S:trapret`, we restore the state of the interrupted
process, by restoring its registers, popping the stack frame inserted
by the interrupt handler, and then returning from the interrupt with `iretq` (interrupt return).

Note that we can also get to `trapasm.S:trapret` in one other way. When
we create a new process (`kernel/proc.c:proc_spawn, proc_fork`), we have to create the
kernel thread for the process and switch back to user space. We do this by making the switch code "return" to
`trapret` (`arch/x86_64/kernel/thread.c:thread_start_context`), so that `trapret` will "restore"
the state of the registers to the initial value expected by the process.

`kernel/trap.c` — Generic trap handling code.
Once we have saved the registers, `trapasm.S:alltraps` calls `arch/x86_64/kernel/trap.c:trap` which
calls `kernel/trap.c:trap_invoke_handler`. This takes in the trap number and the pointer
to the processor register state, i.e. the trapframe, which is saved by the hardware (as it takes the
interrupt and indirects through the vector table) and by software (in `alltraps`).

Depending on the type of interrupt, we perform different actions. One
possibility is a timer interrupt, in which case we need
to run the scheduler code and schedules a different thread to run.
Alternately, this could be a system call, in which case we need to jump to the generic system
call handler in `kernel/syscall.c`.

In the case of a system call, the value in each register, where we might
pass the arguments to the system call, will have been saved and overwritten
as part of going from `vectors.S` to `alltraps` to `trap` to `syscall`. So we
store the trapframe in the process struct,
and then access them in the system call handlers depending on which type
of handler we need to invoke.

`kernel/syscall.c` — code to handle system calls.  Again, we have a table of
possible system calls, which we index using the value of the first
argument to the system call, inserted by the user-level library code
(see `arch/x86_64/user/usyscall.S`). The architecture dependent part of
system calls(`arch/x86_64/kernel/syscall.c:fetch_arg`) is responsible for
invoking the architecture independent syscall interface and fetch user arguments.
`kernel/syscall.c` contains implementations of the file system and process
system calls.  Most of the system calls are unimplemented stubs due
to be filled in by the next few labs. We have provided implementations
of `sys_write` as a simple example (and to allow us to print
to the serial port even if your other system calls aren't working).

`include/lib/syscall-num.h` — the assignment of system calls to system call numbers.
This needs to be the same for user programs and the kernel!

`arch/x86_64/user/usyscall.S` — macros to generate user library code for each system call.
We simply put the system call code into a caller-saved register and
invoke the trap. The other parameters to the system call will be handled
normally.

### Processes, threads and synchronization

For the most part, you will be able to ignore the process and synchronization
code for labs 1 and 2. We include a description for completeness (and
because you'll need it for lab 3). This description will be easier
to follow if you have read Chapters 26, 27, and 28 in OSTEP.

Each process has an address space (a page table and a set of memory regions),
a list of threads (for this course, a process will have only one thread), a process ID,
a reference to parent process (if any), a reference to its current working directory.
A thread can run inside a process. It has a stack to run in the kernel, a state (ready, sleeping, ...),
the current trapframe, and space to save its scheduling context.

`kernel/proc.c` has several interesting procedures. One is `proc_init`.
This allocates a process struct and initializes it. Each process needs at least
one thread to run its code. `kernel/thread.c:thread_create` creates a thread, allocates a kernel stack,
and attaches the thread to the process. Now that a process has set up its address space, and has
a thread to run, it can start running by calling `thread_start_context`.

All runnable threads are on the ready queue (`kernel/sched.c`). The queue is only accessible through
the scheduler interface. Each running thread selects another thread to run when its time slice is up.

osv supports two kinds of locks to enforce mutual exclusion. The difference
concerns what they do when the lock is busy.

`kernel/synch.c:spinlock_*` — A spinlock spins in a loop until the lock is free.

`kernel/synch.c:sleeplock_*` — A sleeplock yields the processor until the lock is free.

A given data structure will generally be protected by either a spinlock
or a sleeplock. There is no guarantee of mutual exclusion if they are mixed.

The low level routines of the kernel typically use spinlocks; for example,
the scheduler needs to use a spinlock as if the scheduler
is busy — you won't be able to put the current process to sleep and pick
another process to run, until the scheduler lock is free.

Similarly, it is generally a bad idea to try to acquire a
sleeplock in an interrupt handler.  Most interrupt handlers
are structured to avoid accessing shared data as much as possible, but
where it is needed, they should use a spinlock. Care must be taken
that interrupts are disabled whenever a spinlock is held, or else
the interrupt handler could get into an infinite loop (waiting for
a spinlock that is held by the interrupted process).

Another synchronization primitive osv offers is condition variables (`kernel/synch.c:condvar_*`).
See read OSTEP chapter 30 for details on condition variables.

A final bit of complexity in the process abstraction concerns the
context switch code (`arch/x86_64/kernel/swtch.S`).
You will need to look at the code, trace the execution into and out of context switch,
and so forth, before you really understand it. You can ignore
it for labs 1 and 2.

### Memory management

Although the memory management code is complex (reflecting the
intricacies of x86), the abstractions are fairly straightforward.
The key idea is that we have two sets of page tables for every process.
One is machine-dependent, with a structure defined by the particular
architecture.  For example, on a 64-bit x86, there are four levels of
page tables, a particular format for each page table entry, etc.

In addition, osv has a machine-independent page table, represented
as a set of `memregions` — contiguous regions of the virtual address space
used for a specific purpose (such as to hold code or data). The reason for
this split is to (i) make osv more portable (not tied to a specific page
table format), and (ii) to allow extra information to be stored per
page as necessary (the reason for this will become obvious in later
assignments).

We have routines to generate the machine-dependent page table from
the machine-independent one on demand — essentially, every time
you make a change to the machine-independent one, you'll need to
invalidate (`kernel/mm/vm.c:memregion_invalidate`) the old machine-dependent
page table, to get the system to generate a new (machine dependent) one.

Thus, a process has information about its virtual address space,
its file descriptors, and its threads.

`include/kernel/vm.h:addrspace` describes the virtual address space of a process.
It consists of a list of virtual memory regions (`include/kernel/vm.h:memregion`),
and also a pointer to the x86 page table (`vpmap`).

`include/kernel/vm.h:memregion` is a 4KB page aligned region of virtual memory
of indefinite page-aligned length. A set of `memregion`s defines the virtual
addresses accessible to a process.  Addresses outside of a `memregion` are considered
invalid.

Operations on `memregion` include: extending the region, mapping a region to an address space,
copy a `memregion`, etc.

A `memregion` tracks its address space, its range, permission of the region, whether it's shared by
multiple address spaces, and its backing `memstore` if any (used for memory mapped files, don't need to
worry about it).

### File system

The file system consists of a VFS layer (`include/kernel/fs.h`) which
specifies the file system interface, and a concrete implementation (`include/kernel/sfs.h`).

In lab 1 we ask you to explore the osv file system organization and how it's using direct pointers, indirect pointers, 
and data blocks. In lab 2 we ask you to implement the system calls relating to opening, closing, and reading files.
Since the file system code is provided as part of the baseline, both these labs primarily
involve understanding the file system interface and use the corresponding data structures and functions.

The on-disk data structure representing a file is called
an inode. A copy of the inode can be stored in memory, e.g., if the file
is open. If so, there is a bit more information kept, e.g., the
reference count. `open` loads an inode if it doesn't already exist
in memory.

The file system locates the on-disk inode by doing a directory lookup.
A directory contains multiple (filename, inode number) pairs, where the inode number is an index into a disk region that
stores all the inodes. Note that the inodes don't actually store file data —
they simply record where on disk to locate file data for that file.
