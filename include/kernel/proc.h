#ifndef _PROC_H_
#define _PROC_H_

#include <kernel/synch.h>
#include <kernel/vm.h>
#include <kernel/types.h>
#include <kernel/list.h>

#define ANY_CHILD -1
#define STATUS_ALIVE 0xbeefeeb
#define PROC_MAX_ARG 128
#define PROC_NAME_LEN 32
#define PROC_MAX_FILE 128

struct fdtable {
    struct file *table[PROC_MAX_FILE];  // array of pointers to open files; NULL if empty
    int max;                            // maximum number of open files for a single process
    int count;                          // current number of open files
    int first_avail;                    // index to begin looking for openings in the table
};

struct proc {
    pid_t pid;
    char name[PROC_NAME_LEN];
    struct addrspace as;
    struct inode *cwd;                  // current working directory
    List threads;                       // list of threads belong to the process, right now just 1 per process
    Node proc_node;                     // used by ptable to keep track each process
    struct fdtable fdtable;             // table storing file descriptors
};

struct proc *init_proc;

/*
 * Verifies that the given file descriptor is within the bounds of possible
 * file descriptor values, and that the given file descriptor is currently
 * in use in the given process's file descriptor table.
 */
bool proc_validate_fd(struct proc *p, int fd);

/*
 * Allocates the lowest available position in the fdtable of the given process
 * for the given file.
 *
 * Return:
 * file descriptor of the allocated file in the table.
 * ERR_NOMEM - The file descriptor table is full.
 */
sysret_t proc_alloc_fd(struct proc *p, struct file *file);

/*
 * Removes the given file descriptor from the given process's file descriptor table
 *
 * Return:
 * On success, the file pointer corresponding to the file descriptor which was removed.
 * ERR_INVAL - The given file descriptor is not in the process's fdtable.
 */
struct file* proc_remove_fd(struct proc *p, int fd);

/*
 * Returns the file pointer stored at the given index of the file descriptor table
 *
 * Return:
 * the file pointer at the given index.
 * ERR_INVAL - The given file descriptor is not in the process's fdtable.
 */
struct file* proc_get_fd(struct proc *p, int fd);

void proc_sys_init(void);

/* Spawn a new process specified by executable name and argument */
err_t proc_spawn(char *name, char** argv, struct proc **p);

/* Fork a new process identical to current process */
struct proc* proc_fork();

/* Return current thread's process. NULL if current thread is not associated with any process */
struct proc* proc_current();

/* Attach a thread to a process. */
void proc_attach_thread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. Returns True if detached thread is the 
 * last thread of the process, False otherwise */
bool proc_detach_thread(struct thread *t);

/*
 * Wait for a process to change state. If pid is ANY_CHILD, wait for any child process.
 * If wstatus is not NULL, store the the exit status of the child in wstatus.
 *
 * Return:
 * pid of the child process that changes state.
 * ERR_CHILD - The caller does not have a child with the specified pid.
 */
int proc_wait(pid_t, int* status);

/* Exit a process with a status */
void proc_exit(int);

#endif /* _PROC_H_ */
