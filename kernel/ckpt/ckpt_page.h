#pragma once

#include <ckpt/ckpt_data.h>
#include <mm/buddy.h>

struct ckpt_page *get_latest_ckpt_page(struct ckpt_page_pair *page_pair);
struct ckpt_page *get_second_latest_ckpt_page(struct ckpt_page_pair *page_pair);

struct ckpt_page_pair *get_page_pair(struct page *page, u64 index);
void free_page_pair(struct ckpt_page_pair *page_pair);
