#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <chcore/type.h>
#include <chcore/syscall.h>

int main(int argc, char *argv[], char *envp[])
{
	int cnt = 0;
	
	while (1) {
		printf("[%d] hello world\n", cnt);
		cnt++;
		sleep(1);
	}
}
