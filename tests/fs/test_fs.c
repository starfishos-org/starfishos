#include <minunit.h>

#include <sys/mman.h>
#include <sys/dir.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../common/test_redef.h"

/* typedef long long off_t; */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#include "../../kernel/lib/cpio.c"
#include "../../kernel/tmpfs/tmpfs.c"
/* #include "../common/test.h" */

#include <fs.h>

void printk(const char *fmt, ...) {}

#if 0
static void print_header(struct cpio_header *header)
{
	kdebug(" --- header ---\n"
	       "  .c_ino = 0x%llx\n"
	       "  .c_mode = 0x%llx\n"
	       "  .c_uid = 0x%llx\n"
	       "  .c_gid = 0x%llx\n"
	       "  .c_nlink = 0x%llx\n"
	       "  .c_mtime = 0x%llx\n"
	       "  .c_filesize = 0x%llx\n"
	       "  .c_devmajor = 0x%llx\n"
	       "  .c_devminor = 0x%llx\n"
	       "  .c_rdevmajor = 0x%llx\n"
	       "  .c_rdevminor = 0x%llx\n"
	       "  .c_namesize = 0x%llx\n"
	       "  .c_check = 0x%llx\n",
	       header->c_ino,
	       header->c_mode,
	       header->c_uid,
	       header->c_gid,
	       header->c_nlink,
	       header->c_mtime,
	       header->c_filesize,
	       header->c_devmajor,
	       header->c_devminor,
	       header->c_rdevmajor,
	       header->c_rdevminor,
	       header->c_namesize,
	       header->c_check);
}
#endif

MU_TEST(test_filecreat)
{
	struct stat statbuf;
	int err = fs_creat("/file", 0666);
	mu_assert_int_eq(0, err);
	/* err = fs_stat("/file", &statbuf); */
	/* mu_check(err == 0); */
	// mu_check(statbuf.*
}

#define FILE_LEN (PAGE_SIZE * 20)
char buf[2][FILE_LEN];
MU_TEST(test_filewriteread)
{
	int ret = fs_write("/file", 0, buf[0], FILE_LEN);
	mu_assert_int_eq(FILE_LEN, ret);
	ret = fs_read("/file", 0, buf[1], FILE_LEN);
	mu_assert_int_eq(FILE_LEN, ret);
	mu_check(memcmp(buf[0], buf[1], FILE_LEN) == 0);
}

MU_TEST(test_readhalf)
{
	int ret = fs_write("/file", 0, buf[0], FILE_LEN);
	mu_assert_int_eq(FILE_LEN, ret);
	ret = fs_read("/file", 0, buf[1], FILE_LEN);
	mu_assert_int_eq(FILE_LEN, ret);
	mu_check(memcmp(buf[0], buf[1], FILE_LEN) == 0);
}

MU_TEST(test_fileremoval)
{
	int err = fs_unlink("/file");
	mu_assert_int_eq(0, err);
}

MU_TEST(test_dircreat)
{
	struct stat statbuf;
	int err = fs_mkdirat(AT_FDCWD, "/dir", 0666);
	mu_assert_int_eq(0, err);
	/* err = fs_stat("/dir", &statbuf); */
	/* mu_check(err == 0); */
	// mu_check(statbuf.*
}

MU_TEST(test_dirremoval)
{
	int err = fs_rmdir("/dir");
	mu_assert_int_eq(0, err);
}

MU_TEST(test_deepfile)
{
	int err = fs_mkdirat(AT_FDCWD, "/dir", 0666);
	mu_assert_int_eq(0, err);
	err = fs_mkdirat(AT_FDCWD, "/dir/dir2", 0666);
	mu_assert_int_eq(0, err);
	err = fs_creat("/dir/dir2/file", 0666);
	mu_assert_int_eq(0, err);

	err = fs_rmdir("/dir/dir2");
	mu_assert_int_eq(-ENOTEMPTY, err);
	err = fs_unlink("/dir/dir2/file");
	mu_assert_int_eq(0, err);

	err = fs_rmdir("/dir");
	mu_assert_int_eq(-ENOTEMPTY, err);
	err = fs_rmdir("/dir/dir2");
	mu_assert_int_eq(0, err);
	err = fs_rmdir("/dir");
	mu_assert_int_eq(0, err);
}

#define TMPFS_IMAGE_PATH "../newc.cpio"
MU_TEST(test_tfs_load_image)
{
	void *addr;
	int fd;
	struct stat stat;

	fd = open(TMPFS_IMAGE_PATH, O_RDONLY);
	mu_check(fd >= 0);
	mu_assert_int_eq(0, fstat(fd, &stat));

	addr = mmap(NULL, stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	mu_check(addr != MAP_FAILED);

	int err = tfs_load_image(addr);
	mu_assert_int_eq(0, err);
	munmap(addr, stat.st_size);
	close(fd);
}

MU_TEST(test_scan)
{
	int count = 4096;
	char *buf = malloc(count);
	char *str = malloc(256);
	int start;
	int ret;
	void *vp;
	struct dirent *p;
	int i;
	start = 0;
	do {
		ret = fs_scan("/tar", start, buf, count);
		vp = buf;
		start += ret;
		printf("ret=%d\n", ret);
		for (i = 0; i < ret; i++) {
			p = vp;
			strcpy(str, p->d_name);
			printf("%s\n", str);
			vp += p->d_reclen;
		}
	} while(ret != 0);
	start = 0;
	do {
		ret = fs_scan("/tar/test", start, buf, count);
		vp = buf;
		start += ret;
		printf("ret=%d\n", ret);
		for (i = 0; i < ret; i++) {
			p = vp;
			strcpy(str, p->d_name);
			printf("%s\n", str);
			vp += p->d_reclen;
		}
	} while(ret != 0);

}

MU_TEST_SUITE(test_suite)
{
	init_fs();
	MU_RUN_TEST(test_tfs_load_image);
	MU_RUN_TEST(test_filecreat);
	MU_RUN_TEST(test_filewriteread);
	MU_RUN_TEST(test_readhalf);
	MU_RUN_TEST(test_fileremoval);
	MU_RUN_TEST(test_dircreat);
	MU_RUN_TEST(test_dirremoval);
	MU_RUN_TEST(test_deepfile);
	MU_RUN_TEST(test_scan);
}

int main(int argc, char *argv[])
{
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return minunit_status;
}
