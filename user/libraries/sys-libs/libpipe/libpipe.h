#pragma once

#include <chcore/type.h>

struct pipe;

struct pipe *create_pipe_from_pmo(u32 size, int pmo_cap);
struct pipe *create_pipe_from_vaddr(u32 size, void *data);
void del_pipe(struct pipe *pp);
void pipe_init(const struct pipe *pp);
u32 pipe_read(const struct pipe *pp, void *buf, u32 n);
u32 pipe_read_exact(const struct pipe *pp, void *buf, u32 n);
u32 pipe_write(const struct pipe *pp, const void *buf, u32 n);
u32 pipe_write_exact(const struct pipe *pp, const void *buf, u32 n);
bool pipe_is_empty(const struct pipe *pp);
bool pipe_is_full(const struct pipe *pp);
