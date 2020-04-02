#include <kernel/proc.h>
#include <kernel/thread.h>
#include <kernel/console.h>
#include <kernel/kmalloc.h>
#include <kernel/fs.h>
#include <kernel/pipe.h>
#include <lib/syscall-num.h>
#include <lib/errcode.h>
#include <lib/stddef.h>
#include <lib/string.h>
#include <arch/asm.h>

// syscall handlers
static sysret_t sys_fork(void* arg);
static sysret_t sys_spawn(void* arg);
static sysret_t sys_wait(void* arg);
static sysret_t sys_exit(void* arg);
static sysret_t sys_getpid(void* arg);
static sysret_t sys_sleep(void* arg);
static sysret_t sys_open(void* arg);
static sysret_t sys_close(void* arg);
static sysret_t sys_read(void* arg);
static sysret_t sys_write(void* arg);
static sysret_t sys_link(void* arg);
static sysret_t sys_unlink(void* arg);
static sysret_t sys_mkdir(void* arg);
static sysret_t sys_chdir(void* arg);
static sysret_t sys_readdir(void* arg);
static sysret_t sys_rmdir(void* arg);
static sysret_t sys_fstat(void* arg);
static sysret_t sys_sbrk(void* arg);
static sysret_t sys_meminfo(void* arg);
static sysret_t sys_dup(void* arg);
static sysret_t sys_pipe(void* arg);
static sysret_t sys_info(void* arg);
static sysret_t sys_halt(void* arg);

extern size_t user_pgfault;
struct sys_info {
    size_t num_pgfault;
};

/*
 * Machine dependent syscall implementation: fetches the nth syscall argument.
 */
extern bool fetch_arg(void *arg, int n, sysarg_t *ret);

/*
 * Validate string passed by user.
 */
static bool validate_str(char *s);
/*
 * Validate buffer passed by user.
 */
static bool validate_bufptr(void* buf, size_t size);

/*
 * Allocate a file descriptor for file f.
 *
 * Return:
 * Non-negative file descriptor on success.
 * -1 - Failed to allocate a file descriptor.
 */
static int alloc_fd(struct file *f);
/*
 * Validate file descriptor fd.
 */
static bool validate_fd(int fd);

static int
alloc_fd(struct file *f)
{
    int i;

    // go through process struct
    kassert(f);
    struct proc *p = proc_current();
    for (i = 0; i < PROC_MAX_FILE; i++) {
        if (p->files[i] == NULL) {
            p->files[i] = f;
            return i;
        }
    }
    return -1;
}

static bool
validate_fd(int fd)
{
    if (fd >= 0 && fd < PROC_MAX_ARG) {
        return True;
    }
    return False;
}

static sysret_t (*syscalls[])(void*) = {
    [SYS_fork] = sys_fork,
    [SYS_spawn] = sys_spawn,
    [SYS_wait] = sys_wait,
    [SYS_exit] = sys_exit,
    [SYS_getpid] = sys_getpid,
    [SYS_sleep] = sys_sleep,
    [SYS_open] = sys_open,
    [SYS_close] = sys_close,
    [SYS_read] = sys_read,
    [SYS_write] = sys_write,
    [SYS_link] = sys_link,
    [SYS_unlink] = sys_unlink,
    [SYS_mkdir] = sys_mkdir,
    [SYS_chdir] = sys_chdir,
    [SYS_readdir] = sys_readdir,
    [SYS_rmdir] = sys_rmdir,
    [SYS_fstat] = sys_fstat,
    [SYS_sbrk] = sys_sbrk,
    [SYS_meminfo] = sys_meminfo,
    [SYS_dup] = sys_dup,
    [SYS_pipe] = sys_pipe,
    [SYS_info] = sys_info,
    [SYS_halt] = sys_halt,
};

static bool
validate_str(char *s)
{
    struct memregion *mr;
    // find given string's memory region
    if((mr = as_find_memregion(&proc_current()->as, (vaddr_t) s, 1)) == NULL) {
        return False;
    }
    // check in case the string keeps growing past user specified amount
    for(; s < (char*) mr->end; s++){
        if(*s == 0) {
            return True;
        }
    }
    return False;
}

static bool
validate_bufptr(void* buf, size_t size)
{
    vaddr_t bufaddr = (vaddr_t) buf;
    if (bufaddr + size < bufaddr) {
        return False;
    }
    // verify argument buffer is valid and within specified size
    if(as_find_memregion(&proc_current()->as, bufaddr, size) == NULL) {
        return False;
    }
    return True;
}


// int fork(void);
static sysret_t
sys_fork(void *arg)
{
    struct proc *p;
    if ((p = proc_fork()) == NULL) {
        return ERR_NOMEM;
    }
    return p->pid;
}

