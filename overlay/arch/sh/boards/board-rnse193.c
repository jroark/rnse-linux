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
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>
#include <linux/memblock.h>
#include <asm/machvec.h>

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

static struct platform_device *rnse193_devices[] __initdata = {
	&fb_device,
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

static struct sh_machine_vector mv_rnse193 __initmv = {
	.mv_name	= "Audi RNS-E 193",
	.mv_setup	= rnse193_setup,
};
