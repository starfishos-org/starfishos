#include <string.h>
#include <stdio.h>
#include <chcore/memory.h>
#include <chcore/syscall.h>

#include "plog.h"
#include "tmpfs.h"

#define PLOG_PREFIX "[plog]"
#define plog_info(fmt, ...)  printf(PLOG_PREFIX " " fmt, ##__VA_ARGS__)
#define plog_error(fmt, ...) fprintf(stderr, PLOG_PREFIX " " fmt, ##__VA_ARGS__)

/* Global p-log header for the current machine */
static struct plog_header *g_plog = NULL;

/*
 * Simple inode-to-path tracking for write logging.
 * When a file is opened, we record inode -> path.
 * When write is called with inode, we look up the path.
 */
#define PLOG_PATH_MAP_SIZE 256
struct plog_path_entry {
	void *inode;
	char path[PLOG_MAX_PATH];
};
static struct plog_path_entry plog_path_map[PLOG_PATH_MAP_SIZE];
static int plog_path_map_count = 0;

void plog_track_inode(void *inode, const char *path)
{
	if (!g_plog) return;
	/* Search for existing entry */
	for (int i = 0; i < plog_path_map_count; i++) {
		if (plog_path_map[i].inode == inode) {
			strncpy(plog_path_map[i].path, path, PLOG_MAX_PATH - 1);
			plog_path_map[i].path[PLOG_MAX_PATH - 1] = '\0';
			return;
		}
	}
	/* Add new entry */
	if (plog_path_map_count < PLOG_PATH_MAP_SIZE) {
		plog_path_map[plog_path_map_count].inode = inode;
		strncpy(plog_path_map[plog_path_map_count].path, path,
		        PLOG_MAX_PATH - 1);
		plog_path_map[plog_path_map_count].path[PLOG_MAX_PATH - 1] = '\0';
		plog_path_map_count++;
	}
}

const char *plog_get_inode_path(void *inode)
{
	for (int i = 0; i < plog_path_map_count; i++) {
		if (plog_path_map[i].inode == inode)
			return plog_path_map[i].path;
	}
	return NULL;
}

static void *map_shm(int shm_id, size_t size)
{
	void *addr = (void *)chcore_alloc_vaddr(size);
	if (!addr) {
		plog_error("Failed to alloc vaddr for shm %d\n", shm_id);
		return NULL;
	}
	int ret = usys_mmap_shm(shm_id, addr);
	if (ret < 0) {
		plog_error("Failed to mmap shm %d: %d\n", shm_id, ret);
		return NULL;
	}
	return addr;
}

struct plog_header *plog_init(int machine_id)
{
	int shm_id = PLOG_SHM_ID(machine_id);
	void *addr = map_shm(shm_id, PLOG_SHM_SIZE);
	if (!addr)
		return NULL;

	struct plog_header *hdr = (struct plog_header *)addr;

	/* Initialize if fresh */
	if (hdr->magic != PLOG_MAGIC) {
		memset(hdr, 0, sizeof(*hdr));
		hdr->magic = PLOG_MAGIC;
		hdr->capacity = PLOG_SHM_SIZE - sizeof(struct plog_header);
		hdr->tail = sizeof(struct plog_header);
		hdr->seq_counter = 0;
		hdr->owner_machine = machine_id;
		hdr->state = PLOG_ACTIVE;
		PLOG_FLUSH(hdr);
		plog_info("Initialized p-log for machine %d (capacity=%lu)\n",
		          machine_id, (unsigned long)hdr->capacity);
	} else {
		hdr->state = PLOG_ACTIVE;
		PLOG_FLUSH(&hdr->state);
		plog_info("Reconnected to p-log for machine %d (tail=%lu)\n",
		          machine_id, (unsigned long)hdr->tail);
	}

	g_plog = hdr;
	return hdr;
}

struct plog_header *plog_map_remote(int remote_machine_id)
{
	int shm_id = PLOG_SHM_ID(remote_machine_id);
	void *addr = map_shm(shm_id, PLOG_SHM_SIZE);
	if (!addr)
		return NULL;

