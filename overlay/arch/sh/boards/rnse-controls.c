// SPDX-License-Identifier: GPL-2.0
/*
 * The Audi RNS-E 192's physical controls under Linux: the faceplate
 * controller's keys and knobs, the fold-down display and the disc, and
 * the way back to the rnse SDK's SD loader.
 *
 * Whatever else runs, a unit must let its display be folded down and its
 * disc be ejected, and a program started by the SD loader must go back
 * to it when RETURN is held for two seconds (CLAUDE.md, "Mandatory
 * hardware behavior", in the rnse repository).  So that does not hang on
 * user space, a kernel thread does it all, every 10 ms:
 *
 *  - It answers the faceplate controller (the companion chip's serial
 *    channel 3 at 0x07a30000; protocol in sdk/src/panel.c) as the loader
 *    left it, started and lit, and gives it its settings again should it
 *    announce itself anew.  Keys and knobs also go to an input device.
 *  - EJECT (the faceplate's key, bit 13) folds the display down or up:
 *    port 3 at 0x12600001, bit 3 runs the motor towards open and bit 4
 *    towards closed, input bit 2 is low only when it is fully open, and
 *    A/D input 3 (0x07d00000) gives the position (at most 94 open, at
 *    least 894 closed).  As in the SDK (sdk/src/display.c) and the stock
 *    software, the motor stops when the position has stood still for
 *    300 ms, and after 8 s whatever happens.
 *  - CD EJECT (bit 31, behind the display) sends the drive START STOP
 *    UNIT with LoEj by programmed I/O (0x07800000), folding the display
 *    down first if it is not.
 *  - RETURN held for 2 s returns to the loader: interrupts, timers and
 *    DMA stopped, caches written back, MMU off, and a call of the SDK's
 *    loader_return in sdk/examples/linuxboot, whose address and stack
 *    linuxboot passes as rnse.return=ADDR,STACK.  Linux leaves that
 *    program and the loader's session alone (board-rnse.c).  A shorter
 *    RETURN is the input device's KEY_BACK, sent when it is let go.
 *
 * Reverse-engineered: docs/hardware.md and docs/linux.md.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/input.h>
#include <linux/irqflags.h>
#include <linux/string.h>
#include <asm/addrspace.h>
#include <asm/cacheflush.h>

#define UART3		0x07a30000
#define PORT3		0x12600001
#define ADC		0x07d00000
#define ATA_REG		0x07800001	/* register n at +2n */
#define ATA_DATA	0x07880000
#define ATA_ALT		0x0790000d

#define SESSION		0xac7c0000	/* the SD loader's control data */
#define SESSION_MAGIC	0x53444c31
#define LOADER_BASE	0x8df00000

#define RETURN_HOLD_MS	2000

/* Key state bits of the faceplate (sdk/include/rnse.h). */
enum {
	FP_INFO = 1, FP_NAV = 2, FP_TEL = 3, FP_NAME = 5, FP_MEDIA = 6,
	FP_RADIO = 7, FP_SOFT_TL = 9, FP_SOFT_TR = 10, FP_RETURN = 11,
	FP_DISPLAY = 13, FP_SETUP = 14, FP_CAR = 15, FP_NEXT = 19,
	FP_PREV = 21, FP_SOFT_BR = 22, FP_SOFT_BL = 23, FP_CD_EJECT = 31,
	FP_SELECT = 44,
};

static const struct { u8 bit; u16 code; } keymap[] = {
	{ FP_INFO, KEY_INFO }, { FP_NAV, KEY_PROG1 }, { FP_TEL, KEY_PHONE },
	{ FP_NAME, KEY_PROG2 }, { FP_MEDIA, KEY_MEDIA }, { FP_RADIO, KEY_RADIO },
	{ FP_SOFT_TL, KEY_F1 }, { FP_SOFT_TR, KEY_F2 }, { FP_SOFT_BL, KEY_F3 },
	{ FP_SOFT_BR, KEY_F4 }, { FP_SETUP, KEY_SETUP }, { FP_CAR, KEY_PROG3 },
	{ FP_NEXT, KEY_NEXT }, { FP_PREV, KEY_PREVIOUS }, { FP_SELECT, KEY_ENTER },
	{ FP_DISPLAY, KEY_EJECTCLOSECD }, { FP_CD_EJECT, KEY_EJECTCD },
};

