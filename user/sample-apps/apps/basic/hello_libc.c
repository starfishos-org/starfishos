#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sched.h>
#include <sys/mman.h>

int main(int argc, char *argv[], char *envp[])
{
        char *str = "Hello, LibC!\n";
        char *buf;
        int size;
        int i;
        int ret = 0;

        if (ret < 0)
                return -1;

        printf("Hello, LibC!\n");
        printf("Hello again, LibC!\n");

        size = strlen(str);
        buf = malloc(size + 1);
        memset(buf, 0, size + 1);

        for (i = 0; i < size; ++i) {
                buf[i] = str[i];
        }

        printf("buf: %p, buf cont: %s\n", buf, buf);

        for (i = 0; i < size; ++i) {
                buf = malloc(0x1000);
                printf("big buf: %p\n", buf);
        }

        for (i = 0; i < size; ++i) {
                buf = malloc(0x16);
                printf("small buf: %p\n", buf);
        }

        buf = mmap(0,
                   0x800,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
        printf("mmap returns: %p\n", buf);

        for (i = 0; i < 0x1000; ++i) {
                buf[i] = i % 256;
        }
        printf("simple mmap test done\n");

        buf = mmap(0,
                   0x1000,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);

        buf[0] = 'T';
        ret = mprotect(buf, 0x1000, PROT_READ);
        assert(ret == 0);
        printf("read after mprotect: %c\n", buf[0]);

#if 0
	/* Test mprotect: trigger error here */
	buf[0] = 'X';
	printf("write after mprotect: %c\n", buf[0]);
#endif

        ret = munmap(buf, 0x1000);
        assert(ret == 0);
#if 0
	/* Test munmap: trigger error here */
	printf("read after mprotect: %c\n", buf[0]);
#endif

        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(3, &mask);
        ret = sched_setaffinity(0, sizeof(mask), &mask);
        printf("set aff return %d\n", ret);

        CPU_ZERO(&mask);
        ret = sched_getaffinity(0, sizeof(mask), &mask);
        printf("get aff return %d\n", ret);

        for (i = 0; i < 128; i++) {
                if (CPU_ISSET(i, &mask)) {
                        printf("aff %d\n", i);
                }
        }

        sched_yield();

        printf("hello libc alive after binding to different core and yield!\n");

        printf("Hello libc finished!\n");

        return 0;
}
