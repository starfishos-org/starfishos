#include "polling_req.h"
#include "polling_config.h"

#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static struct polling_shm_region *connect_service(int shm_id)
{
    void *addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    int ret = usys_mmap_shm(shm_id, addr);

    if (ret < 0) {
        printf("[IPC_ABORT_TEST] connect to shm %d failed: %d\n",
               shm_id, ret);
        return NULL;
    }
    return (struct polling_shm_region *)addr;
}

static int issue_request(struct polling_shm_region *shm,
                         enum polling_request_type type)
{
    struct polling_request req = { .type = type };
    struct dq_node *node = dq_alloc_node(shm);
    int ret;

    dq_enqueue(shm, node, &req);
    ret = dq_wait_for_done_or_crash(node);
    atomic_store_explicit(&node->status, DQ_CONSUMED,
                          memory_order_release);
    return ret;
}

int main(int argc, char **argv)
{
    int shm_id = 1;
    int ret;
    struct polling_shm_region *shm;

    if (argc == 2)
        shm_id = atoi(argv[1]);
    else if (argc != 1) {
        printf("Usage: polling_reconnect_test.bin [service_machine_id]\n");
        return 2;
    }

    shm = connect_service(shm_id);
    if (!shm)
        return 1;

    printf("[IPC_ABORT_TEST] client connected to service machine %d\n",
           shm_id);
    ret = issue_request(shm, POLLING_ABORT_TEST_BLOCK);
    if (ret != -ECONNABORTED) {
        printf("[IPC_ABORT_TEST] expected -ECONNABORTED, got %d\n", ret);
        return 1;
    }
    printf("[IPC_ABORT_TEST] app received -ECONNABORTED\n");

    /*
     * Do not transparently replay the ambiguous request.  Reconnect to the
     * replacement service first, then let the application issue a new,
     * explicitly chosen request.
     */
    shm = connect_service(shm_id);
    if (!shm)
        return 1;
    ret = issue_request(shm, POLLING_REQ_EMPTY);
    if (ret != 0) {
        printf("[IPC_ABORT_TEST] request after reconnect failed: %d\n", ret);
        return 1;
    }

    printf("[IPC_ABORT_TEST] PASS: app reconnected and service replied\n");
    return 0;
}
