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
	// char *pname = "test_cfork_hello.bin\0";
	if (argc != 2) {
		printf("[ckpt_process] usage: %s <program_name>\n", argv[0]);
		return -1;
	}
	char *pname = argv[1];
	printf("[ckpt_process] checkpoint program: %s\n", pname);

	// checkpoint
	ret = usys_ckpt_process(pname, strlen(pname));
	if (ret < 0) {
		printf("[ckpt_process] usys_ckpt_process failed\n");
		return -1;
	}

	return 0;
}
