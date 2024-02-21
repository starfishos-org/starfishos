#include <stdio.h>
#include <sys/mount.h>

int main(int argc, char *argv[])
{
        if (argc < 3) {
                printf("example: mount.bin sda1 /home\n");
                return 1;
        }

        const char *dev = argv[1];
        const char *mountpoint = argv[2];

        printf("mount \"%s\" to \"%s\"\n", dev, mountpoint);
        int ret = mount(dev, mountpoint, NULL, 0, NULL);
        if (ret == 0) {
                printf("succeeded\n");
        } else {
                printf("failed\n");
        }
        return 0;
}
