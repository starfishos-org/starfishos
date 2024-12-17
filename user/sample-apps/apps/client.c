#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <assert.h>

#include <chcore/type.h>
#include <chcore/defs.h>
#include <chcore/syscall.h>
#include <chcore/ipc.h>
#include <chcore/proc.h>
#include <chcore/launcher.h>
#include <chcore/pmu.h>

#define IPC_NUM 100
#define SHM_KEY 0xABC

int years = 70;

int main(int argc, char *argv[], char *envp[])
{
        int ret = 0;
        int new_thread_cap;
        int shared_page_pmo_cap, shared_msg;
        ipc_struct_t *client_ipc_struct;
        ipc_msg_t *ipc_msg;
        int i;
        s64 start_cycle = 0, end_cycle = 0;

        /* For testing shmxxx */
        int shmid;
        void *shmaddr;

        char *args[1];
        struct new_process_caps new_process_caps;

        shmid = shmget(SHM_KEY, 0x1000, IPC_CREAT | IPC_EXCL);
        printf("[client] shmget returns shmid %d\n", shmid);
        if (shmid < 0) {
                printf("EXISTING SHM_KEY\n");
                return -1;
        } else {
                shmaddr = shmat(shmid, 0, 0);
                printf("[client] shmat returns addr %p\n", shmaddr);
                *(u64 *)shmaddr = 0xaabbcc;
        }

        printf("[clinet] Hello World from user space: %d years!\n", years);

        /* create a new process */
        printf("[clinet] create the server process.\n");

        args[0] = "/server.bin";
        create_process(1, args, &new_process_caps);
        new_thread_cap = new_process_caps.mt_cap;

        printf("[clinet] The main thread cap of the newly created process is %d\n",
               new_thread_cap);

        /* register IPC client */
        client_ipc_struct = ipc_register_client(new_thread_cap);
        if (client_ipc_struct == NULL) {
                printf("[clinet] ipc_register_client failed\n");
                usys_exit(-1);
        }

/* IPC send cap */
#define PAGE_SIZE 0x1000
        shared_page_pmo_cap = usys_create_pmo(PAGE_SIZE, PMO_DATA, MALLOC_TYPE_DEFAULT);
        shared_msg = 0xbeefbeef;
        usys_write_pmo(shared_page_pmo_cap, 0, &shared_msg, sizeof(shared_msg));

        ipc_msg = ipc_create_msg(client_ipc_struct, 0, 1);
        ipc_set_msg_cap(ipc_msg, 0, shared_page_pmo_cap);
        ipc_call(client_ipc_struct, ipc_msg);

        if (ipc_msg->cap_slot_number == 1) {
                int ret_cap, read_val;
                ret_cap = ipc_get_msg_cap(ipc_msg, 0);
                printf("[clinet] cap return %d\n", ret_cap);
                usys_read_pmo(ret_cap, 0, &read_val, sizeof(read_val));
                printf("[clinet] read pmo from server, val:%x\n", read_val);
        }

        ipc_destroy_msg(ipc_msg);

        /* IPC send data */
        ipc_msg = ipc_create_msg(client_ipc_struct, 4, 0);
        pmu_clear_cnt();
        start_cycle = pmu_read_real_cycle();
        for (i = 0; i < IPC_NUM; i++) {
                ipc_set_msg_data(ipc_msg, (char *)&i, 0, 4);
                ipc_call(client_ipc_struct, ipc_msg);
        }
        end_cycle = pmu_read_real_cycle();
        printf("[clinet] cycles for %d roundtrips are %ld, average = %ld\n",
               IPC_NUM,
               end_cycle - start_cycle,
               (end_cycle - start_cycle) / IPC_NUM);
        ipc_destroy_msg(ipc_msg);

        printf("[clinet] will test multiple data\n");
        ipc_msg = ipc_create_msg(client_ipc_struct, 4 * 100, 0);
        for (i = 0; i < 100; i++) {
                ipc_set_msg_data(ipc_msg, (char *)&i, i * 4, 4);
        }
        printf("[client] ipc ping to ...\n");
        ipc_call(client_ipc_struct, ipc_msg);
        ipc_destroy_msg(ipc_msg);

        ret = shmdt(shmaddr);
        assert(ret == 0);
        /* Prevent compiler warning/error when release building (assert(x) -> 0)
         */
        unused(ret);

        printf("[User] exit now. Bye!\n");

        return 0;
}
