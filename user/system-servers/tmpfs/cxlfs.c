#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chcore/memory.h>
#include <chcore/syscall.h>

#include "cxlfs.h"
#include "defs.h"
#include "util.h"

#define CXLFS_MAGIC          0x43584c4653313031ULL /* CXLFS101 */
#define CXLFS_VERSION        3
#define CXLFS_BLOCK_SIZE     PAGE_SIZE
#define CXLFS_TOTAL_BLOCKS   (CXLFS_SHM_SIZE / CXLFS_BLOCK_SIZE)
#define CXLFS_NINODES        32768U
#define CXLFS_INODE_SIZE     128U
#define CXLFS_SB0_BLOCK      0U
#define CXLFS_SB1_BLOCK      1U
#define CXLFS_IBMAP_BLOCK    2U
#define CXLFS_BBMAP_BLOCK    3U
#define CXLFS_BBMAP_BLOCKS   8U
#define CXLFS_ITABLE_BLOCK   (CXLFS_BBMAP_BLOCK + CXLFS_BBMAP_BLOCKS)
#define CXLFS_ITABLE_BLOCKS  ((CXLFS_NINODES * CXLFS_INODE_SIZE) / PAGE_SIZE)
#define CXLFS_DATA_BLOCK     (CXLFS_ITABLE_BLOCK + CXLFS_ITABLE_BLOCKS)
#define CXLFS_ROOT_INO       1U
#define CXLFS_DIRECT         10U
#define CXLFS_PTRS_PER_BLOCK (PAGE_SIZE / sizeof(uint64_t))
#define CXLFS_DIRENT_USED    1U

struct cxlfs_super {
	uint64_t magic;
	uint32_t version;
	uint32_t block_size;
	uint64_t total_blocks;
	uint64_t generation;
	uint64_t root_ino;
	uint64_t inode_bitmap_block;
	uint64_t block_bitmap_block;
	uint64_t inode_table_block;
	uint64_t data_block;
	uint32_t inode_count;
	uint32_t flags;
	uint64_t checksum;
} __attribute__((packed));

struct cxlfs_inode_disk {
	uint32_t ino;
	uint32_t type;
	uint32_t mode;
	uint32_t nlinks;
	uint64_t size;
	uint64_t generation;
	uint64_t direct[CXLFS_DIRECT];
	uint64_t indirect;
	uint64_t double_indirect;
} __attribute__((packed));

struct cxlfs_dirent_disk {
	uint64_t ino;
	uint32_t type;
	uint16_t name_len;
	uint16_t flags;
	char name[256];
} __attribute__((packed));

_Static_assert(sizeof(struct cxlfs_inode_disk) == CXLFS_INODE_SIZE,
	       "CXL inode must be exactly 128 bytes");
_Static_assert(sizeof(struct cxlfs_dirent_disk) == 272,
	       "CXL directory entry layout changed");

static unsigned char *fs_base;
static struct cxlfs_super sb;
static int sb_slot;
static int fs_owner = -1;
static int mounted;
static int restoring;
/* Volatile next-fit cursor. It is rebuilt on mount and never persisted. */
static uint64_t next_block_hint = CXLFS_DATA_BLOCK;

static inline void *block_ptr(uint64_t block)
{
	return fs_base + block * CXLFS_BLOCK_SIZE;
}

static void persist_range(const void *addr, size_t len)
{
	uintptr_t p = (uintptr_t)addr & ~63UL;
	uintptr_t end = ((uintptr_t)addr + len + 63UL) & ~63UL;
	for (; p < end; p += 64)
		__asm__ volatile("clwb (%0)" :: "r"((void *)p) : "memory");
	__asm__ volatile("sfence" ::: "memory");
}

static uint64_t super_checksum(const struct cxlfs_super *s)
{
	const unsigned char *p = (const unsigned char *)s;
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < offsetof(struct cxlfs_super, checksum); ++i) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int valid_super(const struct cxlfs_super *s)
{
	return s->magic == CXLFS_MAGIC && s->version == CXLFS_VERSION &&
	       s->block_size == CXLFS_BLOCK_SIZE &&
	       s->total_blocks == CXLFS_TOTAL_BLOCKS &&
	       s->root_ino == CXLFS_ROOT_INO &&
	       s->checksum == super_checksum(s);
}

