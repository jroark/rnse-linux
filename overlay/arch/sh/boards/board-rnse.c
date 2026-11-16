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
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/physmap.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_data/rnse.h>
#include <asm/machvec.h>
#include <asm/addrspace.h>

#define RNSE_WATCHDOG_STROBE	0x12300001	/* port 3 data, bit 0 */
#define RNSE_COMPANION_INTC	0x07b00000	/* external interrupt levels */

#define SRAM_BASE	0x10000000	/* battery-backed, 256 KiB */
#define SRAM_SIZE	0x40000
#define SRAM_OURS	0x3f640		/* Linux's own settings, to 0x3f7ff */
#define SRAM_OURS_END	0x3f800

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

/* ---- The companion chip's interrupt sources ------------------------ *
 *
 * Registers at 0x07b00000, 16 bits each: four 4-bit levels a register
 * from +0x00 to +0x14 (0 masks a source), latched requests at +0x16 and
 * their enables at +0x1e (docs/hardware.md, "Interrupts").  A source
 * asks at its level on the encoded IRL lines; Linux sees IRQ 31 - level.
 * Masking an IRQ gives its source level 0, unmasking its level again:
 * the source's own request stays as it is, and the encoder sees it again
 * when it is unmasked.  The sources taken here are level-triggered
 * (they ask for as long as the device has something to say).
 */
static const struct rnse_source {
	u8 level, reg, shift;
} rnse_sources[] = {
	{ RNSE_LEVEL_ATAPI, 0x00, 4 },
	{ RNSE_LEVEL_UART1, 0x08, 0 },
	{ RNSE_LEVEL_CAN, 0x12, 8 },
};

static DEFINE_RAW_SPINLOCK(intc_lock);

static const struct rnse_source *rnse_source_of(unsigned int irq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rnse_sources); i++)
		if (RNSE_IRQ(rnse_sources[i].level) == irq)
			return &rnse_sources[i];
	return NULL;
}

static void rnse_source_level(unsigned int irq, int on)
{
	const struct rnse_source *s = rnse_source_of(irq);
	void __iomem *r;
	unsigned long flags;
	u16 v;

	if (!s)
		return;
	r = (void __iomem *)P2SEGADDR(RNSE_COMPANION_INTC + s->reg);
	raw_spin_lock_irqsave(&intc_lock, flags);
	v = __raw_readw(r) & ~(0xf << s->shift);
	if (on)
		v |= s->level << s->shift;
	__raw_writew(v, r);
	(void)__raw_readw(r);	/* the write out before interrupts open */
	raw_spin_unlock_irqrestore(&intc_lock, flags);
}

static void rnse_irq_mask(struct irq_data *d)
{
	rnse_source_level(d->irq, 0);
}

static void rnse_irq_unmask(struct irq_data *d)
{
	rnse_source_level(d->irq, 1);
}

static struct irq_chip rnse_irq_chip = {
	.name		= "RNS-E companion",
	.irq_mask	= rnse_irq_mask,
	.irq_unmask	= rnse_irq_unmask,
	.irq_mask_ack	= rnse_irq_mask,
};

static void __init rnse_init_irq(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rnse_sources); i++) {
		unsigned int irq = RNSE_IRQ(rnse_sources[i].level);

		irq_alloc_desc_at(irq, numa_node_id());
		irq_set_chip_and_handler_name(irq, &rnse_irq_chip,
					      handle_level_irq, "level");
	}
}

/* ---- Devices ---------------------------------------------------------- */

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

/*
 * The flash, 32 MiB: two chips side by side on a 32-bit bus.  Read only
 * and taken as a ROM ("map_rom"): no command is ever written to the
 * chips, not even a query, so nothing here can erase or program them.
 * The partitions are the North American software's directory
 * (`rnse info`); with Linux installed in flash, "os" holds linuxboot and
 * "apm" the kernel.
 */
static struct mtd_partition flash_parts[] = {
	{ .name = "bootrom",	.offset = 0x000000, .size = 0x00c000 },
	{ .name = "directory",	.offset = 0x00c000, .size = 0x000100 },
	{ .name = "os",		.offset = 0x00c100, .size = 0x203f00 },
	{ .name = "apm",	.offset = 0x210000, .size = 0x810000 },
	{ .name = "aum",	.offset = 0xa20000, .size = 0x260000 },
	{ .name = "font",	.offset = 0xc80000, .size = 0x180000 },
	{ .name = "rest",	.offset = 0xe00000, .size = MTDPART_SIZ_FULL },
};

static struct physmap_flash_data flash_data = {
	.width		= 4,
	.probe_type	= "map_rom",
	.parts		= flash_parts,
	.nr_parts	= ARRAY_SIZE(flash_parts),
};

static struct resource flash_resources[] = {
	DEFINE_RES_MEM(0x00000000, 0x02000000),
};

