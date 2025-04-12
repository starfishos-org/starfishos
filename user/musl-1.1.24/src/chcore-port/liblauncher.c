#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chcore/bug.h>
#include <chcore/defs.h>
#include <chcore/elf.h>
#include <chcore-internal/fs_defs.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore/proc.h>
#include <chcore/ipc.h>
#include <chcore/syscall.h>
#include <chcore/launcher.h>
#include <chcore/memory.h>

/* Callers need to free binary if the return value is 0. */
static int fs_read(const char *path, char **binary)
{
        int fd, ret, byte_read;
        struct stat statbuf;

        ret = -1;
        if ((fd = openat(AT_FDCWD, path, O_RDONLY)) < 0) {
                goto out;
        }

        if ((ret = fstat(fd, &statbuf)) < 0) {
                goto out_close;
        }

        if ((*binary = malloc(statbuf.st_size)) == NULL) {
                goto out_close;
        }

        byte_read = 0;
        while (byte_read != statbuf.st_size) {
                if ((ret = read(fd,
                                *binary + byte_read,
                                statbuf.st_size - byte_read))
                    < 0) {
                        ret = -1;
                        goto out_free;
                }

                byte_read += ret;
        }
        ret = 0;
        goto out_close;

out_free:
        free(*binary);
out_close:
        close(fd);
out:
        return ret;
}

#define PFLAGS2VMRFLAGS(PF)                                     \
        (((PF)&PF_X ? VM_EXEC : 0) | ((PF)&PF_W ? VM_WRITE : 0) \
         | ((PF)&PF_R ? VM_READ : 0))

#define OFFSET_MASK 0xfff

int is_dyn_loader(const char *bin_name)
{
        return (strncmp(bin_name, CHCORE_LOADER, strlen(CHCORE_LOADER)) == 0
                || strncmp(bin_name, LDD_NAME, strlen(LDD_NAME)) == 0) ?
                       1 :
                       0;
}

int parse_elf_from_binary(const char *binary, struct user_elf *user_elf, bool is_cross_machine)
{
        int ret;
        struct elf_file *elf;
        size_t seg_sz, seg_map_sz;
        u64 p_vaddr;
        int i;
        int j;
        u16 e_type;
        void *tmp_seg;

        u64 start;
        u64 size;

        elf = elf_parse_file(binary);

        if ((elf->header.e_type != ET_EXEC) && (elf->header.e_type != ET_DYN)) {
                printf("%s is non-runnable.\n");
                if (elf)
                        elf_free(elf);
                return -1;
        }

        if ((elf->header.e_type == ET_DYN) && !is_dyn_loader(user_elf->path)) {
                if (elf)
                        elf_free(elf);
                return ET_DYN;
        }

        /* init pmo, -1 indicates that this pmo is not used */
        for (i = 0; i < ELF_MAX_LOAD_SEG; ++i)
                user_elf->user_elf_seg[i].elf_pmo = -1;

        for (i = 0, j = 0; i < elf->header.e_phnum; ++i) {
                if (elf->p_headers[i].p_type != PT_LOAD)
                        continue;

                if (j >= ELF_MAX_LOAD_SEG) {
                        BUG("FIXME: too many PT_LOAD segments");
                }

                seg_sz = elf->p_headers[i].p_memsz;
                p_vaddr = elf->p_headers[i].p_vaddr;
                BUG_ON(elf->p_headers[i].p_filesz > seg_sz);
                seg_map_sz = ROUND_UP(seg_sz + p_vaddr, PAGE_SIZE)
                             - ROUND_DOWN(p_vaddr, PAGE_SIZE);

                user_elf->user_elf_seg[j].elf_pmo =
                        usys_create_pmo(seg_map_sz, PMO_CODE, 
                        is_cross_machine ? MALLOC_TYPE_SHARED : MALLOC_TYPE_DEFAULT);
                BUG_ON(user_elf->user_elf_seg[j].elf_pmo < 0);

                tmp_seg = chcore_auto_map_pmo(user_elf->user_elf_seg[j].elf_pmo,
                                              seg_map_sz,
                                              VM_READ | VM_WRITE);
                BUG_ON(!tmp_seg);

                memset(tmp_seg, 0, seg_map_sz);
                /*
                 * OFFSET_MASK is for calculating the final offset for loading
                 * different segments from ELF.
                 * ELF segment can specify not aligned address.
                 *
                 */
                start = (u64)tmp_seg
                        + (elf->p_headers[i].p_vaddr & OFFSET_MASK);
                size = elf->p_headers[i].p_filesz;
                memcpy((void *)start,
                       (void *)(binary + elf->p_headers[i].p_offset),
                       size);

                user_elf->user_elf_seg[j].seg_sz = seg_sz;
                user_elf->user_elf_seg[j].p_vaddr = p_vaddr;
                user_elf->user_elf_seg[j].flags =
                        PFLAGS2VMRFLAGS(elf->p_headers[i].p_flags);

                /*
                 * After loading code, flushing data cache or invalidate
                 * instruction cache is needed on some archs like ARM.
                 */
                if ((user_elf->user_elf_seg[j].flags) & VM_EXEC) {
                        usys_cache_flush(start, size, SYNC_IDCACHE);
                }

                chcore_auto_unmap_pmo(user_elf->user_elf_seg[j].elf_pmo,
                                      (u64)tmp_seg,
                                      seg_map_sz);
                j++;
        }

        user_elf->elf_meta.phdr_addr =
                elf->p_headers[0].p_vaddr + elf->header.e_phoff;
        user_elf->elf_meta.phentsize = elf->header.e_phentsize;
        user_elf->elf_meta.phnum = elf->header.e_phnum;
        user_elf->elf_meta.flags = elf->header.e_flags;
        user_elf->elf_meta.entry = elf->header.e_entry;
        user_elf->elf_meta.type = elf->header.e_type;

        e_type = elf->header.e_type;
        elf_free(elf);
        return e_type;
}

