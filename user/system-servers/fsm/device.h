#pragma once

#include <chcore-internal/procmgr_defs.h>

#define SD_PARTITION_INFO_OFFSET 446
#define SD_PARTITION_INFO_SIZE 16

#define MAX_DEVICE_PARTITION 16

#define FAT32_PARTITION 0xc
#define EXT4_PARTITION 0x83

typedef struct {
	unsigned char boot;
	unsigned char head;
	unsigned char sector;
	unsigned char cylinder;
	unsigned char fs_id;
	unsigned char head_end;
	unsigned char sector_end;
	unsigned char cylinder_end;
	unsigned int lba;
	unsigned int total_sector;
} partition_struct_t;

typedef struct {
	char device_name[256];
	int partition_type;
	bool valid;
	int server_id;
	int partition_index;
	bool mounted;
	unsigned int partition_lba;
} device_partition_t;

int mount_storage_device(const char *device_name);
device_partition_t* chcore_get_server(const char *device_name);
int chcore_initial_fat_partition(int fs_cap, int partition);
void print_partition(partition_struct_t *p);
void print_devices();
