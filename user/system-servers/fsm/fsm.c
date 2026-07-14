#include "fsm.h"
#include "device.h"
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore/string.h>
#include <chcore-internal/fs_defs.h>

#include "fsm_client_cap.h"
#include "mount_info.h"

int fs_num = 0;
pthread_mutex_t fsm_client_cap_table_lock;
pthread_rwlock_t mount_point_infos_rwlock;

/* Initialize when fsm start */
static inline void init_utils()
{
	init_list_head(&fsm_client_cap_table);
	pthread_mutex_init(&fsm_client_cap_table_lock, NULL);
	init_list_head(&mount_point_infos);
	pthread_rwlock_init(&mount_point_infos_rwlock, NULL);
}


int init_fsm(void)
{
	int ret;

	/* Initialize */
	init_utils();

	ret = fsm_mount_fs("/cxlfs.srv", "/");
	if (ret < 0) {
		error("failed to mount default CXLFS, ret %d\n", ret);
		usys_exit(-1);
	}

	return 0;
}

static int get_cxlfs_cap()
{
	struct proc_request pr;
	ipc_msg_t *ipc_msg;
	int ret;

	ipc_msg = ipc_create_msg(procmgr_ipc_struct, sizeof(struct proc_request), 0);
	pr.req = PROC_REQ_GET_SERVER_CAP;
	pr.server_id = SERVER_CXLFS;

	ipc_set_msg_data(ipc_msg, &pr, 0, sizeof(pr));
	ret = ipc_call(procmgr_ipc_struct, ipc_msg);

	if (ret < 0) {
		goto out;
	}

	ret = ipc_get_msg_cap(ipc_msg, 0);
out:
	ipc_destroy_msg(ipc_msg);
	return ret;
}

static int verify_cxlfs_server(ipc_struct_t *ipc_struct, int fs_cap)
{
	ipc_msg_t *msg = ipc_create_msg(ipc_struct,
					 sizeof(struct fs_request), 0);
	if (!msg)
		return -ENOMEM;
	struct fs_request *fr =
		(struct fs_request *)ipc_get_msg_data(msg);
	memset(fr, 0, sizeof(*fr));
	fr->req = FS_REQ_GET_FS_STATUS;
	int ret = ipc_call(ipc_struct, msg);
	struct fs_server_status status = fr->status;
	ipc_destroy_msg(msg);
	if (ret < 0)
		return ret;
	if (status.magic != FS_SERVER_STATUS_MAGIC ||
	    status.backend != FS_BACKEND_CXLFS || !status.mounted ||
	    !status.data_valid || status.generation == 0 ||
	    status.root_ino == 0 || status.data_checksum == 0 ||
	    status.owner_machine != usys_get_machine_id()) {
		error("[FSM] Invalid CXLFS status: cap=%d magic=%x backend=%u "
		      "mounted=%u data=%u owner=%d generation=%lu root=%lu\n",
		      fs_cap, status.magic, status.backend, status.mounted,
		      status.data_valid, status.owner_machine,
		      (unsigned long)status.generation,
		      (unsigned long)status.root_ino);
		return -EIO;
	}
	info("[FSM] Default CXLFS verified: cap=%d owner=%d generation=%lu "
	     "root=%lu data=ok checksum=%lx fresh=%u\n",
	     fs_cap, status.owner_machine, (unsigned long)status.generation,
	     (unsigned long)status.root_ino,
	     (unsigned long)status.data_checksum, status.fresh);
	return 0;
}

