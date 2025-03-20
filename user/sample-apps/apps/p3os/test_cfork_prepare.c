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
		printf("[cfork] usage: %s <program_name>\n", argv[0]);
		return -1;
	}
	char *pname = argv[1];
	printf("[cfork] checkpoint program: %s\n", pname);

	// prepare the checkpoint
	ret = usys_cfork_prepare(pname, strlen(pname));
	if (ret < 0) {
		printf("[cfork] usys_cfork_prepare failed\n");
		return -1;
	}

	// checkpoint
	ret = usys_cfork_ckpt(pname, strlen(pname));
	if (ret < 0) {
		printf("[cfork] usys_cfork_ckpt failed\n");
		return -1;
	}

	return 0;
}