/* What the stock software sends once the controller has started (as
 * sdk/src/hw192.c panel_settings): command, length of the data, data. */
static const u8 settings[] = {
	0x02, 10, 0x40, 0x1d, 0x4b, 0x42, 0x40, 0x40, 0x40, 0x01, 0xff, 0x05,
	0x03, 13, 0x3c, 0x0a, 0x02, 0x0a, 0x04, 0x0a, 0x04, 0x0a, 0x04, 0x0a, 0x04, 0x0a, 0x0a,
	0x0a, 5, 0x00, 0xff, 0x1c, 0xff, 0x1c,
	0x09, 4, 0x00, 0x00, 0x0f, 0x06,
	0x06, 12, 0x00, 0x00, 0x00, 0x00, 0x30, 0x70, 0x00, 0x00, 0x09, 0xb0, 0x00, 0x00,
	0x07, 4, 0x00, 0x00, 0x00, 0x03,
	0x06, 12, 0x0f, 0x8a, 0x07, 0x98, 0x09, 0x98, 0x00, 0x00, 0x02, 0x18, 0x26, 0xd8,
	0, 0
};

static void __iomem *uart, *port3, *adc, *ata, *ata_data, *ata_alt;
static struct input_dev *input;
static unsigned long return_entry, return_stack;

static int __init rnse_return_setup(char *s)
{
	if (sscanf(s, "%lx,%lx", &return_entry, &return_stack) != 2)
		return_entry = return_stack = 0;
	return 1;
}
__setup("rnse.return=", rnse_return_setup);

/* ---- The faceplate controller --------------------------------------- */

static struct {
	u8 rx[24];
	int rx_len;
	unsigned long rx_time, busy_time, sent_time;
	u8 keys[6];
	const u8 *setting;
	int waiting, attempts;
} fp;

static void fp_putc(u8 c)
{
	int i;

	for (i = 0; i < 10000 && (__raw_readw(uart + 0x08) & 0x0200); i++)
		udelay(1);
	__raw_writew(c, uart + 0x06);
}

static int fp_getc(void)
{
	u16 status = __raw_readw(uart + 0x08);

	if (status & 0x0038)	/* receive errors */
		__raw_writew(status & 0xffc7, uart + 0x08);
	if (!(status & 0x0040))
		return -1;
	return __raw_readw(uart + 0x0a) & 0xff;
}

static void fp_send(u8 cmd, const u8 *data, int n)
{
	u8 sum = cmd + n + 2;
	int i;

	fp_putc(cmd);
	fp_putc(n + 2);
	for (i = 0; i < n; i++) {
		fp_putc(data[i]);
		sum += data[i];
	}
	fp_putc(sum);
	fp.busy_time = jiffies;
}

static void fp_send_setting(void)
{
	fp_send(fp.setting[0], fp.setting + 2, fp.setting[1]);
	fp.waiting = 1;
	fp.sent_time = jiffies;
}

static void fp_frame(void)
{
	int n = fp.rx[1] - 2, i;
	const u8 *d = fp.rx + 2;

	switch (fp.rx[0]) {
	case 0x0f:	/* started anew: it wants its settings */
		fp.setting = settings;
		fp.waiting = 0;
		fp.attempts = 0;
		memset(fp.keys, 0, sizeof(fp.keys));
		break;
	case 0x0c:	/* key state, a bit a key */
		for (i = 0; i < 6 && i < n; i++)
			fp.keys[i] = d[i];
		break;
	case 0x0d:	/* knobs: volume, -, select, -; bit 7 the direction */
		if (n >= 3 && input) {
			int vol = d[0] & 0x80 ? -(d[0] & 0x7f) : (d[0] & 0x7f);
			int sel = d[2] & 0x80 ? (d[2] & 0x7f) : -(d[2] & 0x7f);

			if (vol)
				input_report_rel(input, REL_MISC, vol);
			if (sel)
				input_report_rel(input, REL_DIAL, sel);
			input_sync(input);
		}
		break;
	}
	fp_putc(0x15);	/* acknowledged */
	fp.busy_time = jiffies;
}