static struct cxlfs_inode_disk *disk_inode(uint64_t ino)
{
	if (ino == 0 || ino >= CXLFS_NINODES)
		return NULL;
	return (struct cxlfs_inode_disk *)(block_ptr(CXLFS_ITABLE_BLOCK) +
					 (ino * CXLFS_INODE_SIZE));
}

static unsigned char *inode_bitmap(void)
{
	return block_ptr(CXLFS_IBMAP_BLOCK);
}

static unsigned char *block_bitmap(void)
{
	return block_ptr(CXLFS_BBMAP_BLOCK);
}

static int bitmap_test(unsigned char *map, uint64_t bit)
{
	return !!(map[bit >> 3] & (1U << (bit & 7)));
}

static void bitmap_set(unsigned char *map, uint64_t bit, int value)
{
	unsigned char *byte = &map[bit >> 3];
	if (value)
		*byte |= (1U << (bit & 7));
	else
		*byte &= ~(1U << (bit & 7));
	persist_range(byte, 1);
}

static uint64_t alloc_block(void)
{
	unsigned char *map = block_bitmap();
	uint64_t data_blocks = CXLFS_TOTAL_BLOCKS - CXLFS_DATA_BLOCK;
	for (uint64_t scanned = 0; scanned < data_blocks; ++scanned) {
		uint64_t b = next_block_hint + scanned;
		if (b >= CXLFS_TOTAL_BLOCKS)
			b = CXLFS_DATA_BLOCK + (b - CXLFS_TOTAL_BLOCKS);
		if (!bitmap_test(map, b)) {
			bitmap_set(map, b, 1);
			memset(block_ptr(b), 0, PAGE_SIZE);
			persist_range(block_ptr(b), PAGE_SIZE);
			next_block_hint = b + 1;
			if (next_block_hint >= CXLFS_TOTAL_BLOCKS)
				next_block_hint = CXLFS_DATA_BLOCK;
			return b;
		}
	}
	return 0;
}

static void free_block(uint64_t block)
{
	if (block >= CXLFS_DATA_BLOCK && block < CXLFS_TOTAL_BLOCKS)
		bitmap_set(block_bitmap(), block, 0);
}

static uint64_t alloc_inode(void)
{
	unsigned char *map = inode_bitmap();
	for (uint64_t ino = CXLFS_ROOT_INO + 1; ino < CXLFS_NINODES; ++ino) {
		if (!bitmap_test(map, ino)) {
			bitmap_set(map, ino, 1);
			return ino;
		}
	}
	return 0;
}

static uint64_t inode_block(const struct cxlfs_inode_disk *di, uint64_t lbn)
{
	if (lbn < CXLFS_DIRECT)
		return di->direct[lbn];
	lbn -= CXLFS_DIRECT;
	if (lbn < CXLFS_PTRS_PER_BLOCK) {
		uint64_t *p = di->indirect ? block_ptr(di->indirect) : NULL;
		return p ? p[lbn] : 0;
	}
	lbn -= CXLFS_PTRS_PER_BLOCK;
	if (lbn >= CXLFS_PTRS_PER_BLOCK * CXLFS_PTRS_PER_BLOCK ||
	    !di->double_indirect)
		return 0;
	uint64_t *l1 = block_ptr(di->double_indirect);
	uint64_t leaf = l1[lbn / CXLFS_PTRS_PER_BLOCK];
	if (!leaf)
		return 0;
	return ((uint64_t *)block_ptr(leaf))[lbn % CXLFS_PTRS_PER_BLOCK];
}

