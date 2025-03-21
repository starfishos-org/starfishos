#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chcore/defs.h>
#include <chcore/type.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <directory>\n", argv[0]);
        return 1;
    }

    char *dir = argv[1];
    int ret = mkdir(dir, 0755);
    if (ret < 0) {
        printf("mkdir failed: %s\n", dir);
    }

    return 0;
}
