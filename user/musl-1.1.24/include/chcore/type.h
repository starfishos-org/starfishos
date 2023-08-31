#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

#define ADDR_LOWER32(addr) ((addr)&0xffffffff)
#define ADDR_UPPER32(addr) ADDR_LOWER32((addr) >> 32)

struct process_metadata {
	u64 phdr_addr;
	u64 phentsize;
	u64 phnum;
	u64 flags;
	u64 entry;
	u64 type;
};

typedef u64 vmr_prop_t;

struct user_elf_seg {
	u64 elf_pmo;
	size_t seg_sz;
	u64 p_vaddr;
	vmr_prop_t flags;
};

#define ELF_PATH_LEN 256

/* FIXME(qyc): this should be dynamicaly determined */
#define ELF_MAX_LOAD_SEG 4

struct user_elf {
	struct user_elf_seg user_elf_seg[ELF_MAX_LOAD_SEG];
	char path[ELF_PATH_LEN];
	struct process_metadata elf_meta;
};

/* We only consider ET_EXEC and ET_DYN now */
#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4

#define CHCORE_LOADER "/libc.so"
#define LDD_NAME "/ldd" /* ldd is actually a copy of libc.so but with different name */

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

typedef u64 sysclockid_t;
typedef u64 systime_t;

struct systimespec {
  systime_t tv_sec;
  long tv_nsec;
};

#ifdef __cplusplus
}
#endif
