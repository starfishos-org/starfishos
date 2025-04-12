#include <chcore/syscall.h>
#include <chcore/launcher.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

char *redis_server[] = {"redis-server.bin", "--save", "\"\""};
char *redis_client[] = {"redis-test.bin", "-type", "1", "-n", "10000"};
char *sqlite_test[] = {"test-sqlite3.bin", "tmpfs"};
char *fib[] = {"fib.bin"};

#define launch(args) chcore_new_process(sizeof(args) / sizeof(args[0]), args, false, false)
#define eq(s1, s2) (!strcmp(s1, s2))

int main(int argc, char *argv[]) {
    // int ckpt_count = atoi(argv[1]);
    int app_count = atoi(argv[2]);
    int break_down = 0, run_s = 0, run_r = 0, run_f = 0;
    for (int i = 2; i < argc; i++) {
        char *arg = argv[i];
        if (eq(arg, "-b")) {
            break_down = 1;
        }
        else if (eq(arg, "-s")) {
            run_s = 1;
        }
        else if (eq(arg, "-r")) {
            run_r = 1;
        }
        else if (eq(arg, "-f")) {
            run_f = 1;
        }
    }
    launch(redis_server);
    sleep(1);
    if (!break_down || run_s)
        launch(sqlite_test);
    for (int i = 0; i < app_count; i++) {
        if (!break_down || run_r)
            launch(redis_client);
        if (!break_down || run_f)
            launch(fib);
    }
    sleep(5);
    char *ckpt[] = {"checkpoint.bin", "-i", "%10", "-t", argv[1], "-v", "-a", "6", "-l", "3", "-s"};
    launch(ckpt);
    return 0;
}
