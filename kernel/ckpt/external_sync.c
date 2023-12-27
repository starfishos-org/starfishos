#include "common/types.h"
#include <ckpt/ckpt.h>
#include <ckpt/external_sync.h>

extern vaddr_t transform_vaddr(char *user_buf);

// user/musl-1.1.24/src/chcore-port/socket.c 
struct ringbuf_header {
	char is_delayed_ringbuf;
	int pos_writer;
	int pos_reader;
	int pos_visiable_writer;
};
 
struct ext_ringbuf {
    struct list_head node;
    struct ringbuf_header *buf; // kernel vaddr of ringbuf start
};

struct list_head *ext_ringbuf_list;

/*
 * sys_register_external_ringbuf -- register a ringbuf to ckpt service
 */
int sys_register_external_ringbuf(u64 buffer)
{
#ifndef EXT_SYNC_ENABLED
    return 0;
#endif
    vaddr_t kva;
    struct ext_ringbuf *ext_rbuf;

    kva = transform_vaddr((char *)buffer);
    
    ext_rbuf = (struct ext_ringbuf *)kmalloc(sizeof(*ext_rbuf), __DEFAULT__);
    if (!ext_rbuf) {
        return -ENOMEM;
    }

    if (!ext_ringbuf_list) {
        ext_ringbuf_list =
            (struct list_head *)kmalloc(sizeof(*ext_ringbuf_list), __DEFAULT__);
        if (!ext_ringbuf_list) {
            return -ENOMEM;
        }
        init_list_head(ext_ringbuf_list);
    }

    ext_rbuf->buf = (struct ringbuf_header *)kva;
    ext_rbuf->buf->is_delayed_ringbuf = 1;
    ext_rbuf->buf->pos_visiable_writer = ext_rbuf->buf->pos_writer;
    
    list_add(&(ext_rbuf->node), ext_ringbuf_list);

    return 0;
}

/*
 * sys_unregister_external_ringbuf -- unregister a ringbuf to ckpt service
 */
int sys_unregister_external_ringbuf(u64 buffer)
{
    vaddr_t kva;
    struct ext_ringbuf *ext_rbuf, *tmp;

    kva = transform_vaddr((char *)buffer);

    for_each_in_list_safe(ext_rbuf, tmp, node, ext_ringbuf_list)
    {
        if ((vaddr_t)(ext_rbuf->buf) == kva) {
            list_del(&(ext_rbuf->node));
            kfree(ext_rbuf);
            return 0;
        }
    }

    return -EEXIST;
}

void update_external_ringbufs()
{
    struct ext_ringbuf *ext_rbuf, *tmp;

    if (!ext_ringbuf_list)
        return;

    /* update visiable writer pointer of each ringbuf */
    for_each_in_list_safe(ext_rbuf, tmp, node, ext_ringbuf_list)
    {
        ext_rbuf->buf->pos_visiable_writer = ext_rbuf->buf->pos_writer;
    }
}

bool is_in_ringbuf_list(vaddr_t kva)
{
    struct ext_ringbuf *ext_rbuf, *tmp;

    for_each_in_list_safe(ext_rbuf, tmp, node, ext_ringbuf_list)
    {
        if ((vaddr_t)(ext_rbuf->buf) == kva) {
            return true;
        }
    }
    return false;
}

int get_pos_reader(vaddr_t kva)
{
    struct ringbuf_header *buf = (struct ringbuf_header *)kva;
    return buf->pos_reader;
}

void set_pos_reader(vaddr_t kva, int pos_reader)
{
    struct ringbuf_header *buf = (struct ringbuf_header *)kva;
    buf->pos_reader = pos_reader;
}

void clear_external_ringbuf()
{
    struct ext_ringbuf *ext_rbuf, *tmp;

    if (ext_ringbuf_list) {
        for_each_in_list_safe(ext_rbuf, tmp, node, ext_ringbuf_list)
        {
            list_del(&(ext_rbuf->node));
            kfree(ext_rbuf);
        }
    }

    ext_ringbuf_list = NULL;
}
