// SPDX-License-Identifier: GPL-2.0
/*
 * The R8A77730, the system-on-chip of the Audi RNS-E 193 (Renesas'
 * SH-Navi family, no public manual): an SH-4A core with the SH7780's
 * INTC2 at 0xff804000, a TMU at 0xffd80000 and SCIF-like serial ports.
 *
 * Everything here is reverse-engineered from the unit's own software and
 * checked on a bench 193 where it says so: rnse's docs/hardware-193.md
 * ("Interrupts", "Debug serial port") is the source of every address and
 * vector below.
 */
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/serial.h>
#include <linux/serial_sci.h>
#include <linux/sh_timer.h>
#include <linux/sh_intc.h>
#include <linux/io.h>
#include <asm/platform_early.h>

/*
 * The debug serial port, the SH7780's SCIF layout (separate transmit
 * and receive FIFO counts at +0x1c and +0x20).  Nothing has been found
 * that routes its interrupts: the kernel's console writes to it polled,
 * and its tty is not used.  Its vector here is one no source of the
 * 193 has, so that opening it fails rather than steals another's.
 */
static struct plat_sci_port scif0_platform_data = {
	.scscr		= SCSCR_REIE,
	.type		= PORT_SCIF,
	.regtype	= SCIx_SH4_SCIF_FIFODATA_REGTYPE,
};

static struct resource scif0_resources[] = {
	DEFINE_RES_MEM(0xffe40000, 0x100),
	DEFINE_RES_IRQ(evt2irq(0x700)),
};

static struct platform_device scif0_device = {
	.name		= "sh-sci",
	.id		= 0,
	.resource	= scif0_resources,
	.num_resources	= ARRAY_SIZE(scif0_resources),
	.dev		= {
		.platform_data	= &scif0_platform_data,
	},
};

/*
 * Timer unit 0: channel 0 is the stock software's 1 ms tick (INTEVT
 * 0x400), channel 2 its other timer (0x440).  Linux takes channel 0 for
 * its clock events and channel 1 as its clock source; channel 1's own
 * interrupt (0x420) is not routed, and a clock source needs none.
 */
static struct sh_timer_config tmu0_platform_data = {
	.channels_mask = 3,
};

static struct resource tmu0_resources[] = {
	DEFINE_RES_MEM(0xffd80000, 0x30),
	DEFINE_RES_IRQ(evt2irq(0x400)),
	DEFINE_RES_IRQ(evt2irq(0x420)),
};

static struct platform_device tmu0_device = {
	.name		= "sh-tmu",
	.id		= 0,
	.dev = {
		.platform_data	= &tmu0_platform_data,
	},
	.resource	= tmu0_resources,
	.num_resources	= ARRAY_SIZE(tmu0_resources),
};

static struct platform_device *r8a77730_devices[] __initdata = {
	&scif0_device,
	&tmu0_device,
};

static int __init r8a77730_devices_setup(void)
{
	return platform_add_devices(r8a77730_devices,
				    ARRAY_SIZE(r8a77730_devices));
}
arch_initcall(r8a77730_devices_setup);

static struct platform_device *r8a77730_early_devices[] __initdata = {
	&scif0_device,
	&tmu0_device,
};

void __init plat_early_device_setup(void)
{
	sh_early_platform_add_devices(r8a77730_early_devices,
				      ARRAY_SIZE(r8a77730_early_devices));
}

/*
 * The interrupt controller: twelve priority registers from 0xff804000
 * of four 8-bit fields each, mask-set and mask-clear at +0x38/+0x3c and
 * a second pair at +0x108/+0x10c (docs/hardware-193.md, "Interrupts").
 * The level a field gives is its value halved; the kernel's default
 * priority (2) is level 1, which the core takes with interrupts open.
 */
enum {
	UNUSED = 0,

	DU, TMU0, TMU2, TMU2_TICPI, TMU1_0, ATAPI, GFX2D, CAN, DMAC3,
	SSI, SCIF_RADIO, SCIF_FACEPLATE, SDHI, ADC,

	/* interrupt groups */
	TMU,
};

static struct intc_vect vectors[] __initdata = {
	INTC_VECT(DU, 0x3e0),
	INTC_VECT(TMU0, 0x400),
	INTC_VECT(TMU2, 0x440), INTC_VECT(TMU2_TICPI, 0x460),
	INTC_VECT(TMU1_0, 0x480),
	INTC_VECT(ATAPI, 0x5e0),
	INTC_VECT(GFX2D, 0x780),
	INTC_VECT(CAN, 0x880),
	INTC_VECT(DMAC3, 0x980),
	INTC_VECT(SSI, 0xca0),
	INTC_VECT(SCIF_RADIO, 0xd20),
	INTC_VECT(SCIF_FACEPLATE, 0xdc0),
	INTC_VECT(SDHI, 0xea0),
	INTC_VECT(ADC, 0xf00),
};

static struct intc_group groups[] __initdata = {
	INTC_GROUP(TMU, TMU0, TMU2, TMU2_TICPI),
};

static struct intc_mask_reg mask_registers[] __initdata = {
	{ 0xff804038, 0xff80403c, 32, /* INT2MSKR / INT2MSKCR */
	  { 0, 0, 0, 0, DMAC3, 0, CAN, 0,			/* 31..24 */
	    0, 0, 0, 0, GFX2D, 0, 0, 0,				/* 23..16 */
	    0, 0, 0, 0, 0, 0, 0, 0,				/* 15..8 */
	    ATAPI, 0, 0, 0, 0, TMU1_0, TMU, DU } },		/* 7..0 */
	{ 0xff804108, 0xff80410c, 32, /* the second pair */
	  { 0, 0, 0, 0, 0, 0, 0, 0,				/* 31..24 */
	    0, 0, SDHI, 0, 0, 0, 0, 0,				/* 23..16 */
	    0, SCIF_FACEPLATE, 0, 0, 0, 0, SCIF_RADIO, 0,	/* 15..8 */
	    0, 0, SSI, 0, 0, 0, 0, 0 } },			/* 7..0 */
};

static struct intc_prio_reg prio_registers[] __initdata = {
	{ 0xff804000, 0, 32, 8, /* INT2PRI0 */ { DU, TMU0, TMU2, TMU2_TICPI } },
	{ 0xff804004, 0, 32, 8, /* INT2PRI1 */ { TMU1_0, 0, 0, ATAPI } },
	{ 0xff80400c, 0, 32, 8, /* INT2PRI3 */ { 0, 0, 0, GFX2D } },
	{ 0xff804014, 0, 32, 8, /* INT2PRI5 */ { 0, CAN, 0, 0 } },
	{ 0xff804018, 0, 32, 8, /* INT2PRI6 */ { DMAC3, 0, 0, 0 } },
	{ 0xff804024, 0, 32, 8, /* INT2PRI9 */ { SSI, 0, SCIF_RADIO, 0 } },
	{ 0xff804028, 0, 32, 8, /* INT2PRI10 */ { 0, SCIF_FACEPLATE, 0, 0 } },
	{ 0xff80402c, 0, 32, 8, /* INT2PRI11 */ { SDHI, 0, ADC, 0 } },
};

static DECLARE_INTC_DESC(intc_desc, "r8a77730", vectors, groups,
			 mask_registers, prio_registers, NULL);

void __init plat_irq_setup(void)
{
	register_intc_controller(&intc_desc);
}
