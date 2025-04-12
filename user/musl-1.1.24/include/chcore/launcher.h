#pragma once

#include <chcore/elf.h>

#ifdef __cplusplus
extern "C" {
#endif

struct new_process_caps {
	/* The cap of the main thread of the newly created process */
	int mt_cap;
};

int readelf_from_vaddr(struct user_elf *user_elf, size_t length, void *start, bool is_cross_machine);
int readelf_from_fs(const char *pathbuf, struct user_elf *user_elf, bool is_cross_machine);
int chcore_new_process(int argc, char *__argv[], int is_bbapplet, int is_cross_machine);

int create_process(int argc, char *__argv[], struct new_process_caps *caps, bool is_cross_machine);

#ifdef __cplusplus
}
#endif
