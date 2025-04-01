#include <chcore/cpio.h>
#include <chcore/defs.h>
#include <chcore/launch_kern.h>
#include <chcore/launcher.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void *kernel_cpio_bin = 0;

// This function is implemented in `liblauncher.c`, but `launcher.h` does not
// contain the declaration, maybe for concerns on exposing interfaces to uesr.
int parse_elf_from_binary(const char *binary, struct user_elf *user_elf);

int single_file_handler(const void *start, size_t size, void *data)
{
	struct user_elf *user_elf;
	int ret;

	user_elf = (struct user_elf *)data;
	ret = parse_elf_from_binary(start, user_elf);
	assert(ret == ET_EXEC);

	return ret;
}

int readelf_from_kernel_cpio(const char *filename, struct user_elf *user_elf)
{
	int ret;

	ret = cpio_extract_single(kernel_cpio_bin, filename,
				  single_file_handler, user_elf);

	return ret;
}

const char *fsm_srv = "/fsm.srv";
const char *tmpfs_srv = "/tmpfs.srv";

#include <chcore/uapi/thread.h>

int run_cmd_from_kernel_cpio(const char *filename, int *new_thread_cap,
			     struct pmo_map_request *pmo_map_reqs,
			     int nr_pmo_map_reqs, u64 badge)
{
	struct user_elf user_elf;
	int ret;
	size_t len;
	char *argv[1];

	struct launch_process_args lp_args;

	if (strncmp(filename, fsm_srv, strlen(fsm_srv))
	    && strncmp(filename, tmpfs_srv, strlen(tmpfs_srv))) {
		printf("%s: only booting fs-related server through kernel.\n",
		       __func__);
		return -1;
	}

	ret = readelf_from_kernel_cpio(filename, &user_elf);
	if (ret < 0) {
		printf("[Shell] No such binary in kernel cpio\n");
		return ret;
	}

	/*
	 * Check the string length.
	 * Do not forget the one more byte for the ending '\0'.
	 */
	if (strlen(filename) + strlen(KERNEL_SERVER) >= ELF_PATH_LEN) {
		printf("%s failed due to the long filename (support at most %ld bytes).\n",
		       __func__, ELF_PATH_LEN - strlen(KERNEL_SERVER) - 1);
		return -1;
	}

	/* Launching a system server: add a prefix (as token) to the pathname */
	snprintf(user_elf.path, ELF_PATH_LEN, "%s/%s", KERNEL_SERVER, filename);
	argv[0] = user_elf.path;


	lp_args.user_elf = &user_elf;
	lp_args.child_process_cap = NULL;
	lp_args.child_main_thread_cap = new_thread_cap;
	lp_args.pmo_map_reqs = pmo_map_reqs;
	lp_args.nr_pmo_map_reqs = nr_pmo_map_reqs;
	lp_args.caps = NULL;
	lp_args.nr_caps = 0;
	lp_args.cpuid = 0;
	lp_args.argc = 1;
	lp_args.argv = argv;
	lp_args.badge = badge;
	/* Note: kservice has no PID */
	lp_args.pid = 0;
	lp_args.type = THREAD_TYPE_SERVICES;

	/* Determine which pcid will be assigned to current launching process */
	if (!strcmp(filename, fsm_srv))
		lp_args.pcid = FSM_PCID;
	else
		lp_args.pcid = TMPFS_PCID;

	return launch_process_with_pmos_caps(&lp_args);
}
