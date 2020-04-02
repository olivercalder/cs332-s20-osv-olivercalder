#ifndef _PIPE_H_
#define _PIPE_H_

#include <kernel/types.h>

struct file;
/*
 * Allocate two pipe files, fills in read end and write end pointers
 * 
 * Return:
 * ERR_OK on success
 * ERR_NOMEM if there isn't enough kernel memory available
 * 
*/

err_t pipe_alloc(struct file **read_end, struct file **write_end);

#endif /* _PIPE_H_ */
