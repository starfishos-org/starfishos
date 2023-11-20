#include <common/types.h>

// CXL TYPE3
struct cxl_mem_dev {
        u64 start;
        u64 size;
};

struct cxl_mem_dev cxl_mem_devs[12];
int cxl_mem_dev_num;

inline u64 get_cxl_mem_dev_end(struct cxl_mem_dev *dev)
{
        BUG_ON(!dev);
        return dev->start + dev->size;
}
