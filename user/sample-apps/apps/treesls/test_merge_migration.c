#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <chcore/type.h>
#include <chcore/syscall.h>

int main(int argc, char *argv[], char *envp[])
{
	usys_ckpt_merge_migration();
}