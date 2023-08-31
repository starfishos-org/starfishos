#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define THREAD_NUM	32
#define PLAT_CPU_NUM	4
#define TEST_NUM	20

#include <chcore/syscall.h>

#define t_assert(a) if(!(a)) {printf("assertion failed\n"); exit(-1);}

#define SERVER_PORT 6667
#define SEND_BUF_SIZE (1 * 1024 * 1024 * 4 + 50)
char buf[SEND_BUF_SIZE];

/* time measurement */

double get_time_double(struct systimespec *ts) {
    return ts->tv_sec + ts->tv_nsec / 1000000000.0;
}

double get_duration(struct systimespec *start, struct systimespec *end) {
    return get_time_double(end) - get_time_double(start);
}

/* implement kvstore using in-memory kvs */

#include "llkvs.h"

struct kvs *g_kvs = NULL;

void setup() {
    g_kvs = new_kvs(1000);
}

void reset() {
    if (g_kvs) {
        kvs_destroy(g_kvs);
    }
    setup();
}

/* define kvstore rpc protocol */
enum rpc_type { GET = 0, PUT = 1, DEL = 2, RESET = 3, NRPC = 4 };
enum response_code { OK = 0, NO_KEY = 1, ERROR = 2 };
/*  request
    byte type
    long long key
    [long long len] - type == PUT?
    [char[len] value] - type == PUT?
*/

/*  response
    byte code - 0 for succcessful, 1 for key-not-found, 2 for failed
    [long long len] - type == GET?
    [char[len] value] - type == GET?
*/

int readn(int fd, char *buf, int n) {
    int remain = n, offset = 0;
    static int total = 0;
    while (remain > 0) {
        printf("[dbg] server try read %d\n", remain);
        int ret = read(fd, buf + offset, remain);
        printf("[dbg] server read %d\n", ret);
        if (ret < 0) {
            return offset;
        }
        remain -= ret;
        offset += ret;
        total += ret;
        printf("[dbg] [server read total %d]\n", total);
    }
    return offset;
}

void writen(int fd, void *buf, int n) {
    static int total = 0;
    t_assert(write(fd, buf, n) == n);
    total += n;
    // printf("[dbg] [server write total %d]\n", total);
}

int rpc_get(int fd) {
    char code;
    long long key;
    int n = readn(fd, (char *)&key, sizeof key);
    if (n < sizeof key)
        goto fail;
    // TODO: convert endian
    
    span_t *pvalue;
    pvalue = kvs_get(g_kvs, &key);
    if (!pvalue) {
        code = NO_KEY;
        writen(fd, &code, 1);
        return 0;
    }

    code = OK;

    char *p = buf;

    *p++ = code;
    *(long long *)p = pvalue->len;
    p += sizeof pvalue->len;
    memcpy(p, pvalue->data, pvalue->len);
    p += pvalue->len;

    writen(fd, buf, p - buf);
    return 0;

fail:
    code = ERROR;
    writen(fd, &code, 1);
    return -1;
}

int rpc_put(int fd) {
    char code;
    long long key;
    int n = readn(fd, (char *)&key, sizeof key);
    if (n < sizeof key)
        goto fail;
    // TODO: convert endian

    span_t value;
    n = readn(fd, (char *)&value.len, sizeof value.len);
    if (n < sizeof value.len)
        goto fail;
    // TODO: convert endian

    if (value.len < 0)
        goto fail;
    
    value.data = malloc(value.len);
    if (!value.data)
        goto fail;
    
    n = readn(fd, value.data, value.len);
    if (n < value.len)
        goto release;
    
    // struct systimespec start, end;
    // usys_clock_gettime(0, &start);
    kvs_put(g_kvs, &key, &value);
    // usys_clock_gettime(0, &end);
    // printf("mem server: put duration: %.2lf ms\n", get_duration(&start, &end) * 1000);
    code = OK;
    writen(fd, &code, 1);
    return 0;

release:
    free(value.data);
fail:
    code = ERROR;
    writen(fd, &code, 1);
    return -1;
}

int rpc_del(int fd) {
    char code;
    long long key;
    int n = readn(fd, (char *)&key, sizeof key);
    if (n < sizeof key)
        goto fail;
    // TODO: convert endian

    kvs_del(g_kvs, &key);
    code = OK;
    writen(fd, &code, 1);
    return 0;

fail:
    code = ERROR;
    writen(fd, &code, 1);
    return -1;
}

int rpc_reset(int fd) {
    reset();
    char code = OK;
    writen(fd, &code, 1);
    return 0;
}

/* tcp server */
int (*routines[])(int) = { rpc_get, rpc_put, rpc_del, rpc_reset };

void *serve_client(void *param) {
    /* Set self affinity */
    usys_set_affinity(-1, 2);
    sched_yield();

    /* Start serving */ 
    int fd = (int)(long long)param;
    unsigned char cmd;
    for (;;) {
        printf("[dbg] server[a]: waiting for command...\n");
        int n = readn(fd, (char *)&cmd, 1);
        if (n != 1) {
            printf("[dbg] Server[a]: failed to read cmd(read %d byte)\n", n);
            goto stop;
        }
        printf("[dbg] server[a]: got command: %d\n", (int)cmd);
        if (cmd >= NRPC) {
            printf("[a] Unknown rpc command: %d\n", (int)cmd);
            goto stop;
        }
        printf("[dbg] server[a]: dispatch fd %d\n", fd);
        int ret = routines[cmd](fd);
        printf("[dbg] server[a]: handled command: %d\n", (int)cmd);
        if (ret) {
            printf("[a] Failed when handling client request: %d\n", cmd);
            goto stop;
        }
    }

stop:
    close(fd);
    printf("[dbg] server[a]: Closed connection to client.\n");
    return NULL;
}

void run_server() {
    struct sockaddr_in sa, ac;
    int ret;
    socklen_t len;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    t_assert(server_fd >= 0);

    memset(&sa, 0, sizeof(struct sockaddr));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(SERVER_PORT);
	ret = bind(server_fd, (struct sockaddr *)&sa, sizeof(struct sockaddr_in));
    t_assert(ret >= 0);

    ret = listen(server_fd, 5);
    t_assert(ret >= 0);
    printf("mem-kvs-server: listening...\n");

    do {
		len = sizeof(struct sockaddr_in);
        int accept_fd;
		do {
			accept_fd = accept(server_fd, (struct sockaddr *)&ac, &len);
			if (accept_fd >= 0) /* succ */
				break;
			printf("server[l]: accept socket failed! errno = %d\n", errno);
			sched_yield();
		} while (1);
        struct systimespec ts;
		usys_clock_gettime(0, &ts);
        printf("server accepted {%lu, %lu}\n", ts.tv_sec, ts.tv_nsec);
		pthread_t tid;
        // printf("[dbg] kvs-server[l]: accepted a client.\n");
		pthread_create(&tid, NULL, serve_client,
			       (void *)(long)accept_fd);
	} while (1);

	close(server_fd);
}

int main() {
    reset();
    run_server();
    return 0;
}
