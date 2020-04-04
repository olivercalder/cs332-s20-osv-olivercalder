# Tools

## Toolchain Setup

### 64-bit Linux Users

Native toolchain works.

## Installing QEMU

You need QEMU >= 2.9. You can download it from [QEMU instruction manual](https://www.qemu.org/download).

## Source Control
We use Git for source control. For more information about how to use Git, please consult [Git user's manual](http://www.kernel.org/pub/software/scm/git/docs/user-manual.html).

## Debugging
We use GDB as a remote debugger for osv in QEMU. To attach GDB to osv, you need to open two separate terminals. Both should be in the osv root directory. In one terminal, type "make qemu-gdb". This starts the qemu process and wait for GDB to attach. In another terminal, type "gdb". Now the GDB process is attached to qemu.
