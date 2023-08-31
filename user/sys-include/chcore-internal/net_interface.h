#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NET_DRIVER_DATA_LEN 2048
#define NET_DRIVER_ARGS_NUM 8

#define NET_DRIVER_RET_LINK_DOWN   -100
#define NET_DRIVER_RET_NO_FRAME    -101
#define NET_DRIVER_RET_SEND_FAILED -102

enum NET_DRIVER_REQ {
	NET_DRIVER_WAIT_LINK_UP,
	NET_DRIVER_RECEIVE_FRAME,
	NET_DRIVER_SEND_FRAME,
};

struct net_driver_request {
	enum NET_DRIVER_REQ req;
	unsigned long args[NET_DRIVER_ARGS_NUM];
	char data[NET_DRIVER_DATA_LEN];
};

enum NET_INTERFACE_TYPE {
	NET_INTERFACE_ETHERNET,
	NET_INTERFACE_WLAN,
};

struct __attribute__((packed)) net_interface {
	enum NET_INTERFACE_TYPE type;
	char mac_address[6];
};

#ifdef __cplusplus
}
#endif