static int set_inode_block(struct cxlfs_inode_disk *di, uint64_t lbn,
			   uint64_t block)
{
	if (lbn < CXLFS_DIRECT) {
		di->direct[lbn] = block;
		return 0;
	}
	lbn -= CXLFS_DIRECT;
	if (lbn < CXLFS_PTRS_PER_BLOCK) {
		if (!di->indirect) {
			di->indirect = alloc_block();
			if (!di->indirect)
				return -ENOSPC;
		}
		uint64_t *p = block_ptr(di->indirect);
		p[lbn] = block;
		persist_range(&p[lbn], sizeof(p[lbn]));
		return 0;
	}
	lbn -= CXLFS_PTRS_PER_BLOCK;
	if (lbn >= CXLFS_PTRS_PER_BLOCK * CXLFS_PTRS_PER_BLOCK)
		return -EFBIG;
	if (!di->double_indirect) {
		di->double_indirect = alloc_block();
		if (!di->double_indirect)
			return -ENOSPC;
	}
	uint64_t *l1 = block_ptr(di->double_indirect);
	uint64_t i1 = lbn / CXLFS_PTRS_PER_BLOCK;
	if (!l1[i1]) {
		l1[i1] = alloc_block();
		if (!l1[i1])
			return -ENOSPC;
		persist_range(&l1[i1], sizeof(l1[i1]));
	}
	uint64_t *leaf = block_ptr(l1[i1]);
	uint64_t i2 = lbn % CXLFS_PTRS_PER_BLOCK;
	leaf[i2] = block;
	persist_range(&leaf[i2], sizeof(leaf[i2]));
	return 0;
}

static int publish_super(void)
{
	struct cxlfs_super next = sb;
	next.generation++;
	next.checksum = super_checksum(&next);
	int next_slot = !sb_slot;
	void *dst = block_ptr(next_slot ? CXLFS_SB1_BLOCK : CXLFS_SB0_BLOCK);
	memcpy(dst, &next, sizeof(next));
	persist_range(dst, sizeof(next));
	sb = next;
	sb_slot = next_slot;
	return 0;
}

static int format_image(void)
{
	memset(block_ptr(CXLFS_SB0_BLOCK), 0, PAGE_SIZE);
	memset(block_ptr(CXLFS_SB1_BLOCK), 0, PAGE_SIZE);
	memset(block_ptr(CXLFS_IBMAP_BLOCK), 0, PAGE_SIZE);
	memset(block_ptr(CXLFS_BBMAP_BLOCK), 0,
	       CXLFS_BBMAP_BLOCKS * PAGE_SIZE);
	memset(block_ptr(CXLFS_ITABLE_BLOCK), 0,
	       CXLFS_ITABLE_BLOCKS * PAGE_SIZE);

	for (uint64_t b = 0; b < CXLFS_DATA_BLOCK; ++b)
		block_bitmap()[b >> 3] |= 1U << (b & 7);
	inode_bitmap()[CXLFS_ROOT_INO >> 3] |=
		1U << (CXLFS_ROOT_INO & 7);
	persist_range(block_ptr(CXLFS_IBMAP_BLOCK), PAGE_SIZE);
	persist_range(block_ptr(CXLFS_BBMAP_BLOCK),
		      CXLFS_BBMAP_BLOCKS * PAGE_SIZE);

	struct cxlfs_inode_disk root = { 0 };
	root.ino = CXLFS_ROOT_INO;
	root.type = FS_DIR;
	root.mode = 0755;
	root.nlinks = 1;
	root.generation = 1;
	memcpy(disk_inode(CXLFS_ROOT_INO), &root, sizeof(root));
	persist_range(disk_inode(CXLFS_ROOT_INO), sizeof(root));

	memset(&sb, 0, sizeof(sb));
	sb.magic = CXLFS_MAGIC;
	sb.version = CXLFS_VERSION;
	sb.block_size = CXLFS_BLOCK_SIZE;
	sb.total_blocks = CXLFS_TOTAL_BLOCKS;
	sb.generation = 1;
	sb.root_ino = CXLFS_ROOT_INO;
	sb.inode_bitmap_block = CXLFS_IBMAP_BLOCK;
	sb.block_bitmap_block = CXLFS_BBMAP_BLOCK;
	sb.inode_table_block = CXLFS_ITABLE_BLOCK;
	sb.data_block = CXLFS_DATA_BLOCK;
	sb.inode_count = CXLFS_NINODES;
	sb.checksum = super_checksum(&sb);
	memcpy(block_ptr(CXLFS_SB0_BLOCK), &sb, sizeof(sb));
	persist_range(block_ptr(CXLFS_SB0_BLOCK), sizeof(sb));
	sb_slot = 0;
	return 0;
}

