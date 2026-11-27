// SPDX-License-Identifier: GPL-2.0
/*
 * Audi RNS-E navigation unit, the 193 generation: a Renesas R8A77730
 * (SH-4A), 128 MiB of DDR2 at 0x08000000, an 800x480 screen.
 *
 * The kernel is entered by the rnse SDK's linuxboot (started by the SD
 * loader on a disc) at 0x08201000: CONFIG_ZERO_PAGE_OFFSET is 0x200000,
 * and the kernel keeps its hands off everything below its zero page.
 * That holds the kit's picture, which linuxboot and the SD loader before
 * it have set up and which this kernel takes over as a simple frame
 * buffer: plane 6 of the display unit, 16 bits a pixel (RGB 555), at
 * 0x08110000, 832 pixels a line, the panel's 800 x 480 starting 16
 * pixels in and 8 lines down (rnse-sdk's src/hw193.c; rnse's
 * docs/hardware-193.md, "Display unit").
 *
 * The board has an external watchdog that resets the unit unless its
 * strobe, bit 0 of the port at 0xffc44008, changes regularly; the stock
 * software and the kit toggle it every 10 ms, the rest of that port's
 * lines left as they are.  This board code does the same, once at setup
 * and then from an hrtimer.
 *
 * On the bench 193 the panel stays dark until POWER is pressed, whatever
 * runs: what switches it is outside the system-on-chip.
 *
 * Everything here is reverse-engineered: see rnse's docs/hardware-193.md
 * and this repository's docs/linux.md.
 */
#include <linux/init.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>
#include <linux/memblock.h>
#include <asm/machvec.h>
#include <linux/sh_intc.h>

#define RNSE193_WATCHDOG	0xffc44008	/* bit 0: the strobe */

#define RNSE193_SURFACE		0x08110000	/* the kit's picture */
#define RNSE193_STRIDE		(832 * 2)
#define RNSE193_LEFT		16
#define RNSE193_TOP		8
#define RNSE193_FB		(RNSE193_SURFACE + RNSE193_TOP * RNSE193_STRIDE + RNSE193_LEFT * 2)

static struct hrtimer strobe_timer;

static inline void rnse193_strobe(void)
{
	void __iomem *port = (void __iomem *)RNSE193_WATCHDOG;

	__raw_writel(__raw_readl(port) ^ 1, port);
}

static enum hrtimer_restart rnse193_strobe_tick(struct hrtimer *t)
{
	rnse193_strobe();
	hrtimer_forward_now(t, ms_to_ktime(10));
	return HRTIMER_RESTART;
}

