/*
 * 0: print nothing
 * 1: print ext4_srv_dbg_base
 * 2: print ext4_srv_dbg_base and ext4_srv_dbg
 */
#define EXT_SERVER_DEBUG 0

#if EXT_SERVER_DEBUG >= 1
	#define ext4_srv_dbg_base(fmt, ...) printf("<%s:%d>: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
	#define ext4_srv_dbg_base(fmt, ...) do { } while (0)
#endif

#if EXT_SERVER_DEBUG >= 2
	#define ext4_srv_dbg(fmt, ...) printf("<%s:%d>: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
	#define ext4_srv_dbg(fmt, ...) do { } while (0)
#endif

