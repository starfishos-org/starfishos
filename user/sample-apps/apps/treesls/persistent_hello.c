#include <chcore/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <chcore/type.h>

extern int RIGHT, WRONG;

void print_string(const char *str) {
    while (*str)
        usys_putc(*str++);
}

void println() {
    usys_putc('\n');
}

void printx(u64 n) {
    char buf[16];
    int pos = 0, cur;
    while (n) {
        cur = n % 16;
        n /= 16;
        buf[pos] = cur >= 10 ? cur - 10 + 'a' : cur + '0';
        pos++;
    }
    while (pos--)
        usys_putc(buf[pos]);
}

int main()
{
    // printf("right place at %lx, wrong place at %lx, main %lx, putc %lx\n", &RIGHT, &WRONG, main, usys_putc);
    const char * volatile msg = "aaa\n";
    // printf("before ckpt: %d\n", value);
    print_string(msg);
    // printf("before checkpoint test.\n");
    usys_whole_ckpt("persistent_hello",17);
    printf("after checkpoint test.\n");
    msg = "bbb\n";
    print_string(msg);
    msg = "ccc\n";
    print_string(msg);
    asm volatile ("RIGHT:");
    // sleep(5);
    // usys_yield();

    usys_whole_restore("persistent_hello",17);
    // printf("after restore test.\n");
    msg = "ddd\n";
    print_string(msg);
    asm volatile ("WRONG:");
    return 0;
}
