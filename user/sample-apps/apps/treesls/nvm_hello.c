#include <chcore/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <chcore/type.h>

int main() {
    char *str = malloc(1024);
    memcpy(str,"hello",5);
    printf("%s\n",str);

    usys_whole_ckpt("nvm_hello",10);
    printf("hello world!\n");
    sleep(2);
    
    printf("start do restore.\n");
    usys_whole_restore("nvm_hello",10);
    printf("bye world\n");
    return 0;
}

