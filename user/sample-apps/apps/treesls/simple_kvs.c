#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SERVER_PORT 6666

/* implement kvstore using FS directory */

int freadn(int fd, char *buf, int n) {
    int remain = n, offset = 0;
    static int total = 0;
    while (remain > 0) {
        int ret = read(fd, buf + offset, remain);
        if (ret < 0) {
            return offset;
        }
        remain -= ret;
        offset += ret;
        total += ret;
    }
    return offset;
}

void pname(char *buf, long long key) {
    sprintf(buf, "./store/%llx", key);
}

void put(long long key, void *value, int len) {
    char filename[50];
    pname(filename, key);
    // printf("[debug] put() fn %s\n", filename);
    int file = open(filename, O_CREAT | O_TRUNC | O_WRONLY);
    // printf("[debug] file %p\n", file);
    assert(file >= 0);
    write(file, value, len);
    close(file);
}

int get(long long key, void **result) {
    char filename[30];
    pname(filename, key);
    // printf("get: try to open %s\n", filename);
    int file = open(filename, O_RDONLY);
    if (file < 0)
        return -1;
    struct stat st;
    fstat(file, &st);
    long len = st.st_size;
    // printf("got size %ld\n", len);
    *result = (char *)malloc(len);
    int ret = freadn(file, *result, len);
    close(file);
    return ret;
}

void del(long long key) {
    char filename[30];
    pname(filename, key);
    remove(filename);
}

void setup() {
    // system("mkdir ./store");
    mkdir("./store", 0777);
}

void reset() {
    // system("rm -r ./store");
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
        // printf("[dbg] server try read %d\n", remain);
        int ret = read(fd, buf + offset, remain);
        // printf("[dbg] server read %d\n", ret);
        if (ret < 0) {
            return offset;
        }
        remain -= ret;
        offset += ret;
        total += ret;
        // printf("[dbg] [server read total %d]\n", total);
    }
    return offset;
}

void writen(int fd, void *buf, int n) {
    static int total = 0;
    assert(write(fd, buf, n) == n);
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
    
    char *value;
    int ret = get(key, (void **)&value);
    if (ret == -1) {
        code = NO_KEY;
        writen(fd, &code, 1);
        return 0;
    }

    code = OK;
    long long len = ret;

    char buf[50];
    char *p = buf;

    *p++ = code;
    *(long long *)p = len;
    p += sizeof len;
    memcpy(p, value, len);
    p += len;

    writen(fd, buf, p - buf);

    // writen(fd, &code, 1);
    // writen(fd, &len, sizeof len);
    // writen(fd, value, len);
    free(value);
    return 0;

fail:
    code = ERROR;
    writen(fd, &code, 1);
    return -1;
}

int rpc_put(int fd) {
    char code;
    long long key;
    // printf("server: reading from fd %d\n", fd);
    int n = readn(fd, (char *)&key, sizeof key);
    if (n < sizeof key)
        goto fail;
    // TODO: convert endian
    // printf("server: read key\n");

    long long len;
    n = readn(fd, (char *)&len, sizeof len);
    if (n < sizeof len)
        goto fail;
    // TODO: convert endian
    // printf("server: read value\n");

    if (len < 0)
        goto fail;
    
    char *data = (char *)malloc(len);
    if (!data)
        goto fail;
    
    n = readn(fd, data, len);
    if (n < len)
        goto release;
    // printf("server: read data\n");
    
    // printf("[debug] about to put key %d of value len %d, buffer is %p\n", key, len, data);
    put(key, data, len);
    // printf("[debug] put done.\n");
    free(data);
    code = OK;
    writen(fd, &code, 1);
    return 0;

release:
    free(data);
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

    del(key);
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
    int fd = (int)(long long)param;
    unsigned char cmd;
    for (;;) {
        // printf("[dbg] server[a]: waiting for command...\n");
        int n = readn(fd, (char *)&cmd, 1);
        if (n != 1) {
            // printf("[dbg] Server[a]: failed to read cmd(read %d byte)\n", n);
            goto stop;
        }
        // printf("[dbg] server[a]: got command: %d\n", (int)cmd);
        if (cmd >= NRPC) {
            printf("[a] Unknown rpc command: %d\n", (int)cmd);
            goto stop;
        }
        // printf("[dbg] server[a]: dispatch fd %d\n", fd);
        int ret = routines[cmd](fd);
        // printf("[dbg] server[a]: handled command: %d\n", (int)cmd);
        if (ret) {
            printf("[a] Failed when handling client request: %d\n", cmd);
            goto stop;
        }
    }

stop:
    close(fd);
    // printf("[dbg] server[a]: Closed connection to client.\n");
    return NULL;
}

void run_server() {
    struct sockaddr_in sa, ac;
    int ret;
    socklen_t len;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server_fd >= 0);

    memset(&sa, 0, sizeof(struct sockaddr));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(SERVER_PORT);
	ret = bind(server_fd, (struct sockaddr *)&sa, sizeof(struct sockaddr_in));
    assert(ret >= 0);

    ret = listen(server_fd, 5);
    assert(ret >= 0);
    printf("kvs-server: listening...\n");

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
