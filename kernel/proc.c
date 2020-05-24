#include <kernel/console.h>
#include <kernel/proc.h>
#include <kernel/kmalloc.h>
#include <kernel/thread.h>
#include <kernel/list.h>
#include <kernel/fs.h>
#include <kernel/vpmap.h>
#include <arch/elf.h>
#include <arch/trap.h>
#include <arch/mmu.h>
#include <lib/errcode.h>
#include <lib/stddef.h>
#include <lib/string.h>

List ptable; // process table
struct spinlock ptable_lock;
struct spinlock pid_lock;
struct spinlock exit_lock;
struct condvar wait_var;
static int pid_allocator;
struct kmem_cache *proc_allocator;

/* go through process table */
static void ptable_dump(void);
/* helper function for loading process's binary into its address space */ 
static err_t proc_load(struct proc *p, char *path, vaddr_t *entry_point);
/* helper function to set up the stack */
static err_t stack_setup(struct proc *p, char **argv, vaddr_t* ret_stackptr);
/* tranlsates a kernel vaddr to a user stack address, assumes stack is a single page */
#define USTACK_ADDR(addr) (pg_ofs(addr) + USTACK_UPPERBOUND - pg_size);

static struct proc*
proc_alloc()
{
    struct proc* p = (struct proc*) kmem_cache_alloc(proc_allocator);
    if (p != NULL) {
        spinlock_acquire(&pid_lock);
        p->pid = pid_allocator++;
        spinlock_release(&pid_lock);
    }
    return p;
}

#pragma GCC diagnostic ignored "-Wunused-function"
static void
ptable_dump(void)
{
    kprintf("ptable dump:\n");
    spinlock_acquire(&ptable_lock);
    for (Node *n = list_begin(&ptable); n != list_end(&ptable); n = list_next(n)) {
        struct proc *p = list_entry(n, struct proc, proc_node);
        kprintf("Process %s: pid %d\n", p->name, p->pid);
    }
    spinlock_release(&ptable_lock);
    kprintf("\n");
}

void
proc_free(struct proc* p)
{
    kmem_cache_free(proc_allocator, p);
}

/*
 * Verifies that the given file descriptor is within the bounds of possible
 * file descriptor values, and that the given file descriptor is currently
 * in use in the given process's file descriptor table.
 */
bool
proc_validate_fd(struct proc *p, int fd)
{
    if (fd < 0 || fd > (&p->fdtable)->max) {
        return False;
    }
    if ((&p->fdtable)->table[fd] == NULL) {
        return False;
    }
    return True;
}

/*
 * Allocates the lowest available position in the fdtable of the given process
 * for the given file.
 *
 * Return:
 * file descriptor of the allocated file in the table.
 * ERR_NOMEM - The file descriptor table is full
 */
sysret_t
proc_alloc_fd(struct proc *p, struct file *file)
{
    if ((&p->fdtable)->count >= (&p->fdtable)->max) {
        return ERR_NOMEM;
    }
    int index = (&p->fdtable)->first_avail;
    while (proc_validate_fd(p, index)) {
        index = (index + 1) % (&p->fdtable)->max;
        if (index == (&p->fdtable)->first_avail % (&p->fdtable)->max) {
            return ERR_NOMEM;
        }
    }
    (&p->fdtable)->table[index] = file;
    (&p->fdtable)->first_avail = index + 1;  // no mod here so that in the case of full table, when the next fd is removed from index, min(first_avail, index) will yield index
    (&p->fdtable)->count++;
    return index;
}

/*
 * Removes the given file descriptor from the given process's file descriptor table
 *
 * Return:
 * On success, the file pointer corresponding to the file descriptor which was removed.
 * ERR_INVAL - The given file descriptor is not in the process's fdtable.
 */
struct file*
proc_remove_fd(struct proc *p, int fd)
{
    struct file *file;
    int curr_min = (&p->fdtable)->first_avail;
    if (!proc_validate_fd(p, fd)) {
        return (void *)ERR_INVAL;
    }
    file = (&p->fdtable)->table[fd];
    (&p->fdtable)->table[fd] = NULL;
    (&p->fdtable)->first_avail = fd < curr_min ? fd : curr_min;
    (&p->fdtable)->count--;
    return file;
}

/*
 * Returns the file pointer stored at the given index of the file descriptor table
 *
 * Return:
 * the file pointer at the given index.
 * ERR_INVAL - The given file descriptor is not in the process's fdtable.
 */
struct file*
proc_get_fd(struct proc *p, int fd)
{
    struct file *file;
    if (!proc_validate_fd(p, fd)) {
        return (void *)ERR_INVAL;
    }
    file = (&p->fdtable)->table[fd];
    return file;
}

