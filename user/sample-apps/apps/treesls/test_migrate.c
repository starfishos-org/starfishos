#include <chcore/syscall.h>

int main(int argc, char *argv[], char *envp[])
{
    usys_whole_ckpt("test_migrate", 13);
}