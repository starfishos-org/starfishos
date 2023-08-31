#pragma once

#include <common/types.h>

void update_external_ringbufs();
bool is_in_ringbuf_list(vaddr_t kva);

int get_pos_reader(vaddr_t kva);
void set_pos_reader(vaddr_t kva, int pos_reader);

// syscall
int sys_register_external_ringbuf(u64 buffer);
