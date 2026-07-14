#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <path>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];

    // Use "rb" for portable raw binary reads
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "read: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    unsigned char buf[4096];  // unsigned char so any byte is handled
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (fwrite(buf, 1, n, stdout) != n) {
            fprintf(stderr, "write error: %s\n", strerror(errno));
            fclose(fp);
            return 1;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "read error: %s\n", strerror(errno));
    }

    fclose(fp);
    return 0;
}
