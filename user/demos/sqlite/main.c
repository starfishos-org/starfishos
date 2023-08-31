#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <time.h>
#include <sys/time.h>
#include <assert.h>

#include "sqlite3.h"

static int sq_callback(void* NotUsed, int argc, char** argv, char** azColName)
{
	int i;
	for (i = 0; i < argc; i++) {
		fprintf(stderr,
			"%s = %s\n",
			azColName[i],
			argv[i] ? argv[i] : "NULL");
	}
	fprintf(stderr, "\n");
	return 0;
}

#define INSERT_TIMES 10000
#define UPDATE_TIMES 10000
#define DELETE_TIMES 10000
#define QUERY_TIMES 10000

int speed_test_main(int argc, char** argv)
{
	sqlite3* db;
	char* zErrMsg = 0;
	int rc;
	int i;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s DATABASE SQL-STATEMENT\n", argv[0]);
		return (1);
	}
	// fprintf(stderr,
	// 	"%s begin,file:%s command:%s\n",
	// 	__func__,
	// 	argv[1],
	// 	argv[2]);

	rc = sqlite3_open(argv[1], &db);
	if (rc) {
		fprintf(stderr,
			"Can't open database: %s\n",
			sqlite3_errmsg(db));
		sqlite3_close(db);
		return (1);
	}
	// fprintf(stderr, "%s sqlite3_open success\n", __func__);

	// disable fsync
	sqlite3_exec(db, "PRAGMA synchronous = OFF; ", 0, 0, 0);

	rc = sqlite3_exec(db, argv[2], NULL, 0, &zErrMsg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		fprintf(stderr,
			"sqlite3_exec error, extended errcode:%d\n",
			sqlite3_extended_errcode(db));
		return -1;
	}

	struct timeval pro_begin_time, begin_time, end_time;
	long splashed_time;

	//insert test
	const char* sql_insert = "insert into tb11 values('hello%d',%d);\0";
	char insert_real[128];
	gettimeofday(&begin_time, NULL);
	pro_begin_time.tv_sec = begin_time.tv_sec;
	pro_begin_time.tv_usec = begin_time.tv_usec;
	for (i = 0; i < INSERT_TIMES; i++) {
		int ret = snprintf(insert_real, 128, sql_insert, i, i);
		if (ret <= 0) {
			fprintf(stderr,
				"%s, snprintf error:ret:%d\n",
				__func__,
				ret);
			return -1;
		}
		//fprintf(stderr, "%s, insert test:%d %s\n",__func__,i,insert_real);
		rc = sqlite3_exec(db, insert_real, NULL, 0, &zErrMsg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			fprintf(stderr,
				"sqlite3_exec error, extended errcode:%d\n",
				sqlite3_extended_errcode(db));
			return -1;
		}
	}
	gettimeofday(&end_time, NULL);
	splashed_time = 1000000 * (end_time.tv_sec - begin_time.tv_sec)
			+ (end_time.tv_usec - begin_time.tv_usec);
	fprintf(stderr,
		"%s sqlite3 insert_test success:%d inserts:time: %lu us\n",
		__func__,
		INSERT_TIMES,
		splashed_time);

	//update test
	const char* sql_update =
		"update tb11 set two = %d where one = 'hello%d';\0";
	char update_real[128];
	gettimeofday(&begin_time, NULL);
	for (i = 0; i < UPDATE_TIMES; i++) {
		int ret = snprintf(update_real, 128, sql_update, 0xd, i);
		if (ret <= 0) {
			fprintf(stderr,
				"%s, snprintf error:ret:%d\n",
				__func__,
				ret);
			return -1;
		}
		//fprintf(stderr, "%s, update test:%d %s\n",__func__,i,update_real);
		rc = sqlite3_exec(db, update_real, NULL, 0, &zErrMsg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			fprintf(stderr,
				"sqlite3_exec error, extended errcode:%d\n",
				sqlite3_extended_errcode(db));
			return -1;
		}
	}
	gettimeofday(&end_time, NULL);
	splashed_time = 1000000 * (end_time.tv_sec - begin_time.tv_sec)
			+ (end_time.tv_usec - begin_time.tv_usec);
	fprintf(stderr,
		"%s sqlite3 update_test success:%d update:time: %lu us\n",
		__func__,
		UPDATE_TIMES,
		splashed_time);

	//delete test
	const char* sql_delete = "delete from tb11 where one = 'hello%d';\0";
	char delete_real[128];
	gettimeofday(&begin_time, NULL);
	for (i = 0; i < DELETE_TIMES; i++) {
		int ret = snprintf(delete_real, 128, sql_delete, i);
		if (ret <= 0) {
			fprintf(stderr,
				"%s, snprintf error:ret:%d\n",
				__func__,
				ret);
			return -1;
		}
		//fprintf(stderr, "%s, delete test:%d %s\n",__func__,i,delete_real);
		rc = sqlite3_exec(db, delete_real, NULL, 0, &zErrMsg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			fprintf(stderr,
				"sqlite3_exec error, extended errcode:%d\n",
				sqlite3_extended_errcode(db));
			return -1;
		}
	}

	gettimeofday(&end_time, NULL);
	splashed_time = 1000000 * (end_time.tv_sec - begin_time.tv_sec)
			+ (end_time.tv_usec - begin_time.tv_usec);
	fprintf(stderr,
		"%s sqlite3 delete_test success:%d deletes :time: %lu us\n",
		__func__,
		DELETE_TIMES,
		splashed_time);

	//query test
	const char* sql_query = "select * from tb11;\0";
	gettimeofday(&begin_time, NULL);
	for (i = 0; i < QUERY_TIMES; i++) {
		//fprintf(stderr, "%s, delete test:%d %s\n",__func__,i,delete_real);
		rc = sqlite3_exec(db, sql_query, NULL, 0, &zErrMsg);
		//  query_cycles[i] = (end - begin);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			fprintf(stderr,
				"sqlite3_exec error, extended errcode:%d\n",
				sqlite3_extended_errcode(db));
			return -1;
		}
	}
	gettimeofday(&end_time, NULL);
	splashed_time = 1000000 * (end_time.tv_sec - begin_time.tv_sec)
			+ (end_time.tv_usec - begin_time.tv_usec);
	fprintf(stderr,
		"%s sqlite3 query_test success:%d query :time: %lu us\n",
		__func__,
		QUERY_TIMES,
		splashed_time);
	splashed_time = 1000000 * (end_time.tv_sec - pro_begin_time.tv_sec)
			+ (end_time.tv_usec - pro_begin_time.tv_usec);
	fprintf(stderr,
		"%s sqlite3 success total time: %lu us\n",
		__func__,
		splashed_time);
	sqlite3_close(db);
	return 0;
}