void
proc_sys_init(void)
{
    list_init(&ptable);
    spinlock_init(&ptable_lock);
    spinlock_init(&pid_lock);
    spinlock_init(&exit_lock);
    condvar_init(&wait_var);
    proc_allocator = kmem_cache_create(sizeof(struct proc));
    kassert(proc_allocator);
}

/*
 * Allocate and initialize basic proc structure
*/
static struct proc*
proc_init(char* name)
{
    struct super_block *sb;
    struct proc *parent, *p;
    inum_t inum;
    err_t err;

    p = proc_alloc();
    if (p == NULL) {
        return NULL;
    }

    if (as_init(&p->as) != ERR_OK) {
        proc_free(p);
        return NULL;
    }

    (&p->fdtable)->max = PROC_MAX_FILE;
    (&p->fdtable)->count = 0;
    (&p->fdtable)->first_avail = 0;
    for (int i = 0; i < PROC_MAX_FILE; i++) {
        (&p->fdtable)->table[i] = NULL;
    }
    proc_alloc_fd(p, &stdin);
    proc_alloc_fd(p, &stdout);

    size_t slen = strlen(name);
    slen = slen < PROC_NAME_LEN-1 ? slen : PROC_NAME_LEN-1;
    memcpy(p->name, name, slen);
    p->name[slen] = 0;

    p->parent_live = 0;
    p->status = NULL;  // The pointer to the ctlist_entry->status, which will be created after the thread is created by proc_spawn or proc_fork
    if ((parent = proc_current()) != NULL) {
        p->parent_live = 1;
        p->ppid = parent->pid;  // This causes a page fault in the initial process if not in the if statement
    }

    list_init(&p->threads);
    list_init(&p->ctlist);

    // cwd for all processes are root for now
    sb = root_sb;
    inum = root_sb->s_root_inum;
    if ((err = fs_get_inode(sb, inum, &p->cwd)) != ERR_OK) {
        as_destroy(&p->as);
        proc_free(p);
        return NULL;
    }

    return p;
}

err_t
proc_spawn(char* name, char** argv, struct proc **p)
{
    err_t err;
    struct proc *proc, *parent;
    struct thread *t;
    struct ctlist_entry *ctle;
    vaddr_t entry_point;
    vaddr_t stackptr;

    if ((proc = proc_init(name)) == NULL) {
        return ERR_NOMEM;
    }
    // load binary of the process
    if ((err = proc_load(proc, name, &entry_point)) != ERR_OK) {
        goto error;
    }

    // set up stack and allocate its memregion 
    if ((err = stack_setup(proc, argv, &stackptr)) != ERR_OK) {
        goto error;
    }

    if ((t = thread_create(proc->name, proc, DEFAULT_PRI)) == NULL) {
        err = ERR_NOMEM;
        goto error;
    }

    // add to ptable
    spinlock_acquire(&ptable_lock);
    list_append(&ptable, &proc->proc_node);
    spinlock_release(&ptable_lock);

    if ((parent = proc_current()) != NULL) {
        if ((ctle = (struct ctlist_entry *)kmalloc(sizeof(struct ctlist_entry))) == NULL) {
            err = ERR_NOMEM;
            goto error;
        }
        ctle->pid = proc->pid;
        ctle->thread = t;
        ctle->status = STATUS_ALIVE;
        proc->status = &ctle->status;  // Set up child's pointer to exit status
        list_append(&parent->ctlist, &ctle->node);
    }

    // set up trapframe for a new process
    tf_proc(t->tf, t->proc, entry_point, stackptr);
    thread_start_context(t, NULL, NULL);

    // fill in allocated proc
    if (p) {
        *p = proc;
    }
    return ERR_OK;
error:
    as_destroy(&proc->as);
    proc_free(proc);
    return err;
}

/*
 * Creates a new process as a copy of the current process, with
 * the same open file descriptors.
 *
 * Return:
 * the `struct proc*` of the new process
 * NULL on error: kernel lacks space to create new process
 */
