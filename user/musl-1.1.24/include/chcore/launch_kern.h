#pragma once

#include <chcore/proc.h>
#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

// #define KERNEL_CPIO_BIN 0x5000000
extern void *kernel_cpio_bin;

int single_file_handler(const void *start, size_t size, void *data);
int readelf_from_kernel_cpio(const char *filename, struct user_elf *user_elf);
int run_cmd_from_kernel_cpio(const char *filename, int *new_thread_cap,
			     struct pmo_map_request *pmo_map_reqs,
			     int nr_pmo_map_reqs, u64 badge);

#ifdef __cplusplus
}
#endif
