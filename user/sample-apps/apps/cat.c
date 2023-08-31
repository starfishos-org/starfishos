#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
        FILE *f;
        char buf[100];
        if (argc != 2) {
                printf("Usage: cat [filename]\n");
                exit(-1);
        }
        f = fopen(argv[1], "r");
        if (f == NULL) {
                perror("fopen err:\n");
                exit(-1);
        }
        memset(buf, 0, 100);
        while (fgets((char *)buf, 100, f) != NULL) {
                printf("%s", buf);
                memset(buf, 0, 100);
        }
	fclose(f);
        return 0;
}
