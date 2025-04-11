#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
        if (argc != 3) {
                fprintf(stderr, "Usage: write <path> <content>\n");
                return 1;
        }
        
        const char *path = argv[1];
        const char *content = argv[2];
        size_t content_len = strlen(content);
        
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
                char err_buf[256];
                strerror_r(errno, err_buf, sizeof(err_buf));
                fprintf(stderr, "failed to open file '%s': %s\n", path, err_buf);
                return 1;
        }
        
        ssize_t bytes_written = write(fd, content, content_len);
        if (bytes_written < 0 || (size_t)bytes_written != content_len) {
                char err_buf[256];
                strerror_r(errno, err_buf, sizeof(err_buf));
                fprintf(stderr, "failed to write: %s\n", err_buf);
                close(fd);
                return 1;
        }
        
        close(fd);
        return 0;
}