	struct plog_header *hdr = (struct plog_header *)addr;
	if (hdr->magic != PLOG_MAGIC) {
		plog_error("Remote p-log machine %d: bad magic 0x%x\n",
		           remote_machine_id, hdr->magic);
		return NULL;
	}

	plog_info("Mapped remote p-log for machine %d (tail=%lu, entries)\n",
	          remote_machine_id, (unsigned long)hdr->tail);
	return hdr;
}

/*
 * Append a raw entry to the p-log.
 * Returns pointer to the entry on success, NULL if log is full.
 */
static struct plog_entry *plog_append_raw(uint32_t entry_len)
{
	if (!g_plog) return NULL;

	uint64_t tail = g_plog->tail;
	uint64_t end = tail + entry_len;

	if (end > PLOG_SHM_SIZE) {
		/* Rate-limit: once full, every append fails — logging each
		 * failure floods the console and throttles the whole fs. */
		static uint64_t full_count = 0;
		if ((full_count++ & 0xffff) == 0)
			plog_error("P-log full (tail=%lu, need=%u, capacity=%lu, dropped=%lu)\n",
			           (unsigned long)tail, entry_len,
			           (unsigned long)g_plog->capacity,
			           (unsigned long)full_count - 1);
		return NULL;
	}

	struct plog_entry *entry = (struct plog_entry *)((char *)g_plog + tail);
	entry->entry_len = entry_len;
	entry->seq = g_plog->seq_counter++;
	return entry;
}

static void plog_commit(struct plog_entry *entry)
{
	/* Flush entry data */
	PLOG_FLUSH(entry);

	/* Update tail atomically */
	uint64_t new_tail = (uint64_t)((char *)entry - (char *)g_plog)
	                    + entry->entry_len;
	g_plog->tail = new_tail;
	PLOG_FLUSH(&g_plog->tail);
}

int plog_append_creat(const char *path, int mode)
{
	uint32_t len = PLOG_ENTRY_BASE_SIZE;
	struct plog_entry *e = plog_append_raw(len);
	if (!e) return -1;

	e->op = PLOG_OP_CREAT;
	strncpy(e->path, path, PLOG_MAX_PATH - 1);
	e->path[PLOG_MAX_PATH - 1] = '\0';
	e->creat.mode = mode;

	plog_commit(e);
	return 0;
}

int plog_append_mkdir(const char *path, int mode)
{
	uint32_t len = PLOG_ENTRY_BASE_SIZE;
	struct plog_entry *e = plog_append_raw(len);
	if (!e) return -1;

	e->op = PLOG_OP_MKDIR;
	strncpy(e->path, path, PLOG_MAX_PATH - 1);
	e->path[PLOG_MAX_PATH - 1] = '\0';
	e->creat.mode = mode;

	plog_commit(e);
	return 0;
}

int plog_append_write(const char *path, uint64_t offset,
                      const char *data, uint32_t data_len)
{
	uint32_t len = PLOG_ENTRY_WRITE_HDR + data_len;
	struct plog_entry *e = plog_append_raw(len);
	if (!e) return -1;

	e->op = PLOG_OP_WRITE;
	strncpy(e->path, path, PLOG_MAX_PATH - 1);
	e->path[PLOG_MAX_PATH - 1] = '\0';
	e->write.offset = offset;
	e->write.data_len = data_len;
	memcpy(e->write.data, data, data_len);

	plog_commit(e);
	return 0;
}

/*
 * Replay helpers: create a file/directory at the given path.
 */
extern int __fs_creat(const char *path, mode_t mode);
extern int tmpfs_mkdir(const char *path, mode_t mode);

/*
 * Recursively ensure all parent directories of path exist.
 * path must start with '/'.
 */
static void replay_mkdir_parents(const char *path)
{
	char tmp[PLOG_MAX_PATH];
	strncpy(tmp, path, PLOG_MAX_PATH - 1);
	tmp[PLOG_MAX_PATH - 1] = '\0';

	/* Walk each '/' and mkdir the prefix */
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			tmpfs_mkdir(tmp, 0755); /* ignore errors (EEXIST ok) */
			*p = '/';
		}
	}
}

/*
 * Replay helper: look up an inode by path and write to it.
 */
