#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <chcore/type.h>
#include <chcore/syscall.h>

#define eq(a, b) (!strcmp(a, b))
inline long to_nano(struct systimespec *spec) {
	return spec->tv_sec * 1e9l + spec->tv_nsec;
}
inline long get_duration(struct systimespec *s1, struct systimespec *s2) {
	return to_nano(s2) - to_nano(s1);
}
#define DECLTMR struct systimespec spec1, spec2;
#define start() usys_clock_gettime(0, &spec1)
#define stop() (usys_clock_gettime(0, &spec2), get_duration(&spec1, &spec2))


void usage() {
    printf("Usage: \
ustress.bin cpu {rounds}\
ustress.bin mem {2^mb} {rounds} r|w rand|seq\n");
}

volatile double d = 0;
volatile long l = 0;

int cpu(int argc, char *argv[]) {
    if (argc < 1) {
        usage();
        return 1;
    }
    long rounds = atol(argv[0]);
    double sum = 0;
    DECLTMR;
    start();
    for (long i = 0; i < rounds; i++) {
        long x = rand();
        sum += sqrt(x);
    }
    long ns = stop();
    printf("<ustress> time %.2lf ms\n", ns / 1024.0 / 1024);
    d = sum;
    return 0;
}

int mem(int argc, char *argv[]) {
    if (argc < 4) {
        usage();
        return 1;
    }
    int order_mb = atoi(argv[0]);
    int mb = 1 << order_mb;
    long rounds = atol(argv[1]);
    char *r = argv[2];
    char *m = argv[3];
    long bytes = 1024l * 1024 * mb;
    const long words = bytes / 8;
    long word_mask = words - 1;
    long *mem = (long *)malloc(bytes);
    char rw = r[0];
    if (rw != 'r' && rw != 'w') {
        usage();
        return 1;
    }
    int mode = 0;
    if (eq(m, "rand")) {
        mode = 1;
    } else if (eq(m, "seq")) {
        mode = 2;
    }
    if (mode == 0) {
        usage();
        return 1;
    }

    long sum = 0;
    DECLTMR;
    start();
    for (long r = 0; r < rounds; r++) {
        if (rw == 'r') {
            if (mode == 1) {
                for (long i = 0; i < words; i++)
                    sum ^= mem[rand() & word_mask];
            } else {
                for (long i = 0; i < words; i++)
                    sum ^= mem[i];
            }
        } else {
            if (mode == 1) {
                for (long i = 0; i < words; i++)
                    mem[rand() & word_mask] = i;
            } else {
                for (long i = 0; i < words; i++)
                    mem[i] = i;
            }
        }
    }
    long ns = stop();
    free(mem);
    l = sum;
    printf("<ustress> time %.2lf ms\n", ns / 1024.0 / 1024);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }
    srand((unsigned)time(NULL));
    char *cmd = argv[1];
    if (eq(cmd, "cpu")) {
        return cpu(argc - 2, argv + 2);
    }
    else if (eq(cmd, "mem")) {
        return mem(argc - 2, argv + 2);
    }
    else {
        usage();
        return 1;
    }
    return 0;
}
