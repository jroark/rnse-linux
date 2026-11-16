// SPDX-License-Identifier: GPL-2.0
/*
 * The Audi RNS-E 192's CD/DVD drive: a standard ATAPI drive on the
 * companion chip's bus (docs/hardware.md, "CD/DVD drive").
 *
 *   0x07800001 + 2n   the task file's register n as bytes (1 error and
 *                     features, ... 7 status and command)
 *   0x07880000        the 16-bit data register, wired so that the first
 *                     byte of a pair is the high half of the word: read
 *                     and written raw, the bytes are in their order
 *   0x0790000d        alternate status and device control
 *
 * The stock software moves disc data with the DMA controller (channel 1,
 * through boot ROM services); this driver uses programmed I/O only, as
 * the SDK does (sdk/src/cd.c), with the drive's INTRQ through the
 * companion chip (level 3).  The drive answers as the master.
 *
 * The disc has to come out whatever Linux is doing with it (CLAUDE.md,
 * "Mandatory hardware behavior"): rnse_ata_eject, which the controls'
 * thread (arch/sh/boards/rnse-controls.c) calls on CD EJECT, allows the
 * medium's removal and then sends START STOP UNIT with LoEj through the
 * SCSI layer, behind whatever the drive is doing.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/libata.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>
#include <linux/platform_data/rnse.h>

#define DRV_NAME "pata_rnse"

static struct ata_port *the_port;

static int rnse_set_mode(struct ata_link *link, struct ata_device **unused)
{
	struct ata_device *dev;

	/* The bus has nothing to set: whatever mode the drive is in. */
	ata_for_each_dev(dev, link, ENABLED) {
		dev->pio_mode = dev->xfer_mode = XFER_PIO_0;
		dev->xfer_shift = ATA_SHIFT_PIO;
		dev->flags |= ATA_DFLAG_PIO;
		ata_dev_info(dev, "configured for PIO\n");
	}
	return 0;
}

static const struct scsi_host_template rnse_sht = {
	ATA_PIO_SHT(DRV_NAME),
};

static struct ata_port_operations rnse_port_ops = {
	.inherits	= &ata_sff_port_ops,
	.cable_detect	= ata_cable_40wire,
	.set_mode	= rnse_set_mode,
	.sff_data_xfer	= ata_sff_data_xfer,
};

static struct scsi_device *drive(void)
{
	struct ata_device *dev;

	if (!the_port)
		return NULL;
	dev = &the_port->link.device[0];
	return ata_dev_enabled(dev) ? dev->sdev : NULL;
}

bool rnse_ata_eject_available(void)
{
	return drive() != NULL;
}

int rnse_ata_eject(void)
{
	static const u8 allow[6] = { 0x1e, 0, 0, 0, 0, 0 };	/* PREVENT ALLOW: allow */
	static const u8 eject[6] = { 0x1b, 1, 0, 0, 2, 0 };	/* START STOP UNIT: LoEj */
	struct scsi_device *sdev = drive();
	const struct scsi_exec_args args = {};
	int r, i;

	if (!sdev)
		return -ENODEV;
	if (scsi_device_get(sdev))
		return -ENODEV;
	scsi_execute_cmd(sdev, allow, REQ_OP_DRV_IN, NULL, 0, 10 * HZ, 1, &args);
	/* A "medium changed" or "becoming ready" to be heard first. */
	for (i = 0; i < 3; i++) {
		r = scsi_execute_cmd(sdev, eject, REQ_OP_DRV_IN, NULL, 0, 30 * HZ, 1, &args);
		if (!r)
			break;
	}
	scsi_device_put(sdev);
	return r ? -EIO : 0;
}

static int rnse_ata_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ata_host *host;
	struct ata_port *ap;
	void __iomem *task, *data, *ctl;
	int irq;

	task = devm_platform_ioremap_resource(pdev, 0);
	data = devm_platform_ioremap_resource(pdev, 1);
	ctl = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(task) || IS_ERR(data) || IS_ERR(ctl))
		return -ENOMEM;
	irq = platform_get_irq_optional(pdev, 0);

	host = ata_host_alloc(dev, 1);
	if (!host)
		return -ENOMEM;
	ap = host->ports[0];
	ap->ops = &rnse_port_ops;
	ap->pio_mask = ATA_PIO4;
	if (irq <= 0) {
		irq = 0;
		ap->flags |= ATA_FLAG_PIO_POLLING;
	}

	/* Register n at +1 + 2n; the data register on its own. */
	ap->ioaddr.cmd_addr = task + 1;
	ap->ioaddr.data_addr = data;
	ap->ioaddr.error_addr = task + 1 + 2 * ATA_REG_ERR;
	ap->ioaddr.feature_addr = task + 1 + 2 * ATA_REG_FEATURE;
	ap->ioaddr.nsect_addr = task + 1 + 2 * ATA_REG_NSECT;
	ap->ioaddr.lbal_addr = task + 1 + 2 * ATA_REG_LBAL;
	ap->ioaddr.lbam_addr = task + 1 + 2 * ATA_REG_LBAM;
	ap->ioaddr.lbah_addr = task + 1 + 2 * ATA_REG_LBAH;
	ap->ioaddr.device_addr = task + 1 + 2 * ATA_REG_DEVICE;
	ap->ioaddr.status_addr = task + 1 + 2 * ATA_REG_STATUS;
	ap->ioaddr.command_addr = task + 1 + 2 * ATA_REG_CMD;
	ap->ioaddr.ctl_addr = ctl + 1;
	ap->ioaddr.altstatus_addr = ctl + 1;

	ata_port_desc(ap, "mmio cmd 0x07800001 data 0x07880000 ctl 0x0790000d");
	the_port = ap;
	return ata_host_activate(host, irq, irq ? ata_sff_interrupt : NULL, 0, &rnse_sht);
}

static struct platform_driver rnse_ata_driver = {
	.driver = { .name = "pata-rnse" },
	.probe = rnse_ata_probe,
};
builtin_platform_driver(rnse_ata_driver);
MODULE_DESCRIPTION("Audi RNS-E 192 CD/DVD drive");
MODULE_LICENSE("GPL");
