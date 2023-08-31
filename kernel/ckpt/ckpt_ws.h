#include <common/types.h>
#include <common/list.h>
#include <ckpt/ckpt_data.h>

typedef u64 timestamp_t;

#define MAX_CKPT_NAME_LEN (64)

#define KVS_SIZE (503)

/* 
* store ckpt_ws_info in list, sorted by ts
* use ckpt_id as key to map to ckpt_data
*/
struct ckpt_ws_info {
	struct list_head node;
	struct list_head kvs_val_node;
	struct ckpt_ws_data *ckpt_data; // ckpt_ws_data ptr
	timestamp_t ts;
	u64 name_len;
	char name[MAX_CKPT_NAME_LEN];
};

typedef struct ckpt_ws_info_list {
	struct list_head list;
	struct ckpt_ws_info *ckpt_info;
} ckpt_ws_info_list_t;

/* init */
int ckpt_ws_init(void);

/* get and put */
struct ckpt_ws_data *ckpt_ws_get(u64 ckpt_id);
struct ckpt_ws_data *ckpt_ws_get_latest();
u64 ckpt_ws_put(struct ckpt_ws_data *ckpt_data, u64 name_buf, u64 len);

/* query utils */
struct ckpt_ws_info *ckpt_ws_query_by_name(char *name, u64 name_len);
ckpt_ws_info_list_t ckpt_ws_query_by_time(timestamp_t st, timestamp_t et);
ckpt_ws_info_list_t ckpt_ws_query_latest(u64 cnt);
