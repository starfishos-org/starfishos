#include <pthread.h>
#include <stdint.h> /* Definition of uint64_t */
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>

#define handle_error(msg)           \
        do {                        \
                perror(msg);        \
                exit(EXIT_FAILURE); \
        } while (0)

int glb_argc;
char **glb_argv;
int efd;

void *thread_routine(void *arg)
{
        int j;
        for (j = 1; j < glb_argc; j++) {
                sleep(1);
                printf("Child writing %s to efd\n", glb_argv[j]);
                uint64_t u = strtoull(glb_argv[j], NULL, 0);
                /* strtoull() allows various bases */
                int s = write(efd, &u, sizeof(uint64_t));
                if (s != sizeof(uint64_t))
                        handle_error("write");
        }
        printf("Child completed write loop\n");

        return 0;
}

int main(int argc, char *argv[])
{
        pthread_t tid;
        uint64_t u;
        ssize_t s;

        glb_argc = argc;
        glb_argv = argv;

        if (argc < 2) {
                fprintf(stderr, "Usage: %s <num>...\n", argv[0]);
                exit(EXIT_FAILURE);
        }

        efd = eventfd(0, 0);
        if (efd == -1)
                handle_error("eventfd");
        pthread_create(&tid, NULL, thread_routine, NULL);

#ifdef READ
        printf("Parent about to read\n");
        s = read(efd, &u, sizeof(uint64_t));
        if (s != sizeof(uint64_t))
                handle_error("read");
        printf("Parent read %llu (0x%llx) from efd\n",
               (unsigned long long)u,
               (unsigned long long)u);
#else
        while (1) {
                struct pollfd item;
                item.fd = efd;
                item.events = POLLIN;
                item.revents = 0;

                int ret = poll(&item, 1, -1);
                if (ret == -1) {
                        printf("error");
                        exit(EXIT_SUCCESS);
                } else if (item.revents & POLLIN) {
                        s = read(efd, &u, sizeof(uint64_t));
                        if (s != sizeof(uint64_t))
                                handle_error("read");
                        printf("Parent read %llu (0x%llx) from efd\n",
                               (unsigned long long)u,
                               (unsigned long long)u);
                } else if (ret == 0) {
                        printf("timeout\n");
                        exit(EXIT_SUCCESS);
                }
        }
#endif
        pthread_join(tid, NULL);
        return 0;
}