int fsm_mount_fs(const char *path, const char *mount_point)
{
	int fs_cap, ret;
	struct mount_point_info_node *mp_node;
	int machine_id;

	ret = -1;
	if (fs_num == MAX_FS_NUM) {
		error("maximal number of FSs is reached: %d\n", fs_num);
		goto out;
	}

	if (strlen(mount_point) > MAX_MOUNT_POINT_LEN) {
		error("mount point too long: > %d\n", MAX_MOUNT_POINT_LEN);
		goto out;
	}

	if (mount_point[0] != '/') {
		error("mount point should start with '/'\n");
		goto out;
	}

	if (strcmp(path, "/cxlfs.srv") == 0) {
		/* The default CXLFS process is launched and owned by procmgr. */
		info("Mounting fs from local binary: %s...\n", path);

		fs_cap = get_cxlfs_cap();
		info("Mounting CXLFS, cap = %d\n", fs_cap);

		if (fs_cap <= 0) {
			info("Failed to obtain CXLFS cap %d\n", fs_cap);
			goto out;
		}

		pthread_rwlock_wrlock(&mount_point_infos_rwlock);
		machine_id = usys_get_machine_id();
		mp_node = set_mount_point("/", 1, fs_cap, machine_id);

		// Register remote fs and server
		for (int i = 0; i < MAX_REMOTE_MACHINE_NUM; i++) {
			char path[MAX_MOUNT_POINT_LEN + 1];
			snprintf(path, sizeof(path), "/%d", i);
			set_mount_point(path, strlen(path), -1, i);
		}
		usys_register_fs_server(fs_cap);

		info("CXLFS process cap acquired: %d\n", fs_cap);
	} else {
		fs_cap = mount_storage_device(path);
		pthread_rwlock_wrlock(&mount_point_infos_rwlock);
		mp_node = set_mount_point(mount_point, strlen(mount_point), fs_cap, -1);
	}

	/* Connect to the FS that we mount now. */
	mp_node->_fs_ipc_struct = ipc_register_client(mp_node->fs_cap);

	if (mp_node->_fs_ipc_struct == NULL) {
		info("ipc_register_client failed\n");
		BUG_ON(remove_mount_point(mp_node->path) != 0);
		pthread_rwlock_unlock(&mount_point_infos_rwlock);
		goto out;
	}
	if (!strcmp(path, "/cxlfs.srv")) {
		ret = verify_cxlfs_server(mp_node->_fs_ipc_struct, fs_cap);
		if (ret < 0) {
			BUG_ON(remove_mount_point(mp_node->path) != 0);
			pthread_rwlock_unlock(&mount_point_infos_rwlock);
			goto out;
		}
	}

	strlcpy(mp_node->path, mount_point, sizeof(mp_node->path));

	fs_num++;
	ret = 0;
	pthread_rwlock_unlock(&mount_point_infos_rwlock);

out:
	return ret;
}

/*
 * @args: 'path' is device name, like 'sda1'...
 * send FS_REQ_UMOUNT to corresponding fs_server
 */
int fsm_umount_fs(const char *path)
{
	int fs_cap;
	int ret;
	ipc_msg_t *ipc_msg;
	ipc_struct_t *ipc_struct;
	struct fs_request *fr_ptr;

	pthread_rwlock_wrlock(&mount_point_infos_rwlock);

	/* get corresponding fs_server_cap by device name */
	/* TODO(HYQ): embarrassing function name... */
	fs_cap = mount_storage_device(path);

	ipc_struct = ipc_register_client(fs_cap);
	ipc_msg = ipc_create_msg(ipc_struct, sizeof(struct fs_request), 0);
	fr_ptr = (struct fs_request *)ipc_get_msg_data(ipc_msg);

	fr_ptr->req = FS_REQ_UMOUNT;

	ipc_set_msg_data(ipc_msg, (char *)fr_ptr, 0, sizeof(struct fs_request));
	ret = ipc_call(ipc_struct, ipc_msg);
	ipc_destroy_msg(ipc_msg);

	/* TODO(HYQ): reclaim resources and ensure fd closed */

	pthread_rwlock_unlock(&mount_point_infos_rwlock);

	return ret;
}

/*
 * @args: 'path' is device name, like 'sda1'...
 * send FS_REQ_UMOUNT to corresponding fs_server
 */
