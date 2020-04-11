# Lab 1: File System Exploration

## Important Deadlines
- Lab due 4/20/2020 (Monday) at 9:00pm.

## Introduction
All of our labs are based on OSV. OSV is a new experimental
operating system kernel for teaching the principles and practice of operating systems.
OSV's baseline code is a complete, bootable operating system. It provides some simple system
calls that it is capable of running a minimal shell program. This lab is about exploring the osv codebase and generally getting oriented. Your task for futures labs will be to make OSV complete and also add a few functionalities. If you haven't already, you should [set up your osv GitHub repo](http://cs.carleton.edu/faculty/awb/cs332/s20/topics/topic1.html#org2d511e4).

## Logistics
To pull this writeup into your repo, run the command
```
git pull upstream master
```
and merge.

Before you start coding, add a git tag
```
$ git tag start_lab1
$ git push origin master --tags
```

Git allows you to keep track of the changes you make to the code. For example, if you are
finished with one of the exercises, and want to checkpoint your progress, you can git add 
changed files and then commit your changes by running:
```
$ git commit -m 'my solution for lab1'
Created commit 60d2135: my solution for lab1
 1 files changed, 1 insertions(+), 0 deletions(-)
```

You can keep track of your changes by using the `git diff` command. Running `git diff` will display
the changes to your code since your last commit, and `git diff upstream/master` will display the
changes relative to the initial code supplied. Here, `upstream/master` is the name of the git
branch with the initial code you downloaded from the course server for this assignment.

## Organization of source code

```
osv
├── arch(x86_64)      // all architecture dependent code for different architecture
│   └── boot          // bootloader code
│   └── include(arch) // architecture dependent header files (contain architecture specific macros)
│   └── kernel        // architecture dependent kernel code
│   └── user          // architecture dependent user code (user space syscall invocation code)
│   └── Rules.mk      // architecture dependent makefile macros
├── include           // all architecture independent header files
│   └── kernel        // header files for kernel code
│   └── lib           // header files for library code, used by both kernel and user code
├── kernel            // the kernel source code
│   └── drivers       // driver code
│   └── mm            // memory management related code, both physical and virtual memory
│   └── fs            // file system code, generic filesys interface (VFS)
│       └── sfs       // a simple file system implementation implementing VFS
│   └── Rules.mk      // kernel specific makefile macros
├── user              // all the source code for user applications
│   └── lab*          // tests for lab* (no tests for lab 1)
│   └── Rules.mk      // user applications makefile macros
├── lib               // all the source code for library code
│   └── Rules.mk      // user applications makefile macros
├── tools             // utility program for building osv
├── labs              // handouts for labs and osv overview
└── Makefile          // Makefile for building osv kernel
```

After compilation (`make`), a new folder `build` will appear, which contains the kernel and fs image and all the intermediate binaries (.d, .o, .asm).

## Part 1: Debugging OSV

The purpose of the first exercise is to get you started with QEMU and QEMU/GDB debugging.

### A Note on x86_64 Assembly

