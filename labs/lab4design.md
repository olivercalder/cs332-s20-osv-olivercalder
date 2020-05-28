# Lab 4 Design Doc: Address Space Management

## Overview

The goal of this project is to implement core system memory management functionality. This functionality consists of two primary components. First, we implement dynamic user stack growth to allow user programs to grow the stack on demand. Second, we implement the user-level heap.

### Major Parts

#### Grow User Stack on Demand - Handling Page Faults

`handle_page_fault` grows the stack by allocating memory requested via a page fault (memory not mapped).  The function uses the address of the page fault to expand the range of desired virtual, and thus physical memory, by identifying the memory region that owns the page, then allocating the physical page, blanking its memory, and mapping the physical page to the page table.  Should not return anything.

The primary data structures used for this task are:

- `struct memregion`: keeps track of the virtual addresses; not actual page table.  Would modify when successfully allocated space.
- `struct vpmap`: has the actual page table; located within `struct addrspace`, which is a pointer inside of `struct memregion`.

#### sbrk - Grow User Level Memory

`sbrk` dynamically grows the heap (as opposed to the stack growth) by n bytes.  It uses `memregion_extend` to actually do the work of growing the heap, and returns the old address if successful,  and memory error if not.  

#### Expanding Heap Memory 

`memregion_extend` takes in a memregion struct, the old address, and a desired size to increase, and then delves into the vpmap, and changes the region of memory.  

- `struct memregion`: keeps track of the virtual addresses; not actual page table.  Would modify when successfully allocated space.
- `struct vpmap`: has the actual page table; located within `struct addrspace`, which is a pointer inside of `struct memregion`.

## In-depth Analysis and Implementation

### Key functions to use

- `pmem_alloc()` allocates a page (4096 bytes) of physical memory
- `pmem_nalloc()` allocates `n` contiguous pages of physical memory
  - Each page of physical memory allocated has a corresponding `struct page` to track information about the physical memory
- `kmap_p2v()` returns the corresponding kernel virtual address for a given physical address
- `vpmap_map()` maps a page of physical memory into a process’s address space

### Bookkeeping

- `include/kernel/vpmap.h` defines the `vpmap_map()` function
- `include/kernel/vm.h` defines memory-, address space-, and permission-related functions
  - `kernel/mm/vm.c` defines `memregion_extend`, which we need to implement
- `include/kernel/pmem.h` defines the `struct page` and physical memory allocation functions

### Functions to implement

#### `kernel/pgfault.c:handle_page_fault`

```c
void
handle_page_fault(vaddr_t fault_addr, int present, int write, int user) {
    if (user) {
        __sync_add_and_fetch(&user_pgfault, 1);
    }
    // turn on interrupt now that we have the fault address 
    intr_set_level(INTR_ON);
```

- `fault_addr` is the address that was attempted to be accessed.
- `present` is set if it was a page protection issue (fault address has a corresponding physical page mapped, but permission is off). This is not set if the page is not present.
- `write` is set if the fault occurred on a write.
- `user` is set if the fault occurred in user mode.

General steps:

- Check permission using functions in `include/kernel/vm.h`
- Call `as_find_memregion()` to identify which memory region owns the address which triggered the page fault
  - How do we get the address space which triggered the page fault handler? We need this in memregion
- If `user` is `0`, then it is the kernel, so use `kvm_base`, `kmap_start`, and `kmap_end` instead of user-level address spaces
- Call `pmem_alloc()` to allocate a physical page
- Call `vpmap_map()` to map the fault address to the allocated physical page
- Zero out the page using `memset()`
  - First need to get kernel virtual address using `kmap_p2v(paddr)`

#### `kernel/syscall.c:sys_sbrk`

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

General steps:

- Parse `arg`
- Call `memregion_extend` to expand the memory included in the given memregion
- Rely on the page fault handler to actually allocate a page for that memory when there is an attempt to access it

#### `memregion_extend`

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

`regions` is an ordered list of memregions, and we can find the next memregion by looking at `list_entry(list_next(memregion->node), struct memregion, as_node)`
  - Once we have the next memregion, check that `curr_memregion->end + size < next_memregion->start`
- modify the `end` parameter of the memregion

General steps:

- Find the next memregion, and make sure that extending the current bound by the size will not hit that memregion
  - Check that `as_find_memregion(curr_memregion->as, curr_memregion->end, size)` returns `NULL`, so that we know that the address to which we are trying to extend the current memregion is not part of another memregion

## Risk Analysis

### Edge cases

- What if `handle_page_fault` attempts to write on a read-only: exit with error message.
- What if `fault_address` is invalid: use `as_find_memregion` to check if it is valid.
- What if `handle_page_fault` wants to exceed the ten page limit;  a page is 4096 bytes: if it doesn’t, return error.
- How to avoid intersecting memory regions in `memregion_extend`: use `memregion_find_internal`.
- What is different about user vs kernel mode page faults: they use different address spaces (kernel mode is lower), if kernel, use the “k” methods, like `kvm_base`.


### Unanswered questions

- Is it our responsibility to implement a multilevel page table?
- What other uses for the functions in vm.c have we not mentioned here?
- How does one get the address space from a single address (like is presented as a parameter for `handle_page_fault`). If we need to work with the memregion and pages in that memregion, we need to know which address space we are working in.

### Staging of Work

Start by doing `handle_page_fault` because it isn’t dependent on any other implementations, then move onto `memregion_extend` because it is the basis of `sbrk`, and finally finish with our `sys_sbrk`, which relies explicitly on `memregion_extend`, and relies implicitly on `handle_page_fault` in order to allocate actual memory when the process goes to access memory in the newly extended heap

### Time Estimation
- `handle_page_fault`:
    - Best Case: .5 hour
    - Worst Case: 4 hour
    - Average Case: 2 hour

- `sbrk`:
    - Best Case: .25 hour
    - Worst Case: 1 hour
    - Average Case: .5 hour 

- `memregion_extend`:
    - Best Case: 1 hour
    - Worst Case: 5 hour
    - Average Case: 3 hour