struct proc*
proc_fork()
{
    struct proc *proc, *child_proc;
    struct thread *thread, *child_thread;
    struct ctlist_entry *ctle;
    struct file *curr_file;
    int i;

    kassert(proc_current());  // caller of fork must be a process

    proc = proc_current();
    if ((child_proc = proc_init(proc->name)) == NULL) {
        return NULL;
    }

    if (as_copy_as(&proc->as, &child_proc->as) == ERR_NOMEM) {
        proc_free(child_proc);
        return NULL;
    }

    thread = thread_current();
    if ((child_thread = thread_create(child_proc->name, child_proc, DEFAULT_PRI)) == NULL) {
        goto error;
    }

    if ((ctle = (struct ctlist_entry *)kmalloc(sizeof(struct ctlist_entry))) == NULL) {
        goto error;
    }

    // At this point, ERR_NOMEM should not occur, so safe to make other changes

    ctle->pid = child_proc->pid;
    ctle->thread = child_thread;
    ctle->status = STATUS_ALIVE;
    child_proc->status = &ctle->status;  // Set up child's pointer to exit status

    list_append(&proc->ctlist, &ctle->node);

    for (i = 0; i < (&proc->fdtable)->max; i++) {
        curr_file = (&proc->fdtable)->table[i];
        if ((curr_file != NULL) && (curr_file != &stdin) && (curr_file != &stdout)) {
            (&child_proc->fdtable)->table[i] = curr_file;
            fs_reopen_file(curr_file);
        }
    }

    *(child_thread->tf) = *(thread->tf);
    tf_set_return(child_thread->tf, 0);
    thread_start_context(child_thread, NULL, NULL);
    return child_proc;
error:
    as_destroy(&child_proc->as);
    proc_free(child_proc);
    return NULL;
}

struct proc*
proc_current()
{
    return thread_current()->proc;
}

void
proc_attach_thread(struct proc *p, struct thread *t)
{
    kassert(t);
    if (p) {
        list_append(&p->threads, &t->thread_node);
    }
}

bool
proc_detach_thread(struct thread *t)
{
    bool last_thread = False;
    struct proc *p = t->proc;
    if (p) {
        list_remove(&t->thread_node);
        last_thread = list_empty(&p->threads);
    }
    return last_thread;
}

/*
 * Wait for a process to change state. If pid is ANY_CHILD, wait for any child process.
 * If wstatus is not NULL, store the exit status of the child in wstatus.
 *
 * Return:
 * pid of the child process that changes state.
 * ERR_CHILD - The caller does not have a child with the specified pid.
 */
int
proc_wait(pid_t pid, int* status)
{
    struct proc *p;
    Node *currnode;
    struct ctlist_entry *ctle;

    p = proc_current();

    if (list_empty(&p->ctlist)) {
        return ERR_CHILD;
    }

    currnode = list_begin(&p->ctlist);
    if (pid == ANY_CHILD) {
        while (1) {
            if (currnode == &(&p->ctlist)->header) {
                currnode = currnode->next;  // List uses a circular list with a dummy header
            }
            ctle = list_entry(currnode, struct ctlist_entry, node);
            if (ctle->status != STATUS_ALIVE) {
                if (status) {
                    *status = ctle->status;
                }
                pid = ctle->pid;
                list_remove(&ctle->node);
                kfree(ctle);
                break;
            }
            currnode = list_next(currnode);
        }
    } else {
        while (currnode != &(&p->ctlist)->header) {
            ctle = list_entry(currnode, struct ctlist_entry, node);
            if (ctle->pid == pid) {
                spinlock_acquire(&exit_lock);
                while (ctle->status == STATUS_ALIVE) {
                    condvar_wait(&wait_var, &exit_lock);
                }
                spinlock_release(&exit_lock);
                if (status) {
                    *status = ctle->status;
                }
                list_remove(&ctle->node);
                kfree(ctle);
                break;
            }
            currnode = list_next(currnode);
        }
        if (currnode == &(&p->ctlist)->header) {
            pid = ERR_CHILD;
        }
    }
    return pid;
}

/* Exit a process with a statys */
void
proc_exit(int status)
{
    struct thread *t = thread_current();
    struct proc *p = proc_current();
    Node *currnode;
    struct ctlist_entry *ctle;
    int i;

    for (i = 0; i < (&p->fdtable)->max; i++) {
        if ((&p->fdtable)->table[i] != NULL) {
            fs_close_file((&p->fdtable)->table[i]);
        }
    }

    spinlock_acquire(&exit_lock);

    if (p->parent_live == 1) {
        *p->status = status;
    }

    currnode = list_begin(&p->ctlist);
    while (currnode != &(&p->ctlist)->header) {
        ctle = list_entry(currnode, struct ctlist_entry, node);
        if (ctle->status == STATUS_ALIVE) {
            ctle->thread->proc->parent_live = 0;
        }
        list_remove(&ctle->node);
        kfree(ctle);
    }

    spinlock_release(&exit_lock);

    condvar_broadcast(&wait_var);

    // detach current thread, switch to kernel page table
    // free current address space if proc has no more threads
    // order matters here
    proc_detach_thread(t);
    t->proc = NULL;
    vpmap_load(kas->vpmap);
    as_destroy(&p->as);

    // release process's cwd
    fs_release_inode(p->cwd);

    proc_free(p);

    thread_exit(status);
}