The definitive reference for x86_64 assembly language programming is Intel’s
instruction set architecture reference is
[Intel 64 and IA-32 Architectures Software Developer’s Manuals](https://software.intel.com/en-us/articles/intel-sdm).
It covers all the features of the most recent processors that we won’t need in class but
you may be interested in learning about. An equivalent (and often friendlier) set of
manuals is [AMD64 Architecture Programmer’s Manual](http://developer.amd.com/resources/developer-guides-manuals/).
Save the Intel/AMD architecture manuals for later or use them for reference when you want
to look up the definitive explanation of a particular processor feature or instruction.

You don't have to read them now, but you may want
to refer to some of this material when reading and writing x86_64 assembly.

### GDB

GDB can be used as a remote debugger for osv. We have provided a gdbinit file for you to use as `~/.gdbinit`.
It connects to a port that qemu will connect to when running `make qemu-gdb` and loads in kernel symbols.
You can generate your `~/.gdbinit` using the following command.
```
cp arch/x86_64/gdbinit ~/.gdbinit
```

To attach GDB to osv, you need to open two separate terminals. Both of them should be in the osv root directory. In one terminal, type `make qemu-gdb`. This starts the qemu process and wait for GDB to attach. In another terminal, type `gdb`. Now the GDB process is attached to qemu.

In osv, when bootloader loads the kernel from disk to memory, the CPU operates in 32-bit mode. The starting point of the 32-bit kernel is in `arch/x86_64/kernel/entry.S`. `entry.S` setups 64-bit virtual memory and enables 64-bit mode. You don't need to understand `entry.S`. `entry.S` jumps to `main` function in `kernel/main.c` which is the starting point of 64-bit OS.

### Question #1:
After attaching the qemu instance to gdb, set a breakpoint at the entrance of osv by typing in "b main". You should see a breakpoint set at `main` in `kernel/main.c`.
Then type "c" to continue execution. OSV will go through booting and stop at `main`. Which line of code in `main` prints the physical memory table? (Hint: use the `n` comand)

### Question #2:
We can examine memory using GDB’s `x` command. The GDB manual has full details, but for now, it is enough to know that the command `x/nx ADDR` prints `n` words of memory at `ADDR`. (Note that both ‘x’s in the command are lowercase, the second x tells gdb to display memory content in hex)

To examine instructions in memory (besides the immediate next one to be executed, which GDB prints automatically), use the `x/i` command. This command has the syntax `x/ni ADDR`, where `n` is the number of consecutive instructions to disassemble and `ADDR` is the memory address at which to start disassembling.

Repeat the previous process to break at main, what's the memory address of `main` (`p main`)? Does GDB work with real physical addresses?

### Question #3:
You may have noticed that when gdb hit your breakpoint, the message specified a thread:
```
Thread 1 hit Breakpoint 1, main () at kernel/main.c:34
```
osv boots up with multiple threads, which you can see by running `info threads`. We'll talk a lot more about threads in the coming weeks—the basic idea is that each thread is an independent unit of execution, which enables simultaneous computation. For now the goal is to explore gdb's capabilities when it comes to multi-threaded programs. At the start of `main`, `info threads` indicates that one thread is halted. What function starts it running? What "main" function does the second thread start in? What happens if you restart and set a breakpoint for that function?

## Part 2: The File System
The goal of this part of the lab is twofold: (1) examine how the file system concepts we've looked at operate in practice and (2) practice the skill of navigating a large, complex codebase you didn't write (this second one occurs all the time in real-world practice, and very rarely in the classroom). To help you navigate the code for osv, you will probably want the ability to jump to the definition of a function, type, or macro. Command-line editors like emacs and vim can use ctags, see [this tutorial](https://courses.cs.washington.edu/courses/cse451/10au/tutorials/tutorial_ctags.html). Many others can be configured to support this in various ways. Feel free to crowdsource editor-setup ideas in Slack.

Refer to the [overview document](https://github.com/Carleton-College-CS/osv-s20/blob/master/labs/overview.md#file-system) for a high-level description of the file system in osv.

It's up to you how to go about answering the following questions. For reference, I added about 30 lines to `kernel/fs/fs.c` to print (via `kprintf`) the information I needed for questions 5–8, including adding `#include <kernel/bdev.h>` to `fc.c`. I spent most of my time reading `kernel/fs/fs.c`, `kernel/fs/sfs/sfs.c`, `kernel/bdev.c`, `include/kernel/fs.h`, `include/kernel/sfs.h`, and `include/kernel/bdev.h`.
You may also find it useful to look at the utility that constructs the disk image that osv uses, `tools/mkfs.c`.
Only your answers to the questions will be evaluated, but make sure you explain how you arrived at them for full credit.

### Question #4:
We've had a discussion about what should happen if one program unlinks a file another program is reading. 
Now it's your turn to track down what osv will do in that situation.
Start by studying `fs_unlink` in `kernel/fs/fs.c` and go from there.
You don't have to be 100% certain or run exhaustive tests—just provide your best understanding and justify it by what you observe in the code.

### Question #5:
What is the structure of an inode in osv? Include in your answer the connection from an inode to data blocks and osv's maximum file size (given `BDEV_BLK_SIZE`).

### Question #6:
Which inodes are in use? How many are used for files and how many are used for directories?

### Question #7:
How many data blocks are in use? Only count data blocks used for file contents, not blocks allocated for other things. Are each file's data blocks contiguous (i.e., do the blocks for a given file have consecutive numbers)?

### Question #8:
How much space is wasted in storing `smallfile`?

## Handin
Create a file `lab1.txt` in the top-level osv directory with
your answers to the questions listed above.

When you're finished, create a `end_lab1` git tag as described above so I know the point at which you
submitted your code.
