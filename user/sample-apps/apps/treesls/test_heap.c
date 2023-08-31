#include <chcore/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <chcore/type.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <chcore/launcher.h>

int main(int argc, char *argv[]) 
{
    char* arr;

    for(int i = 0; i < 5; i++) {
        for(int j = 0; j < 100; j++) {
            printf("malloc 1\n");
            arr = (char*)malloc(0x1000);
            *arr = 1;
            printf("arr=%p\n", arr);
        }
        usys_whole_ckpt(0,0);
    }
    
    for(int i = 5; i < 10;i++) {
        for(int j = 0; j < 100;j++) {
            printf("malloc 2\n");
            arr = (char*)malloc(0x1000);
            *arr = 1;
            printf("arr=%p\n", arr);
        }
    }


    // (void*)arr;
    printf("test done %p\n", arr);
    usys_wait(usys_create_notifc(), 1, NULL);

    return 0;
}