#include <lib/errcode.h>
#include <lib/string.h>
#include <kernel/proc.h>
#include <kernel/console.h>
#include <kernel/trap.h>
#include <kernel/vpmap.h>


size_t user_pgfault = 0;

#define error(user) (user ? proc_exit(-1) : panic("Kernel error in page fault handler\n"))

void
handle_page_fault(vaddr_t fault_addr, int present, int write, int user) {
    struct addrspace *as;
    struct memregion *region;
    paddr_t paddr;
    if (user) {
        __sync_add_and_fetch(&user_pgfault, 1);
    }
    // turn on interrupt now that we have the fault address
    intr_set_level(INTR_ON);

    // kernel vs user addrspace
    if (user) {
        as = &(proc_current()->as);
    } else {
        as = kas;
    }
    if (((region = as_find_memregion(as, fault_addr, 1)) == NULL) || (region->end == fault_addr)) {
        error(user);
    }
    if (present) {
        if (write) {
            // Do copy-on-write things
            error(user);
        } else {
            error(user);
        }
    } else {
        if (pmem_alloc(&paddr) != ERR_OK) {
            error(user);
        }
        if (vpmap_map(as->vpmap, fault_addr, paddr, 1, region->perm) == ERR_VPMAP_MAP) {
            pmem_free(paddr);
            error(user);
        }
        memset((void*) kmap_p2v(paddr), 0, pg_size);
    }
}
