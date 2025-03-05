#include <chcore/syscall.h>

int main(int argc, char *argv[], char *envp[])
{
    usys_cfork_restore("test_migrate", 13);
}