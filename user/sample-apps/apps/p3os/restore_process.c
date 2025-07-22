#include <chcore/syscall.h>
#include <string.h>

int main(int argc, char *argv[], char *envp[])
{
    int ret;
    if (argc != 2) {
        printf("[restore_process] usage: %s <program_name>\n", argv[0]);
        return -1;
    }
	char *pname = argv[1];

    ret = usys_restore_process(pname, strlen(pname));
    if (ret < 0) {
        printf("[restore_process] usys_restore_process failed\n");
        return -1;
    }

    return ret;
}
