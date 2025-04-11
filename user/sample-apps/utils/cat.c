#include <stdio.h>
#include <stdlib.h>

#define BUFFER_SIZE 100

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: cat [filename]\n");
        return EXIT_FAILURE;
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }

    char buf[BUFFER_SIZE];
    while (fgets(buf, BUFFER_SIZE, f) != NULL) {
        fputs(buf, stdout);
    }

    fclose(f);
    return EXIT_SUCCESS;
}
