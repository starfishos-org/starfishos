/* Calculate the Fibonacci sequence */
#include <stdlib.h>
#include <pthread.h>
#include <chcore/syscall.h>
#define BOUND 1000000
#define THRD 4
#include <time.h>

long long nstime(void) {
    struct timespec tv;
    long long nst;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &tv);
    nst = ((long long)tv.tv_sec)*1000000000;
    nst += tv.tv_nsec;
    return nst;
}

void Fibonacci(int bound) {
    unsigned long long t1, t2, t3;
    for (int i = 0; i < bound; ++i)
    {
        t3 = t1 + t2;
        t1 = t2;
        t2 = t3;
    }
}

void *thread(void *useless) {
    unsigned long long start = nstime();
    Fibonacci(BOUND);
    printf("Fib tot take %llu ns\n", nstime() - start);
    return NULL;
}

int main() {
    int i;
    pthread_t t;
    for (i = 1; i < THRD; i++) {
        pthread_create(&t, NULL, thread, NULL);
    }
    thread(NULL);
    return 0;
}
