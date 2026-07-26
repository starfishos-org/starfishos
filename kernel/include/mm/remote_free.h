#pragma once

#include <common/types.h>

/*
 * Freeing pages that belong to another machine.
 *
 * With a DRAM-first user placement an anonymous PMO's pages are allocated by
 * whichever machine first faults on them, so a cross-machine process's radix
 * tree holds pages from several machines' private DRAM.  Only the owning
 * machine has a struct page for those, so whoever tears the PMO down hands
 * them back instead of freeing them locally.
 */

/*
 * Free one page by physical address, wherever it lives: directly if we own it
 * (or it is in CXL), queued to the owner otherwise.  Queued pages are not
 * handed over until remote_page_free_flush().
 */
void free_machine_page(paddr_t pa);

#ifdef DSM_ENABLED

/* Boot-time setup; drops entries queued for us before this boot. */
void remote_page_free_init(void);

/* Publish every partially filled batch to its owner. */
void remote_page_free_flush(void);

/* Owner side: reclaim pages other machines queued for us. */
bool drain_remote_page_free(void);

/*
 * Owner side, allocation path: drain occasionally.  The queue head lives in
 * CXL, so this amortizes the read instead of paying it on every page fault.
 */
void remote_page_free_poll(void);

#else /* !DSM_ENABLED */

#define remote_page_free_init()  do {} while (0)
#define remote_page_free_flush() do {} while (0)
#define remote_page_free_poll()  do {} while (0)

#endif /* DSM_ENABLED */
