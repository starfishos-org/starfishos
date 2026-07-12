#include <common/types.h>
#include <common/lock.h>
#include <common/kprint.h>
#include <arch/machine/smp.h>
#include <common/macro.h>
#include <mm/kmalloc.h>
#include <mm/slab.h>
#include <mm/buddy.h>

#ifdef SLAB_CRASH_RECOVERY

#include <dsm/dsm-single.h>
#include "tests.h"

/*
 * Slab crash recovery test
 *
 * Simulates crashes at every critical point during slab alloc/free
 * by manually writing partial state + per-CPU inflight log in dsm_meta,
 * then calling recover_cxl_slabs() and verifying consistency.
 *
 * Only runs on CPU 0 (single-threaded test).
 */

/* Shorthand for this machine's CPU 0 log */
#define MY_LOG (&dsm_meta->cxl_slab_meta[CUR_MACHINE_ID].cpu_logs[0])

/* Walk free list and count entries */
static unsigned long count_free_list(struct slab_header *slab)
{
	struct slab_slot_list *slot;
	unsigned long cnt = 0;

	slot = (struct slab_slot_list *)slab->free_list_head;
	while (slot != NULL) {
		cnt++;
		slot = slot->next_free;
	}
	return cnt;
}

/* Verify slab consistency: free_list length == current_free_cnt */
static int verify_slab_consistency(struct slab_header *slab, const char *ctx)
{
	unsigned long actual = count_free_list(slab);

	if (actual != slab->current_free_cnt) {
		kinfo("[SLAB_TEST] FAIL (%s): free_list has %lu entries "
		      "but current_free_cnt=%u\n",
		      ctx, actual, slab->current_free_cnt);
		return -1;
	}
	return 0;
}

/* Helper: write a fake crash log for CPU 0 */
static void fake_log(struct slab_header *slab, u8 op,
                     void *old_head, u16 old_cnt)
{
	struct slab_cpu_log *log = MY_LOG;
	log->slab_addr     = (void *)slab;
	log->old_free_head = old_head;
	log->old_free_cnt  = old_cnt;
	log->op            = op;
}

/*
 * Test 1: Crash during ALLOC — after log written, before mutation.
 */
static int test_crash_alloc_before_mutation(void)
{
	void *slot = alloc_in_cxl_slab(64);
	BUG_ON(slot == NULL);

	struct page *page = virt_to_page(slot);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *saved_head = slab->free_list_head;
	u16 saved_cnt = slab->current_free_cnt;

	/* Log written but no mutation */
	fake_log(slab, SLAB_OP_ALLOC, saved_head, saved_cnt);

	recover_cxl_slabs();

	if (verify_slab_consistency(slab, "alloc_before_mutation") != 0)
		return -1;
	BUG_ON(MY_LOG->op != SLAB_OP_NONE);

	free_in_cxl_slab(slot);
	kinfo("[SLAB_TEST] PASS: crash_alloc_before_mutation\n");
	return 0;
}

/*
 * Test 2: Crash during ALLOC — after mutation, before log cleared.
 */
static int test_crash_alloc_after_mutation(void)
{
	void *slot1 = alloc_in_cxl_slab(64);
	BUG_ON(slot1 == NULL);

	struct page *page = virt_to_page(slot1);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *pre_head = slab->free_list_head;
	u16 pre_cnt = slab->current_free_cnt;

	/* Manually perform alloc mutation */
	struct slab_slot_list *free_slot =
		(struct slab_slot_list *)slab->free_list_head;
	slab->free_list_head = free_slot->next_free;
	slab->current_free_cnt -= 1;

	/* Log not cleared */
	fake_log(slab, SLAB_OP_ALLOC, pre_head, pre_cnt);

	recover_cxl_slabs();

	BUG_ON(slab->free_list_head != pre_head);
	BUG_ON(slab->current_free_cnt != pre_cnt);
	if (verify_slab_consistency(slab, "alloc_after_mutation") != 0)
		return -1;
	BUG_ON(MY_LOG->op != SLAB_OP_NONE);

	free_in_cxl_slab(slot1);
	kinfo("[SLAB_TEST] PASS: crash_alloc_after_mutation\n");
	return 0;
}

/*
 * Test 3: Crash during FREE — after log written, before mutation.
 */
static int test_crash_free_before_mutation(void)
{
	void *slot = alloc_in_cxl_slab(128);
	BUG_ON(slot == NULL);

	struct page *page = virt_to_page(slot);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *saved_head = slab->free_list_head;
	u16 saved_cnt = slab->current_free_cnt;

	fake_log(slab, SLAB_OP_FREE, saved_head, saved_cnt);

	recover_cxl_slabs();

	if (verify_slab_consistency(slab, "free_before_mutation") != 0)
		return -1;
	BUG_ON(MY_LOG->op != SLAB_OP_NONE);

	free_in_cxl_slab(slot);
	kinfo("[SLAB_TEST] PASS: crash_free_before_mutation\n");
	return 0;
}