/* helper function for loading process's binary into its address space */ 
static err_t
proc_load(struct proc *p, char *path, vaddr_t *entry_point)
{
    int i;
    err_t err;
    offset_t ofs = 0;
    struct elfhdr elf;
    struct proghdr ph;
    struct file *f;
    paddr_t paddr;
    vaddr_t vaddr;
    vaddr_t end = 0;

    if ((err = fs_open_file(path, FS_RDONLY, 0, &f)) != ERR_OK) {
        return err;
    }

    // check if the file is actually an executable file
    if (fs_read_file(f, (void*) &elf, sizeof(elf), &ofs) != sizeof(elf) || elf.magic != ELF_MAGIC) {
        return ERR_INVAL;
    }

    // read elf and load binary
    for (i = 0, ofs = elf.phoff; i < elf.phnum; i++) {
        if (fs_read_file(f, (void*) &ph, sizeof(ph), &ofs) != sizeof(ph)) {
            return ERR_INVAL;
        }
        if(ph.type != PT_LOAD)
            continue;

        if(ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr) {
            return ERR_INVAL;
        }

        memperm_t perm = MEMPERM_UR;
        if (ph.flags & PF_W) {
            perm = MEMPERM_URW;
        }

        // found loadable section, add as a memregion
        struct memregion *r = as_map_memregion(&p->as, pg_round_down(ph.vaddr), 
            pg_round_up(ph.memsz + pg_ofs(ph.vaddr)), perm, NULL, ph.off, False);
        if (r == NULL) {
            return ERR_NOMEM;
        }
        end = r->end;

        // pre-page in code and data, may span over multiple pages
        int count = 0;
        size_t avail_bytes;
        size_t read_bytes = ph.filesz;
        size_t pages = pg_round_up(ph.memsz + pg_ofs(ph.vaddr)) / pg_size;
        // vaddr may start at a nonaligned address
        vaddr = pg_ofs(ph.vaddr);
        while (count < pages) {
            // allocate a physical page and zero it first
            if ((err = pmem_alloc(&paddr)) != ERR_OK) {
                return err;
            }
            vaddr += kmap_p2v(paddr);
            memset((void*)pg_round_down(vaddr), 0, pg_size);
            // calculate how many bytes to read from file
            avail_bytes = read_bytes < (pg_size - pg_ofs(vaddr)) ? read_bytes : (pg_size - pg_ofs(vaddr));
            if (avail_bytes && fs_read_file(f, (void*)vaddr, avail_bytes, &ph.off) != avail_bytes) {
                return ERR_INVAL;
            }
            // map physical page with code/data content to expected virtual address in the page table
            if ((err = vpmap_map(p->as.vpmap, ph.vaddr+count*pg_size, paddr, 1, perm)) != ERR_OK) {
                return err;
            }
            read_bytes -= avail_bytes;
            count++;
            vaddr = 0;
        }
    }
    *entry_point = elf.entry;

    // create memregion for heap after data segment
    if ((p->as.heap = as_map_memregion(&p->as, end, 0, MEMPERM_URW, NULL, 0, 0)) == NULL) {
        return ERR_NOMEM;
    }

    return ERR_OK;
}

err_t
stack_setup(struct proc *p, char **argv, vaddr_t* ret_stackptr)
{
    err_t err;
    paddr_t paddr;
    vaddr_t stackptr;
    vaddr_t stacktop = USTACK_UPPERBOUND - pg_size;

    // allocate a page of physical memory for stack
    if ((err = pmem_alloc(&paddr)) != ERR_OK) {
        return err;
    }
    memset((void*) kmap_p2v(paddr), 0, pg_size);
    // create memregion for stack
    if (as_map_memregion(&p->as, USTACK_UPPERBOUND - 10*pg_size, 10*pg_size, MEMPERM_URW, NULL, 0, False) == NULL) {
        err = ERR_NOMEM;
        goto error;
    }
    // map in first stack page
    if ((err = vpmap_map(p->as.vpmap, stacktop, paddr, 1, MEMPERM_URW)) != ERR_OK) {
        goto error;
    }
    // kernel virtual address of the user stack, points to top of the stack
    // as you allocate things on stack, move stackptr downward.
    stackptr = kmap_p2v(paddr) + pg_size;

    /* Your Code Here.  */
    // allocate space for fake return address, argc, argv
    // remove following line when you actually set up the stack
    stackptr -= 3 * sizeof(void*);

    // translates stackptr from kernel virtual address to user stack address
    *ret_stackptr = USTACK_ADDR(stackptr); 
    return err;
error:
    pmem_free(paddr);
    return err;
}
