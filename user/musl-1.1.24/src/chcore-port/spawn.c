#include <chcore/syscall.h>
#include <chcore/defs.h>
#include <chcore-internal/fs_defs.h>
#include <stdio.h>
#include <chcore/proc.h>
#include <chcore/defs.h>
#include <chcore/launcher.h>
#include <errno.h>

#include <string.h>
#include <assert.h>
#include <stdlib.h>

/* An example to launch an (libc) application process */

#define AT_NULL     0 /* end of vector */
#define AT_IGNORE   1 /* entry should be ignored */
#define AT_EXECFD   2 /* file descriptor of program */
#define AT_PHDR     3 /* program headers for program */
#define AT_PHENT    4 /* size of program header entry */
#define AT_PHNUM    5 /* number of program headers */
#define AT_PAGESZ   6 /* system page size */
#define AT_BASE     7 /* base address of interpreter */
#define AT_FLAGS    8 /* flags */
#define AT_ENTRY    9 /* entry point of program */
#define AT_NOTELF   10 /* program is not ELF */
#define AT_UID      11 /* real uid */
#define AT_EUID     12 /* effective uid */
#define AT_GID      13 /* real gid */
#define AT_EGID     14 /* effective gid */
#define AT_PLATFORM 15 /* string identifying CPU for optimizations */
#define AT_HWCAP    16 /* arch dependent hints at CPU capabilities */
#define AT_CLKTCK   17 /* frequency at which times() increments */
/* AT_* values 18 through 22 are reserved */
#define AT_SECURE 23 /* secure mode boolean */
#define AT_BASE_PLATFORM                            \
        24 /* string identifying real platform, may \
            * differ from AT_PLATFORM. */
#define AT_RANDOM 25 /* address of 16 random bytes */
#define AT_HWCAP2 26 /* extension of AT_HWCAP */

#define AT_EXECFN 31 /* filename of program */

#define INIT_ENV_SIZE PAGE_SIZE

// TODO: ARCH-dependent string
const char PLAT[] = "aarch64";

/* enviroments */
#define SET_LD_LIB_PATH
/* A user can mount external storage on /user/lib for dynamic libraries. */
const char LD_LIB_PATH[] = "LD_LIBRARY_PATH=/:/lib:/user/lib";

struct env_buf {
        u64 *entries_tail;
        u64 *entries_end;
        char *strings_tail;
        char *strings_end;
        u64 strings_offset;
};

static void env_buf_append_int(struct env_buf *env_buf, u64 val)
{
        assert(env_buf->entries_tail < env_buf->entries_end);
        *(env_buf->entries_tail++) = val;
}