/*
 * Test 4: Crash during FREE — after mutation, before log cleared.
 */
static int test_crash_free_after_mutation(void)
{
	void *slot = alloc_in_cxl_slab(256);
	BUG_ON(slot == NULL);

	struct page *page = virt_to_page(slot);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *pre_head = slab->free_list_head;
	u16 pre_cnt = slab->current_free_cnt;

	/* Manually perform free mutation */
	struct slab_slot_list *s = (struct slab_slot_list *)slot;
	s->next_free = slab->free_list_head;
	slab->free_list_head = slot;
	slab->current_free_cnt += 1;

	fake_log(slab, SLAB_OP_FREE, pre_head, pre_cnt);

	recover_cxl_slabs();

	BUG_ON(slab->free_list_head != pre_head);
	BUG_ON(slab->current_free_cnt != pre_cnt);
	if (verify_slab_consistency(slab, "free_after_mutation") != 0)
		return -1;
	BUG_ON(MY_LOG->op != SLAB_OP_NONE);

	kinfo("[SLAB_TEST] PASS: crash_free_after_mutation\n");
	return 0;
}

/*
 * Test 5: No crash — recovery should be a no-op.
 */
static int test_no_crash_noop(void)
{
	void *slots[8];
	int i;

	for (i = 0; i < 8; i++) {
		slots[i] = alloc_in_cxl_slab(64);
		BUG_ON(slots[i] == NULL);
	}

	recover_cxl_slabs();

	struct page *page = virt_to_page(slots[0]);
	struct slab_header *slab = (struct slab_header *)page->slab;
	if (verify_slab_consistency(slab, "no_crash_noop") != 0)
		return -1;

	void *post_slot = alloc_in_cxl_slab(64);
	BUG_ON(post_slot == NULL);
	free_in_cxl_slab(post_slot);

	for (i = 0; i < 8; i++)
		free_in_cxl_slab(slots[i]);

	kinfo("[SLAB_TEST] PASS: no_crash_noop\n");
	return 0;
}

/*
 * Test 6: Multiple allocs then crash mid-alloc.
 */
static int test_crash_alloc_multi_slot(void)
{
	void *slots[16];
	int i;

	for (i = 0; i < 16; i++) {
		slots[i] = alloc_in_cxl_slab(64);
		BUG_ON(slots[i] == NULL);
	}

	struct page *page = virt_to_page(slots[0]);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *pre_head = slab->free_list_head;
	u16 pre_cnt = slab->current_free_cnt;

	struct slab_slot_list *free_slot =
		(struct slab_slot_list *)slab->free_list_head;
	if (free_slot != NULL) {
		slab->free_list_head = free_slot->next_free;
		slab->current_free_cnt -= 1;
		fake_log(slab, SLAB_OP_ALLOC, pre_head, pre_cnt);

		recover_cxl_slabs();

		BUG_ON(slab->free_list_head != pre_head);
		BUG_ON(slab->current_free_cnt != pre_cnt);
	}

	if (verify_slab_consistency(slab, "alloc_multi_slot") != 0)
		return -1;

	for (i = 0; i < 16; i++)
		free_in_cxl_slab(slots[i]);

	kinfo("[SLAB_TEST] PASS: crash_alloc_multi_slot\n");
	return 0;
}

/*
 * Test 7: Crash with partial mutation (only free_list_head updated).
 */
static int test_crash_alloc_partial_mutation(void)
{
	void *slot = alloc_in_cxl_slab(64);
	BUG_ON(slot == NULL);

	struct page *page = virt_to_page(slot);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *pre_head = slab->free_list_head;
	u16 pre_cnt = slab->current_free_cnt;

	struct slab_slot_list *free_slot =
		(struct slab_slot_list *)slab->free_list_head;
	if (free_slot != NULL) {
		slab->free_list_head = free_slot->next_free;
		/* Don't update free_cnt — partial crash */
		fake_log(slab, SLAB_OP_ALLOC, pre_head, pre_cnt);

		recover_cxl_slabs();

		BUG_ON(slab->free_list_head != pre_head);
		BUG_ON(slab->current_free_cnt != pre_cnt);
	}

	if (verify_slab_consistency(slab, "alloc_partial_mutation") != 0)
		return -1;

	free_in_cxl_slab(slot);
	kinfo("[SLAB_TEST] PASS: crash_alloc_partial_mutation\n");
	return 0;
}

