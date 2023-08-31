#include <chcore/type.h>

struct serial_ctrl_data {
	int set;
	int clear;
	int on;
};

int serial_ctrl(struct dev_desc *dev_desc, u64 request, void *data, int len);