#include <sys/mount.h>

char *test_db_file_tmpfs = "/test.db\0";
char *test_db_file_fatfs = "/fat/test.db\0";
char *test_db_file_ext4fs = "/ext4/test.db\0";

static void print_usage(void)
{
	printf("Usage: ./test-sqlite3.bin [fatfs/tmpfs/ext4fs]\n");
}

int main(int argc, char **argv)
{
	char *test_db_file = NULL;

	if (argc != 2) {
		print_usage();
		return -1;
	}

	if (strncmp("fatfs", argv[1], 5) == 0) {
		test_db_file = test_db_file_fatfs;
		mount("sda1", "/fat", 0, 0, 0);
	} else if (strncmp("tmpfs", argv[1], 5) == 0) {
		test_db_file = test_db_file_tmpfs;
	} else if (strncmp("ext4fs", argv[1], 6) == 0) {
		test_db_file = test_db_file_ext4fs;
		mount("sda2", "/ext4", 0, 0, 0);
	}
	else {
		print_usage();
		return -1;
	}

	int sql_argc = 3;
	fprintf(stderr, "sqlite3 main = %lx\n", (unsigned long)main);
	const char* sql_argv1[] = {
		"sqlite3\0",
		test_db_file,
		"create table tb11(one varchar(10), two smallint); insert into tb11 values('hello',10); select * from tb11;\0"};
	// for (;;) {
		speed_test_main(sql_argc, (char**)sql_argv1);
		printf("sqlite_test done\n");

		unlink(test_db_file);
	// }
	return 0;
}
