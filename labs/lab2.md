# Lab 2: System Calls
## Important Deadlines
- Lab due 5/1/2020 (Friday) at 9:00pm.

## Introduction
This lab adds system calls to osv for interacting with the file system.
Your task is to implement the system calls listed below. The understanding of the osv file system you gained in lab 1 will prove useful.
Unimplemented system calls will [panic](https://en.wikipedia.org/wiki/Kernel_panic) if they are called, as you implement each system call, remove the panic. 
**For this lab you do not need to worry about synchronization. There will only be one process.**

To pull new changes, lab2 tests, and this writeup, run the command
```
git pull upstream master
```
and merge.

osv has to support a list of system calls. Here is a list of system calls that are already implemented.

Process system calls:
- `int spawn(const char *args)`
  + creates a new process
- `int getpid()`
  + returns pid of process

File system system calls:
- `int link(const char *oldpath, const char *newpath)`
  + create a hard link for a file
- `int unlink(const char *pathname)`
  + remove a hard link.
- `int mkdir(const char *pathname)`
  + create a directory
- `int chdir(const char *path)`
  + change the current workig directory
- `int rmdir(const char *pathname)`
  + remove a directory

Utility system calls:
- `void meminfo()`
  + print information about the current process's address space
- `void info(struct sys_info *info)`
  + report system info

### Trap
osv uses software interrupts to implement system calls. When a user application needs to invoke a system call,
it issues an interrupt with `int 0x40`. System call numbers are defined in `include/lib/syscall-num.h`. 
When the `int` instruction is being issued, the user program is responsible to set the register `%rax` to be the chosen system call number.

The software interrupt is captured by the registered trap vector (`arch/x86_64/kernel/vectors.S`) and the handler in `arch/x86_64/kernel/vectors.S`
will run. The handler will reach the `trap` function in `arch/x86_64/kernel/trap.c` and the `trap` function to route the interrupt to
`syscall` function implemented in `kernel/syscall.c`. `syscall` then routes the call to the respective handler in `kernel/syscall.c`.

### File, file descriptor and inode
The kernel needs to keep track of the open files so it can read, write, and eventually close the
files. A file descriptor is an integer that represents this open file. Somewhere in the kernel you
will need to keep track of these open files. Remember that file descriptors must be reusable between
processes. File descriptor 4 in one process should be able to be different than file descriptor 4 in another
(although they could reference the same open file).

Traditionally the file descriptor is an index into an array of open files.

The console is simply a file (file descriptor) from the user application's point of view. Reading
from keyboard and writing to screen is done through the kernel file system call interface.
Currently read/write to console is implemented as hard coded numbers, but as you implement file descriptors, 
you should use stdin and stdout file structs as backing files for console reserved file descriptors (0 and 1).

### Question #1
What is the first line of C code executed in the kernel when there is an interrupt? To force an interrupt, perform a system call. 
Add a `getpid` call within `sh.c` and use gdb to trace through it with the `si` command (`si` steps by one assembly instruction). You can add `build/user/sh` as a symbol file in your gdbinit (adding `add-symbol-file build/user/sh 0x00000000004000e8` should let you add a breakpoint at `sh.c:main`).
Hint: you can use the cs register, which selects the code segment, to tell when you move from user to kernel mode (its value will change). Display its value with `p $cs`.

### Question #2
How large (in bytes) is a trap frame?

### Question #3
Set a breakpoint in the kernel implementation of a system call (e.g., `sys_getpid`) and continue
executing until the breakpoint is hit (be sure to call `getpid` within `sh.c`. Do a backtrace, `bt` in gdb. 
What kernel functions are reported by the backtrace when it reaches `sys_getpid`?

### Implementation

I have provided you with a [lab 2 design document](lab2design.md) to guide you through your implementation. 
This will also serve as an example when you write your own design docs for future labs. 

#### Hints:
- File descriptors are just integers.
- Look at already implemented system calls to see how to parse the arguments. (`kernel/syscall.c:sys_read`)
- If a new file descriptor is allocated, it must be saved in the process's file descriptor tables. Similarly, if a file descriptor is released, this must be reflected in the file descriptor table.
- A full file descriptor table is a user error (return an error value instead of calling `panic`).
- A complete file system is already implemented. You can use `fs_read_file`/`fs_write_file` to read/write from a file. 
  You can use `fs_open_file` to open a file. If you decide to have multiple file descriptors referring to a single file struct, make sure to call `fs_reopen_file()` on the file each time.
  You can find information about a file in the file struct and the inode struct inside of the file struct.
- For this lab, the reference solution makes changes to `kernel/syscall.c`, `kernel/proc.c` and `include/kernel/proc.h`.

#### What To Implement
1) File Descriptor Opening
```c
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
sysret_t
sys_open(void *arg);
```

2) File Descriptor Reading
```c
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
sysret_t
sys_read(void *arg);
```

3) Close a File
```c
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
sysret_t
sys_close(void *arg);
```

4) File Descriptor Writing
```c
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
sysret_t
sys_write(void *arg);
```

5) Reading a Directory
```c
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
sysret_t
sys_readdir(void *arg);
```

6) Duplicate a File Descriptor
```c
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
sysret_t
sys_dup(void *arg);
```

7) File Stat
```c
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
sysret_t
sys_fstat(void *arg);
```

## Testing
After you implement each of the system calls described above. You can go through `user/lab2/*` files and run individual test
in the osv shell program by typing `close-test` or `open-bad-args` and so on. To run all tests in lab2, run `python3 test.py 2` in the osv directory (from your normal shell, not osv). The script relies on python >=3.6. For each test passed, you should see a `passed <testname>` message. At the end of the test it will display a score for the test run. Note that when grading the lab, tests will be run multiple times.

The tests have different weights—you can earn an S on the lab with correct answers to the questions and correct implementations of open, close, and read.

## Handin
Create a file `lab2.txt` in the top-level osv directory with
your answers to the questions listed above.

When you're finished, create a `end_lab2` git tag as described above so we know the point at which you
submitted your code.

