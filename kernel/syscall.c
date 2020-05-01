#include <kernel/proc.h>
#include <kernel/thread.h>
#include <kernel/console.h>
#include <kernel/kmalloc.h>
#include <kernel/fs.h>
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

/*
 * Verifies that the given file descriptor is within the bounds of possible
 * file descriptor values, and that the given file descriptor is currently
 * in use in the given process's file descriptor table.
 */
static bool
validate_fd(int fd)
{
    struct proc *p = proc_current();
    return proc_validate_fd(p, fd);
}

/*
 * Allocates the lowest available position in the fdtable of the given process
 * for the given file.
 *
 * Return:
 * file descriptor of the allocated file in the table.
 * ERR_NOMEM - The file descriptor table is full.
 */
static sysret_t
alloc_fd(struct file *f)
{
    struct proc *p = proc_current();
    return proc_alloc_fd(p, f);
}

/*
 * Removes the given file descriptor from the given process's file descriptor table
 *
 * Return:
 * On success, the file pointer corresponding to the file descriptor which was removed.
 * ERR_INVAL - The given file descriptor is not in the process's fdtable.
 */
static struct file*
remove_fd(int fd)
{
    struct proc *p = proc_current();
    return proc_remove_fd(p, fd);
}

/*
 * Returns the file pointer stored at the given index of the file descriptor table
 *
 * Return:
 * the file pointer at the given index.
 * NULL - No file stored at the given index.
 */
static struct file*
get_fd(int fd)
{
    struct proc *p = proc_current();
    return proc_get_fd(p, fd);
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
    /* remove when writing your own solution */
    for (;;) {}
    panic("unreacchable");
}

// void exit(int status);
static sysret_t
sys_exit(void* arg)
{
    // temp code for lab2 to terminate the kernel after one process exits
    // remove for lab3
    kprintf("shutting down\n");
    shutdown();
    kprintf("oops still running\n");
    for(;;) {}
    panic("syscall exit not implemented");
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

/*
 * Corresponds to int open(const char *pathname, int flags, int mode); 
 * 
 * pathname: path to the file
 * flags: access mode of the file
 * mode: file permission mode if flags contains FS_CREAT
 * 
 * Open the file specified by pathname. Argument flags must include exactly one
 * of the following access modes:
 *   FS_RDONLY - Read-only mode
 *   FS_WRONLY - Write-only mode
 *   FS_RDWR - Read-write mode
 * flags can additionally include FS_CREAT. If FS_CREAT is included, a new file
 * is created with the specified permission (mode) if it does not exist yet.
 * 
 * Each open file maintains a current position, initially zero.
 *
 * Return:
 * Non-negative file descriptor on success.
 * The file descriptor returned by a successful call will be the lowest-numbered
 * file descriptor not currently open for the process.
 * 
 * ERR_FAULT - Address of pathname is invalid.
 * ERR_INVAL - flags has invalid value.
 * ERR_NOTEXIST - File specified by pathname does not exist, and FS_CREAT is not
 *                specified in flags.
 * ERR_NOTEXIST - A directory component in pathname does not exist.
 * ERR_NORES - Failed to allocate inode in directory (FS_CREAT is specified)
 * ERR_FTYPE - A component used as a directory in pathname is not a directory.
 * ERR_NOMEM - Failed to allocate memory.
 */
// int open(const char *pathname, int flags, fmode_t mode);
static sysret_t
sys_open(void *arg)
{
    sysarg_t pathname, flags, mode;
    struct file *file;
    err_t err;

    kassert(fetch_arg(arg, 1, &pathname));
    kassert(fetch_arg(arg, 2, &flags));
    kassert(fetch_arg(arg, 3, &mode));

    if (!validate_str((char *)pathname)) {
        return ERR_FAULT;
    }

    if (flags & (flags >> 1)) {
        return ERR_INVAL;
    }

    err = fs_open_file((char *)pathname, (int) flags, (fmode_t) mode, &file);
    if (err != ERR_OK) {
        return err;
    }

    return alloc_fd(file);
}

/*
 * Corresponds to int close(int fd);
 *
 * fd: file descriptor of a file
 *
 * Close the given file descriptor.
 *
 * Return:
 * ERR_OK - File successfully closed.
 * ERR_INVAL - fd isn't a valid open file descriptor.
 */
// int close(int fd);
static sysret_t
sys_close(void *arg)
{
    struct file *file;
    sysarg_t fd;
    kassert(fetch_arg(arg, 1, &fd));

    if ((file = remove_fd((int)fd)) == (void *)ERR_INVAL) {
        return ERR_INVAL;
    }
    fs_close_file(file);
    return ERR_OK;
}

/*
 * Corresponds to ssize_t read(int fd, void *buf, size_t count);
 *
 * fd: file descriptor of a file
 * buf: buffer to write read bytes to
 * count: number of bytes to read
 *
 * Read from a file descriptor. Reads up to count bytes from the current position of the file descriptor
 * fd and places those bytes into buf. The current position of the file descriptor is updated by number of bytes read.
 *
 * If there are insufficient available bytes to complete the request,
 * reads as many as possible before returning with that number of bytes.
 * Fewer than count bytes can be read in various conditions:
 * If the current position + count is beyond the end of the file.
 * If this is a pipe or console device and fewer than count bytes are available
 * If this is a pipe and the other end of the pipe has been closed.
 *
 * Return:
 * On success, the number of bytes read (non-negative). The file position is
 * advanced by this number.
 * ERR_FAULT - Address of buf is invalid.
 * ERR_INVAL - fd isn't a valid open file descriptor.
 */
// int read(int fd, void *buf, size_t count);
static sysret_t
sys_read(void* arg)
{
    sysarg_t fd, buf, count;
    struct file *file;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 3, &count));
    kassert(fetch_arg(arg, 2, &buf));

    if (!validate_bufptr((void*)buf, (size_t)count)) {
        return ERR_FAULT;
    }

    if (fd == 0) {
        return console_read((void*)buf, (size_t)count);
    }

    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }

    file = get_fd((int)fd);

    return fs_read_file(file, (void *)buf, (size_t)count, &file->f_pos);
}

