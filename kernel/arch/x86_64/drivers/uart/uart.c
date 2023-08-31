#include <common/types.h>

#include <arch/io.h>
#include <io/uart.h>

#define COM1    0x3f8
#define COM2    0x2f8

#define IRQ_COM1 4
#define IRQ_COM2 4

#define COM_IN_RECEIVE             0	// DLAB = 0, in
#define COM_OUT_TRANSMIT           0	// DLAB = 0, out
#define COM_INT_EN                 1	// DLAB = 0
#define COM_INT_RECEIVE            0x01
#define COM_INT_TRANSMIT           0x02
#define COM_INT_LINE               0x04
#define COM_INT_MODEM              0x08
#define COM_DIVISOR_LSB            0	// DLAB = 1
#define COM_DIVISOR_MSB            1	// DLAB = 1
#define COM_IN_IIR                 2
#define COM_IN_IIR_NOT_PENDING     0x01
#define COM_IN_IIR_ID_MASK         0x0E
#define COM_OUT_FIFO_CTL           2
#define COM_OUT_FIFO_ENABLE        0x01
#define COM_OUT_FIFO_RECV_RESET    0x02
#define COM_OUT_FIFO_XMIT_RESET    0x04
#define COM_OUT_FIFO_DMA           0x08
#define COM_OUT_FIFO_TRIGGER_MASK  0xC0
#define COM_OUT_FIFO_TRIGGER_1     (0<<6)
#define COM_OUT_FIFO_TRIGGER_4     (1<<6)
#define COM_OUT_FIFO_TRIGGER_8     (2<<6)
#define COM_OUT_FIFO_TRIGGER_14    (3<<6)
#define COM_LINE_CTL               3
#define COM_LINE_LEN_MASK          0x03
#define COM_LINE_LEN_5             0
#define COM_LINE_LEN_6             1
#define COM_LINE_LEN_7             2
#define COM_LINE_LEN_8             3
#define COM_LINE_STOP_BITS         0x04
#define COM_LINE_PARITY            0x08
#define COM_LINE_EVEN_PARITY       0x10
#define COM_LINE_STICK_PARITY      0x20
#define COM_LINE_BREAK_CTL         0x40
#define COM_LINE_DLAB              0x80
#define COM_MODEM_CTL              4
#define COM_LINE_STATUS            5
#define COM_LINE_DATA_READY        0x01
#define COM_LINE_OVERRUN_ERR       0x02
#define COM_LINE_PARITY_ERR        0x04
#define COM_LINE_FRAMING_ERR       0x08
#define COM_LINE_BREAK             0x10
#define COM_LINE_XMIT_HOLDING      0x20
#define COM_LINE_XMIT_EMPTY        0x40
#define COM_LINE_FIFO_ERR          0x80
#define COM_MODEM_STATUS           6
#define COM_SCRATCH                7

static int com;
static int irq_com;
static struct {
	int com;
	int irq;
} conf[] = {
	// Try COM2 (aka ttyS1) first, because it usually does SOL for IPMI.
	// { COM2, IRQ_COM2 },
	// Still try COM1 (aka ttyS0), because it is what QEMU emulates.
	{ COM1, IRQ_COM1 },
};

/* Non-blocking receive */
u32 nb_uart_recv(void)
{
	for (int i = 0; i < sizeof(conf) / sizeof(conf[0]); i++) {
		com = conf[i].com;
		irq_com = conf[i].irq;
		if (!(get8(com + COM_LINE_STATUS) & COM_LINE_DATA_READY))
			continue;	
		
		u8 ret = get8(com + COM_IN_RECEIVE);
		if (ret == 0xFF) {
			// This is a hack to work around a bug in QEMU.
			// QEMU sometimes sends 0xFF as a byte, and then
			// sends 0x00 as the next byte.
			// This is a hack to ignore the 0xFF byte.
			continue;
		}
		return ret;
	}
	return NB_UART_NRET;
}

void uart_send(u32 c)
{
	int cnt;

	for (int i = 0; i < sizeof(conf) / sizeof(conf[0]); i++) {
		com = conf[i].com;
		irq_com = conf[i].irq;

		cnt = 0;
		while (!(get8(com + COM_LINE_STATUS) & COM_LINE_XMIT_HOLDING)) {
			cnt++;
			if (cnt > 100) {
				break;
			}
		}
		put8(com + COM_OUT_TRANSMIT, c);
	}
}

u32 uart_recv(void)
{
	while (!(get8(com + COM_LINE_STATUS) & COM_LINE_DATA_READY)) ;
	return get8(com + COM_IN_RECEIVE);
}

void uart_init(void)
{
	int i;
	int baud = 115200;

	for (i = 0; i < sizeof(conf) / sizeof(conf[0]); i++) {
		com = conf[i].com;
		irq_com = conf[i].irq;

		// Turn off the FIFO
		put8(com + COM_OUT_FIFO_CTL, 0);
		// 19200 baud
		put8(com + COM_LINE_CTL, COM_LINE_DLAB);	// Unlock divisor
		put8(com + COM_DIVISOR_LSB, 115200 / baud);
		put8(com + COM_DIVISOR_MSB, 0);
		// 8 bits, one stop bit, no parity
		put8(com + COM_LINE_CTL, COM_LINE_LEN_8);	// Lock divisor, 8 data bits.
		put8(com + COM_INT_EN, 0);	// Enable receive interrupts.
		// Data terminal ready
		put8(com + COM_MODEM_CTL, 0x0);

		// If status is 0xFF, no serial port.
		// if (get8(com + COM_LINE_STATUS) != 0xFF)
			// break;
	}

	uart_send(12);
	uart_send(27);
	uart_send('[');
	uart_send('2');
	uart_send('J');
	uart_send('\r');
}
