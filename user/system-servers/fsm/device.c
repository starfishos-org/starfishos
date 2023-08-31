#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <chcore-internal/fs_defs.h>
#include <chcore-internal/lwip_defs.h>
#include <chcore/ipc.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore-internal/sd_defs.h>
#include <chcore/services.h>

#include "device.h"

/*
 * partition informations
 * TODO: now only support sda1 sda2 sda3 sda4 for corresponding 4 sd partition
 */
static device_partition_t dparts[4];

/* lock to protect 'dparts' */
static pthread_mutex_t dparts_lock = PTHREAD_MUTEX_INITIALIZER;

/* whether fill the 'dparts' */
static bool dparts_inited = false;

/* TODO: device_name mapping */
static void initialize_dparts()
{
	memcpy(dparts[0].device_name, "sda1", 5);
	memcpy(dparts[1].device_name, "sda2", 5);
	memcpy(dparts[2].device_name, "sda3", 5);
	memcpy(dparts[3].device_name, "sda4", 5);
}

/* Get sd_card's first block (mbr) */
static void sd_read_mbr(char *mbr)
{
	struct sd_msg *sd_req;
	ipc_msg_t *sd_ipc_msg;
	ipc_struct_t *sd_ipc_struct;

	sd_ipc_struct = chcore_conn_srv(SERVER_SD_CARD);
	sd_ipc_msg = ipc_create_msg(sd_ipc_struct, sizeof(struct sd_msg), 0);
	sd_req = (struct sd_msg *)ipc_get_msg_data(sd_ipc_msg);
	sd_req->req = SD_REQ_READ;
	sd_req->op_size = 512;
	sd_req->block_number = 0;
	ipc_call(sd_ipc_struct, sd_ipc_msg);

	/* fill mbr */
	memcpy(mbr, sd_req->pbuffer, 512);
	ipc_destroy_msg(sd_ipc_msg);
}

static int chcore_parse_devices()
{
	int i;
	char mbr[512];

	/* check if 'dparts' ready */
	if (dparts_inited) {
		return 0;
	}

	/* fill 'mbr' from sd_card */
	sd_read_mbr(mbr);

	/* init 'dparts' */
	initialize_dparts();

	/* parse mbr entrys to get file system type */
	partition_struct_t *pinfo;
	for (i = 0; i < 4; i++) {
		pinfo = (partition_struct_t*)(mbr + SD_PARTITION_INFO_OFFSET + i * SD_PARTITION_INFO_SIZE);
		dparts[i].valid = pinfo->fs_id ? true : false;
		dparts[i].partition_index = i + 1;
		dparts[i].mounted = false;
		dparts[i].partition_lba = pinfo->lba;
		if (pinfo->fs_id == FAT32_PARTITION) {
			/* FAT32 */
			dparts[i].partition_type = FAT32_PARTITION;
			dparts[i].server_id = SERVER_FAT32_FS;
		} else if (pinfo->fs_id == EXT4_PARTITION) {
			/* EXT4 */
			dparts[i].partition_type = EXT4_PARTITION;
			dparts[i].server_id = SERVER_EXT4_FS;
		} else if (pinfo->fs_id == 0) {
			/* do nothing */
		} else {
			printf("[WARNING] not supported fs type %x\n", pinfo->fs_id);
		}
	}
	dparts_inited = true;

	return 0;
}

/*
 * @arg: 'device_name' is like 'sda1'...etc
 * @return: device_partition_t* dparts, 0 for any error
 */
device_partition_t* chcore_get_server(const char *device_name)
{
	int i;

	for (i = 0; i < 4; i++)
		if (strcmp(dparts[i].device_name, device_name) == 0)
			return &(dparts[i]);

	printf("[debug %d] can't find server id\n", __LINE__);
	return 0;
}

/*
 * @arg: 'fs_cap' is fatfs server_cap, 'partition' is partition_index in device
 * @return: error code returned by fatfs server
 */
int chcore_initial_fat_partition(int fs_cap, int partition)
{
	int ret;
	ipc_msg_t *ipc_msg;
	struct fs_request *fr_ptr;
	ipc_struct_t *ipc_struct = ipc_register_client(fs_cap);
	ipc_msg = ipc_create_msg(ipc_struct, sizeof(struct fs_request), 0);
	fr_ptr = (struct fs_request *)ipc_get_msg_data(ipc_msg);

	fr_ptr->req = FS_REQ_MOUNT;
	fr_ptr->mount.paritition = partition; // TODO: Store partition index in fd

	ipc_set_msg_data(ipc_msg, (char *)fr_ptr, 0, sizeof(struct fs_request));
	ret = ipc_call(ipc_struct, ipc_msg);
	ipc_destroy_msg(ipc_msg);

	return ret;
}