/*
 * Corresponds to ssize_t write(int fd, const void *buf, size_t count);
 *
 * fd: file descriptor of a file
 * buf: buffer of bytes to write to the given fd
 * count: number of bytes to write
 *
 * Write to a file descriptor. Writes up to count bytes from buf to the current position of
 * the file descriptor. The current position of the file descriptor is updated by that number of bytes.
 *
 * If the full write cannot be completed, writes as many as possible before returning with
 * that number of bytes. For example, if the disk runs out of space.
 *
 * Return:
 * On success, the number of bytes (non-negative) written. The file position is
 * advanced by this number.
 * ERR_FAULT - Address of buf is invalid;
 * ERR_INVAL - fd isn't a valid open file descriptor.
 * ERR_END - if fd refers to a pipe with no open read
 */
// int write(int fd, const void *buf, size_t count)
static sysret_t
sys_write(void* arg)
{
    sysarg_t fd, count, buf;
    struct file *file;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 3, &count));
    kassert(fetch_arg(arg, 2, &buf));

    if (!validate_bufptr((void*)buf, (size_t)count)) {
        return ERR_FAULT;
    }

    if (fd == 1) {
        // write some stuff for now assuming one string
        return console_write((void*)buf, (size_t) count);
    }
    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }

    file = get_fd((int)fd);

    return fs_write_file(file, (void *)buf, (size_t)count, &file->f_pos);
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

/*
 * Corresponds to int readdir(int fd, struct dirent *dirent);
 *
 * fd: file descriptor of a directory
 * dirent: struct direct pointer
 *
 * Populate the struct dirent pointer with the next entry in a directory.
 * The current position of the file descriptor is updated to the next entry.
 * Only fds corresponding to directories are valid for readdir.
 *
 * Return:
 * ERR_OK - A directory entry is successfully read into dirent.
 * ERR_FAULT - Address of dirent is invalid.
 * ERR_INVAL - fd isn't a valid open file descriptor.
 * ERR_FTYPE - fd does not point to a directory.
 * ERR_NOMEM - Failed to allocate memory.
 * ERR_END - End of the directory is reached.
 */
// int readdir(int fd, struct dirent *dirent);
static sysret_t
sys_readdir(void *arg)
{
    sysarg_t fd, dirent;
    struct file *file;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 2, &dirent));

    if (!validate_fd((int)fd)) {
        return ERR_INVAL;
    }

    if (!validate_bufptr((struct dirent *)dirent, sizeof(struct dirent *))) {
        return ERR_FAULT;
    }

    file = get_fd((int)fd);

    return fs_readdir(file, (struct dirent *)dirent);
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

/*
 * Corresponds to int fstat(int fd, struct stat *stat);
 *
 * fd: file descriptor of a file
 * stat: struct stat pointer
 *
 * Populate the struct stat pointer passed in to the function.
 * Console (stdin, stdout) and all console dupped fds are not valid fds for fstat.
 * Only real files in the file system are valid for fstat.
 *
 * Return:
 * ERR_OK - File status is written in stat.
 * ERR_FAULT - Address of stat is invalid.
 * ERR_INVAL - fd isn't a valid open file descriptor or refers to non file.
 */
// int fstat(int fd, struct stat *stat);
static sysret_t
sys_fstat(void *arg)
{
    sysarg_t fd, stat;
    struct file *file;

    kassert(fetch_arg(arg, 1, &fd));
    kassert(fetch_arg(arg, 2, &stat));

    if (!validate_bufptr((struct stat *)stat, sizeof(struct stat *))) {
        return ERR_FAULT;
    }

    file = get_fd((int)fd);

    if (file == (void *)ERR_INVAL || file == &stdin || file == &stdout) {
        return ERR_INVAL;
    }

    ((struct stat *)stat)->ftype = file->f_inode->i_ftype;
    ((struct stat *)stat)->inode_num = file->f_inode->i_inum;
    ((struct stat *)stat)->size = file->f_inode->i_size;

    return ERR_OK;
}

// void *sbrk(size_t increment);
static sysret_t
sys_sbrk(void *arg)
{
    panic("syscall sbrk not implemented");
}

// void memifo();
static sysret_t
sys_meminfo(void *arg)
{
    as_meminfo(&proc_current()->as);
    return ERR_OK;
}

/*
 * Corresponds to int dup(int fd);
 *
 * fd: file descriptor of a file
 *
 * Duplicate the file descriptor fd, must use the smallest unused file descriptor.
 * Reading/writing from a dupped fd should advance the file position of the original fd
 * and vice versa.
 *
 * Return:
 * Non-negative file descriptor on success
 * ERR_INVAL if fd is invalid
 * ERR_NOMEM if no available new file descriptor
 */
// int dup(int fd);
static sysret_t
sys_dup(void *arg)
{
    sysarg_t fd;
    struct file *file;
    int dup_fd;

    kassert(fetch_arg(arg, 1, &fd));

    if ((file = get_fd((int)fd)) == (void *)ERR_INVAL) {
        return ERR_INVAL;
    }

    if ((dup_fd = alloc_fd(file)) == ERR_NOMEM) {
        return ERR_NOMEM;
    }

    fs_reopen_file(file);

    return dup_fd;
}

// int pipe(int* fds);
static sysret_t
sys_pipe(void* arg)
{
    panic("syscall pipe not implemented");
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

