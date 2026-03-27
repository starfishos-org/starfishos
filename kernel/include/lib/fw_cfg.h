#pragma once

/*
 * Values parsed from QEMU fw_cfg bootargs:
 *   machine_id=<n>[,machine_num=<n>][,cpu_num=<n>][,tmp_size=<size>][,dram_size=<size>]
 * Size examples: 1G, 512M, 4096K, or raw bytes (decimal).
 */
extern int FW_MACHINE_ID;
extern unsigned long long FW_TMP_SIZE_BYTES;
extern unsigned long long FW_DRAM_SIZE_BYTES;
extern int FW_MACHINE_NUM;
extern int FW_CPU_NUM;
void fw_cfg_init(void);
