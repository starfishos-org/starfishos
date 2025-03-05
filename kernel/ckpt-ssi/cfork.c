#include <object/thread.h>
#include <ckpt/ckpt_data.h>
#include <common/kvstore.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <object/cap_group.h>
#include <object/user_fault.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/kmalloc.h>
#include <ckpt/ckpt.h>
#include <sched/context.h>
#include <perf/measure.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/hybird_mem.h>
#include <ckpt/external_sync.h>

#include "ckpt_ws.h"
#include "ckpt_object_pool.h"
#include "ckpt_objects.h"
#include "log.h"

void flush_tlb_all(void);

int sys_cfork_prepare(u64 pname_ptr, u64 pname_len)
{
    char *pname;

    pname = (char *)kmalloc(pname_len, __PRIVATE__);
    copy_from_user(pname, (void *)pname_ptr, pname_len);
    CFORK_LOG_INFO("cfork_prepare: pname: %s, pname_len: %d", pname, pname_len);

    kfree(pname);
    return 0;
}

int sys_cfork_ckpt(u64 pname_ptr, u64 pname_len)
{
    char *pname;

    pname = (char *)kmalloc(pname_len, __PRIVATE__);
    copy_from_user(pname, (void *)pname_ptr, pname_len);
    CFORK_LOG_INFO("cfork_ckpt: pname: %s, pname_len: %d", pname, pname_len);

    kfree(pname);

    return 0;
}

int sys_cfork_restore(u64 pname_ptr, u64 pname_len)
{
    char *pname;

    pname = (char *)kmalloc(pname_len, __PRIVATE__);

    copy_from_user(pname, (void *)pname_ptr, pname_len);
    CFORK_LOG_INFO("cfork_restore: pname: %s, pname_len: %d", pname, pname_len);

    kfree(pname);

    return 0;
}
