#pragma once

#include "polling.h"

void handle_polling_request(struct dq_node *node);
void handle_batch_reads(struct dq_node **batch, int count);

void handle_polling_fs_open(struct dq_node *node);
void handle_polling_fs_read(struct dq_node *node);
void handle_polling_fs_write(struct dq_node *node);
void handle_polling_fs_close(struct dq_node *node);
void handle_polling_fs_empty(struct dq_node *node);
void handle_polling_kernel_flush_tlb(struct dq_node *node);
void handle_polling_print_debug_info(struct dq_node *node);
