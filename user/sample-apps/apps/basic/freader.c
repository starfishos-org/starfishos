#include <stdio.h>
#include <unistd.h>
#include <sys/file.h> 

int main(int argc, char *argv[]) {
    char *filename = argv[1];
    if (argc != 2) {
        printf("Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    char buf[256];

    while (1) {
        FILE *fp = fopen(filename, "r");
        if (!fp) { perror("fopen"); continue; }

        while (fgets(buf, sizeof(buf), fp)) {
            printf("Reader got: %s", buf);
        }
        fclose(fp);
        sleep(1);
    }
}
