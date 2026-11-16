// SPDX-License-Identifier: GPL-2.0
/*
 * The serial channels of the Audi RNS-E 192's companion chip, of which
 * this driver takes channel 1, the line to the radio processor (tuner,
 * amplifier, power: docs/hardware.md "Radio/audio processor"), as
 * /dev/ttyRN1.  Channel 3 is the faceplate's and belongs to
 * arch/sh/boards/rnse-controls.c; channel 5 is the loaders' debug text.
 *
 * Sixteen-bit registers at 0x07aN0000 (src/dev/uart.rs):
 *
 *   +0x00 mode (0: eight bits, no parity, the radio processor's format)
 *   +0x02 bit rate divisor: 14.7 MHz / 16 / (divisor + 1)
 *   +0x04 control: 7 TX interrupt, 6 RX interrupt, 5 transmitter on,
 *         4 receiver on, 2 TX-finished interrupt
 *   +0x06 transmit data, +0x0a receive data
 *   +0x08 status: 9 TX FIFO full, 8 TX ready, 6 RX data, 5..3 receive
 *         errors (written 0 to clear: an error stops the receiver),
 *         2 TX finished
 *   +0x0c FIFO control: 15 resets the RX FIFO, 7 the TX FIFO
 *
 * As the stock software (and sdk/src/radio.c) does, the transmitter is
 * on only while something is being sent: switched on with the first
 * byte and off once the last has gone out.  The interrupt is the
 * companion chip's level 2.  Only eight bits without parity, at the bit
 * rates the divisor gives; the radio processor's line is 9600 bit/s
 * (divisor 0x5f), which is the default.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/platform_data/rnse.h>

#define MODE	0x00
#define DIV	0x02
#define CTRL	0x04
#define TDR	0x06
#define STATUS	0x08
#define RDR	0x0a
#define FIFO	0x0c

#define C_TXI	BIT(7)
#define C_RXI	BIT(6)
#define C_TE	BIT(5)
#define C_RE	BIT(4)
#define C_TEI	BIT(2)

#define S_TXFULL	BIT(9)
#define S_RX		BIT(6)
#define S_ERRORS	(BIT(5) | BIT(4) | BIT(3))
#define S_TXEND		BIT(2)

#define CLOCK	14700000
#define NR	6

static struct uart_driver rnse_uart_driver;
static struct uart_port ports[NR];

static u16 rd(struct uart_port *p, int r)
{
	return __raw_readw(p->membase + r);
}

static void wr(struct uart_port *p, int r, u16 v)
{
	__raw_writew(v, p->membase + r);
}

static void ctrl(struct uart_port *p, u16 clear, u16 set)
{
	wr(p, CTRL, (rd(p, CTRL) & ~clear) | set);
}

static unsigned int rnse_tx_empty(struct uart_port *p)
{
	return rd(p, CTRL) & C_TE ? 0 : TIOCSER_TEMT;
}

static void rnse_set_mctrl(struct uart_port *p, unsigned int mctrl)
{
}

static unsigned int rnse_get_mctrl(struct uart_port *p)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void rnse_stop_tx(struct uart_port *p)
{
	ctrl(p, C_TXI, 0);
}

/* As much as the FIFO takes; when nothing is left, wait for the last
 * byte to go out and then switch the transmitter off. */
static void fill(struct uart_port *p)
{
	struct tty_port *tport = &p->state->port;
	unsigned char c;

	if (p->x_char) {
		wr(p, TDR, p->x_char);
		p->icount.tx++;
		p->x_char = 0;
	}
	while (!(rd(p, STATUS) & S_TXFULL) && uart_fifo_get(p, &c))
		wr(p, TDR, c);
	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(p);
	if (kfifo_is_empty(&tport->xmit_fifo) || uart_tx_stopped(p))
		ctrl(p, C_TXI, C_TEI);
}

static void rnse_start_tx(struct uart_port *p)
{
	ctrl(p, C_TEI, C_TE | C_TXI);
	fill(p);
}

static void rnse_stop_rx(struct uart_port *p)
{
	ctrl(p, C_RXI, 0);
}

static void receive(struct uart_port *p)
{
	struct tty_port *tport = &p->state->port;
	u16 st;
	int n = 0;

	while ((st = rd(p, STATUS)) & (S_RX | S_ERRORS)) {
		if (st & S_ERRORS) {
			wr(p, STATUS, st & ~S_ERRORS);
			p->icount.overrun++;
			tty_insert_flip_char(tport, 0, TTY_OVERRUN);
		}
		if (!(st & S_RX))
			continue;
		p->icount.rx++;
		tty_insert_flip_char(tport, rd(p, RDR) & 0xff, TTY_NORMAL);
		if (++n > 256)
			break;
	}
	tty_flip_buffer_push(tport);
}

static irqreturn_t rnse_uart_irq(int irq, void *data)
{
	struct uart_port *p = data;
	unsigned long flags;
	u16 c;

	uart_port_lock_irqsave(p, &flags);
	c = rd(p, CTRL);
	if (c & C_RXI)
		receive(p);
	if ((c & C_TXI) && (c & C_TE))
		fill(p);
	c = rd(p, CTRL);
	if ((c & C_TEI) && (rd(p, STATUS) & S_TXEND))
		ctrl(p, C_TEI | C_TE, 0);	/* all out: transmitter off */
	uart_port_unlock_irqrestore(p, flags);
	return IRQ_HANDLED;
}

static int rnse_startup(struct uart_port *p)
{
	int r;

	wr(p, CTRL, 0);
	wr(p, FIFO, 0x8080);
	wr(p, FIFO, 0);
	wr(p, STATUS, rd(p, STATUS) & ~S_ERRORS);
	r = request_irq(p->irq, rnse_uart_irq, 0, "rnse-uart", p);
	if (r)
		return r;
	wr(p, CTRL, C_RE | C_RXI);
	return 0;
}

