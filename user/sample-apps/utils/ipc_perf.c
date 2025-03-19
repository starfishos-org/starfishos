#include <chcore/launcher.h>
#include <chcore/perf.h>
#include <chcore/proc.h>
#include <chcore/syscall.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern volatile bool ipc_perf_enabled;
extern volatile u64 ipc_perf_count_p1;
extern volatile u64 ipc_perf_count_p6;
extern u64 ipc_perf_time_p1[IPC_PERF_TIME_SIZE];
extern u64 ipc_perf_time_p6[IPC_PERF_TIME_SIZE];

void ipc_perf_start(void) {
  ipc_perf_enabled = true;
  ipc_perf_count_p1 = 0;
  ipc_perf_count_p6 = 0;
  usys_ipc_perf_start();
}
void ipc_perf_end(void) {
  ipc_perf_enabled = false;
  usys_ipc_perf_end();
  printf("printing p1 count: %lu\n", ipc_perf_count_p1);
  for (int i = 0; i < ipc_perf_count_p1; i++) {
    printf("%lu ", ipc_perf_time_p1[i]);
  }
  printf("\n");
  printf("printing p6 count: %lu\n", ipc_perf_count_p6);
  for (int i = 0; i < ipc_perf_count_p6; i++) {
    printf("%lu ", ipc_perf_time_p6[i]);
  }
  printf("\n");
}

void workfunc(char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    printf("open %s failed\n", path);
    return;
  }
  char buf[100];
  if (fd < 0) {
    printf("open %s failed\n", path);
    return;
  }
  for (int i = 0; i < 100; i++) {
    sprintf(buf, "%d", i);
    write(fd, buf, 1);
  }
  extern int chcore_ipc_perf(int fd);
  chcore_ipc_perf(fd);
  // close(fd);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <path>\n", argv[0]);
    return 1;
  }

  ipc_perf_start();

  workfunc(argv[1]);

  ipc_perf_end();
  printf("perf finished\n");

  return 0;
}