static void fp_poll(void)
{
	unsigned long now = jiffies;
	int c;

	if (fp.rx_len && time_after(now, fp.rx_time + msecs_to_jiffies(100)))
		fp.rx_len = 0;
	while ((c = fp_getc()) >= 0) {
		fp.rx_time = fp.busy_time = now;
		if (fp.rx_len == 0 && (c == 0x15 || c == 0x16)) {
			if (fp.waiting) {
				fp.waiting = 0;
				fp.attempts = 0;
				fp.setting += 2 + fp.setting[1];
			}
			continue;
		}
		fp.rx[fp.rx_len++] = c;
		if (fp.rx_len >= 2) {
			int last = fp.rx[1] < 2 ? 2 : fp.rx[1];

			if (last >= sizeof(fp.rx)) {
				fp.rx_len = 0;
			} else if (fp.rx_len > last) {
				u8 sum = 0;
				int i;

				for (i = 0; i < last; i++)
					sum += fp.rx[i];
				fp.rx_len = 0;
				if (sum == fp.rx[last]) {
					fp_frame();
				} else {
					static const u8 code = 0x80;

					fp_send(0x18, &code, 1);
				}
			}
		}
	}
	if (fp.waiting) {
		if (time_after(now, fp.sent_time + msecs_to_jiffies(300)) &&
		    ++fp.attempts <= 5)
			fp_send_setting();
	} else if (fp.setting[1] &&
		   time_after(now, fp.busy_time + msecs_to_jiffies(20))) {
		fp_send_setting();
	}
}

static int fp_key(int bit)
{
	return fp.keys[bit >> 3] >> (bit & 7) & 1;
}

/* ---- The display's motor, and the disc ------------------------------- */

enum { STILL, OPENING, CLOSING };
static int motor = STILL, eject_pending;
static unsigned long motor_since, moved_at;
static u16 moved_from;

static u16 display_position(void)
{
	int i;
	u16 v;

	__raw_writew(0x2003, adc + 8);	/* start: inputs 0 to 3 */
	for (i = 0; i < 1000 && !(__raw_readw(adc + 8) & 0x8000); i++)
		udelay(1);
	v = __raw_readw(adc + 6) >> 6;
	__raw_writew(0, adc + 8);
	return v;
}

static int display_open(void)
{
	return !(__raw_readb(port3) & 0x04) || display_position() <= 94;
}

static void motor_stop(void)
{
	__raw_writeb(__raw_readb(port3) & ~0x18, port3);
	motor = STILL;
}

static void motor_run(int way)
{
	__raw_writeb((__raw_readb(port3) & ~0x18) | (way == OPENING ? 0x08 : 0x10), port3);
	motor = way;
	motor_since = moved_at = jiffies;
	moved_from = display_position();
}

static void motor_step(void)
{
	unsigned long now = jiffies;
	u16 pos;
	int d;

	if (motor == STILL)
		return;
	pos = display_position();
	d = (int)pos - (int)moved_from;
	if (d > 8 || d < -8) {
		moved_from = pos;
		moved_at = now;
	}
	if (time_after(now, moved_at + msecs_to_jiffies(300)) ||
	    time_after(now, motor_since + msecs_to_jiffies(8000)))
		motor_stop();
}

static int ata_wait_clear(u8 bits, unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);

	while (__raw_readb(ata_alt) & bits) {
		if (time_after(jiffies, end))
			return -1;
		usleep_range(500, 1000);
		fp_poll();
	}
	return 0;
}

/* An ATAPI packet command without data, as sdk/src/cd.c sends it:
 * 0, -1 the drive did not react, or 0x100 + its error register. */
static int ata_command(const u8 *packet)
{
	int i;

	if (ata_wait_clear(0x80, 5000))
		return -1;
	__raw_writeb(0xa0, ata + 2 * 6);	/* the drive */
	if (ata_wait_clear(0x88, 5000))
		return -1;
	__raw_writeb(0x00, ata + 2 * 1);	/* no DMA */
	__raw_writeb(0x00, ata + 2 * 4);	/* byte count limit */
	__raw_writeb(0x08, ata + 2 * 5);
	__raw_writeb(0xa0, ata + 2 * 7);	/* PACKET */
	__raw_readb(ata_alt);
	if (ata_wait_clear(0x80, 5000))
		return -1;
	if (!(__raw_readb(ata_alt) & 0x08))
		return 0x100 | __raw_readb(ata + 2 * 1);
	for (i = 0; i < 12; i += 2)
		__raw_writew(packet[i] << 8 | packet[i + 1], ata_data);
	__raw_readb(ata_alt);
	if (ata_wait_clear(0x80, 30000))
		return -1;
	if (__raw_readb(ata + 2 * 7) & 0x01)
		return 0x100 | __raw_readb(ata + 2 * 1);
	return 0;
}

