#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
        if (argc != 3) {
                fprintf(stderr, "Usage: write <path> <content>\n");
                return 1;
        }
        
        const char *path = argv[1];
        const char *content = argv[2];
        size_t content_len = strlen(content);
        
        FILE *f = fopen(path, "w");
        if (f == NULL) {
                perror(path);
                return 1;
        }
        
        fwrite(content, 1, content_len, f);
        if (ferror(f)) {
                perror(path);
                fclose(f);
                return 1;
        }
        
        fclose(f);
        return 0;
}
