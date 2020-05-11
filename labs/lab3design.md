# Lab 3 Design Doc: Multiprocessing

## Overview

## In-depth Analysis and Implementation

*Example* 
### fork: 
- A new process needs to be created through `kernel/proc.c:proc_init`
- Parent must copy its memory to the child via `kernel/mm/vm.c:as_copy_as` 
- All the opened files must be duplicated in the new process (not as simple as a memory copy)
- Create a new thread to run the process
- Duplicate current thread (parent process)'s trapframe in the new thread (child process)
- Set up trapframe to return 0 in the child via `kernel/trap.c:tf_set_return`, while returning the child's pid in the parent

## Risk Analysis
