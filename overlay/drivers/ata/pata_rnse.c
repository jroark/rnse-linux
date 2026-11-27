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

/* The 193 exposes the low bits of each word on a 32-bit bus.
 * ATAPI packet data starts in the low byte of the first data word. */
static u8 status193(struct ata_port *ap)
{
	return __raw_readl(ap->ioaddr.status_addr);
}

static u8 alt193(struct ata_port *ap)
{
	return __raw_readl(ap->ioaddr.ctl_addr);
}

static void ctl193(struct ata_port *ap, u8 value)
{
	__raw_writel(value, ap->ioaddr.ctl_addr);
}

static void select193(struct ata_port *ap, unsigned int device)
{
	__raw_writel(ATA_DEVICE_OBS | device << 4, ap->ioaddr.device_addr);
	ata_sff_pause(ap);
}

static void load193(struct ata_port *ap, const struct ata_taskfile *tf)
{
	struct ata_ioports *io = &ap->ioaddr;

	if (tf->ctl != ap->last_ctl) {
		ctl193(ap, tf->ctl);
		ap->last_ctl = tf->ctl;
	}
	if (tf->flags & ATA_TFLAG_ISADDR) {
		__raw_writel(tf->feature, io->feature_addr);
		__raw_writel(tf->nsect, io->nsect_addr);
		__raw_writel(tf->lbal, io->lbal_addr);
		__raw_writel(tf->lbam, io->lbam_addr);
		__raw_writel(tf->lbah, io->lbah_addr);
	}
	if (tf->flags & ATA_TFLAG_DEVICE)
		__raw_writel(tf->device, io->device_addr);
	ata_sff_pause(ap);
}

static void read193(struct ata_port *ap, struct ata_taskfile *tf)
{
	struct ata_ioports *io = &ap->ioaddr;

	tf->status = status193(ap);
	tf->error = __raw_readl(io->error_addr);
	tf->nsect = __raw_readl(io->nsect_addr);
	tf->lbal = __raw_readl(io->lbal_addr);
	tf->lbam = __raw_readl(io->lbam_addr);
	tf->lbah = __raw_readl(io->lbah_addr);
	tf->device = __raw_readl(io->device_addr);
}

static void exec193(struct ata_port *ap, const struct ata_taskfile *tf)
{
	__raw_writel(tf->command, ap->ioaddr.command_addr);
	ata_sff_pause(ap);
}

static unsigned int xfer193(struct ata_queued_cmd *qc, unsigned char *buf,
			    unsigned int len, int rw)
{
	void __iomem *port = qc->ap->ioaddr.data_addr;
	unsigned int i;

	for (i = 0; i < len; i += 2) {
		if (rw == READ) {
			u32 pair = __raw_readl(port);

			buf[i] = pair;
			if (i + 1 < len)
				buf[i + 1] = pair >> 8;
		} else {
			__raw_writel(buf[i] | (i + 1 < len ? buf[i + 1] << 8 : 0), port);
		}
	}
	return ALIGN(len, 2);
}

static int reset193(struct ata_link *link, unsigned int *classes, unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	int err;

	ctl193(ap, ap->ctl | ATA_SRST);
	udelay(20);
	ctl193(ap, ap->ctl);
	err = ata_sff_wait_after_reset(link, 1, deadline);
	if (err)
		return err;
	classes[0] = ata_sff_dev_classify(&link->device[0], 1, NULL);
	return 0;
}

static struct ata_port_operations ops193 = {
	.inherits = &rnse_port_ops, .sff_check_status = status193,
	.sff_check_altstatus = alt193, .sff_set_devctl = ctl193,
	.sff_dev_select = select193, .sff_tf_load = load193, .sff_tf_read = read193,
	.sff_exec_command = exec193, .sff_data_xfer = xfer193, .reset.softreset = reset193,
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
	int err, irq, spacing = IS_ENABLED(CONFIG_SH_RNSE193) ? 4 : 2;

	task = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(task))
		return PTR_ERR(task);
	if (IS_ENABLED(CONFIG_SH_RNSE193)) {
		data = task;
		ctl = task + 0x38;
		irq = 0;
		__raw_writel(0, task + 0x88); /* polled PIO */
	} else {
		data = devm_platform_ioremap_resource(pdev, 1);
		ctl = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(data) || IS_ERR(ctl))
			return -ENOMEM;
		task++;
		ctl++;
		irq = platform_get_irq_optional(pdev, 0);
	}

	host = ata_host_alloc(dev, 1);
	if (!host)
		return -ENOMEM;
	ap = host->ports[0];
	ap->ops = IS_ENABLED(CONFIG_SH_RNSE193) ? &ops193 : &rnse_port_ops;
	ap->pio_mask = ATA_PIO4;
	if (irq <= 0) {
		irq = 0;
		ap->flags |= ATA_FLAG_PIO_POLLING;
	}

	/* Register n at +1 + 2n; the data register on its own. */
	ap->ioaddr.cmd_addr = task;
	ap->ioaddr.data_addr = data;
	ap->ioaddr.error_addr = task + spacing * ATA_REG_ERR;
	ap->ioaddr.feature_addr = task + spacing * ATA_REG_FEATURE;
	ap->ioaddr.nsect_addr = task + spacing * ATA_REG_NSECT;
	ap->ioaddr.lbal_addr = task + spacing * ATA_REG_LBAL;
	ap->ioaddr.lbam_addr = task + spacing * ATA_REG_LBAM;
	ap->ioaddr.lbah_addr = task + spacing * ATA_REG_LBAH;
	ap->ioaddr.device_addr = task + spacing * ATA_REG_DEVICE;
	ap->ioaddr.status_addr = task + spacing * ATA_REG_STATUS;
	ap->ioaddr.command_addr = task + spacing * ATA_REG_CMD;
	ap->ioaddr.ctl_addr = ctl;
	ap->ioaddr.altstatus_addr = ctl;

	err = ata_host_activate(host, irq, irq ? ata_sff_interrupt : NULL, 0, &rnse_sht);

	if (!err)
		the_port = ap;
	return err;
}

static struct platform_driver rnse_ata_driver = {
	.driver = { .name = "pata-rnse" },
	.probe = rnse_ata_probe,
};
builtin_platform_driver(rnse_ata_driver);
MODULE_DESCRIPTION("Audi RNS-E 192 CD/DVD drive");
MODULE_LICENSE("GPL");
