#pragma once

#include <ckpt/ckpt_data.h>
#include <mm/buddy.h>
#include <mm/page.h>

struct ckpt_page *get_latest_ckpt_page(struct ckpt_page_pair *page_pair);
struct ckpt_page *get_second_latest_ckpt_page(struct ckpt_page_pair *page_pair);
struct ckpt_page *get_ckpt_page_with_version(struct ckpt_page_pair *page_pair,
                                             u64 version_number);

struct ckpt_page_pair *get_page_pair(struct page *page, u64 index);
void free_page_pair(struct ckpt_page_pair *page_pair);
