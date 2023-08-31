#include <common/util.h>

#include "thread_env.h"

/*
 * Setup the initial environment for a user process (main thread).
 *
 * According to Libc convention, we current set the environment
 * on the user stack.
 *
 */

#define AT_NULL   0	/* end of vector */
#define AT_IGNORE 1	/* entry should be ignored */
#define AT_EXECFD 2	/* file descriptor of program */
#define AT_PHDR   3	/* program headers for program */
#define AT_PHENT  4	/* size of program header entry */
#define AT_PHNUM  5	/* number of program headers */
#define AT_PAGESZ 6	/* system page size */
#define AT_BASE   7	/* base address of interpreter */
#define AT_FLAGS  8	/* flags */
#define AT_ENTRY  9	/* entry point of program */
#define AT_NOTELF 10	/* program is not ELF */
#define AT_UID    11	/* real uid */
#define AT_EUID   12	/* effective uid */
#define AT_GID    13	/* real gid */
#define AT_EGID   14	/* effective gid */
#define AT_PLATFORM 15  /* string identifying CPU for optimizations */
#define AT_HWCAP  16    /* arch dependent hints at CPU capabilities */
#define AT_CLKTCK 17	/* frequency at which times() increments */
/* AT_* values 18 through 22 are reserved */
#define AT_SECURE 23   /* secure mode boolean */
/* string identifying real platform, may differ from AT_PLATFORM. */
#define AT_BASE_PLATFORM 24
#define AT_RANDOM 25	/* address of 16 random bytes */
#define AT_HWCAP2 26	/* extension of AT_HWCAP */
#define AT_EXECFN 31	/* filename of program */

#if defined(CHCORE_ARCH_X86_64)
const char PLAT[] = "x86_64";
#endif

struct env_buf {
	u64 *entries_tail;
	u64 *entries_end;
	char *strings_tail;
	char *strings_end;
	u64 strings_offset;
};

static void env_buf_append_int(struct env_buf *env_buf, u64 val)
{
	BUG_ON(env_buf->entries_tail >= env_buf->entries_end);
	*(env_buf->entries_tail++) = val;
}

static void env_buf_append_str(struct env_buf *env_buf, const char *val)
{
	int i = 0;
	int val_size = strlen(val) + 1;
	
	BUG_ON(env_buf->strings_tail + val_size > env_buf->strings_end);
	while (val[i] != '\0') {
		env_buf->strings_tail[i] = val[i];
		i++;
	}
	env_buf_append_int(
		env_buf, (u64)env_buf->strings_tail + env_buf->strings_offset);
	env_buf->strings_tail += val_size;
}

static void env_buf_append_int_auxv(struct env_buf *env_buf, u64 type, u64 val)
{
	env_buf_append_int(env_buf, type);
	env_buf_append_int(env_buf, val);
}

static void env_buf_append_str_auxv(struct env_buf *env_buf, u64 type,
				    const char *val)
{
	env_buf_append_int(env_buf, type);
	env_buf_append_str(env_buf, val);
}


/*
 * For setting up the stack (env) of some process.
 *
 * env: stack top address used by kernel
 * top_vaddr: stack top address mapped to user
 */
// TODO (tmac): fix magic numbers
void prepare_env(char *env, u64 top_vaddr, char *name, struct process_metadata *meta)
{
	struct env_buf env_buf;
	/* clear env */
	memset(env, 0, ENV_SIZE_ON_STACK);

	env_buf.entries_tail = (u64 *)env;
	env_buf.entries_end = (u64 *)(env + ENV_SIZE_ON_STACK / 2);
	env_buf.strings_tail = (char *)env_buf.entries_end;
	env_buf.strings_end = env + ENV_SIZE_ON_STACK;
	env_buf.strings_offset = top_vaddr - ENV_SIZE_ON_STACK - (u64)env;

	env_buf_append_int(&env_buf, 1);
	env_buf_append_str(&env_buf, name);

	env_buf_append_int(&env_buf, 0);

	env_buf_append_int(&env_buf, -1);
	env_buf_append_int(&env_buf, -1);

	env_buf_append_int(&env_buf, 0);

	env_buf_append_int_auxv(&env_buf, AT_SECURE, 0);
	env_buf_append_int_auxv(&env_buf, AT_PAGESZ, 0x1000);
	env_buf_append_int_auxv(&env_buf, AT_PHDR, meta->phdr_addr);
	env_buf_append_int_auxv(&env_buf, AT_PHENT, meta->phentsize);
	env_buf_append_int_auxv(&env_buf, AT_PHNUM, meta->phnum);
	env_buf_append_int_auxv(&env_buf, AT_FLAGS, meta->flags);
	env_buf_append_int_auxv(&env_buf, AT_ENTRY, meta->entry);
	env_buf_append_int_auxv(&env_buf, AT_UID, 1000);
	env_buf_append_int_auxv(&env_buf, AT_EUID, 1000);
	env_buf_append_int_auxv(&env_buf, AT_GID, 1000);
	env_buf_append_int_auxv(&env_buf, AT_EGID, 1000);
	env_buf_append_int_auxv(&env_buf, AT_CLKTCK, 100);
	env_buf_append_int_auxv(&env_buf, AT_HWCAP, 0);
	env_buf_append_str_auxv(&env_buf, AT_PLATFORM, PLAT);
	env_buf_append_int_auxv(&env_buf, AT_RANDOM, top_vaddr - 64);
	env_buf_append_int_auxv(&env_buf, AT_NULL, 0);

	/* add more auxv here */
}
