#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chcore/type.h>
#include <chcore/syscall.h>

#define ARRAY_SIZE (1024 * 1024 * 32) // 1 GB
#define ITERATIONS 1000000

uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void clflush(volatile void *p) {
    asm volatile("clflush (%0)" :: "r"(p));
}

#define UNUSED(x) ((void)x)

uint64_t measure_latency(volatile uint8_t *array, size_t size) {
    uint64_t start, end;
    volatile uint8_t value = 0;
    (void)value;

    for (size_t i = 0; i < size; i += 64) {
        clflush(&array[i]);
    }

    start = get_time_ns();
    for (size_t i = 0; i < size; i += 64) {
        value = array[i];
    }
    end = get_time_ns();

    return (end - start) / (size / 64);
}

uint64_t measure_bandwidth(volatile uint8_t *array, size_t size) {
    uint64_t start, end;
    volatile uint8_t value = 0;
    (void)value;

    for (size_t i = 0; i < size; i += 64) {
        clflush(&array[i]);
    }

    start = get_time_ns();
    for (size_t i = 0; i < size; i += 64) {
        value = array[i];
    }
    end = get_time_ns();

    return (size / ((end - start) / 1e9)) / (1024 * 1024);
}

void work() {
    volatile uint8_t *array = (volatile uint8_t *)malloc(ARRAY_SIZE);
    if (array == MAP_FAILED) {
        printf("mmap");
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        array[i] = (uint8_t)i;
    }

    uint64_t latency = measure_latency(array, ARRAY_SIZE);
    printf("Average memory latency: %lu ns\n", latency);

    uint64_t bandwidth = measure_bandwidth(array, ARRAY_SIZE);
    printf("Memory bandwidth: %lu MB/second\n", bandwidth);

    free((void *)array);
}

int main(int argc, char *argv[], char *envp[])
{
	int cnt = 0;
	
	while (1) {
		fprintf(stderr, "[%d] hello world\n", cnt);
        cnt++;
		work();
        // sleep(1);
		if (cnt == 10) {
			break;
		}
	}
    fprintf(stderr, "exit\n");
}
