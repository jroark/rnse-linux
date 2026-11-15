// SPDX-License-Identifier: GPL-2.0
/*
 * Audi RNS-E navigation unit, the 192 generation (SH7750R-like SH-4,
 * 64 MiB SDRAM at 0x0c000000, Fujitsu MB86293 "Coral" graphics).
 *
 * The board has an external watchdog that resets the unit unless its
 * strobe, bit 0 of the byte-wide port at 0x12300001, changes regularly.
 * The stock software and the rnse SDK toggle it every 10 ms; this board
 * code does the same, first once at setup and then from an hrtimer.
 *
 * Everything here is reverse-engineered: see docs/hardware.md and
 * docs/linux.md in the rnse repository.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/memblock.h>
#include <asm/machvec.h>
#include <asm/addrspace.h>

#define RNSE_WATCHDOG_STROBE	0x12300001	/* port 3 data, bit 0 */
#define RNSE_COMPANION_INTC	0x07b00000	/* external interrupt levels */

static void __iomem *strobe;
static struct hrtimer strobe_timer;

static inline void rnse_strobe(void __iomem *port)
{
	__raw_writeb(__raw_readb(port) ^ 1, port);
}

static enum hrtimer_restart rnse_strobe_tick(struct hrtimer *t)
{
	rnse_strobe(strobe);
	hrtimer_forward_now(t, ms_to_ktime(10));
	return HRTIMER_RESTART;
}

static int __init rnse_watchdog_start(void)
{
	strobe = ioremap(RNSE_WATCHDOG_STROBE, 1);
	if (!strobe)
		return -ENOMEM;
	hrtimer_setup(&strobe_timer, rnse_strobe_tick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	hrtimer_start(&strobe_timer, ms_to_ktime(10), HRTIMER_MODE_REL);
	return 0;
}
early_initcall(rnse_watchdog_start);

/* The Coral graphics chip: video memory from 0x18000000, its registers
 * at the top of the same 32 MiB (drivers/video/fbdev/rnse-coralfb.c). */
static struct resource coral_resources[] = {
	DEFINE_RES_MEM(0x18000000, 0x02000000),
};

static struct platform_device coral_device = {
	.name		= "rnse-coralfb",
	.id		= -1,
	.resource	= coral_resources,
	.num_resources	= ARRAY_SIZE(coral_resources),
};

static struct platform_device *rnse_devices[] __initdata = {
	&coral_device,
};

static int __init rnse_devices_setup(void)
{
	return platform_add_devices(rnse_devices, ARRAY_SIZE(rnse_devices));
}
device_initcall(rnse_devices_setup);

static void __init rnse_setup(char **cmdline_p)
{
	void __iomem *intc = (void __iomem *)P2SEGADDR(RNSE_COMPANION_INTC);
	int i;

	rnse_strobe((void __iomem *)P2SEGADDR(RNSE_WATCHDOG_STROBE));

	/*
	 * The companion chip raises the SH-4's IRL lines for the drive,
	 * the CAN controller, the card processors and others.  No driver
	 * here takes them yet, so give every source level 0 (masked) and
	 * clear the edge-latch enables, as the SDK's board_init does.
	 */
	for (i = 0; i <= 0x14; i += 2)
		__raw_writew(0, intc + i);
	__raw_writew(0, intc + 0x1e);

	pr_info("Audi RNS-E 192\n");
}

/*
 * The way back to the SD loader (rnse-controls.c): its session and saved
 * copy (0x0c7c0000, 256 KiB) and the linuxboot program whose
 * loader_return restores it (0x0df00000, 128 KiB) stay as they are.
 */
static void __init rnse_mem_reserve(void)
{
	memblock_reserve(0x0c7c0000, 0x40000);
	memblock_reserve(0x0df00000, 0x20000);
}

static struct sh_machine_vector mv_rnse __initmv = {
	.mv_name	= "Audi RNS-E 192",
	.mv_setup	= rnse_setup,
	.mv_mem_reserve	= rnse_mem_reserve,
};