static void eject(void)
{
	static const u8 start_stop_unit[12] = { 0x1b, 1, 0, 0, 2 };
	int r = ata_command(start_stop_unit);

	if (r > 0)	/* a "disc changed" to be heard first */
		r = ata_command(start_stop_unit);
	if (r)
		pr_warn("rnse: the drive did not eject (%d)\n", r);
	else
		pr_info("rnse: disc ejected\n");
}

/* ---- Back to the loader ---------------------------------------------- */

static int in_session(void)
{
	const volatile u32 *s = (const volatile u32 *)SESSION;

	return return_entry && s[0] == SESSION_MAGIC && s[1] == ~SESSION_MAGIC &&
	       s[2] == LOADER_BASE && s[3] < 2 && s[4] == 1;
}

/* Run from its uncached address: every dirty line of the operand cache
 * written back (the kernel has used its stack since flush_cache_all; and
 * by OCBP over all of RAM, about 25 ms, rather than through the cache's
 * address array, which not every emulator build models), MMU
 * off and its TLB emptied, the caches as the stock software has them
 * (0x80008181) and both emptied, then loader_return(status) on the
 * loader's own stack.  r4 status, r5 the function, r6 its stack. */
void rnse_leave(int status, unsigned long entry, unsigned long stack);
asm(
"	.text\n"
"	.align	2\n"
"	.global	rnse_leave\n"
"rnse_leave:\n"
"	mov	r4, r10\n"	/* out of the banked registers: SR changes RB */
"	mov	r5, r8\n"
"	mov	r6, r9\n"
"	mov.l	5f, r1\n"	/* every line of RAM purged from the operand */
"	mov.l	6f, r2\n"	/* cache (written back if dirty): 2M lines */
"7:	ocbp	@r1\n"
"	add	#32, r1\n"
"	cmp/hs	r2, r1\n"
"	bf	7b\n"
"	mov.l	1f, r1\n"	/* MMUCR: TI, AT off */
"	mov	#4, r0\n"
"	mov.l	r0, @r1\n"
"	mov.l	2f, r1\n"	/* CCR */
"	mov.l	3f, r0\n"
"	mov.l	r0, @r1\n"
"	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n"
"	mov.l	4f, r0\n"	/* SR: privileged, interrupts masked */
"	ldc	r0, sr\n"
"	mov	r9, r15\n"
"	mov	r10, r4\n"
"	jmp	@r8\n"
"	 nop\n"
"	.align	2\n"
"1:	.long	0xff000010\n"
"2:	.long	0xff00001c\n"
"3:	.long	0x80008989\n"	/* 0x80008181 | ICI | OCI */
"4:	.long	0x400000f0\n"
"5:	.long	0x8c000000\n"
"6:	.long	0x90000000\n");

static void return_to_loader(void)
{
	void (*leave)(int, unsigned long, unsigned long);
	int i;

	pr_info("rnse: back to the SD loader\n");
	motor_stop();
	local_irq_disable();
	/* Timers (both units), DMA, and every interrupt source. */
	__raw_writeb(0, (void __iomem *)0xffd80004);
	__raw_writeb(0, (void __iomem *)0xfe100004);
	for (i = 0; i < 8; i++)
		__raw_writel(0, (void __iomem *)(0xffa0000c + (i < 4 ? i : i + 1) * 0x10));
	__raw_writel(0, (void __iomem *)0xffa00040);
	__raw_writew(0, (void __iomem *)0xffd00004);
	__raw_writew(0, (void __iomem *)0xffd00008);
	__raw_writew(0, (void __iomem *)0xffd0000c);
	__raw_writel(0, (void __iomem *)0xfe080000);
	__raw_writew(0, (void __iomem *)0xffe80008);	/* SCIF interrupts */
	for (i = 0; i <= 0x14; i += 2)
		__raw_writew(0, (void __iomem *)P2SEGADDR(0x07b00000 + i));
	__raw_writew(0, (void __iomem *)P2SEGADDR(0x07b0001e));
	flush_cache_all();
	leave = (void *)P2SEGADDR((unsigned long)rnse_leave);
	leave(0, return_entry, return_stack);
}

