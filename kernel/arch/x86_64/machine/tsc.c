#include <arch/io.h>
#include <arch/time.h>

#define PIT_MODE_PORT		0x43
#define PIT_COUNTER0_PORT	0x40

#define PIT_MODE0		0
#define PIT_SEL_COUNTER0	0
#define PIT_SEL_COUNTER_READ	0xe0
#define PIT_RW_16BIT		0x30
#define PIT_STATUS_OUT		0x80

#define PIT_FREQ		1193182

u64 get_tsc_through_pit()
{
	u64 begin, end;
	u8 out;

	/* Use mode 0 and counter 0. Set 16 bit counter in the following. */
	put8(PIT_MODE_PORT, PIT_MODE0 | PIT_SEL_COUNTER0 | PIT_RW_16BIT);

	/* Set counter as 0xffff */
	put8(PIT_COUNTER0_PORT, 0xff);
	put8(PIT_COUNTER0_PORT, 0xff);

	begin = get_cycles();
	while (1) {
		/* Read counter 0 in the following */
		put8(PIT_MODE_PORT, PIT_SEL_COUNTER_READ | 2);

		/* Read OUT pin until it is set */
		out = get8(PIT_COUNTER0_PORT);
		if (out & PIT_STATUS_OUT)
			break;
	}
	end = get_cycles();

	return (end - begin) * PIT_FREQ / 0xffff;
}