static void rnse_shutdown(struct uart_port *p)
{
	wr(p, CTRL, C_RE);	/* as the stock software leaves it: listening */
	free_irq(p->irq, p);
}

static void rnse_set_termios(struct uart_port *p, struct ktermios *t,
			     const struct ktermios *old)
{
	unsigned int baud = uart_get_baud_rate(p, t, old, 300, 115200);
	unsigned int div = DIV_ROUND_CLOSEST(CLOCK / 16, baud) - 1;
	unsigned long flags;

	/* Eight bits, no parity, one stop bit: all this driver knows. */
	t->c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	t->c_cflag |= CS8;
	uart_port_lock_irqsave(p, &flags);
	wr(p, MODE, 0);
	wr(p, DIV, div);
	uart_update_timeout(p, t->c_cflag, baud);
	uart_port_unlock_irqrestore(p, flags);
	tty_termios_encode_baud_rate(t, CLOCK / 16 / (div + 1), CLOCK / 16 / (div + 1));
}

static const char *rnse_type(struct uart_port *p)
{
	return "RNS-E companion";
}

static void rnse_config_port(struct uart_port *p, int flags)
{
	p->type = PORT_UNKNOWN + 1;
}

static int rnse_verify_port(struct uart_port *p, struct serial_struct *s)
{
	return -EINVAL;
}

static void rnse_release_port(struct uart_port *p)
{
}

static int rnse_request_port(struct uart_port *p)
{
	return 0;
}

static const struct uart_ops rnse_uart_ops = {
	.tx_empty	= rnse_tx_empty,
	.set_mctrl	= rnse_set_mctrl,
	.get_mctrl	= rnse_get_mctrl,
	.stop_tx	= rnse_stop_tx,
	.start_tx	= rnse_start_tx,
	.stop_rx	= rnse_stop_rx,
	.startup	= rnse_startup,
	.shutdown	= rnse_shutdown,
	.set_termios	= rnse_set_termios,
	.type		= rnse_type,
	.config_port	= rnse_config_port,
	.verify_port	= rnse_verify_port,
	.release_port	= rnse_release_port,
	.request_port	= rnse_request_port,
};

/*
 * A frame for the radio processor from the kernel itself: the keep-alive
 * (81 [06] once a minute, which the controls thread sends whatever user
 * space does; docs/hardware.md).  Sent only between user space's frames:
 * -EBUSY while the transmitter is on or bytes wait to go, -ENODEV when
 * the line has no driver (the caller then writes the registers itself).
 * The answer, if any, is for whoever reads the line; rnse-radio throws
 * away what it finds before each of its own frames.
 */
int rnse_radio_send(const u8 *buf, int n)
{
	struct uart_port *p = &ports[1];
	unsigned long flags, end;
	int i;

	if (!p->membase)
		return -ENODEV;
	uart_port_lock_irqsave(p, &flags);
	if ((rd(p, CTRL) & C_TE) ||
	    (p->state && !kfifo_is_empty(&p->state->port.xmit_fifo))) {
		uart_port_unlock_irqrestore(p, flags);
		return -EBUSY;
	}
	wr(p, STATUS, rd(p, STATUS) & ~S_ERRORS);
	ctrl(p, C_TXI | C_TEI, C_TE);
	for (i = 0; i < n; i++) {
		end = jiffies + msecs_to_jiffies(20);
		while ((rd(p, STATUS) & S_TXFULL) && time_before(jiffies, end))
			udelay(10);
		wr(p, TDR, buf[i]);
	}
	uart_port_unlock_irqrestore(p, flags);
	/* Some 6 ms at 9600 bit/s: then the transmitter off again, unless
	 * user space has started a frame meanwhile. */
	end = jiffies + msecs_to_jiffies(20);
	while (!(rd(p, STATUS) & S_TXEND) && time_before(jiffies, end))
		usleep_range(500, 1000);
	uart_port_lock_irqsave(p, &flags);
	if (!(rd(p, CTRL) & C_TXI))
		ctrl(p, C_TE | C_TEI, 0);
	uart_port_unlock_irqrestore(p, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(rnse_radio_send);

static struct uart_driver rnse_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "rnse-uart",
	.dev_name	= "ttyRN",
	.nr		= NR,
};

static int rnse_uart_probe(struct platform_device *pdev)
{
	struct uart_port *p;
	struct resource *res;
	int irq;

	if (pdev->id < 0 || pdev->id >= NR)
		return -EINVAL;
	p = &ports[pdev->id];
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (!res || irq < 0)
		return -ENODEV;
	p->membase = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!p->membase)
		return -ENOMEM;
	p->mapbase = res->start;
	p->irq = irq;
	p->iotype = UPIO_MEM16;
	p->uartclk = CLOCK;
	p->fifosize = 16;
	p->ops = &rnse_uart_ops;
	p->flags = UPF_BOOT_AUTOCONF;
	p->line = pdev->id;
	p->dev = &pdev->dev;
	spin_lock_init(&p->lock);
	return uart_add_one_port(&rnse_uart_driver, p);
}

static struct platform_driver rnse_uart_platform = {
	.driver = { .name = "rnse-uart" },
	.probe = rnse_uart_probe,
};

static int __init rnse_uart_init(void)
{
	int r = uart_register_driver(&rnse_uart_driver);

	if (r)
		return r;
	r = platform_driver_register(&rnse_uart_platform);
	if (r)
		uart_unregister_driver(&rnse_uart_driver);
	return r;
}
device_initcall(rnse_uart_init);
MODULE_DESCRIPTION("Audi RNS-E 192 companion chip serial channels");
MODULE_LICENSE("GPL");
