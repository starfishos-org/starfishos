#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <chcore/type.h>
#include <chcore/syscall.h>
#include <chcore/launcher.h>

int main(int argc, char *argv[], char *envp[])
{
	int ret;
	char *pname = "test_cfork_hello.bin\0";
	// char *argv1[] = {pname};

	// create a new process "hello.bin"
	// chcore_new_process(sizeof(argv1) / sizeof(*argv1), argv1, 0);

	// prepare the checkpoint
	ret = usys_cfork_prepare(pname, strlen(pname));
	printf("usys_cfork_prepare ret: %d\n", ret);

	// checkpoint
	ret = usys_cfork_ckpt(pname, strlen(pname));
	printf("usys_cfork_ckpt ret: %d\n", ret);

	return 0;
}
