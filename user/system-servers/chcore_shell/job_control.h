#pragma once
#include <pthread.h>
#include <chcore/ipc.h>

#include <chcore/type.h>

/* Unify the length of uart buffer and foreground process' input buffer. */
#define INPUT_BUFLEN (4096 - sizeof(int) * 2)
#define NR_ARGS_MAX  64
#define JOBS_MAX     64

#define CTRL(ch) (ch & 0x1f)

extern void *foreground_buffer_addr;
extern void *foreground_pmo_addr;

struct job {
	pid_t pid;
	int notific_cap;
	int pmo_cap;
	char job_name[64];
};

extern struct job *bg_jobs[JOBS_MAX];
extern struct job fg_job;
extern int job_count;
extern pthread_mutex_t job_mutex;

int check_job_state(int pid);
void clean_jobs(void);
int add_job(char *job_name, int pid, int pmo_cap, int notific_cap);
void del_job(long index);
