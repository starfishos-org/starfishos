#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
        int r;

        if (argc != 2) {
                printf("Usage: export [key=value]\n");
                exit(-1);
        }
        r = putenv(argv[1]);
        if (r) {
                perror("failed to export");
                exit(-1);
        }

        return 0;
}
