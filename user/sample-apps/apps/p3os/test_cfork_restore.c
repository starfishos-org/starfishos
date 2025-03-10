#include <chcore/syscall.h>
#include <string.h>

int main(int argc, char *argv[], char *envp[])
{
    int ret;
	char *pname = "test_cfork_hello.bin\0";

    ret = usys_cfork_restore(pname, strlen(pname));
    printf("usys_cfork_restore ret: %d\n", ret);

    return ret;
}