int cxlfs_mount(int fs_machine, struct inode *runtime_root)
{
	void *addr = (void *)chcore_alloc_vaddr(CXLFS_SHM_SIZE);
	if (!addr)
		return -ENOMEM;
	int ret = usys_mmap_shm(CXLFS_SHM_ID(fs_machine), addr);
	if (ret < 0)
		return ret;
	fs_base = addr;
	fs_owner = fs_machine;
	next_block_hint = CXLFS_DATA_BLOCK;

	struct cxlfs_super *s0 = block_ptr(CXLFS_SB0_BLOCK);
	struct cxlfs_super *s1 = block_ptr(CXLFS_SB1_BLOCK);
	int v0 = valid_super(s0), v1 = valid_super(s1);
	int fresh = !v0 && !v1;
	if (fresh) {
		format_image();
	} else if (v1 && (!v0 || s1->generation > s0->generation)) {
		sb = *s1;
		sb_slot = 1;
	} else {
		sb = *s0;
		sb_slot = 0;
	}
	mounted = 1;
	runtime_root->disk_ino = CXLFS_ROOT_INO;
	struct cxlfs_inode_disk *root = disk_inode(CXLFS_ROOT_INO);
	runtime_root->mode = root->mode;
	return fresh;
}

int cxlfs_is_mounted(void) { return mounted; }
int cxlfs_is_restoring(void) { return restoring; }
uint64_t cxlfs_generation(void) { return mounted ? sb.generation : 0; }
int cxlfs_machine(void) { return fs_owner; }
uint64_t cxlfs_root_ino(void) { return CXLFS_ROOT_INO; }
int cxlfs_sync(void) { return mounted ? publish_super() : -ENODEV; }

ssize_t cxlfs_read(uint64_t ino, off_t offset, void *buf, size_t size)
{
	struct cxlfs_inode_disk *di = disk_inode(ino);
	if (!di || !bitmap_test(inode_bitmap(), ino))
		return -ENOENT;
	if ((uint64_t)offset >= di->size)
		return 0;
	if (size > di->size - (uint64_t)offset)
		size = di->size - (uint64_t)offset;
	size_t done = 0;
	while (done < size) {
		uint64_t pos = (uint64_t)offset + done;
		uint64_t lbn = pos / PAGE_SIZE;
		size_t in = pos % PAGE_SIZE;
		size_t n = size - done;
		if (n > PAGE_SIZE - in)
			n = PAGE_SIZE - in;
		uint64_t b = inode_block(di, lbn);
		if (b)
			memcpy((char *)buf + done, block_ptr(b) + in, n);
		else
			memset((char *)buf + done, 0, n);
		done += n;
	}
	return done;
}

ssize_t cxlfs_write(uint64_t ino, off_t offset, const void *buf, size_t size)
{
	struct cxlfs_inode_disk *dip = disk_inode(ino);
	if (!dip || !bitmap_test(inode_bitmap(), ino))
		return -ENOENT;
	if (offset < 0)
		return -EINVAL;
	struct cxlfs_inode_disk di = *dip;
	size_t nblocks = size ? (((uint64_t)offset % PAGE_SIZE + size + PAGE_SIZE - 1) /
				  PAGE_SIZE) : 0;
	uint64_t *old_blocks = nblocks ? calloc(nblocks, sizeof(*old_blocks)) : NULL;
	if (nblocks && !old_blocks)
		return -ENOMEM;

	size_t done = 0, old_count = 0;
	while (done < size) {
		uint64_t pos = (uint64_t)offset + done;
		uint64_t lbn = pos / PAGE_SIZE;
		size_t in = pos % PAGE_SIZE;
		size_t n = size - done;
		if (n > PAGE_SIZE - in)
			n = PAGE_SIZE - in;
		uint64_t old = inode_block(&di, lbn);
		uint64_t newb = alloc_block();
		if (!newb)
			break;
		if (old)
			memcpy(block_ptr(newb), block_ptr(old), PAGE_SIZE);
		memcpy(block_ptr(newb) + in, (const char *)buf + done, n);
		persist_range(block_ptr(newb), PAGE_SIZE);
		if (set_inode_block(&di, lbn, newb) < 0) {
			free_block(newb);
			break;
		}
		if (old)
			old_blocks[old_count++] = old;
		done += n;
	}
	if (!done && size) {
		free(old_blocks);
		return -ENOSPC;
	}
	uint64_t end = (uint64_t)offset + done;
	if (end > di.size)
		di.size = end;
	di.generation++;
	memcpy(dip, &di, sizeof(di));
	persist_range(dip, sizeof(di));
	publish_super();
	for (size_t i = 0; i < old_count; ++i)
		free_block(old_blocks[i]);
	free(old_blocks);
	return done;
}

