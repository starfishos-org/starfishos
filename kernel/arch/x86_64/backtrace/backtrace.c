/*
 * Copyright (c) 2023 Institute of Parallel And Distributed Systems (IPADS), Shanghai Jiao Tong University (SJTU)
 * Licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <common/debug.h>
#include <common/kprint.h>
#include <common/types.h>
#include <object/thread.h>
#include <mm/uaccess.h>

#if ENABLE_BACKTRACE_FUNC == ON
static bool in_backtrace = false;
#endif

int backtrace(void)
{
#if ENABLE_BACKTRACE_FUNC == ON
	u64 rbp, rip;
	struct vmregion *vmr;

	/* already in backtrace, double fault */
	if (in_backtrace == true) {
		for (;;);
	}
	
	in_backtrace = true;

	/*
	 * backtrace on x86_64 currently supports programs compiled with
	 * -fno-omit-frame-pointer and in user space
	 */
	kinfo("user backtrace:\n");
	kinfo("\tIP 0x%lx\n", current_thread->thread_ctx->ec.reg[RIP]);

	rbp = current_thread->thread_ctx->ec.reg[RBP];
	// disable_smap();
	while (1) {
		/*
		 * Why find_vmr_for_va instead of copy_from_user:
		 * Copy_from_user handles invalid access.
		 * But the function may be called with vmspace_lock
		 * held. The page fault triggered by copy_from_user
		 * may cause deadlock
		 */
		kinfo("\trbp 0x%lx\n", rbp);
		vmr = find_vmr_for_va(current_thread->vmspace, (vaddr_t)rbp);
		if (!vmr)
			break;
		rip = *(u64 *)(rbp + 8);
		rbp = *(u64 *)rbp;

		kinfo("\trip 0x%lx\n", rip);
	}
	// enable_smap();
	in_backtrace = false;
#endif
	return 0;
}
