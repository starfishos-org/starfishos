#include <stdio.h>
#include <stdlib.h>
#include <hiredis.h>
#include <string.h>

int main(int argc, const char **argv)
{
    char *hostname = "127.0.0.1";
    int port = 6379;
    char *password = "";
    redisContext *conn;
    redisReply *reply;
    struct timeval timeout = {1, 500000};
    int save_type = 0;
    int lastarg, i;
    int MB_num = 1;
    int type = 0;
    int presave_MB_size = 0; 
    for (i = 1; i < argc; i++) {
        lastarg = (i == (argc-1));
        if (!strcmp(argv[i],"-type")) {
            if (lastarg) goto invalid;
            type = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-save")) {
            if (lastarg) goto invalid;
            save_type = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-n")) {
            if (lastarg) goto invalid;
            MB_num = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-a")) {
            if (lastarg) goto invalid;
            usys_set_affinity(-1, atoi(argv[++i]));
        } else if (!strcmp(argv[i],"-help")) {
            goto usage;
        } else if (!strcmp(argv[i],"-presave")) {
            if (lastarg) goto invalid;
            presave_MB_size = atoi(argv[++i]);
        } else {
            goto invalid;
        }
    }

    if (save_type == 2) {
        usys_whole_ckpt("checkpoint_time", 16);
    }

    conn = redisConnect(hostname, port);
    // conn erro
    if (conn == NULL || conn->err) {
        if (conn) {
            printf("connection error %s\n", conn->errstr);
            exit(1);
        } else {
            printf("cannot alloc redis context\n");
            exit(1);
        }
    }
    unsigned long long free_mem_start, free_mem_end;
    free_mem_start = usys_get_free_mem_size();
    // pre-save some kv
    char presave_key[10];
    char presave_value[4096];
    for(int i = 0;i < 1024 * presave_MB_size / 4; i++) {
        sprintf(presave_key,"pre_%d",i);
        memset(presave_value, i % 0xFF,4096);
        reply = redisCommand(conn, "SET %s %s", presave_key, presave_value);
        freeReplyObject(reply);
    }
    if(presave_MB_size > 0) {
        usys_whole_ckpt("checkpoint_time", 16);
    }
    free_mem_end = usys_get_free_mem_size();

    printf("pre-save %d MB data, occupy %f MB memory\n",presave_MB_size, (free_mem_start - free_mem_end) / (1024.0 * 1024));

    // set test
    switch (type) {
        case 0: {
            char key[10];
            char value[4096];
            for(int i = 0; i < 1024 * MB_num; i++) {
                sprintf(key,"%d",i);
                memset(value, i % 0xFF,4096);
                reply = redisCommand(conn, "SET %s %s", key, value);
                freeReplyObject(reply);
            }
            break;
        }
        case 1: {
            char key[10];
            for (;;) {
                reply = redisCommand(conn, "GET %s", key);
                freeReplyObject(reply);
            }
        }
    }

    if (save_type == 1) {
        printf("redis-test call save_type\n");
        reply = redisCommand(conn, "BGSAVE");
    } else if (save_type == 2) {
        usys_whole_ckpt_for_test("checkpoint_time", 16, 2);
    }

    redisFree(conn);
    printf("redis-test exit\n");
    return 0;
invalid:
    printf("Invalid option \"%s\" or option argument missing\n",argv[i]);
usage:
	printf("Usage: redis-benchmark [-save <1: save>] [-n <n MB>] [-t <type>] [-a <affinity>] [-help]\n"
	       " -n <n MB>          set n MB data\n"
           " -save <1: save>     call save_type or not\n"
	       " -type <type>          0: defalut, set test; 1: get\n");
    
	return 0;
}