// int spawn(const char *args);
static sysret_t
sys_spawn(void *arg)
{
    int argc = 0;
    sysarg_t args;
    size_t len;
    char *token, *buf, **argv;
    struct proc *p;
    err_t err;

    // argument fetching and validating
    kassert(fetch_arg(arg, 1, &args));
    if (!validate_str((char*)args)) {
        return ERR_FAULT;
    }

    len = strlen((char*)args) + 1;
    if ((buf = kmalloc(len)) == NULL) {
        return ERR_NOMEM;
    }
    // make a copy of the string to not modify user data
    memcpy(buf, (void*)args, len);
    // figure out max number of arguments possible
    len = len / 2 < PROC_MAX_ARG ? len/2 : PROC_MAX_ARG;
    if ((argv = kmalloc((len+1)*sizeof(char*))) == NULL) {
        kfree(buf);
        return ERR_NOMEM;
    }
    // parse arguments  
    while ((token = strtok_r(NULL, " ", &buf)) != NULL) {
        argv[argc] = token;
        argc++;
    }
    argv[argc] = NULL;

    if ((err = proc_spawn(argv[0], argv, &p)) != ERR_OK) {
        return err;
    }
    return p->pid;
}

// int wait(int pid, int *wstatus);
static sysret_t
sys_wait(void* arg)
{
    sysarg_t pid, wstatus;
    kassert(fetch_arg(arg, 1, &pid));
    kassert(fetch_arg(arg, 2, &wstatus));
    // wstatus is optional so NULL is allowed
    if (wstatus != NULL && !validate_bufptr((int *)wstatus, sizeof(int))) {
        return ERR_FAULT;
    }
    return proc_wait((int)pid, (int*)wstatus);
}

// void exit(int status);
static sysret_t
sys_exit(void* arg)
{
    sysarg_t status = 0;
    kassert(fetch_arg(arg, 1, &status));
    proc_exit((int)status);
    panic("unreachable");
}

// int getpid(void);
static sysret_t
sys_getpid(void* arg)
{
    return proc_current()->pid;
}

// void sleep(unsigned int, seconds);
static sysret_t
sys_sleep(void* arg)
{
    panic("syscall sleep not implemented");
}

// int open(const char *pathname, int flags, fmode_t mode);
static sysret_t
sys_open(void *arg)
{
    sysarg_t pathname, flags, mode;
    struct file *f;
    int fd;
    err_t err;

    kassert(fetch_arg(arg, 1, &pathname));
    if (!validate_str((char*)pathname)) {
        return ERR_FAULT;
    }
    kassert(fetch_arg(arg, 2, &flags));
    kassert(fetch_arg(arg, 3, &mode));
    if ((err = fs_open_file((char*)pathname, (int)flags, (fmode_t)mode, &f)) != ERR_OK) {
        return err;
    }
    if ((fd = alloc_fd(f)) < 0) {
        return ERR_NOMEM;
    }
    return fd;
}

// int close(int fd);
static sysret_t
sys_close(void *arg)
{
    sysarg_t fd;
    struct proc *p;

    kassert(fetch_arg(arg, 1, &fd));
    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }
    p = proc_current();
    if (p->files[fd] == NULL) {
        return ERR_INVAL;
    }
    fs_close_file(p->files[fd]);
    p->files[fd] = NULL;
    return ERR_OK;
}

// int read(int fd, void *buf, size_t count);
static sysret_t
sys_read(void* arg)
{
    sysarg_t fd, buf, count;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 3, &count));
    kassert(fetch_arg(arg, 2, &buf));

    if (!validate_bufptr((void*)buf, (size_t)count)) {
        return ERR_FAULT;
    }

    struct file *f;
    struct proc *p;
    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }
    p = proc_current();
    f = p->files[fd];
    if (f == NULL) {
        return ERR_INVAL;
    }
    return fs_read_file(f, (void*)buf, (size_t)count, &f->f_pos);
}

// int write(int fd, const void *buf, size_t count)
static sysret_t
sys_write(void* arg)
{
    sysarg_t fd, count, buf;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 3, &count));
    kassert(fetch_arg(arg, 2, &buf));

    if (!validate_bufptr((void*)buf, (size_t)count)) {
        return ERR_FAULT;
    }

    struct file *f = NULL;
    struct proc *p;
    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }

    p = proc_current();
    f = p->files[fd];
    if (f == NULL) {
        return ERR_INVAL;
    }
    return fs_write_file(f, (void*)buf, (size_t)count, &f->f_pos);
}

// int link(const char *oldpath, const char *newpath)
static sysret_t
sys_link(void *arg)
{
    sysarg_t oldpath, newpath;

    kassert(fetch_arg(arg, 1, &oldpath));
    kassert(fetch_arg(arg, 2, &newpath));

    if (!validate_str((char*)oldpath) || !validate_str((char*)newpath)) {
        return ERR_FAULT;
    }

    return fs_link((char*)oldpath, (char*)newpath);
}

// int unlink(const char *pathname)
static sysret_t
sys_unlink(void *arg)
{
    sysarg_t pathname;

    kassert(fetch_arg(arg, 1, &pathname));

    if (!validate_str((char*)pathname)) {
        return ERR_FAULT;
    }

    return fs_unlink((char*)pathname);
}