static int __init rnse193_watchdog_start(void)
{
	hrtimer_setup(&strobe_timer, rnse193_strobe_tick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	hrtimer_start(&strobe_timer, ms_to_ktime(10), HRTIMER_MODE_REL);
	return 0;
}
early_initcall(rnse193_watchdog_start);

/* ---- The kernel's log on the CAN bus -------------------------------- */

/*
 * A 193 has no serial port that leaves the unit, and its panel may be
 * dark: what the kernel prints goes out on the CAN bus as well, through
 * the controller as the stock software and the kit left it (running,
 * 100 kbit/s, mailbox 1 theirs for sending: rnse's docs/hardware-193.md,
 * "CAN"; the top bit of its control register is not touched).  Frames
 * 0x7FA: a counter, then up to seven characters.  A frame that does not
 * go out in 3 ms is taken back and its text dropped, so a bus with
 * nobody on it costs the kernel little; the first lines of a start come
 * out in full, later ones as the bus has room (a line a millisecond and
 * a half at best).  rnse-sdk's tools/canbench.py and bench193.py log
 * the bus.
 */
#define RCAN		0xffdf0000
#define RCAN_MB1	(RCAN + 0x100 + 0x20)
#define CANLOG_ID	0x7fa

static bool canlog_frame(const char *s, unsigned int n)
{
	static u8 counter;
	void __iomem *can = (void __iomem *)RCAN, *mb = (void __iomem *)RCAN_MB1;
	unsigned int i;

	/* Not while the controller is in reset or halted. */
	if (__raw_readw(can) & 0x0003)
		return false;
	/* A frame takes more than a millisecond, and the log's first
	 * lines come before anything else serves the watchdog. */
	rnse193_strobe();
	__raw_writew(CANLOG_ID << 2, mb);
	__raw_writeb(counter++, mb + 8);
	for (i = 0; i < 7; i++)
		__raw_writeb(i < n ? s[i] : 0, mb + 9 + i);
	__raw_writew(1 + n, mb + 0x10);
	__raw_writel(0x00000002, can + 0x20);		/* send mailbox 1 */
	for (i = 0; i < 300; i++) {
		if (__raw_readw(can + 0x32) & 0x0002) {
			__raw_writew(0x0002, can + 0x32);	/* sent */
			return true;
		}
		udelay(10);
	}
	__raw_writel(0x00000002, can + 0x28);		/* take it back */
	for (i = 0; i < 200; i++) {
		if (!(__raw_readw(can + 0x22) & 0x0002) || (__raw_readw(can + 0x3a) & 0x0002))
			break;
		udelay(10);
	}
	__raw_writew(0x0002, can + 0x3a);
	return false;
}

static void canlog_write(struct console *con, const char *s, unsigned int n)
{
	static unsigned int failed;

	/* A bus that takes nothing: a frame is tried now and then only. */
	if (failed >= 4 && (failed++ & 63))
		return;
	while (n) {
		unsigned int k = n > 7 ? 7 : n;

		if (!canlog_frame(s, k)) {
			failed++;
			return;
		}
		failed = 0;
		s += k;
		n -= k;
	}
}

static struct console canlog_console = {
	.name	= "canlog",
	.write	= canlog_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED,
	.index	= -1,
};

static int __init rnse193_canlog_start(void)
{
	register_console(&canlog_console);
	return 0;
}
console_initcall(rnse193_canlog_start);

/* ---- The screen --------------------------------------------------- */

static struct simplefb_platform_data fb_data = {
	.width	= 800,
	.height	= 480,
	.stride	= RNSE193_STRIDE,
	.format	= "x1r5g5b5",
};

static struct resource fb_resources[] = {
	DEFINE_RES_MEM(RNSE193_FB, RNSE193_STRIDE * 480),
};

static struct platform_device fb_device = {
	.name		= "simple-framebuffer",
	.id		= 0,
	.resource	= fb_resources,
	.num_resources	= ARRAY_SIZE(fb_resources),
	.dev.platform_data = &fb_data,
};

static struct resource ata_resources[] = {
	DEFINE_RES_MEM(0xffc62100, 0xc0),
};

static struct platform_device ata_device = {
	.name = "pata-rnse", .id = -1,
	.resource = ata_resources, .num_resources = ARRAY_SIZE(ata_resources),
};

static struct resource can_resources[] = {
	DEFINE_RES_MEM(0xffdf0000, 0x500),
	DEFINE_RES_IRQ(evt2irq(0x880)),
};

static struct platform_device can_device = {
	.name = "rnse-hcan2", .id = -1,
	.resource = can_resources, .num_resources = ARRAY_SIZE(can_resources),
};

static struct resource uart_resources[] = {
	DEFINE_RES_MEM(0xff915000, 0x20),
	DEFINE_RES_IRQ(evt2irq(0xd20)),
};

static struct platform_device uart_device = {
	.name = "rnse-uart", .id = 1,
	.resource = uart_resources, .num_resources = ARRAY_SIZE(uart_resources),
};

static struct platform_device *rnse193_devices[] __initdata = {
	&fb_device,
	&ata_device,
	&can_device,
	&uart_device,
};

static int __init rnse193_devices_setup(void)
{
	return platform_add_devices(rnse193_devices, ARRAY_SIZE(rnse193_devices));
}
device_initcall(rnse193_devices_setup);

static void __init rnse193_setup(char **cmdline_p)
{
	rnse193_strobe();
	pr_info("Audi RNS-E 193\n");
}

/*
 * The way back to the SD loader (rnse-controls.c): the loader's saved
 * copy and session (0x0c7c0000, 256 KiB) and the linuxboot program
 * whose loader_return restores it (0x09b60000, 1 MiB) stay as they are.
 */
static void __init rnse193_mem_reserve(void)
{
	memblock_reserve(0x0c7c0000, 0x40000);
	memblock_reserve(0x09b60000, 0x100000);
}

static struct sh_machine_vector mv_rnse193 __initmv = {
	.mv_name	= "Audi RNS-E 193",
	.mv_setup	= rnse193_setup,
	.mv_mem_reserve	= rnse193_mem_reserve,
};
