#include <stdio.h>
#include <minunit.h>

const char binary_dtb_start = 'A';
const char binary_dtb_end = 'A';
const char binary_dtb_size = 'A';


#define kdebug(x, ...) printf(x, ##__VA_ARGS__)
#define kinfo(x, ...) printf(x, ##__VA_ARGS__)
#define printk(x, ...) printf(x, ##__VA_ARGS__)
#define kmalloc malloc
#define kfree free

#include "../../kernel/driver/dtb.c"
#include "../../kernel/driver/devtree.c"



#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>


MU_TEST(test_dtb_parser)
{
	struct devtree *dt;
	struct stat st;
	void *code;
	int fd;
	int i;

	fd = open("./kirin970-hikey970.dtb", O_RDONLY);
	mu_check(fd > 0);

	fstat(fd, &st);
#ifdef __APPLE__
	code = mmap(NULL, st.st_size, MAP_SHARED,
		    PROT_READ | PROT_EXEC, fd, 0);
#else
	code = mmap(NULL, st.st_size, PROT_READ | PROT_EXEC,
		    MAP_SHARED, fd, 0);
#endif
	mu_check(code != MAP_FAILED);

	dt = dtb_parse(code);
	mu_check(!IS_ERR(dt));

	kinfo("dtb loaded\n");

	kdebug(" dtb: reserved memory regions:\n");
	for (i = 0; i < dt->nr_rsvmaps; ++i) {
		kdebug(" [0x%llx ~ 0x%llx]\n", dt->rsvmaps[i].address,
		       dt->rsvmaps[i].address + dt->rsvmaps[i].size - 1);
	}

	kprint_dt_node(dt->root, 0);
}

MU_TEST_SUITE(test_suite)
{
	MU_RUN_TEST(test_dtb_parser);
}

int main(int argc, char *argv[])
{
        MU_RUN_SUITE(test_suite);
        MU_REPORT();
        return minunit_status;
}
