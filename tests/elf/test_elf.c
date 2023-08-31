#include <stdio.h>
#include <minunit.h>

#define kdebug(x, ...) printf(x, ##__VA_ARGS__)
#define kinfo(x, ...) printf(x, ##__VA_ARGS__)

#define kmalloc malloc
#define kfree free

#include "../../kernel/lib/elf.c"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>


MU_TEST(test_elf_parser)
{
	struct stat st;
	void *code;
	int fd;

	fd = open("./hello.out", O_RDONLY);
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

	struct elf_file *elf = elf_parse_file(code);
	mu_check(!IS_ERR(elf));
#if 1
	if (IS_ERR(elf)) {
		printf("Error! %ld\n", (long)elf);
	} else {
		printf("elf_file <- %p\n", elf);
		kprint_elf(elf);
	}
#endif
	if (!IS_ERR(elf))
		elf_free(elf);
}

MU_TEST_SUITE(test_suite)
{
	MU_RUN_TEST(test_elf_parser);
}

int main(int argc, char *argv[])
{
        MU_RUN_SUITE(test_suite);
        MU_REPORT();
        return minunit_status;
}
