// fw_cfg.c - fw_cfg library

#include <common/types.h>
#include <lib/printk.h>
#include <common/util.h>
#include <lib/fw_cfg.h>
#include <dsm/dsm-single.h>
#include <common/size.h>

#define FW_CFG_PORT_SELECT 0x510
#define FW_CFG_PORT_DATA   0x511

#define FW_CFG_FILE_DIR    0x19  // Directory of available fw_cfg files

#define BOOT_ARGS_NAME "opt/chcore/bootargs"

int FW_MACHINE_ID = -1;
unsigned long long FW_TMP_SIZE_BYTES = SIZE_1G;
unsigned long long FW_DRAM_SIZE_BYTES = 0;
int FW_MACHINE_NUM = 0;
int FW_CPU_NUM = 0;

// fw_cfg directory entry format (big-endian fields)
struct fw_cfg_file {
    u32 size;   // file size
    u16 select; // selector number
    u16 reserved;
    char name[56];   // file name (may not be \0-terminated)
} __attribute__((packed));

// I/O helpers
static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline u16 be16_to_cpu(u16 val) {
    return (val >> 8) | (val << 8);
}

static inline u32 be32_to_cpu(u32 val) {
    return ((val >> 24) & 0xff) |
           ((val >> 8)  & 0xff00) |
           ((val << 8)  & 0xff0000) |
           ((val << 24) & 0xff000000);
}

// Read data from fw_cfg
void fw_cfg_read(u16 selector, void *buf, size_t len) {
    outw(FW_CFG_PORT_SELECT, selector);
    u8 *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = inb(FW_CFG_PORT_DATA);
    }
}

// Find the fw_cfg entry with the given name and read it
int fw_cfg_read_file(const char *target_name, void *buf, size_t buf_size) {
    u32 count;

    // 1. Read the number of directory entries
    outw(FW_CFG_PORT_SELECT, FW_CFG_FILE_DIR);
    count = inb(FW_CFG_PORT_DATA) << 24;
    count |= inb(FW_CFG_PORT_DATA) << 16;
    count |= inb(FW_CFG_PORT_DATA) << 8;
    count |= inb(FW_CFG_PORT_DATA);

    // 2. Walk the directory
    for (u32 i = 0; i < count; i++) {
        struct fw_cfg_file entry;
        for (size_t b = 0; b < sizeof(entry); b++) {
            ((u8*)&entry)[b] = inb(FW_CFG_PORT_DATA);
        }

        // Ensure name is \0-terminated
        char name_str[57] = {0};
        memcpy(name_str, entry.name, 56);

        if (strcmp(name_str, target_name) == 0) {
            u32 file_size = be32_to_cpu(entry.size);
            u16 selector = be16_to_cpu(entry.select);

            if (file_size >= buf_size)
                file_size = buf_size - 1;

            fw_cfg_read(selector, buf, file_size);
            ((char*)buf)[file_size] = '\0'; // ensure terminator when used as a string
            return file_size;
        }
    }
    return -1; // not found
}

static unsigned long long parse_size_bytes(const char *s)
{
    unsigned long long val = 0;
    const char *p = s;

    if (!p || !(*p))
        return 0;

    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (unsigned long long)(*p - '0');
        p++;
    }

    if (p == s)
        return 0;

    if (*p == '\0')
        return val;

    if ((p[0] == 'G' || p[0] == 'g') && p[1] == '\0')
        return val * SIZE_1G;
    if ((p[0] == 'M' || p[0] == 'm') && p[1] == '\0')
        return val * SIZE_1M;
    if ((p[0] == 'K' || p[0] == 'k') && p[1] == '\0')
        return val * SIZE_1K;

    return 0;
}

static void parse_bootargs_kv(char *buf)
{
    char *p = buf;

    while (p && *p) {
        char *tok = p;
        char *sep = NULL;
        char *eq = NULL;

        while (*p) {
            if ((*p == ',' || *p == ';') && !sep) {
                sep = p;
                break;
            }
            p++;
        }

        if (sep) {
            *sep = '\0';
            p = sep + 1;
        } else {
            p = NULL;
        }

        for (char *q = tok; *q; q++) {
            if (*q == '=') {
                eq = q;
                break;
            }
        }
        if (!eq)
            continue;

        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;

        if (!strcmp(key, "machine_id")) {
            unsigned long long id = parse_size_bytes(val);
            FW_MACHINE_ID = (int)id;
        } else if (!strcmp(key, "tmp_size")) {
            unsigned long long tmp = parse_size_bytes(val);
            if (tmp)
                FW_TMP_SIZE_BYTES = tmp;
        } else if (!strcmp(key, "dram_size")) {
            unsigned long long dram = parse_size_bytes(val);
            if (dram)
                FW_DRAM_SIZE_BYTES = dram;
        } else if (!strcmp(key, "machine_num")) {
            unsigned long long n = parse_size_bytes(val);
            FW_MACHINE_NUM = (int)n;
        } else if (!strcmp(key, "cpu_num")) {
            unsigned long long n = parse_size_bytes(val);
            FW_CPU_NUM = (int)n;
        }
    }
}

void fw_cfg_init(void) {
    char buf[256];
    int len = fw_cfg_read_file(BOOT_ARGS_NAME, buf, sizeof(buf));
    if (len > 0) {
        parse_bootargs_kv(buf);
        if (FW_MACHINE_ID < 0) {
            BUG("[FW_CFG] machine_id is negative\n");
        }
        kdebug("[FW_CFG] machine_id: %d, machine_num=%d, cpu_num=%d, tmp_size=0x%llx, dram_size=0x%llx\n",
               FW_MACHINE_ID, FW_MACHINE_NUM, FW_CPU_NUM, FW_TMP_SIZE_BYTES, FW_DRAM_SIZE_BYTES);
        CUR_MACHINE_ID = FW_MACHINE_ID;
    } else {
        kinfo("[FW_CFG] machine_id not found!\n");
        return;
    }
}