static struct platform_device flash_device = {
	.name		= "physmap-flash",
	.id		= 0,
	.resource	= flash_resources,
	.num_resources	= ARRAY_SIZE(flash_resources),
	.dev.platform_data = &flash_data,
};

/* The CAN controller (drivers/net/can/rnse-hcan2.c). */
static struct resource can_resources[] = {
	DEFINE_RES_MEM(0x07c00000, 0x400),
	DEFINE_RES_IRQ(RNSE_IRQ_CAN),
};

static struct platform_device can_device = {
	.name		= "rnse-hcan2",
	.id		= -1,
	.resource	= can_resources,
	.num_resources	= ARRAY_SIZE(can_resources),
};

/* The CD drive (drivers/ata/pata_rnse.c): the task file as bytes at odd
 * addresses, two apart; the data register, 16 bits; the control block. */
static struct resource ata_resources[] = {
	DEFINE_RES_MEM(0x07800000, 0x10),
	DEFINE_RES_MEM(0x07880000, 0x2),
	DEFINE_RES_MEM(0x0790000c, 0x2),
	DEFINE_RES_IRQ(RNSE_IRQ_ATAPI),
};

static struct platform_device ata_device = {
	.name		= "pata-rnse",
	.id		= -1,
	.resource	= ata_resources,
	.num_resources	= ARRAY_SIZE(ata_resources),
};

/* Serial channel 1 of the companion chip: the radio processor
 * (drivers/tty/serial/rnse-uart.c). */
static struct resource uart1_resources[] = {
	DEFINE_RES_MEM(0x07a10000, 0x10),
	DEFINE_RES_IRQ(RNSE_IRQ_UART1),
};

static struct platform_device uart1_device = {
	.name		= "rnse-uart",
	.id		= 1,
	.resource	= uart1_resources,
	.num_resources	= ARRAY_SIZE(uart1_resources),
};

/* The two SD card processors, their cards and their sound
 * (drivers/misc/rnse-media.c). */
static struct platform_device media_device = {
	.name		= "rnse-media",
	.id		= -1,
};

/* The battery-backed memory (below). */
static struct resource sram_resources[] = {
	DEFINE_RES_MEM(SRAM_BASE, SRAM_SIZE),
};

static struct platform_device sram_device = {
	.name		= "rnse-sram",
	.id		= -1,
	.resource	= sram_resources,
	.num_resources	= ARRAY_SIZE(sram_resources),
};

/* The A/D converter's inputs (drivers/iio/adc/rnse-adc.c). */
static struct platform_device adc_device = {
	.name		= "rnse-adc",
	.id		= -1,
};

static struct platform_device *rnse_devices[] __initdata = {
	&coral_device,
	&sram_device,
	&adc_device,
	&flash_device,
	&can_device,
	&ata_device,
	&uart1_device,
	&media_device,
};

/* ---- The battery-backed memory ---------------------------------------- *
 *
 * 256 KiB at 0x10000000 as an nvmem device, "rnse-sram".  All of it can
 * be read; only Linux's own 448 bytes, 0x3f640 to 0x3f7ff, can be
 * written.  Below that are the stock software's settings (up to
 * 0x3f630); above, the SDK's modern firmware's records (0x3f800 to
 * 0x3feff) and the guard of a program in flash at 0x3ff00
 * (sdk/src/flash.c), which rnse-controls.c alone writes.
 */

static void __iomem *sram;

static int sram_read(void *priv, unsigned int off, void *val, size_t n)
{
	memcpy_fromio(val, sram + off, n);
	return 0;
}

static int sram_write(void *priv, unsigned int off, void *val, size_t n)
{
	if (off < SRAM_OURS || off + n > SRAM_OURS_END)
		return -EPERM;
	memcpy_toio(sram + off, val, n);
	return 0;
}

static int rnse_sram_probe(struct platform_device *pdev)
{
	struct nvmem_config config = {
		.dev		= &pdev->dev,
		.name		= "rnse-sram",
		.id		= NVMEM_DEVID_NONE,
		.owner		= THIS_MODULE,
		.size		= SRAM_SIZE,
		.word_size	= 1,
		.stride		= 1,
		.reg_read	= sram_read,
		.reg_write	= sram_write,
	};

	sram = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sram))
		return PTR_ERR(sram);
	return PTR_ERR_OR_ZERO(devm_nvmem_register(&pdev->dev, &config));
}

static struct platform_driver rnse_sram_driver = {
	.driver = { .name = "rnse-sram" },
	.probe = rnse_sram_probe,
};
builtin_platform_driver(rnse_sram_driver);

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
	 * the CAN controller, the card processors and others.  A source
	 * nobody takes would interrupt for ever, so give every source
	 * level 0 (masked) and clear the edge-latch enables, as the SDK's
	 * board_init does; a driver's request_irq unmasks its own.
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
	.mv_init_irq	= rnse_init_irq,
	.mv_mem_reserve	= rnse_mem_reserve,
};