static void free_inode_blocks(struct cxlfs_inode_disk *di)
{
	uint64_t max = (di->size + PAGE_SIZE - 1) / PAGE_SIZE;
	for (uint64_t lbn = 0; lbn < max; ++lbn) {
		uint64_t b = inode_block(di, lbn);
		if (b)
			free_block(b);
	}
	if (di->indirect)
		free_block(di->indirect);
	if (di->double_indirect) {
		uint64_t *l1 = block_ptr(di->double_indirect);
		for (size_t i = 0; i < CXLFS_PTRS_PER_BLOCK; ++i)
			if (l1[i])
				free_block(l1[i]);
		free_block(di->double_indirect);
	}
}

int cxlfs_truncate(uint64_t ino, size_t size)
{
	struct cxlfs_inode_disk *dip = disk_inode(ino);
	if (!dip)
		return -ENOENT;
	if (size >= dip->size) {
		dip->size = size;
		dip->generation++;
		persist_range(dip, sizeof(*dip));
		return publish_super();
	}
	uint64_t first_free = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t old_max = (dip->size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t count = old_max - first_free + ((size % PAGE_SIZE) ? 1 : 0);
	uint64_t *old_blocks = count ? calloc(count, sizeof(*old_blocks)) : NULL;
	if (count && !old_blocks)
		return -ENOMEM;
	uint64_t old_count = 0;
	if (size % PAGE_SIZE) {
		uint64_t lbn = size / PAGE_SIZE;
		uint64_t old = inode_block(dip, lbn);
		if (old) {
			uint64_t newb = alloc_block();
			if (!newb) {
				free(old_blocks);
				return -ENOSPC;
			}
			memcpy(block_ptr(newb), block_ptr(old), PAGE_SIZE);
			memset(block_ptr(newb) + (size % PAGE_SIZE), 0,
			       PAGE_SIZE - (size % PAGE_SIZE));
			persist_range(block_ptr(newb), PAGE_SIZE);
			if (set_inode_block(dip, lbn, newb) < 0) {
				free_block(newb);
				free(old_blocks);
				return -ENOSPC;
			}
			old_blocks[old_count++] = old;
		}
	}
	for (uint64_t lbn = first_free; lbn < old_max; ++lbn) {
		uint64_t old = inode_block(dip, lbn);
		if (old) {
			set_inode_block(dip, lbn, 0);
			old_blocks[old_count++] = old;
		}
	}
	dip->size = size;
	dip->generation++;
	persist_range(dip, sizeof(*dip));
	int ret = publish_super();
	for (uint64_t i = 0; i < old_count; ++i)
		free_block(old_blocks[i]);
	free(old_blocks);
	return ret;
}

int cxlfs_allocate(uint64_t ino, off_t offset, off_t len, int keep_size)
{
	if (offset < 0 || len < 0)
		return -EINVAL;
	char zero[PAGE_SIZE] = { 0 };
	uint64_t end = (uint64_t)offset + len;
	struct cxlfs_inode_disk *di = disk_inode(ino);
	uint64_t old_size = di ? di->size : 0;
	for (uint64_t pos = (uint64_t)offset; pos < end;) {
		size_t n = end - pos;
		if (n > PAGE_SIZE)
			n = PAGE_SIZE;
		if (!inode_block(di, pos / PAGE_SIZE) &&
		    cxlfs_write(ino, pos, zero, n) != (ssize_t)n)
			return -ENOSPC;
		pos += n;
	}
	if (keep_size && disk_inode(ino)->size != old_size)
		return cxlfs_truncate(ino, old_size);
	return 0;
}

void *cxlfs_page_addr(uint64_t ino, uint64_t page_no)
{
	struct cxlfs_inode_disk *di = disk_inode(ino);
	uint64_t b = di ? inode_block(di, page_no) : 0;
	return b ? block_ptr(b) : NULL;
}

static int find_dirent(uint64_t parent_ino, const char *name, size_t len,
		       struct cxlfs_dirent_disk *out, uint64_t *offset_out,
		       uint64_t *free_offset)
{
	struct cxlfs_inode_disk *dir = disk_inode(parent_ino);
	if (!dir || dir->type != FS_DIR)
		return -ENOTDIR;
	struct cxlfs_dirent_disk de;
	uint64_t free_at = UINT64_MAX;
	for (uint64_t off = 0; off + sizeof(de) <= dir->size;
	     off += sizeof(de)) {
		if (cxlfs_read(parent_ino, off, &de, sizeof(de)) != sizeof(de))
			return -EIO;
		if (!(de.flags & CXLFS_DIRENT_USED)) {
			if (free_at == UINT64_MAX)
				free_at = off;
			continue;
		}
		if (de.name_len == len && !memcmp(de.name, name, len)) {
			if (out) *out = de;
			if (offset_out) *offset_out = off;
			if (free_offset) *free_offset = free_at;
			return 0;
		}
	}
	if (free_offset)
		*free_offset = free_at == UINT64_MAX ? dir->size : free_at;
	return -ENOENT;
}

int cxlfs_create_node(uint64_t parent_ino, const char *name, size_t len,
		      uint32_t type, mode_t mode, uint64_t *ino_out)
{
	if (!mounted || restoring)
		return 0;
	if (!len || len > sizeof(((struct cxlfs_dirent_disk *)0)->name))
		return -ENAMETOOLONG;
	uint64_t slot;
	if (find_dirent(parent_ino, name, len, NULL, NULL, &slot) == 0)
		return -EEXIST;
	uint64_t ino = alloc_inode();
	if (!ino)
		return -ENOSPC;
	struct cxlfs_inode_disk di = { 0 };
	di.ino = ino;
	di.type = type;
	di.mode = mode;
	di.nlinks = 1;
	di.generation = 1;
	memcpy(disk_inode(ino), &di, sizeof(di));
	persist_range(disk_inode(ino), sizeof(di));

	struct cxlfs_dirent_disk de = { 0 };
	de.ino = ino;
	de.type = type;
	de.name_len = len;
	de.flags = CXLFS_DIRENT_USED;
	memcpy(de.name, name, len);
	ssize_t wr = cxlfs_write(parent_ino, slot, &de, sizeof(de));
	if (wr != sizeof(de)) {
		bitmap_set(inode_bitmap(), ino, 0);
		return wr < 0 ? wr : -ENOSPC;
	}
	if (ino_out)
		*ino_out = ino;
	return 0;
}

static int clear_dirent(uint64_t parent_ino, uint64_t off)
{
	struct cxlfs_dirent_disk zero = { 0 };
	return cxlfs_write(parent_ino, off, &zero, sizeof(zero)) == sizeof(zero)
		? 0 : -EIO;
}

static void destroy_disk_inode(uint64_t ino)
{
	struct cxlfs_inode_disk *di = disk_inode(ino);
	if (!di || !bitmap_test(inode_bitmap(), ino))
		return;
	/* First make the inode unreachable/durable, then recycle its blocks. */
	struct cxlfs_inode_disk old = *di;
	memset(di, 0, sizeof(*di));
	persist_range(di, sizeof(*di));
	publish_super();
	free_inode_blocks(&old);
	bitmap_set(inode_bitmap(), ino, 0);
}

int cxlfs_unlink_node(uint64_t parent_ino, const char *name, size_t len)
{
	struct cxlfs_dirent_disk de;
	uint64_t off;
	int ret = find_dirent(parent_ino, name, len, &de, &off, NULL);
	if (ret)
		return ret;
	ret = clear_dirent(parent_ino, off);
	if (!ret) {
		destroy_disk_inode(de.ino);
		publish_super();
	}
	return ret;
}

int cxlfs_rename_node(uint64_t old_parent, const char *old_name,
		      size_t old_len, uint64_t new_parent,
		      const char *new_name, size_t new_len)
{
	struct cxlfs_dirent_disk oldde, newde;
	uint64_t oldoff, newoff, freeoff;
	int ret = find_dirent(old_parent, old_name, old_len, &oldde, &oldoff, NULL);
	if (ret)
		return ret;
	int new_exists = find_dirent(new_parent, new_name, new_len,
				     &newde, &newoff, &freeoff) == 0;
	if (!new_exists)
		newoff = freeoff;
	struct cxlfs_dirent_disk moved = oldde;
	moved.name_len = new_len;
	memset(moved.name, 0, sizeof(moved.name));
	memcpy(moved.name, new_name, new_len);
	if (cxlfs_write(new_parent, newoff, &moved, sizeof(moved)) != sizeof(moved))
		return -EIO;
	if (!(old_parent == new_parent && oldoff == newoff))
		clear_dirent(old_parent, oldoff);
	if (new_exists && newde.ino != oldde.ino)
		destroy_disk_inode(newde.ino);
	ret = publish_super();
	/* LevelDB installs CURRENT through an fsynced dbtmp rename.  Verify the
	 * durable namespace immediately while the original tmpfs metadata write
	 * lock is still held. */
	if (!ret && new_len == 7 && !memcmp(new_name, "CURRENT", 7)) {
		struct cxlfs_dirent_disk check;
		int found = find_dirent(new_parent, new_name, new_len,
					&check, NULL, NULL);
		printf("[CXLFS] durable rename %.*s -> %.*s: ret=%d ino=%lu\n",
		       (int)old_len, old_name, (int)new_len, new_name, found,
		       found ? 0UL : (unsigned long)check.ino);
		if (found || check.ino != oldde.ino)
			return -EIO;
	}
	return ret;
}

static int restore_dir(struct inode *runtime_dir, uint64_t disk_ino)
{
	struct cxlfs_inode_disk *dir = disk_inode(disk_ino);
	struct cxlfs_dirent_disk de;
	for (uint64_t off = 0; off + sizeof(de) <= dir->size;
	     off += sizeof(de)) {
		if (cxlfs_read(disk_ino, off, &de, sizeof(de)) != sizeof(de))
			return -EIO;
		if (!(de.flags & CXLFS_DIRENT_USED) || !de.name_len)
			continue;
		struct cxlfs_inode_disk *child = disk_inode(de.ino);
		if (!child || !bitmap_test(inode_bitmap(), de.ino))
			continue;
		int leveldb_file =
			(de.name_len == 7 && !memcmp(de.name, "CURRENT", 7)) ||
			(de.name_len == 3 && !memcmp(de.name, "LOG", 3)) ||
			(de.name_len == 4 && !memcmp(de.name, "LOCK", 4)) ||
			(de.name_len >= 9 && !memcmp(de.name, "MANIFEST-", 9)) ||
			(de.name_len >= 4 &&
			 !memcmp(de.name + de.name_len - 4, ".log", 4));
		if (leveldb_file)
			printf("[CXLFS] restoring durable %.*s: parent=%lu ino=%lu size=%lu\n",
			       (int)de.name_len, de.name, (unsigned long)disk_ino,
			       (unsigned long)de.ino, (unsigned long)child->size);
		struct inode *runtime_child = NULL;
		int ret = tfs_attach_disk_node(runtime_dir, de.name, de.name_len,
					       de.ino, child->type, child->mode,
					       child->size, &runtime_child);
		if (ret)
			return ret;
		if (child->type == FS_DIR) {
			ret = restore_dir(runtime_child, de.ino);
			if (ret)
				return ret;
		}
	}
	return 0;
}

int cxlfs_restore_tree(struct inode *runtime_root)
{
	if (!mounted)
		return -ENODEV;
	restoring = 1;
	int ret = restore_dir(runtime_root, CXLFS_ROOT_INO);
	restoring = 0;
	return ret;
}
