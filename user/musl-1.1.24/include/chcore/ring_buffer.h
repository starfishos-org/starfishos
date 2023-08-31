#include <chcore/type.h>
#include <sys/types.h>

#define RING_BUFFER_FULL     1
#define RING_BUFFER_NOT_FULL 0
#define MSG_OP_SUCCESS       1
#define MSG_OP_FAILURE       0

/*
 * Ring buffer struct layout
 * buffer_size:                         size_t, size of whole struct including the meta data
 * consumer_offset, producer_offset:    int
 * msg_size:                            size_t, size of the msg that will be stored in the buffer
 * There is also a data buffer to actually store data, closely after the meta data above.
 */

struct ring_buffer {
        size_t buffer_size;
        off_t consumer_offset;
        off_t producer_offset;
        size_t msg_size;
};

int get_one_msg(struct ring_buffer *ring_buf, void *msg);
int set_one_msg(struct ring_buffer *ring_buf, void *msg);
int if_buffer_full(struct ring_buffer *ring_buf);
struct ring_buffer *new_ringbuffer(int msg_num, size_t msg_size);
void free_ringbuffer(struct ring_buffer *ring_buf);