static int replay_write(const char *path, uint64_t offset,
                        const char *data, uint32_t data_len)
{
	/* Walk path to find inode */
	struct tmpfs_walk_path walk;
	walk.path = path;
	walk.path_len = strlen(path);
	walk.not_found_callback = NULL;
	walk.callback_data = NULL;
	walk.follow_symlink = 1;

	int err = handle_xxxat(AT_FDROOT, &walk);
	if (err) {
		/*
		 * File may have been created via open(O_CREAT) rather than
		 * creat(), so no CREAT entry was logged. Auto-create it now.
		 */
		plog_info("replay_write: %s not found, auto-creating\n", path);
		replay_mkdir_parents(path);
		int creat_ret = __fs_creat(path, 0644);
		if (creat_ret && creat_ret != -EEXIST) {
			plog_error("replay_write: auto-creat failed for %s (%d)\n",
			           path, creat_ret);
			return creat_ret;
		}
		/* Retry the path walk after creating the file */
		walk.path = path;
		walk.path_len = strlen(path);
		err = handle_xxxat(AT_FDROOT, &walk);
		if (err) {
			plog_error("replay_write: path still not found after creat: %s (err=%d)\n",
			           path, err);
			path_fini(&walk.path_record);
			return err;
		}
	}

	ssize_t written = tfs_file_write(walk.target, offset, data, data_len);
	path_fini(&walk.path_record);

	if (written != (ssize_t)data_len) {
		plog_error("replay_write: short write %ld/%u for %s\n",
		           written, data_len, path);
		return -1;
	}
	return 0;
}

void plog_truncate(void)
{
	if (!g_plog)
		return;

	g_plog->tail = sizeof(struct plog_header);
	g_plog->seq_counter = 0;
	PLOG_FLUSH(g_plog);
	plog_info("P-log truncated (checkpoint after fsync)\n");
}

int plog_replay(struct plog_header *hdr)
{
	if (!hdr || hdr->magic != PLOG_MAGIC) {
		plog_error("Invalid p-log header for replay\n");
		return -1;
	}

	uint64_t tail = hdr->tail;
	uint64_t pos = sizeof(struct plog_header);
	int count = 0, errors = 0;

	plog_info("Replaying p-log (tail=%lu)...\n", (unsigned long)tail);

	while (pos < tail) {
		struct plog_entry *e = (struct plog_entry *)((char *)hdr + pos);

		if (e->entry_len == 0 || pos + e->entry_len > tail) {
			plog_error("Corrupt entry at offset %lu\n",
			           (unsigned long)pos);
			break;
		}

		switch (e->op) {
		case PLOG_OP_MKDIR: {
			plog_info("  replay[%lu]: MKDIR %s mode=%d\n",
			          (unsigned long)e->seq, e->path, e->creat.mode);
			/* Ensure all ancestor dirs exist first */
			replay_mkdir_parents(e->path);
			int ret = tmpfs_mkdir(e->path, e->creat.mode);
			if (ret && ret != -EEXIST) {
				plog_error("  MKDIR failed: %d\n", ret);
				errors++;
			}
			break;
		}
		case PLOG_OP_CREAT: {
			plog_info("  replay[%lu]: CREAT %s mode=%d\n",
			          (unsigned long)e->seq, e->path, e->creat.mode);
			/* Ensure parent dirs exist (handles old logs without MKDIR) */
			replay_mkdir_parents(e->path);
			int ret = __fs_creat(e->path, e->creat.mode);
			if (ret && ret != -EEXIST) {
				plog_error("  CREAT failed: %d\n", ret);
				errors++;
			}
			break;
		}
		case PLOG_OP_WRITE: {
			plog_info("  replay[%lu]: WRITE %s off=%lu len=%u\n",
			          (unsigned long)e->seq, e->path,
			          (unsigned long)e->write.offset,
			          e->write.data_len);
			int ret = replay_write(e->path, e->write.offset,
			                       e->write.data, e->write.data_len);
			if (ret) errors++;
			break;
		}
		default:
			plog_error("  Unknown op %u at offset %lu\n",
			           e->op, (unsigned long)pos);
			break;
		}

		pos += e->entry_len;
		count++;
	}

	plog_info("Replay done: %d entries, %d errors\n", count, errors);
	return errors ? -1 : 0;
}
