#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <assert.h>

#include <chcore/syscall.h>
#include <chcore/defs.h>
#include <chcore/ipc.h>

int years = 70;

void server_trampoline(ipc_msg_t *ipc_msg, u64 client_badge)
{
        int ret = 0;
        int ret_cap, write_val;
        bool ret_with_cap = false;

        printf("[server] handle IPC with client_badge 0x%lx\n", client_badge);

        if (ipc_msg->cap_slot_number == 1) {
                int cap;
                int shared_msg;

                cap = ipc_get_msg_cap(ipc_msg, 0);
                usys_read_pmo(cap, 0, &shared_msg, sizeof(shared_msg));

                /* read from shared memory should be MAGIC_NUM */
                printf("[server] read %x\n", shared_msg);

                /* return a pmo cap */
                ret_cap = usys_create_pmo(0x1000, PMO_DATA, MALLOC_TYPE_DEFAULT);
                if (ret_cap < 0) {
                        printf("[server] usys_create_pmo ret %d\n", ret_cap);
                        usys_exit(ret_cap);
                }
                ipc_msg->cap_slot_number = 1;
                ipc_set_msg_cap(ipc_msg, 0, ret_cap);
                ret_with_cap = true;

                write_val = 0x233;
                usys_write_pmo(ret_cap, 0, &write_val, sizeof(write_val));

                ret = 0;
        }

        if (ipc_msg->data_len == 4) {
                ret = ((int *)ipc_get_msg_data(ipc_msg))[0] + 100;
                //		printf("server ipc ret = %d\n", ret);
        } else {
                ret = 0;
                if (ipc_msg->data_len > 4) {
                        int i = 0;
                        int len = ipc_msg->data_len;
                        while ((i * 4) < len) {
                                ret += ((int *)ipc_get_msg_data(ipc_msg))[i];
                                i++;
                        }
                }
        }

        if (ret_with_cap)
                ipc_return_with_cap(ipc_msg, ret);
        else
                ipc_return(ipc_msg, ret);
}

#define SHM_KEY 0xABC

int main(int argc, char *argv[])
{
        int shmid;
        void *shmaddr;

        shmid = shmget(SHM_KEY, 0x1000, IPC_CREAT);
        shmaddr = shmat(shmid, 0, 0);
        printf("[server] shmat returns addr %p\n", shmaddr);
        printf("[server] read shm: get 0x%lx (should be 0xaabbcc)\n",
               *(u64 *)shmaddr);
        assert(*(u64 *)shmaddr == 0xaabbcc);

        printf("[server] Happy birthday %d years! I am server.\n", years);
        printf("[server] register server value = %u\n",
               ipc_register_server(server_trampoline,
                                   DEFAULT_CLIENT_REGISTER_HANDLER));

        usys_exit(0);
        return 0;
}
