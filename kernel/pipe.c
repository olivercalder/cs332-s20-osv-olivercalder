#include <kernel/pipe.h>
#include <kernel/fs.h>
#include <kernel/kmalloc.h>
#include <kernel/synch.h>
#include <lib/errcode.h>
#include <lib/stddef.h>

#define PIPESIZE 512

static struct kmem_cache *pipe_allocator;

struct pipe {
    struct spinlock lock;
    struct condvar read_avail;
    struct condvar write_avail;
    char data[PIPESIZE];
    size_t nwrite;  // number of bytes written
    bool readopen;   // read fd is still open
    bool writeopen;  // write fd is still open
};

static ssize_t pipe_read(struct file *file, void *buf, size_t count, offset_t *ofs);
static ssize_t pipe_write(struct file *file, const void *buf, size_t count, offset_t *ofs);
static void pipe_close(struct file *p);

static struct file_operations pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close
};

err_t
pipe_alloc(struct file **read_end, struct file **write_end)
{
    struct pipe *p;
    struct file *read = NULL;
    struct file *write = NULL;

    if (pipe_allocator == NULL) {
        if ((pipe_allocator = kmem_cache_create(sizeof(struct pipe))) == NULL) {
            return ERR_NOMEM;
        }
    }

    if ((read = fs_alloc_file()) == NULL) {
        return ERR_NOMEM;
    }
    read->oflag = FS_RDONLY;
    read->f_ops = &pipe_ops;

    if ((write = fs_alloc_file()) == NULL) {
        goto error;
    }
    write->oflag = FS_WRONLY;
    write->f_ops = &pipe_ops;

    if ((p = kmem_cache_alloc(pipe_allocator)) == NULL) {
        goto error;
    }

    p->readopen = True;
    p->writeopen = True;
    p->nwrite = 0;
    spinlock_init(&p->lock);
    condvar_init(&p->read_avail);
    condvar_init(&p->write_avail);

    read->info = p;
    write->info = p;
    *read_end = read;
    *write_end = write;
    return ERR_OK;
error:
    if (read) {
        fs_close_file(read);
    }
    if (write) {
        fs_close_file(write); 
    }
    return ERR_NOMEM;
}

static ssize_t
pipe_read(struct file *file, void *buf, size_t count, offset_t *ofs)
{
    ssize_t i;
    struct pipe *p = (struct pipe*) file->info;
    char* user_buf = (char*) buf;

    spinlock_acquire(&p->lock);
    // while pipe is empty or we've already read all the data
    // wait for more data to be read
    while (file->f_pos == p->nwrite && p->writeopen) { 
        condvar_wait(&p->read_avail, &p->lock);
    }
    // now we have some data, copy into buffer
    for(i = 0; i < count; i++){
        if (file->f_pos == p->nwrite) {
            break;
        }
        user_buf[i] = p->data[file->f_pos++ % PIPESIZE];
    }
    // we read some data, meaning there are more rooms for writing
    // data into the pipe, so signal write_avail
    condvar_broadcast(&p->write_avail);
    spinlock_release(&p->lock);
    return i;
}

static ssize_t
pipe_write(struct file *file, const void *buf, size_t count, offset_t *ofs)
{
    ssize_t i;
    struct pipe *p = (struct pipe*) file->info;
    const char* user_buf = (const char*) buf;

    spinlock_acquire(&p->lock);
    if (count && !p->readopen) {
        spinlock_release(&p->lock);
        return ERR_END;
    }
    for(i = 0; i < count; i++){
        // while pipe is full, wait for room to write data
        while (p->nwrite == file->f_pos + PIPESIZE) {
            // read end has been close
            if (!p->readopen) {
                spinlock_release(&p->lock);
                return ERR_END;
            }
            // we've written some data in the loop, signal readers to read
            condvar_broadcast(&p->read_avail);
            condvar_wait(&p->write_avail, &p->lock);
        }
        p->data[p->nwrite++ % PIPESIZE] = user_buf[i];
    }
    
    // we wrote some data, meaning there are more data to read 
    // from the pipe, so signal read_avail
    condvar_broadcast(&p->read_avail);
    spinlock_release(&p->lock);
    return i;
}

static void
pipe_close(struct file *file)
{
    struct pipe *p = (struct pipe*) file->info;

    // check file's open flag to see which end is closed
    spinlock_acquire(&p->lock);
    if(file->oflag == FS_RDONLY){
        p->readopen = False;
        // closing read end means no more reads, signal writers so they 
        // can give up their writes
        condvar_broadcast(&p->write_avail);
    } else {
        p->writeopen = False;
        condvar_broadcast(&p->read_avail);
    }

    if(!p->readopen && !p->writeopen){
        spinlock_release(&p->lock);
        kmem_cache_free(pipe_allocator, p);
    } else {
        spinlock_release(&p->lock);
    }
}