/*
 * Test 8: Multiple orders — crash across all size classes.
 * Note: per-CPU log holds one op, so we test one at a time per order.
 */
static int test_crash_multiple_orders(void)
{
	unsigned long sizes[] = {32, 64, 128, 256, 512, 1024, 2048};
	int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
	void *slots[7];
	int i;

	for (i = 0; i < num_sizes; i++) {
		slots[i] = alloc_in_cxl_slab(sizes[i]);
		BUG_ON(slots[i] == NULL);
	}

	/* Test crash+recovery on each order sequentially */
	for (i = 0; i < num_sizes; i++) {
		struct page *page = virt_to_page(slots[i]);
		struct slab_header *slab = (struct slab_header *)page->slab;

		void *pre_head = slab->free_list_head;
		u16 pre_cnt = slab->current_free_cnt;

		struct slab_slot_list *fs =
			(struct slab_slot_list *)slab->free_list_head;
		if (fs != NULL) {
			slab->free_list_head = fs->next_free;
			slab->current_free_cnt -= 1;
			fake_log(slab, SLAB_OP_ALLOC, pre_head, pre_cnt);

			recover_cxl_slabs();

			BUG_ON(slab->free_list_head != pre_head);
			BUG_ON(slab->current_free_cnt != pre_cnt);
		}

		if (verify_slab_consistency(slab, "multiple_orders") != 0)
			return -1;
		BUG_ON(MY_LOG->op != SLAB_OP_NONE);
	}

	for (i = 0; i < num_sizes; i++)
		free_in_cxl_slab(slots[i]);

	kinfo("[SLAB_TEST] PASS: crash_multiple_orders\n");
	return 0;
}

/*
 * Test 9: Post-recovery functionality.
 */
static int test_post_recovery_functional(void)
{
	void *pre_slots[4];
	void *post_slots[8];
	int i;

	for (i = 0; i < 4; i++) {
		pre_slots[i] = alloc_in_cxl_slab(128);
		BUG_ON(pre_slots[i] == NULL);
	}

	struct page *page = virt_to_page(pre_slots[0]);
	struct slab_header *slab = (struct slab_header *)page->slab;

	void *pre_head = slab->free_list_head;
	u16 pre_cnt = slab->current_free_cnt;
	struct slab_slot_list *fs =
		(struct slab_slot_list *)slab->free_list_head;
	if (fs) {
		slab->free_list_head = fs->next_free;
		slab->current_free_cnt -= 1;
		fake_log(slab, SLAB_OP_ALLOC, pre_head, pre_cnt);
	}

	recover_cxl_slabs();

	for (i = 0; i < 8; i++) {
		post_slots[i] = alloc_in_cxl_slab(128);
		BUG_ON(post_slots[i] == NULL);
		*(u64 *)post_slots[i] = 0xDEADBEEF + i;
	}

	for (i = 0; i < 8; i++) {
		BUG_ON(*(u64 *)post_slots[i] != (u64)(0xDEADBEEF + i));
		free_in_cxl_slab(post_slots[i]);
	}

	for (i = 0; i < 4; i++)
		free_in_cxl_slab(pre_slots[i]);

	kinfo("[SLAB_TEST] PASS: post_recovery_functional\n");
	return 0;
}

/* Main entry point */
void tst_slab_recovery(void)
{
	u32 cpu_id = smp_get_cpu_id();
	int failures = 0;

	if (cpu_id != 0)
		return;

	kinfo("[SLAB_TEST] ========================================\n");
	kinfo("[SLAB_TEST] Starting slab crash recovery tests\n");
	kinfo("[SLAB_TEST] ========================================\n");

	failures += (test_crash_alloc_before_mutation() != 0);
	failures += (test_crash_alloc_after_mutation() != 0);
	failures += (test_crash_free_before_mutation() != 0);
	failures += (test_crash_free_after_mutation() != 0);
	failures += (test_no_crash_noop() != 0);
	failures += (test_crash_alloc_multi_slot() != 0);
	failures += (test_crash_alloc_partial_mutation() != 0);
	failures += (test_crash_multiple_orders() != 0);
	failures += (test_post_recovery_functional() != 0);

	kinfo("[SLAB_TEST] ========================================\n");
	if (failures == 0)
		kinfo("[SLAB_TEST] ALL 9 TESTS PASSED\n");
	else
		kinfo("[SLAB_TEST] %d TEST(S) FAILED\n", failures);
	kinfo("[SLAB_TEST] ========================================\n");
}

#endif /* SLAB_CRASH_RECOVERY */
