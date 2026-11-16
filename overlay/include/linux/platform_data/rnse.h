/* SPDX-License-Identifier: GPL-2.0 */
/*
 * What the Audi RNS-E 192's drivers share: the companion chip's
 * interrupt sources, the A/D converter, the CD drive's eject, and
 * whether the kernel was started from flash or by the SD loader.
 *
 * See docs/linux.md and docs/hardware.md in the rnse repository.
 */
#ifndef __LINUX_PLATFORM_DATA_RNSE_H
#define __LINUX_PLATFORM_DATA_RNSE_H

#include <linux/types.h>

/*
 * The companion chip drives the SH-4's IRL lines, encoded: a source
 * asks at the level its 4-bit field holds, 0 masks it, and the CPU sees
 * the highest level asking as INTEVT 0x200 + (15 - level) * 0x20, which
 * is Linux IRQ INTEVT >> 5, 31 - level (arch/sh/kernel/cpu/sh3/entry.S).  Each source here has a level of its own
 * (the stock software's), so that IRQ and source are one.
 */
#define RNSE_LEVEL_ATAPI	3
#define RNSE_LEVEL_UART1	2
#define RNSE_LEVEL_CAN		5

#define RNSE_IRQ(level)		(31 - (level))
#define RNSE_IRQ_ATAPI		RNSE_IRQ(RNSE_LEVEL_ATAPI)
#define RNSE_IRQ_UART1		RNSE_IRQ(RNSE_LEVEL_UART1)
#define RNSE_IRQ_CAN		RNSE_IRQ(RNSE_LEVEL_CAN)

/* rnse-controls.c */
int rnse_adc_read(int input);		/* 0..1023, or a negative error */
bool rnse_started_from_flash(void);

/*
 * pata_rnse.c: eject the disc through libata when it owns the drive.
 * Returns -ENODEV when no such driver is bound (the caller then talks to
 * the drive itself), else 0 or another negative error.
 */
int rnse_ata_eject(void);
bool rnse_ata_eject_available(void);

/*
 * The two card processors (rnse-media.c).  Before the way back to the
 * SD loader: stop sound and leave the processors as that loader can use
 * them.
 */
void rnse_media_quiesce(void);

/* rnse-uart.c: a frame to the radio processor between user space's. */
int rnse_radio_send(const u8 *buf, int n);
void rnse_can_quiesce(void);

#endif
