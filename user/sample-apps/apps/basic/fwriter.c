#include <stdio.h>
#include <unistd.h>
#include <sys/file.h>

int main(int argc, char *argv[]) {
    FILE *fp;
    int counter = 0;
    char *filename = argv[1];
    if (argc != 2) {
        printf("Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    while (1) {
        fp = fopen(filename, "w");
        if (!fp) { perror("fopen"); return 1; }

        fprintf(fp, "Message %d\n", counter++);
        fflush(fp); // flush to kernel buffer
        fsync(fileno(fp));  // flush to disk

        fclose(fp);

        sleep(1); // update once per second
    }
}
