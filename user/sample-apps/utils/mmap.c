#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <chcore/type.h>

int main(int argc, char *argv[]) {
    char *path = argv[1];
    int fd = open(path, O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "open %s failed\n", path);
        return 1;
    }
    fprintf(stderr, "open %s success fd=%d\n", path, fd);
    char data[16];
    void *addr = mmap(NULL, 1024, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "mmap %s failed\n", path);
        return 1;
    }
    fprintf(stderr, "mmap success\n");
    memcpy(data, addr, 16);
    fprintf(stderr, "data: %s\n", data);

    close(fd);
    
    return 0;
}