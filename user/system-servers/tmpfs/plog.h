#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Ananke-style persistent operation log (p-log) for tmpfs recovery.
 *
 * The p-log lives on CXL shared memory (one per machine).
 * SHM ID for machine N's p-log = CLUSTER_MAX_MACHINE_NUM + N.
 *
 * Format: fixed header + append-only variable-size entries.
 * Crash consistency: write entry -> FLUSH -> update tail -> FLUSH.
 */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define PLOG_SHM_SIZE      (PAGE_SIZE * 256UL) /* 1 MB */
#define PLOG_MAGIC         0x504C4F47U         /* "PLOG" */
#define PLOG_MAX_PATH      256
#define PLOG_MAX_DATA      PAGE_SIZE

/* P-log SHM ID = CLUSTER_MAX_MACHINE_NUM + machine_id */
#ifndef CLUSTER_MAX_MACHINE_NUM
#define CLUSTER_MAX_MACHINE_NUM 8
#endif
#define PLOG_SHM_ID(mid)   (CLUSTER_MAX_MACHINE_NUM + (mid))

/* FLUSH: clwb + sfence for CXL persistence */
#define PLOG_FLUSH(addr) do { \
	__asm__ volatile("clwb (%0)" :: "r"(addr) : "memory"); \
	__asm__ volatile("sfence" ::: "memory"); \
} while (0)

/* P-log states */
enum plog_state {
	PLOG_INACTIVE = 0,
	PLOG_ACTIVE   = 1,
	PLOG_CRASHED  = 2,
};

/* Operation types */
enum plog_op_type {
	PLOG_OP_INVALID = 0,
	PLOG_OP_CREAT   = 1,
	PLOG_OP_WRITE   = 2,
	PLOG_OP_MKDIR   = 3,
};

/* P-log header (at start of SHM region) */
struct plog_header {
	uint32_t magic;
	volatile uint32_t state;
	volatile uint64_t tail;    /* byte offset from region start of next write */
	uint64_t capacity;         /* usable bytes (region size - header size) */
	uint64_t seq_counter;      /* next sequence number */
	uint32_t owner_machine;    /* machine that owns this p-log */
	uint32_t _pad;
} __attribute__((aligned(64)));

/* P-log entry (variable size due to write data) */
struct plog_entry {
	uint32_t op;               /* enum plog_op_type */
	uint32_t entry_len;        /* total bytes of this entry */
	uint64_t seq;              /* sequence number */
	char path[PLOG_MAX_PATH];  /* file path */
	union {
		struct {
			int mode;
		} creat;
		struct {
			uint64_t offset;
			uint32_t data_len;
			char data[];   /* inline file data */
		} write;
	};
} __attribute__((packed));

/* Computed minimum entry sizes */
#define PLOG_ENTRY_BASE_SIZE  (offsetof(struct plog_entry, creat) + sizeof(int))
#define PLOG_ENTRY_WRITE_HDR  (offsetof(struct plog_entry, write) + \
                               offsetof(typeof(((struct plog_entry *)0)->write), data))

/*
 * API
 */

/* Initialize p-log for this machine. Returns the mapped header, or NULL. */
struct plog_header *plog_init(int machine_id);

/* Map another machine's p-log (for recovery). Returns header, or NULL. */
struct plog_header *plog_map_remote(int remote_machine_id);

/* Append a creat entry. Returns 0 on success, -1 if log full. */
int plog_append_creat(const char *path, int mode);

/* Append a mkdir entry. Returns 0 on success, -1 if log full. */
int plog_append_mkdir(const char *path, int mode);

/* Append a write entry with inline data. Returns 0 on success, -1 if log full. */
int plog_append_write(const char *path, uint64_t offset,
                      const char *data, uint32_t data_len);

/* Replay all entries in a p-log header to reconstruct tmpfs state. */
int plog_replay(struct plog_header *hdr);

/*
 * Truncate the p-log back to empty (after a successful fsync/flush).
 * All prior entries are considered durable; the log is reset for reuse.
 */
void plog_truncate(void);

/* Track inode -> path mapping (for write logging). */
void plog_track_inode(void *inode, const char *path);

/* Get path for an inode (returns NULL if not tracked). */
const char *plog_get_inode_path(void *inode);