int fsm_sync_page_cache(void)
{
	ipc_msg_t *ipc_msg;
	ipc_struct_t *ipc_struct;
	struct fs_request *fr_ptr;
	struct mount_point_info_node *iter;
	int ret = 0;

	for_each_in_list(iter, struct mount_point_info_node, node, &mount_point_infos) {
		ipc_struct = iter->_fs_ipc_struct;
		ipc_msg = ipc_create_msg(ipc_struct, sizeof(struct fs_request), 0);
		fr_ptr = (struct fs_request *)ipc_get_msg_data(ipc_msg);

		fr_ptr->req = FS_REQ_SYNC;

		ret = ipc_call(ipc_struct, ipc_msg);
		ipc_destroy_msg(ipc_msg);
		if (ret != 0) {
			printf("Failed to sync in %s\n", iter->path);
			goto out;
		}
	}

out:
	return ret;
}

/*
 * Types in the following two functions would conflict with existing builds,
 * I suggest to move the tmpfs code out of kernel tree to resolve this.
 */

void fsm_dispatch(ipc_msg_t *ipc_msg, u64 client_badge)
{
	int ret = 0;
	struct fsm_request *fsm_req;
	struct mount_point_info_node *mpinfo;
	int mount_id;
	bool ret_with_cap = false;

	if (ipc_msg->data_len >= 4) {
		fsm_req = (struct fsm_request *)ipc_get_msg_data(ipc_msg);

		switch (fsm_req->req) {
		case FSM_REQ_PARSE_PATH: {

			/*
			 * Validate the client-supplied path: it must be
			 * NUL-terminated within the buffer and absolute
			 * (start with '/'). Otherwise get_mount_point would
			 * hit BUG_ON(matched_fs == NULL) and abort fsm (DoS),
			 * and an unterminated path would overrun strlen.
			 */
			size_t parse_path_len =
				strnlen(fsm_req->path, FS_REQ_PATH_BUF_LEN);
			if (parse_path_len == 0
			    || parse_path_len >= FS_REQ_PATH_BUF_LEN
			    || fsm_req->path[0] != '/') {
				ipc_return(ipc_msg, -EINVAL);
			}

			// Get Corresponding MOUNT_INFO
			pthread_rwlock_rdlock(&mount_point_infos_rwlock);
			mpinfo = get_mount_point(fsm_req->path, parse_path_len);
			pthread_mutex_lock(&fsm_client_cap_table_lock);
			if (mpinfo->fs_cap == -1) {
				mount_id = mpinfo->target_machine_id + MAX_MOUNT_ID;
			} else {
				mount_id = fsm_get_client_cap(client_badge, mpinfo->fs_cap);
			}

			if (mount_id == -1) {
				/* Client not hold corresponding fs_cap */

				// Newly generated mount_id
				mount_id = fsm_set_client_cap(client_badge, mpinfo->fs_cap);
				pthread_mutex_unlock(&fsm_client_cap_table_lock);

				// Filling responses
				fsm_req->mount_id = mount_id;
				strncpy(fsm_req->mount_path, mpinfo->path, mpinfo->path_len);
				fsm_req->mount_path[mpinfo->path_len] = '\0';
				fsm_req->mount_path_len = mpinfo->path_len;
				if (fsm_req->mount_path_len == 1)
					fsm_req->mount_path_len = 0;
				fsm_req->new_cap_flag = 1;
				if (strstr(fsm_req->path, "leveldb_recovery"))
					info("[FSM_TRACE] path=%s badge=%lx cap=%d mount=%d new=1\n",
					     fsm_req->path, client_badge,
					     mpinfo->fs_cap, mount_id);

				// Return with cap
				pthread_rwlock_unlock(&mount_point_infos_rwlock);
				ipc_msg->cap_slot_number = 1;
				ipc_set_msg_cap(ipc_msg, 0, mpinfo->fs_cap);
				ipc_return_with_cap(ipc_msg, 0);
			} else {
				/* Client holds corresponding fs_cap */
				pthread_mutex_unlock(&fsm_client_cap_table_lock);
				fsm_req->mount_id = mount_id;
				strncpy(fsm_req->mount_path, mpinfo->path, mpinfo->path_len);
				fsm_req->mount_path[mpinfo->path_len] = '\0';
				fsm_req->mount_path_len = mpinfo->path_len;
				if (fsm_req->mount_path_len == 1)
					fsm_req->mount_path_len = 0;
				fsm_req->new_cap_flag = 0;
				if (strstr(fsm_req->path, "leveldb_recovery"))
					info("[FSM_TRACE] path=%s badge=%lx cap=%d mount=%d new=0\n",
					     fsm_req->path, client_badge,
					     mpinfo->fs_cap, mount_id);

				pthread_rwlock_unlock(&mount_point_infos_rwlock);
				ipc_return(ipc_msg, 0);
			}
			break;
		}
		case FSM_REQ_MOUNT: {
			ret = fsm_mount_fs(fsm_req->path, fsm_req->mount_path); // path=(device_name), path2=(mount_point)
			break;
		}
		case FSM_REQ_UMOUNT: {
			ret = fsm_umount_fs(fsm_req->path);
			break;
		}
		case FSM_REQ_SYNC: {
			ret = fsm_sync_page_cache();
			break;
		}
		case FSM_CHILD_FINISH_FORK: {
			ipc_msg_t *finish_ipc_msg;
			ipc_struct_t *ipc_struct;
			struct fs_request *fr_ptr;
			struct mount_point_info_node* iter;
			for_each_in_list(iter, struct mount_point_info_node, node, &mount_point_infos) {
				ipc_struct = iter->_fs_ipc_struct;
				finish_ipc_msg = ipc_create_msg(ipc_struct, sizeof(struct fs_request), 0);
				fr_ptr = (struct fs_request *)ipc_get_msg_data(finish_ipc_msg);
				fr_ptr->req = FS_CHILD_FINISH_FORK;
				fr_ptr->fork.parentBagde = fsm_req->parentBagde;
				fr_ptr->fork.childBadge = client_badge;
				ret = ipc_call(ipc_struct, finish_ipc_msg);
				ipc_destroy_msg(finish_ipc_msg);
			}
			break;
		}
		case FSM_REQ_REPLACE_FS: {
			/* Recovery: replace mount point with new FS instance */
			int new_cap = ipc_msg->cap_slot_number > 0
				? ipc_get_msg_cap(ipc_msg, 0) : -EINVAL;
			/*
			 * A shell-launched recovery server cannot transfer a cap to
			 * its own main thread.  Procmgr owns that cap and publishes it
			 * as the current CXLFS server when the process is launched.
			 */
			if (new_cap <= 0)
				new_cap = get_cxlfs_cap();
			if (new_cap <= 0) {
				error("[FSM] REPLACE_FS: no replacement server cap\n");
				ret = -1;
				break;
			}
			pthread_rwlock_wrlock(&mount_point_infos_rwlock);
			struct mount_point_info_node *mp =
				get_mount_point(fsm_req->mount_path,
						strlen(fsm_req->mount_path));
			if (mp) {
				mp->fs_cap = new_cap;
				mp->_fs_ipc_struct = ipc_register_client(new_cap);
				mp->target_machine_id = fsm_req->target_machine_id;
				usys_register_fs_server(new_cap);
				info("[FSM] Replaced FS for %s (new_cap=%d, mid=%d)\n",
				     mp->path, new_cap, mp->target_machine_id);
				ret = 0;
			} else {
				error("[FSM] REPLACE_FS: mount point %s not found\n",
				      fsm_req->mount_path);
				ret = -1;
			}
			pthread_rwlock_unlock(&mount_point_infos_rwlock);
			break;
		}
		default:
			error("%s: %d Not impelemented yet\n", __func__,
			      ((int *)(ipc_get_msg_data(ipc_msg)))[0]);
			usys_exit(-1);
			break;
		}
	} else {
		error("FSM: no operation num\n");
		usys_exit(-1);
	}

	if (ret_with_cap)
		ipc_return_with_cap(ipc_msg, ret);
	else
		ipc_return(ipc_msg, ret);
}

int main(int argc, char *argv[], char *envp[])
{
	init_fsm();
	info("[FSM] register server value = %u\n",
	     ipc_register_server(fsm_dispatch,
				 DEFAULT_CLIENT_REGISTER_HANDLER));

	usys_exit(0);
	return 0;
}
