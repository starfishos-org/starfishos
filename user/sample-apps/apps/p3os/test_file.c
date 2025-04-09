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
        // if (argc != 3) {
        //         printf("Usage: write <path> <content>\n");
        //         exit(-1);
        // }
        char *path = "/test_write.txt";
        char *content = "hello world";
        int cnt = 0;

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
                perror("failed to open file");
                exit(-1);
        }

        printf("open file success\n");

        while (1) {
                write(fd, content, strlen(content));
                printf("[%d] write success %d\n", getpid(), cnt++);
                sleep(1);
        }
        close(fd);
        fprintf(stderr, "write success\n");
        return 0;
}
#pragma GCC pop_options