/* ---- The thread ------------------------------------------------------ */

static int controls(void *unused)
{
	u8 was[6] = { 0 };
	int display_down = 0, cd_eject_down = 0, return_down, said = 0;
	unsigned long return_since = jiffies;
	int i;

	/* A RETURN still down from the loader is let go first. */
	return_down = 1;
	while (!kthread_should_stop()) {
		fp_poll();
		motor_step();
		if (eject_pending && motor == STILL) {
			eject_pending = 0;
			if (display_open())
				eject();
		}

		if (fp_key(FP_DISPLAY) && !display_down) {
			if (motor != STILL)
				motor_stop();
			else
				motor_run(display_position() < 500 ? CLOSING : OPENING);
		}
		display_down = fp_key(FP_DISPLAY);
		if (fp_key(FP_CD_EJECT) && !cd_eject_down) {
			if (display_open()) {
				eject();
			} else {
				eject_pending = 1;
				motor_run(OPENING);
			}
		}
		cd_eject_down = fp_key(FP_CD_EJECT);

		if (!fp_key(FP_RETURN)) {
			if (return_down == 2 && input) {	/* a short one */
				input_report_key(input, KEY_BACK, 1);
				input_sync(input);
				input_report_key(input, KEY_BACK, 0);
				input_sync(input);
			}
			return_down = 0;
		} else if (!return_down) {
			return_down = 2;
			return_since = jiffies;
		} else if (return_down == 2 &&
			   time_after(jiffies, return_since + msecs_to_jiffies(RETURN_HOLD_MS))) {
			return_down = 1;	/* used up: no KEY_BACK */
			if (in_session())
				return_to_loader();
			if (!said++)
				pr_warn("rnse: return to the SD loader is not available here\n");
		}

		if (input) {
			for (i = 0; i < ARRAY_SIZE(keymap); i++) {
				int b = keymap[i].bit;
				int now = fp.keys[b >> 3] >> (b & 7) & 1;

				if (now != (was[b >> 3] >> (b & 7) & 1))
					input_report_key(input, keymap[i].code, now);
			}
			input_sync(input);
		}
		memcpy(was, fp.keys, sizeof(was));
		usleep_range(10000, 11000);
	}
	return 0;
}

static int __init rnse_controls_init(void)
{
	struct task_struct *t;
	const u8 *p = settings;
	int i;

	uart = ioremap(UART3, 0x10);
	port3 = ioremap(PORT3, 1);
	adc = ioremap(ADC, 0x10);
	ata = ioremap(ATA_REG, 0x10);
	ata_data = ioremap(ATA_DATA, 2);
	ata_alt = ioremap(ATA_ALT, 1);
	if (!uart || !port3 || !adc || !ata || !ata_data || !ata_alt)
		return -ENOMEM;

	/* The controller as the loader left it: started, settings given. */
	while (p[1])
		p += 2 + p[1];
	fp.setting = p;
	fp.busy_time = jiffies;

	input = input_allocate_device();
	if (input) {
		input->name = "RNS-E faceplate";
		input->phys = "rnse/faceplate";
		input->id.bustype = BUS_HOST;
		for (i = 0; i < ARRAY_SIZE(keymap); i++)
			input_set_capability(input, EV_KEY, keymap[i].code);
		input_set_capability(input, EV_KEY, KEY_BACK);
		input_set_capability(input, EV_REL, REL_DIAL);
		input_set_capability(input, EV_REL, REL_MISC);
		if (input_register_device(input)) {
			input_free_device(input);
			input = NULL;
		}
	}

	t = kthread_run(controls, NULL, "rnse-controls");
	if (IS_ERR(t))
		return PTR_ERR(t);
	pr_info("rnse: faceplate, display and eject controls; return to the loader %s\n",
		in_session() ? "with RETURN held" : "not available");
	return 0;
}
device_initcall(rnse_controls_init);
