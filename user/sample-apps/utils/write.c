#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <chcore/type.h>
#pragma GCC push_options
#pragma GCC optimize ("O0")
int main(int argc, char *argv[])
{
        if (argc != 3) {
                printf("Usage: write <path> <content>\n");
                exit(-1);
        }
        char *path = argv[1];
        char *content = argv[2];
        (void)path;
        (void)content;

        fprintf(stderr, "write path: %s\n", path);

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
                perror("failed to open file");
                exit(-1);
        }
        write(fd, content, strlen(content));
        close(fd);
        return 0;
}
#pragma GCC pop_options
