#include <stdio.h>
#include <stdlib.h>
#include <chcore/syscall.h>

int main(int argc, char *argv[]) {
	int flag;

	flag = 1;
    if (argc > 1) {
        flag = atoi(argv[1]);
    }
	usys_shutdown(flag);

	return 0;
}