// int mkdir(const char *pathname)
static sysret_t
sys_mkdir(void *arg)
{
    sysarg_t pathname;

    kassert(fetch_arg(arg, 1, &pathname));

    if (!validate_str((char*)pathname)) {
        return ERR_FAULT;
    }

    return fs_mkdir((char*)pathname);
}

// int chdir(const char *path)
static sysret_t
sys_chdir(void *arg)
{
    sysarg_t path;
    struct inode *inode;
    struct proc *p;
    err_t err;

    kassert(fetch_arg(arg, 1, &path));

    if (!validate_str((char*)path)) {
        return ERR_FAULT;
    }

    if ((err = fs_find_inode((char*)path, &inode)) != ERR_OK) {
        return err;
    }

    p = proc_current();
    kassert(p);
    kassert(p->cwd);
    fs_release_inode(p->cwd);
    p->cwd = inode;
    return ERR_OK;
}

// int readdir(int fd, struct dirent *dirent);
static sysret_t
sys_readdir(void *arg)
{
    panic("syscall readir not implemented");
}

// int rmdir(const char *pathname);
static sysret_t
sys_rmdir(void *arg)
{
    sysarg_t pathname;

    kassert(fetch_arg(arg, 1, &pathname));

    if (!validate_str((char*)pathname)) {
        return ERR_FAULT;
    }

    return fs_rmdir((char*)pathname);
}

// int fstat(int fd, struct stat *stat);
static sysret_t
sys_fstat(void *arg)
{
    sysarg_t fd, stat;
    struct proc *p;
    struct file *f;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 2, &stat));
    if (!validate_bufptr((struct stat*)stat, sizeof(struct stat))) {
        return ERR_FAULT;
    }
    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }
    p = proc_current();
    f = p->files[fd];
    if (f == NULL || f->f_inode == NULL) {
        return ERR_INVAL;
    }
    ((struct stat*)stat)->inode_num = f->f_inode->i_inum;
    ((struct stat*)stat)->ftype = f->f_inode->i_ftype;
    ((struct stat*)stat)->size = f->f_inode->i_size;
    return ERR_OK;
}

// void *sbrk(size_t increment);
static sysret_t
sys_sbrk(void *arg)
{
    vaddr_t retaddr = 0;
    sysarg_t increment;
    struct proc *p;

    kassert(fetch_arg(arg, 1, &increment));

    // expand heap size
    p = proc_current();
    if (memregion_extend(p->as.heap, (size_t)increment, &retaddr) != ERR_OK) {
		return ERR_NOMEM;
    }
    return retaddr;
}

// void memifo();
static sysret_t
sys_meminfo(void *arg)
{
    as_meminfo(&proc_current()->as);
    return ERR_OK;
}

// int dup(int fd);
static sysret_t
sys_dup(void *arg)
{
    sysarg_t fd;
    struct file *f;
    struct proc *p;

    kassert(fetch_arg(arg, 1, &fd));
    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }
    p = proc_current();
    f = p->files[fd];
    if (f == NULL) {
        return ERR_INVAL;
    }
    if ((fd = alloc_fd(f)) < 0) {
        return ERR_NOMEM;
    }
    fs_reopen_file(f);
    return fd;
}

// int pipe(int* fds);
static sysret_t
sys_pipe(void* arg)
{
    sysarg_t fds;
    struct file *read_end, *write_end;
    struct proc *p = proc_current();
    kassert(fetch_arg(arg, 1, &fds));
    if (!validate_bufptr((int*)fds, sizeof(int)*2)) {
        return ERR_FAULT;
    }
    if (pipe_alloc(&read_end, &write_end) != ERR_OK) {
        return ERR_NOMEM;
    }
    if ((((int*)fds)[0] = alloc_fd(read_end)) < 0) {
        return ERR_NOMEM;
    }
    if ((((int*)fds)[1] = alloc_fd(write_end)) < 0) {
        fs_close_file(read_end);
        p->files[((int*)fds)[0]] = NULL;
        return ERR_NOMEM;
    }
    return ERR_OK;
}

// void sys_info(struct sys_info *info);
static sysret_t
sys_info(void* arg)
{
    sysarg_t info;

    kassert(fetch_arg(arg, 1, &info));

    if (!validate_bufptr((void*)info, sizeof(struct sys_info))) {
        return ERR_FAULT;
    }
    // fill in using user_pgfault 
    ((struct sys_info*)info)->num_pgfault = user_pgfault;
    return ERR_OK;
}

// void halt();
static sysret_t 
sys_halt(void* arg)
{
    shutdown();
    panic("shutdown failed");
}


sysret_t
syscall(int num, void *arg)
{
    kassert(proc_current());
    if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
        return syscalls[num](arg);
    } else {
        panic("Unknown system call");
    }
}

