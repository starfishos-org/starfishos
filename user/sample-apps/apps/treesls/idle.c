#include <chcore/syscall.h>
#include <pthread.h>

#define PLAT_CPU_NUM		16

void *idle_thread(void *param) {
    int cpu = (long)param;
    // printf("user idle on core %d: started\n", cpu);
    usys_set_affinity(-1, cpu);
    for (;;) {
        // printf("user idle on core %d: yield\n", cpu);
        usys_yield();
    }
}

int main() {
    pthread_t tid;
    int i;
    for (i = 1; i < PLAT_CPU_NUM; i++) {
        pthread_create(&tid, NULL, idle_thread, (void *)(long)i);
        // printf("tid: %p\n", tid);
    }
    idle_thread(0);
    return 0;
}
