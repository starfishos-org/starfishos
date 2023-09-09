#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PAGE_SIZE 4096

int main() {
    pid_t child_pid;
    int num = PAGE_SIZE / sizeof(int);
    int stack_array1[PAGE_SIZE / sizeof(int)], stack_array2[PAGE_SIZE / sizeof(int)];
    int *heap_array1, *heap_array2;
    heap_array1 = (int*)malloc(PAGE_SIZE);
    heap_array2 = (int*)malloc(PAGE_SIZE);

    for (int i = 0;i < num; i++) {
        stack_array1[i] = i; // 0-1023
        stack_array2[i] = num + i; // 1024-2047
        heap_array1[i] = num * 2 + i; // 2047-3071
        heap_array2[i] = num * 3 + i; // 3071-4095
    }

    child_pid = fork();

    if (child_pid == -1) {
        perror("my_fork");
        exit(EXIT_FAILURE);
    } else if (child_pid == 0) {
        // child
        int pass = 1;
        printf("This is the child process. PID: %d\n", getpid());
        for (int i = 0;i < num; i++) {
            stack_array1[i] = -i;
            heap_array1[i] = -(num * 2 + i); 
        }

        for (int i = 0; i < num; i++) {
            if (stack_array1[i] != -i || stack_array2[i] != num + i || 
                heap_array1[i] != -(num * 2 + i) || heap_array2[i] != (num * 3 + i)) {
                printf("fork error: COW failed\n");
                pass = 0;
            }
        }
        if(pass) 
            printf("child: fork pass\n");

        free(heap_array1);
        free(heap_array2);
        exit(EXIT_SUCCESS);
    } else {
        // parent
        int pass = 1;
        printf("This is the parent process. Child PID: %d\n", child_pid);
        for (int i = 0;i < num; i++) {
            stack_array2[i] = -(num + i);
            heap_array2[i] = -(num * 3 + i); 
        }

        for (int i = 0; i < num; i++) {
            if (stack_array1[i] != i || stack_array2[i] != -(num + i) || 
                heap_array1[i] != (num * 2 + i) || heap_array2[i] != -(num * 3 + i)) {
                printf("fork error: COW failed\n");
                pass = 0;
            }
        }
        if(pass) 
            printf("parent: fork pass\n");
        printf("parent sleep\n");
        int status;
        waitpid(child_pid, &status, 0);
        if (WIFEXITED(status)) {
            printf("Child process exited with status %d\n", WEXITSTATUS(status));
        }
    }

    free(heap_array1);
    free(heap_array2);
    return 0;
}
