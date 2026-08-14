/*
 * Checkpoint/restore target that parks every one of its threads inside
 * sys_futex_wait, so the cap group has a non-empty kernel futex table when
 * test_cfork_prepare.bin checkpoints it. That is what makes futex_copy()
 * actually copy entries instead of walking an empty hash table.
 *
 * Every thread blocks in FUTEX_WAIT and none of them is ever woken, so all of
 * them sit in TS_WAITING. stop_all_threads() marks those stopped directly
 * rather than putting them on its waiting list, which keeps this test clear of
 * the unrelated kernel_stack_state BUG that a compute-bound target (pca) hits
 * on the same path.
 *
 * Usage:
 *   test_cfork_futex.bin [nr_waiters] [raw]   (on the source machine)
 *   test_cfork_prepare.bin test_cfork_futex.bin
 *   test_cfork_restore.bin test_cfork_futex.bin   (on the target machine)
 *
 * Without "raw" the waiters park in pthread_cond_wait(); with it they issue
 * FUTEX_WAIT directly, which pins the futex words to known addresses and keeps
 * the test independent of how musl implements condition variables.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define MAX_WAITERS 8

#define FUTEX_WAIT_OP 0
#define FUTEX_PRIVATE_OP 128

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

/* One futex word per waiter, never changed, so FUTEX_WAIT never returns. */
static int futex_words[MAX_WAITERS];
static int use_raw_futex;

static void *waiter(void *arg)
{
	long idx = (long)arg;
	long ret;

	if (use_raw_futex) {
		ret = syscall(SYS_futex, &futex_words[idx],
			      FUTEX_WAIT_OP | FUTEX_PRIVATE_OP, 0, NULL);
		printf("[cfork-futex] raw FUTEX_WAIT returned %ld\n", ret);
		fflush(stdout);
		return NULL;
	}

	pthread_mutex_lock(&mutex);
	/* Nobody ever signals cond, so this parks the thread for good. */
	while (1)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);
	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t threads[MAX_WAITERS];
	int nr_waiters = 3;
	long i;

	if (argc > 1)
		nr_waiters = atoi(argv[1]);
	if (nr_waiters < 1 || nr_waiters > MAX_WAITERS)
		nr_waiters = 3;
	if (argc > 2 && strcmp(argv[2], "raw") == 0)
		use_raw_futex = 1;

	for (i = 0; i < nr_waiters; i++) {
		if (pthread_create(&threads[i], NULL, waiter, (void *)i) != 0) {
			printf("[cfork-futex] pthread_create failed\n");
			return -1;
		}
	}

	/*
	 * Give the waiters time to reach FUTEX_WAIT before announcing that the
	 * process is ready to be checkpointed.
	 */
	sleep(2);
	printf("[cfork-futex] %d %s waiters parked, ready to checkpoint\n",
	       nr_waiters, use_raw_futex ? "raw" : "pthread");
	fflush(stdout);

	/* Park the main thread too: pthread_join blocks in FUTEX_WAIT. */
	pthread_join(threads[0], NULL);
	return 0;
}
