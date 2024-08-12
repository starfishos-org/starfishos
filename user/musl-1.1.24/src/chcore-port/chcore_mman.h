#pragma once
#include <chcore/container/hashtable.h>
#include <chcore/type.h>

#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02

#define MAP_ANONYMOUS	0x20
#define MAP_CXL       0x200000

#define PROT_NONE	0
#define PROT_READ	1
#define PROT_WRITE	2
#define PROT_EXEC	4

#define PROT_CHECK_MASK (~(PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC))

#define HASH_TABLE_SIZE 509
#define VA_TO_KEY(va) ((u32)((vaddr_t)va >> 12))

#define UNMAPSELF_STACK_SIZE	(0x1000UL)

#if __SIZEOF_POINTER__ == 4
#define HEAP_START	(0x60000000UL)
#else
#define HEAP_START	(0x600000000000UL)
#endif

struct pmo_node {
	int cap;
	vaddr_t va;
	size_t pmo_size;
	struct list_head list_node;
	struct hlist_node hash_node;
};

void *chcore_mmap(void *start, size_t length, int prot, int flags, int fd, off_t off);
int chcore_munmap(void *start, size_t length);
vaddr_t chcore_unmapself(void *start, size_t length);