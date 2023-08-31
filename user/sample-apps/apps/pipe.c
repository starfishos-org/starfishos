#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <chcore/bug.h>

int main()
{
        int pipefd[2];
        char msg[] = "simple message";
        char read_buf[32];
        int r;

        r = pipe(pipefd);
        if (r != 0) {
                printf("open pipe failed r=%d\n", r);
                exit(-1);
        }
        printf("open pipe read:%d write:%d\n", pipefd[0], pipefd[1]);

        /* TODO: coupled with fork */
        r = write(pipefd[1], msg, strlen(msg));
        BUG_ON(r != strlen(msg));
        r = read(pipefd[0], read_buf, sizeof(read_buf));
        BUG_ON(r != strlen(msg));
        printf("%s\n", read_buf);

        return 0;
}
