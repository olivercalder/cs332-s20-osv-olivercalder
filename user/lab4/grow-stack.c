#include <lib/test.h>

int
main()
{
    int i;
    struct sys_info info1, info2;
    info(&info1);

    // allocate 8 page of buffer on the stack and then access it to page them in
    size_t buf_size = 8 * 4096;
    char buf[buf_size];
    for (i = 0; i < buf_size/4096; i++) {
        buf[i * 4096] = 'a';
        buf[i * 4096 - 1] = buf[i * 4096];
    }
    info(&info2);

    // if grow ustack on-demand is implemented, then the 8 pages are allocated at
    // run-time
    if (info2.num_pgfault - info1.num_pgfault < 8 ) {
        error("user stack is not growing dynamically");
    }
    pass("grow-stack");
    exit(0);
    return 0;
}