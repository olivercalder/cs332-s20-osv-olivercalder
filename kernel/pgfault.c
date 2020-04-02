#include <kernel/proc.h>
#include <kernel/console.h>
#include <kernel/trap.h>
#include <kernel/vpmap.h>
#include <lib/errcode.h>
#include <lib/string.h>

size_t user_pgfault = 0;

/*
 * Handle the case in which the page is not present but in a valida memory region
 */
static err_t handle_no_page(struct memregion *region, vaddr_t fault_addr);

/*
 * Handle copy on write.
 */
static err_t handle_cow(struct memregion *region, vaddr_t fault_addr);

static err_t
handle_no_page(struct memregion *mr, vaddr_t fault_addr) {
    err_t err;
    paddr_t paddr;
    // Page not present, stack growth case
    if (pmem_alloc(&paddr) != ERR_OK) {
        return ERR_PGFAULT_ALLOC;
    }
    memset((void*)kmap_p2v(paddr), 0, pg_size);
    err = vpmap_map(mr->as->vpmap, fault_addr, paddr, 1, mr->perm);
    if (err != ERR_OK) {
        pmem_dec_refcnt(paddr);
        err = ERR_PGFAULT_ALLOC;
    }
    return err;
}

static err_t
handle_cow(struct memregion *mr, vaddr_t fault_addr) {
    paddr_t paddr, new_paddr;
    err_t err;
    // find current backing physical address
    kassert(vpmap_lookup_vaddr(mr->as->vpmap, pg_round_down(fault_addr), &paddr, 0) == ERR_OK);
    new_paddr = paddr;
    // If the page has more than one references, we need to create a copy of the page.
    if (paddr_to_page(paddr)->refcnt > 1) {
        // allocate new page for copying
        if (pmem_alloc(&new_paddr) != ERR_OK) {
            return ERR_PGFAULT_ALLOC;
        }
        // copy over the data and decrement reference count
        memcpy((void*)kmap_p2v(new_paddr), (void*)kmap_p2v(paddr), pg_size);
        pmem_dec_refcnt(paddr);
    }
    // map our new paddr with full permission
    err = vpmap_map(mr->as->vpmap, fault_addr, new_paddr, 1, mr->perm);
    if (err != ERR_OK) {
        pmem_dec_refcnt(new_paddr);
        return ERR_PGFAULT_ALLOC;
    }
    // flush tlb because we have changed address permission
    vpmap_flush_tlb();
    return ERR_OK;
}

void
handle_page_fault(vaddr_t fault_addr, int present, int write, int user) {
    if (user) {
        __sync_add_and_fetch(&user_pgfault, 1);
    }
    // turn on interrupt now that we have the fault address 
    intr_set_level(INTR_ON);
    
    err_t err;
    struct proc *p = proc_current();
    struct addrspace *as = p ? &p->as : kas;
    struct memregion *mr = NULL;

    if ((mr = as_find_memregion(as, fault_addr, 1)) == NULL) {
        err = ERR_INVAL;
        goto error;
    }
    kassert(mr && mr->as == as);

    // Check permission violation
    if (write && !is_write_memperm(mr->perm)) {
        err = ERR_INVAL;
        goto error;
    }

    if (!present) {
        err = handle_no_page(mr, fault_addr);
    } else {
        // The page is present but a page fault occured. Must be copy on write.
        kassert(write);
        err = handle_cow(mr, fault_addr);
    }

    if (err == ERR_OK) {
        return;
    }

    // error occured
error:
    if (user) {
     //   kprintf("fault addres %p, present %d, wrie %d, user %d\n", fault_addr, present, write, user);
        proc_exit(-1);
        panic("unreachable");
    } else {
        kprintf("fault addr %p, err code %d\n", fault_addr, err);
        panic("Kernel error in page fault handler\n");
    }
}