int readelf_from_vaddr(struct user_elf *user_elf, size_t length, void *start, bool is_cross_machine)
{
        /* Currently, only static libraries are dealt with. */
        int ret;
        char *binary;

        binary = malloc(length);
        memcpy(binary, start, length);
        ret = parse_elf_from_binary(binary, user_elf, is_cross_machine);
        free(binary);
        return ret;
}

int readelf_from_fs(const char *pathbuf, struct user_elf *user_elf, bool is_cross_machine)
{
        int ret;
        char *binary;

        /*
         * Read the binary first.
         * TODO: Just read the file header first, read the whole binary may be
         * useless.
         */
        if ((ret = fs_read(pathbuf, &binary)) < 0) {
                return ret;
        }

        strcpy(user_elf->path, pathbuf);
        ret = parse_elf_from_binary(binary, user_elf, is_cross_machine);

        /*
         * If the binary is dynamic, we free the binary and read the
         * dynamic_loader (libc.so). In such case, we will execute libc.so and
         * pass @pathbuf as an argument later.
         *
         * If the binary is libc.so or ldd (although ET_DYN),
         * we do not need to read libc.so again.
         */
        if ((ret == ET_DYN) && !is_dyn_loader(pathbuf)) {
                free(binary);

                memset((void *)user_elf, 0, sizeof(*user_elf));

                if ((ret = fs_read(CHCORE_LOADER, &binary)) < 0) {
                        printf("func (%s) failed in loading CHCORE_LOADER (%s)\n",
                               __func__,
                               CHCORE_LOADER);
                        return ret;
                }
                strcpy(user_elf->path, CHCORE_LOADER);
                ret = parse_elf_from_binary(binary, user_elf, is_cross_machine);
        }

        free(binary);
        return ret;
}

pid_t chcore_new_process(int argc, char *__argv[], int is_bbapplet, int is_cross_machine)
{
        struct user_elf user_elf;
        int ret;
        int caps[3];
        const int MAX_ARGC = 128;
        char *argv[MAX_ARGC];
        int i;
        int argv_start;
        struct proc_request *pr;
        ipc_msg_t *ipc_msg;
        int text_i;

        assert(argc + 1 < PROC_REQ_ARGC_MAX);
        /*
         * Reserve argv[0] for busybox applets.
         * Dynamic loaders are handled by procmgr.
         */
        argv_start = 1;
        for (i = 0; i < argc; ++i)
                argv[i + argv_start] = __argv[i];

        if (is_bbapplet) {
                /* This is a busybox applet. Invoke busybox. */
                argv[0] = "/busybox";
                argv_start = 0;
                argc += 1;
        }

        ipc_msg = ipc_create_msg(
                procmgr_ipc_struct, sizeof(struct proc_request), 0);
        pr = (struct proc_request *)ipc_get_msg_data(ipc_msg);

        pr->req = PROC_REQ_NEWPROC;
        pr->argc = argc;
        pr->is_cross_machine = is_cross_machine;

        /* Dump argv[] to the text area of the pr. */
        for (i = 0, text_i = 0; i < argc; ++i) {
                /* Plus 1 for the trailing \0. */
                int len = strlen(argv[argv_start + i]) + 1;
                assert(text_i + len <= PROC_REQ_TEXT_SIZE);

                memcpy(&pr->text[text_i], argv[argv_start + i], len);

                pr->argv[i] = text_i;
                text_i += len;
        }

        ret = ipc_call(procmgr_ipc_struct, ipc_msg);
        ipc_destroy_msg(ipc_msg);

        return ret;
}

int get_new_process_mt_cap(int pid)
{
        ipc_msg_t *ipc_msg;
        struct proc_request *pr;
        int mt_cap;

        ipc_msg = ipc_create_msg(
                procmgr_ipc_struct, sizeof(struct proc_request), 1);

        pr = (struct proc_request *)ipc_get_msg_data(ipc_msg);
        pr->req = PROC_REQ_GET_MT_CAP;
        pr->pid = pid;

        ipc_call(procmgr_ipc_struct, ipc_msg);
        mt_cap = ipc_get_msg_cap(ipc_msg, 0);
        ipc_destroy_msg(ipc_msg);

        return mt_cap;
}

int create_process(int argc, char *__argv[], struct new_process_caps *caps, bool is_cross_machine)
{
        pid_t pid;
        pid = chcore_new_process(argc, __argv, 0, is_cross_machine);
        if (caps != NULL)
                caps->mt_cap = get_new_process_mt_cap(pid);
        return pid;
}
