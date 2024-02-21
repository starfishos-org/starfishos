#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFLEN 100

int main(int argc, char *argv[])
{
        FILE *f;
        FILE *f2;
        char buf[BUFLEN];
        char buf2[BUFLEN];
        int ret;

        if (argc != 3) {
                printf("Usage: diff [filename] [filename]\n");
                exit(-1);
        }
        f = fopen(argv[1], "r");
        if (f == NULL) {
                printf("%s:", argv[1]);
                perror("No such file or directory.\n");
                exit(-1);
        }
        f2 = fopen(argv[2], "r");
        if (f2 == NULL) {
                printf("%s:", argv[2]);
                perror("No such file or directory.\n");
                exit(-1);
        }
        memset(buf, 0, 100);
        memset(buf2, 0, 100);
        while (fgets((char *)buf, BUFLEN, f) != NULL) {
                if (fgets((char *)buf2, BUFLEN, f2) == NULL) {
                        printf("<%s", buf);
                }
                if ((ret = memcmp((void *)buf, (void *)buf2, BUFLEN)) != 0) {
                        printf("ret = %d\n", ret);
                        printf("<%s>%s", buf, buf2);
                }
                memset(buf, 0, 100);
                memset(buf2, 0, 100);
        }
        while (fgets((char *)buf2, BUFLEN, f2) != NULL) {
                printf(">%s", buf2);
        }
        return 0;
}