static void env_buf_append_str(struct env_buf *env_buf, const char *val)
{
        int val_size = strlen(val) + 1;

        assert(env_buf->strings_tail + val_size <= env_buf->strings_end);
        strcpy(env_buf->strings_tail, val);
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

/**
 * NOTE: The stack format:
 * http://articles.manugarg.com/aboutelfauxiliaryvectors.html
 * The url shows a 32-bit format stack, but we implemented a 64-bit stack below.
 * People as smart as you could understand the difference.
 */
static void construct_init_env(char *env, u64 top_vaddr,
                               struct process_metadata *meta, char *name,
                               struct pmo_map_request *pmo_map_reqs,
                               int nr_pmo_map_reqs, int caps[], int nr_caps,
                               int argc, char **argv, int pid, u64 load_offset)
{
        int i;
        u64 offset;
        struct env_buf env_buf;
        char pidstr[16];

        memset(env, 0, INIT_ENV_SIZE);

        /* The strings part starts from the middle of the page. */
        env_buf.entries_tail = (u64 *)env;
        env_buf.entries_end = (u64 *)(env + INIT_ENV_SIZE / 2);
        env_buf.strings_tail = (char *)env_buf.entries_end;
        env_buf.strings_end = env + INIT_ENV_SIZE;
        env_buf.strings_offset = top_vaddr - INIT_ENV_SIZE - (u64)env;

        /* argc, argv */
        env_buf_append_int(&env_buf, argc);

        for (i = 0; i < argc; i++) {
                env_buf_append_str(&env_buf, argv[i]);
        }

        env_buf_append_int(&env_buf, 0);

        /* envp */
        env_buf_append_int(&env_buf, nr_pmo_map_reqs ?: ENVP_NONE_PMOS);
        for (i = 0; i < nr_pmo_map_reqs; i++) {
                env_buf_append_int(&env_buf, pmo_map_reqs[i].addr);
        }

        env_buf_append_int(&env_buf, nr_caps ?: ENVP_NONE_CAPS);
        for (i = 0; i < nr_caps; i++) {
                env_buf_append_int(&env_buf, caps[i]);
        }

        env_buf_append_str(&env_buf, LD_LIB_PATH);
        snprintf(pidstr, 15, "PID=%d\n", pid);
        env_buf_append_str(&env_buf, pidstr);

        env_buf_append_int(&env_buf, 0);

        /* auxv */
        env_buf_append_int_auxv(&env_buf, AT_SECURE, 0);
        env_buf_append_int_auxv(&env_buf, AT_PAGESZ, PAGE_SIZE);
        env_buf_append_int_auxv(
                &env_buf, AT_PHDR, meta->phdr_addr + load_offset);
        env_buf_append_int_auxv(&env_buf, AT_PHENT, meta->phentsize);
        env_buf_append_int_auxv(&env_buf, AT_PHNUM, meta->phnum);
        env_buf_append_int_auxv(&env_buf, AT_FLAGS, meta->flags);
        env_buf_append_int_auxv(&env_buf, AT_ENTRY, meta->entry + load_offset);
        env_buf_append_int_auxv(&env_buf, AT_UID, 1000);
        env_buf_append_int_auxv(&env_buf, AT_EUID, 1000);
        env_buf_append_int_auxv(&env_buf, AT_GID, 1000);
        env_buf_append_int_auxv(&env_buf, AT_EGID, 1000);
        env_buf_append_int_auxv(&env_buf, AT_CLKTCK, 100);
        env_buf_append_int_auxv(&env_buf, AT_HWCAP, 0);
        env_buf_append_str_auxv(&env_buf, AT_PLATFORM, PLAT);
        env_buf_append_int_auxv(&env_buf, AT_RANDOM, top_vaddr - 64);
        env_buf_append_int_auxv(&env_buf, AT_BASE, 0);
        env_buf_append_int_auxv(&env_buf, AT_NULL, 0);
}

/*
 * user_elf: elf struct
 * child_process_cap: if not NULL, set to child_process_cap that can be
 *                    used in current process.
 *
 * child_main_thread_cap: if not NULL, set to child_main_thread_cap
 *                        that can be used in current process.
 *
 * pmo_map_reqs, nr_pmo_map_reqs: input pmos to map in the new process
 *
 * caps, nr_caps: copy from farther process to child process
 *
 * cpuid: affinity
 *
 * argc/argv: the number of arguments and the arguments.
 *
 * badge: the badge is generated by the invokder (usually procmgr).
 *
 * pcid: the pcid is fixed for root process and servers,
 * 	 and the rest is generated by procmgr.
 */
int launch_process_with_pmos_caps(struct launch_process_args *lp_args)
{
        int new_process_cap;
        /* The name of process/cap_group: mainly used for debugging */
        char *process_name;
        int main_thread_cap;
        int ret;
        long pc;
        /* for creating pmos */
        struct pmo_request pmo_requests[2];
        int main_stack_cap;
        int forbid_area_cap;
        u64 offset;
        u64 stack_top;
        u64 p_vaddr;
        int i;
        /* for mapping pmos */
        struct pmo_map_request pmo_map_requests[2 + ELF_MAX_LOAD_SEG];
        int *transfer_caps = NULL;
        u64 load_offset = 0;
        char init_env[INIT_ENV_SIZE];

        struct user_elf *user_elf = lp_args->user_elf;
        int *child_process_cap = lp_args->child_process_cap;
        int *child_main_thread_cap = lp_args->child_main_thread_cap;
        struct pmo_map_request *pmo_map_reqs = lp_args->pmo_map_reqs;
        int nr_pmo_map_reqs = lp_args->nr_pmo_map_reqs;
        int *caps = lp_args->caps;
        int nr_caps = lp_args->nr_caps;
        s32 cpuid = lp_args->cpuid;
        int argc = lp_args->argc;
        char **argv = lp_args->argv;
        u64 badge = lp_args->badge;
        int pid = lp_args->pid;
        u64 pcid = lp_args->pcid;
        u32 type = lp_args->type;

        /* for usys_creat_thread */
        struct thread_args args;

        process_name = user_elf->path;

        /* Load libc.so first instead of loading the dynamic application
         * TODO: the load_offset for libc.so
         */
        if (strstr(user_elf->path, "libc.so") != NULL) {
                // printf("CHCORE_LOADER warning: loader itself is loaded at a
                // magic address\n");
                load_offset = 0x400000000000UL;

                for (i = 0; i < ELF_MAX_LOAD_SEG; ++i) {
                        user_elf->user_elf_seg[i].p_vaddr += load_offset;
                }

                if (argc > 1) {
                        /* Set the process_name to the dynamic application */
                        process_name = argv[1];
                }
        }
        assert(process_name != NULL);

        /* create a new process with an empty vmspace */
        new_process_cap = usys_create_cap_group(
                badge, process_name, strlen(process_name), pcid);
        if (new_process_cap < 0) {
                printf("%s: fail to create new_process_cap (ret: %d)\n",
                       __func__,
                       new_process_cap);
                goto fail;
        }

        if (nr_caps > 0) {
                transfer_caps = malloc(sizeof(int) * nr_caps);
                /* usys_transfer_caps is used during process creation */
                ret = usys_transfer_caps(
                        new_process_cap, caps, nr_caps, transfer_caps);
                if (ret != 0) {
                        printf("usys_transfer_caps ret %d\n", ret);
                        usys_exit(-1);
                }
        }

        // debug("meta.pc: 0x%lx\n", user_elf->elf_meta->entry);

        /* L1 icache & dcache have no coherence */
        /* TODO: actually, I am not sure whether this is necessary here*/
        // flush_idcache();

        pc = user_elf->elf_meta.entry;
        // printf("pc is 0x%lx\n", pc);

        /* create pmos in current process */
        pmo_requests[0].size = MAIN_THREAD_STACK_SIZE;
        pmo_requests[0].type = PMO_STACK;

        pmo_requests[1].size = PAGE_SIZE;
        pmo_requests[1].type = PMO_FORBID;

        ret = usys_create_pmos((void *)pmo_requests, 2, MALLOC_TYPE_DEFAULT);

        if (ret != 0) {
                printf("%s: fail to create_pmos (ret: %d)\n", __func__, ret);
                goto fail;
        }

        /* get result caps */
        main_stack_cap = pmo_requests[0].ret_cap;
        if (main_stack_cap < 0) {
                printf("%s: fail to create_pmos (ret cap: %d)\n",
                       __func__,
                       main_stack_cap);
                goto fail;
        }

        /* Map a forbidden pmo in case of stack overflow */
        forbid_area_cap = pmo_requests[1].ret_cap;
        if (forbid_area_cap < 0) {
                printf("%s: fail to create_pmos (ret cap: %d)\n",
                       __func__,
                       forbid_area_cap);
                goto fail;
        }

        /* usys_write_pmo -> prepare the stack */
        stack_top = MAIN_THREAD_STACK_BASE + MAIN_THREAD_STACK_SIZE;
        construct_init_env(init_env,
                           stack_top,
                           &user_elf->elf_meta,
                           user_elf->path,
                           pmo_map_reqs,
                           nr_pmo_map_reqs,
                           transfer_caps,
                           nr_caps,
                           argc,
                           argv,
                           pid,
                           load_offset);
        offset = MAIN_THREAD_STACK_SIZE - INIT_ENV_SIZE;

        free(transfer_caps);

        // printf("init_env argc: %ld\n", (*(u64 *)init_env));
        ret = usys_write_pmo(main_stack_cap, offset, init_env, INIT_ENV_SIZE);
        if (ret != 0) {
                printf("%s: fail to write_pmo (ret: %d)\n", __func__, ret);
                goto fail;
        }

        /* Map the main stack pmo */
        pmo_map_requests[0].pmo_cap = main_stack_cap;
        pmo_map_requests[0].addr = MAIN_THREAD_STACK_BASE;
        pmo_map_requests[0].perm = VM_READ | VM_WRITE;
        pmo_map_requests[0].free_cap = 1;

        /* Map the forbidden area in case of stack overflow */
        pmo_map_requests[1].pmo_cap = forbid_area_cap;
        pmo_map_requests[1].addr = MAIN_THREAD_STACK_BASE - PAGE_SIZE;
        pmo_map_requests[1].perm = VM_FORBID;
        pmo_map_requests[1].free_cap = 1;

        /* Map each segment in the elf binary */
        for (i = 0; i < ELF_MAX_LOAD_SEG; ++i) {
                if (user_elf->user_elf_seg[i].elf_pmo == -1) {
                        /* reach the last LOAD segment */
                        break;
                }
                pmo_map_requests[2 + i].pmo_cap =
                        user_elf->user_elf_seg[i].elf_pmo;
                pmo_map_requests[2 + i].addr = ROUND_DOWN(
                        user_elf->user_elf_seg[i].p_vaddr, PAGE_SIZE);
                pmo_map_requests[2 + i].perm = user_elf->user_elf_seg[i].flags;
                pmo_map_requests[2 + i].free_cap = 1;
        }

        ret = usys_map_pmos(new_process_cap, (void *)pmo_map_requests, 2 + i, MALLOC_TYPE_DEFAULT);

        if (ret != 0) {
                printf("%s: fail to map_pmos (ret: %d)\n", __func__, ret);
                /* TODO(wzx): maybe part of map requests succeed. Deal with it.
                 */
                goto fail;
        }

        // printf("stack: 0x%lx\n", MAIN_THREAD_STACK_BASE + offset);

        if (nr_pmo_map_reqs) {
                ret = usys_map_pmos(
                        new_process_cap, (void *)pmo_map_reqs, nr_pmo_map_reqs, MALLOC_TYPE_DEFAULT);
                if (ret != 0) {
                        printf("%s: fail to map_pmos (ret: %d)\n",
                               __func__,
                               ret);
                        /* TODO(wzx): maybe part of map requests succeed. Deal
                         * with it. */
                        goto fail;
                }
        }
        /*
         * create main thread in the new process.
         * main_thread_cap is the cap can be used in current process.
         */
        args.cap_group_cap = new_process_cap;
        args.stack = MAIN_THREAD_STACK_BASE + offset;
        args.pc = pc + load_offset;
        args.arg = (u64)NULL;
        args.prio = MAIN_THREAD_PRIO;
        args.tls = cpuid;
        args.type = type;
        main_thread_cap = usys_create_thread((u64)&args);

        if (child_process_cap)
                *child_process_cap = new_process_cap;
        if (child_main_thread_cap)
                *child_main_thread_cap = main_thread_cap;
        // printf("[%s succeeds] main_thread (cap: %d) in the new process (cap:
        // %d), pc is 0x%lx\n",
        //        __func__, main_thread_cap, new_process_cap, pc);

        return 0;
fail:
        return -EINVAL;
}
