// fw_cfg.c - fw_cfg library

#include <common/types.h>
#include <lib/printk.h>
#include <common/util.h>
#include <lib/fw_cfg.h>

#define FW_CFG_PORT_SELECT 0x510
#define FW_CFG_PORT_DATA   0x511

#define FW_CFG_FILE_DIR    0x19  // Directory of available fw_cfg files

#define BOOT_ARGS_NAME "opt/chcore/bootargs"

int FW_MACHINE_ID = -1;

// fw_cfg directory entry format (big-endian fields)
struct fw_cfg_file {
    u32 size;   // 文件大小
    u16 select; // selector 编号
    u16 reserved;
    char name[56];   // 文件名（不一定以 \0 结束）
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

// 从 fw_cfg 读取数据
void fw_cfg_read(u16 selector, void *buf, size_t len) {
    outw(FW_CFG_PORT_SELECT, selector);
    u8 *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = inb(FW_CFG_PORT_DATA);
    }
}

// 查找指定 name 的 fw_cfg 项并读取
int fw_cfg_read_file(const char *target_name, void *buf, size_t buf_size) {
    u32 count;

    // 1. 读取目录项数量
    outw(FW_CFG_PORT_SELECT, FW_CFG_FILE_DIR);
    count = inb(FW_CFG_PORT_DATA) << 24;
    count |= inb(FW_CFG_PORT_DATA) << 16;
    count |= inb(FW_CFG_PORT_DATA) << 8;
    count |= inb(FW_CFG_PORT_DATA);

    // 2. 遍历目录
    for (u32 i = 0; i < count; i++) {
        struct fw_cfg_file entry;
        for (size_t b = 0; b < sizeof(entry); b++) {
            ((u8*)&entry)[b] = inb(FW_CFG_PORT_DATA);
        }

        // 确保 name 是 \0 结尾
        char name_str[57] = {0};
        memcpy(name_str, entry.name, 56);

        if (strcmp(name_str, target_name) == 0) {
            u32 file_size = be32_to_cpu(entry.size);
            u16 selector = be16_to_cpu(entry.select);

            if (file_size >= buf_size)
                file_size = buf_size - 1;

            fw_cfg_read(selector, buf, file_size);
            ((char*)buf)[file_size] = '\0'; // 作为字符串用时确保结束符
            return file_size;
        }
    }
    return -1; // not found
}


// format: machine_id=1
void fw_cfg_init(void) {
    char buf[256];
    int len = fw_cfg_read_file(BOOT_ARGS_NAME, buf, sizeof(buf));
    if (len > 0) {
        sscanf(buf, "machine_id=%d", &FW_MACHINE_ID);
        if (FW_MACHINE_ID < 0) {
            BUG("[FW_CFG] machine_id is negative\n");
        }
        printk("[FW_CFG] machine_id: %d\n", FW_MACHINE_ID);
    } else {
        printk("[FW_CFG] machine_id not found!\n");
        return;
    }
}
