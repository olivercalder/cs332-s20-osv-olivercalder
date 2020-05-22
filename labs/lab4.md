# Lab 4: Address Space Management
**Design Doc Due: 5/27/20 at 9pm. \
Peer Review Due: 6/1/20 at 9pm. \
Complete Lab Due: 6/8/20 at 9pm.**

## Introduction
In this lab, we are going to cover address space management. 
Optionally, you will also implement a common technique to save memory.

osv uses an address space and memory regions to track information about a virtual address space. A virtual address space ranges 
from virtual address `0` to `0xffffffffffffffff` on a 64-bit machine. A memory region tracks a contiguous region of memory within 
an address space. A memory region belongs to only one address space, an address space can have many memory regions.
The kernel sets up memory regions for each process and use them to track valid memory ranges for a process to access.
Each user process has a memory region for code, stack, and heap on start up. More details can be found [here](memory.md).

In addition to managing each process's virtual address space, the kernel is also responsible for setting up the address translation 
table (page table) which maps a virtual address to a physical address. Each virtual address space has its own page table, which is why
when two processes access the same virtual address, they access different data. osv uses `struct vpmap` to represent the page table.
`include/kernel/vpmap.h` defines a list of operations on the page table. For example `vpmap_map` maps a virtual page to a physical page.

You will be interacting with all these abstractions in this lab.

### Partners
You can complete this lab with a partner if you wish. 
If you worked with a partner on lab 3, I will assume you will continue working with that partner on lab 4 unless I hear
otherwise.
To start working with a partner, or to request to be matched with a partner, send me an email by Monday 5/25.

### Submission details
Create a lab4design.md in labs/ and follow the guidelines on [how to write a design document](designdoc.md). 
When finished with your design doc, tag your repo with:
```
git tag lab4_design
```

When finished with your lab3, tag your repo with:
```
git tag end_lab4
```

Don't forget to push the tags!
```
git push origin master --tags
```

The reference solution for this lab has made changes to
- `kernel/syscall.c`
- `kernel/pgfault.c`
- `kernel/mm/vm.c`
- `kernel/proc.c`

### Configuration
To pull new changes, second lab tests and description, run the command
```
git pull upstream master
```
and merge.

**After the merge, double check that your code still passes lab2 and lab3 tests.**


## Part 1: Grow user stack on-demand

In lab 3, you may have set up the user stack with an initial page to store application arguments. Great! But what if a process wants to use more stack?
One option is to allocate more physical pages for the stack on start up and map them into the process's address space.
But if a process doesn't actually use all of its stack, the allocated physical pages are wasted while the process executes.

To reduce this waste, a common technique is to allocate physical pages for additional stack pages *on demand*.
For this part of the lab, you will extend osv to do on-demand stack growth. 
This means that the physical memory of the *additional stack page* is allocated at run-time.
Whenever a user application issues an instruction that reads or writes to the user stack (e.g., creating a stack frame, 
accessing local variables), we grow the stack as needed. For this lab, you should limit your overall stack size to 10 pages total.
The upstream commit modifies `stack_setup` to set up a limit for the user stack memregion of 10 pages.

To implement on-demand stack growth, you will need to understand how to handle page faults.
A page fault is a hardware exception that occurs when a process accesses a virtual memory page without a valid page table mapping, 
or with a valid mapping, but where the process does not have permission to perform the operation.

On a page fault, the process will trap into the kernel and trigger the page fault handler.
If the fault address is within a valid memory region and has the proper permission, the page fault handler
should allocate a physical page, map it to the process's page table, and and resume process execution.
Note that a write on a read only memory permission is not a valid access and the calling process should terminate. 

osv's page fault handler is `kernel/pgfault.c:handle_page_fault`.
```c
    void
    handle_page_fault(vaddr_t fault_addr, int present, int write, int user) {
        if (user) {
            __sync_add_and_fetch(&user_pgfault, 1);
        }
        // turn on interrupt now that we have the fault address 
        intr_set_level(INTR_ON);
```
`fault_addr` is the address that was attempted to be accessed <br />
`present` is set if it was a page protection issue (fault address has a corresponding physical page mapped, but permission is off). This is not set if the page is not present.<br />
`write` is set if the fault occurred on a write.<br />
`user` is set if the fault occurred in user mode.<br />