/*
 * Once start a ext4fs server, transform parititon offset to server using
 * FS_REQ_MOUNT
 * @arg: 'fs_cap' is ext4fs server_cap, 'partition_lba' is partition offset
 * @return: error code returned by ext4fs server
 */
int chcore_initial_ext4_partition(int fs_cap, uint64_t partition_lba)
{
	int ret;
	ipc_msg_t *ipc_msg;
	struct fs_request *fr_ptr;
	ipc_struct_t *ipc_struct = ipc_register_client(fs_cap);
	ipc_msg = ipc_create_msg(ipc_struct, sizeof(struct fs_request), 0);
	fr_ptr = (struct fs_request *)ipc_get_msg_data(ipc_msg);

	fr_ptr->req = FS_REQ_MOUNT;
	fr_ptr->mount.offset = partition_lba; // Store partition_lba in offset

	ipc_set_msg_data(ipc_msg, (char *)fr_ptr, 0, sizeof(struct fs_request));
	ret = ipc_call(ipc_struct, ipc_msg);
	ipc_destroy_msg(ipc_msg);

	return ret;
}

/*
 * @arg: 'device_name' is like 'sda1'.. etc
 * @return: corresponding fs server cap
 */
int mount_storage_device(const char *device_name)
{
	struct proc_request *proc_req;
	u64 fs_server_cap = 0;
	device_partition_t *fs_server;
	ipc_msg_t *proc_ipc_msg;

	/*
	 * Refill 'dparts' to refresh partition_information
	 * Write 'dparts' only first call 'chcore_parse_devices'
	 */
	pthread_mutex_lock(&dparts_lock);
	chcore_parse_devices();
	print_devices();
	pthread_mutex_unlock(&dparts_lock);

	/* prepare structs to IPC call FSM */
	proc_ipc_msg = ipc_create_msg(procmgr_ipc_struct, sizeof(struct proc_request), 0);
	proc_req = (struct proc_request *)ipc_get_msg_data(proc_ipc_msg);
	proc_req->req = PROC_REQ_GET_SERVER_CAP;

	/* get corresponding fs server id */
	fs_server = chcore_get_server(device_name);
	proc_req->server_id = fs_server->server_id;

	ipc_call(procmgr_ipc_struct, proc_ipc_msg);
	fs_server_cap = ipc_get_msg_cap(proc_ipc_msg, 0);
	ipc_destroy_msg(proc_ipc_msg);

	/* deal with different fs type */
	if (fs_server->server_id == SERVER_FAT32_FS) {
		/* FAT type */
		if (!fs_server->mounted) {
			chcore_initial_fat_partition(fs_server_cap, fs_server->partition_index);
			fs_server->mounted = true;
		}
	} else if (fs_server->server_id == SERVER_EXT4_FS) {
		/* EXT4 type */
		if (!fs_server->mounted) {
			chcore_initial_ext4_partition(fs_server_cap, fs_server->partition_lba);
		}
	} else {
		/* others */
		printf("[WARNING] Not supported fs type \n");
	}

	return fs_server_cap;
}

void print_partition(partition_struct_t *p)
{
	printf("partition infomation:\n");

	/* parse boot */
	if (p->boot == 0x0)
		printf("  boot: no\n");
	else if (p->boot == 0x80)
		printf("  boot: yes\n");
	else
		printf("  boot: (unknown)\n");

	/* print fs id */
	printf("  file system id: 0x%x\n", p->fs_id);

	/* parse fs type */
	if (p->fs_id == EXT4_PARTITION)
		printf("  file system: Linux File System\n");
	else if (p->fs_id == FAT32_PARTITION)
		printf("  file system: W95 FAT32 (LBA)\n");
	else if (p->fs_id == 0)
		printf("  file system: Unused\n");
	else
		printf("  file system: (unknown)\n");

	/* parse partition lba */
	printf("  partition lba: 0x%x = %d\n", p->lba, p->lba);

	/* parse partition size */
	printf("  partition size: 0x%x = %d\n", p->total_sector, p->total_sector);
}

void print_devices()
{
	/* Now, only support sd device */
	int i;
	printf("[SDCARD]\n");
	printf("Device\tServer\tPartition\tType\n");
	for (i = 0; i < 4; i++)
		if (dparts[i].valid)
			printf("%s\t%d\t%d\t%x\n", dparts[i].device_name, dparts[i].server_id,
				 dparts[i].partition_index, dparts[i].partition_type);
}