To support stack growth, you should implement the page fault handler. To avoid information leaking, you need to memset 
the allocated physical page to 0s. Note that you cannot directly
access physical memory, so you need to translate the physical address to a kernel virtual address using `kmap_p2v(paddr)` before you do the memset.

### Implementation
Implement growing the user stack on-demand. Note that our test code
uses the system call `sysinfo` to figure out how many page faults have happened.

## Part 2: Create a user-level heap
After you have set up page fault handler to handle on-demand stack growth, you will now
support dynamic heap growth. Heap growth differs in that a process has to explicitly request
for more virtual address to be allocated to its heap. A process that needs more memory at runtime 
can call `sbrk` (set program break) to grow its heap size. The common use case is the situation where 
a user library routine, `malloc` in C or `new` in C++, calls `sbrk` whenever the application asks to allocate 
a data region that cannot fit on the current heap (e.g., if the heap is completely allocated due to prior calls to `malloc`).

If a user application wants to increase the heap size by `n` bytes, it calls `sbrk(n)`. `sbrk(n)` returns the OLD limit.
The user application can also decrease the amount of the heap by passing negative values to `sbrk`.
Generally, the user library asks `sbrk` to provide more space than immediately needed, to reduce the number of system calls.

When a user process is first spawned, its heap memory region is initialized to size 0, so the first call to `malloc`
always calls `sbrk`. osv needs to track how much memory has been allocated to each process's heap, and also extend/decrease
size of heap base on the process's request. To do so, you need to implement (`kernel/mm/vm.c:memregion_extend`).

Once you have properly set up the memory region range for dynamic heap growth, you can handle page fault from heap address
similar to how you handle on-demand stack growth. osv internally allocates and frees user memory at page granularity, 
but a process can call `sbrk` to allocate/deallocate memory at byte granularity. 
The OS does this to be portable, since an application cannot depend on the machine adopting a specific page size. 

In user space, we have provided an implementation of `malloc` and `free` (in `lib/malloc.c`) that is going to use `sbrk`. 
After the implementation of `sbrk` is done, user-level applications should be able to call `malloc` and `free`.

### Implementation
Implement `sys_sbrk` and `memregion_extend`. Need to support increment, decrement, and 0 case.

```c
 /*
  * Corresponds to void* sbrk(int increment) 
  * Increase/decrement current process' heap size.
  * If user requests to decrement more than the amount of heap allocated, treat it as sbrk(0).
  *
  * Return:
  * On success, address of the previous program break/heap limit.
  * ERR_NOMEM - Failed to extend the heap segment.
  */
sysret_t
sys_sbrk(void *arg);
```

```c
/*
 * Extend a region of allocated memory by size bytes.
 * End is extended size and old_bound is returned.
 * Return ERR_VM_BOUND if the extended region overlaps with other regions in the
 * address space.
 * Return ERR_VM_INVALID if the extended region would have a negative extent.
 */
err_t
memregion_extend(struct memregion *region, int size, vaddr_t *old_bound);
```

## Testing and hand-in
After you implement the system calls described above, test lab 4 code by either running 
individual tests in osv shell, or run `python3 test.py 4`.

Running the tests from the _previous_ labs is a good way to boost your confidence
in your solution for _this_ lab.

## Tips
Carefully reading through the following header files will be helpful as you think about your design
- `include/kernel/vm.h`: functions to interact with virtual memory abstractions including address spaces and memregions
- `include/kernel/pmem.h`: functions for physical memory allocation
- `include/kernel/vpmap.h`: functions to interact with the mapping from virtual to physical addresses

Questions to consider as you work on your design:
- How will you check whether the `fault_addr` is a valid address?
- How will you allocate a new physical page?
- How will you map a new physical page into a virtual address space/memregion?
- How do know how large a page is?
- What's different about a kernel page fault versus a user page fault?
- How will you detect when extending a memregion would cause it to overlap with another memregion?

When you're finished, create an `end_lab4` git tag as described above,
so we know the point at which you submitted your code.