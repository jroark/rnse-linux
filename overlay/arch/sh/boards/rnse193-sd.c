// SPDX-License-Identifier: GPL-2.0
/* Two independent SD cards share one SDHI register bank and a slot mux.
 * SDK sdhi_start/read establish the register order and FIFO byte order. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>

#define SDHI ((void __iomem *)0xff90a000)
#define STATUS 0x1c
#define STATUS2 0x1e
#define ERRORS 0x807f
static DEFINE_MUTEX(sd_lock);
static int selected = -1;

struct slot {
	int number;
	bool app;
	struct work_struct work;
	struct mmc_request *request;
	u8 buffer[PAGE_SIZE];
};

static u16 rd(unsigned int off)
{
	return __raw_readw(SDHI + off);
}

static void wr(unsigned int off, u16 value)
{
	__raw_writew(value, SDHI + off);
}

static void select_slot(int number)
{
	if (selected == number)
		return;
	wr(0x24, 0);
	wr(2, number);
	msleep(25);
	selected = number;
}

static int wait_status(unsigned int off, u16 flag)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);

	do {
		u16 error = rd(STATUS2) & ERRORS;

		if (error)
			return error & 0x48 ? -ETIMEDOUT : -EIO;
		if (rd(off) & flag)
			return 0;
		usleep_range(50, 150);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static void transfer(struct work_struct *work)
{
	struct slot *slot = container_of(work, struct slot, work);
	struct mmc_host *host = mmc_from_priv(slot);
	struct mmc_request *req = slot->request;
	struct mmc_command *cmd = req->cmd;
	struct mmc_data *data = req->data;
	u16 word = cmd->opcode | (slot->app ? 0x40 : 0);
	u16 divider = host->ios.clock <= 400000 ? 0x20 : 0;
	unsigned int block;
	int i;

	mutex_lock(&sd_lock);
	select_slot(slot->number);
	slot->app = false;
	if (data && (!(data->flags & MMC_DATA_READ) || data->blocks * data->blksz > sizeof(slot->buffer) ||
		     (data->blksz & 1))) {
		cmd->error = -EINVAL;
		goto done;
	}
	wr(0x24, divider);
	wr(0x24, divider | 0x100);
	if (data) {
		wr(0x26, data->blksz);
		wr(8, data->blocks > 1 ? 0x100 : 0);
		wr(0xa, data->blocks);
	}
	wr(4, cmd->arg);
	wr(6, cmd->arg >> 16);
	wr(STATUS, (u16)~5);
	wr(STATUS2, (u16)~(ERRORS | 0x100));
	wr(0, word);
	cmd->error = wait_status(STATUS, 1);
	if (cmd->error)
		goto done;
	slot->app = cmd->opcode == MMC_APP_CMD;
	if (cmd->flags & MMC_RSP_136) {
		for (i = 0; i < 4; i++) {
			int reg = 0x18 - 4 * i;
			u32 value = rd(reg) | (u32)rd(reg + 2) << 16;

			cmd->resp[i] = value << 8;
			if (i < 3)
				cmd->resp[i] |= rd(reg - 2) >> 8;
		}
	} else {
		cmd->resp[0] = rd(0x0c) | (u32)rd(0x0e) << 16;
	}
	if (data) {
		for (block = 0; block < data->blocks; block++) {
			u8 *dst = slot->buffer + block * data->blksz;

			data->error = wait_status(STATUS2, 0x100);
			if (data->error)
				goto done;
			wr(STATUS2, (u16)~0x100);
			for (i = 0; i < data->blksz; i += 2) {
				u16 pair = rd(0x30);

				dst[i] = pair >> 8;
				dst[i + 1] = pair;
			}
		}
		data->error = wait_status(STATUS, 4);
		if (!data->error)
			data->bytes_xfered = sg_copy_from_buffer(data->sg, data->sg_len,
								slot->buffer, data->blksz * data->blocks);
	}
done:
	if (data && data->blocks > 1) {
		if (cmd->error || data->error)
			wr(8, 1); /* stop the active transfer */
		wr(8, 0);
	}
	mutex_unlock(&sd_lock);
	mmc_request_done(host, req);
}

static void request(struct mmc_host *host, struct mmc_request *req)
{
	struct slot *slot = mmc_priv(host);

	slot->request = req;
	schedule_work(&slot->work);
}

static int card_present(struct mmc_host *host)
{
	struct slot *slot = mmc_priv(host);
	int present;

	mutex_lock(&sd_lock);
	select_slot(slot->number);
	present = !(rd(STATUS) & 0x20);
	mutex_unlock(&sd_lock);
	return present;
}

static int read_only(struct mmc_host *host)
{
	return 1;
}

static void set_ios(struct mmc_host *host, struct mmc_ios *ios)
{
	/* Clock and slot state are applied together under sd_lock. */
}

static const struct mmc_host_ops ops = {
	.request = request, .set_ios = set_ios, .get_cd = card_present, .get_ro = read_only,
};

static int __init rnse193_sd_init(void)
{
	struct platform_device *dev;
	struct mmc_host *hosts[2];
	int i, err;

	dev = platform_device_register_simple("rnse193-sd", -1, NULL, 0);
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	/* linuxboot enabled the SD pins before reading the kernel. */
	wr(0xe0, 0);
	wr(0xe0, 7);
	wr(0xd8, 0);
	wr(0x28, 0x80d9);
	wr(0xf0, 0xda);
	wr(0xfc, 0xfc);
	wr(0x20, 0xffff);
	wr(0x22, 0xffff);
	wr(0xb2, 0xffff);
	for (i = 0; i < 2; i++) {
		struct mmc_host *host = mmc_alloc_host(sizeof(struct slot), &dev->dev);
		struct slot *slot;

		if (!host) {
			err = -ENOMEM;
			goto fail;
		}
		hosts[i] = host;
		slot = mmc_priv(host);
		slot->number = i;
		INIT_WORK(&slot->work, transfer);
		host->ops = &ops;
		host->caps = MMC_CAP_NEEDS_POLL;
		host->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
		host->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
		host->f_min = 400000;
		host->f_max = 24924000;
		host->max_blk_count = PAGE_SIZE / 512;
		host->max_req_size = host->max_seg_size = PAGE_SIZE;
		host->max_blk_size = 512;
		err = mmc_add_host(host);
		if (err) {
			mmc_free_host(host);
			goto fail;
		}
	}
	return 0;
fail:
	while (i--) {
		mmc_remove_host(hosts[i]);
		mmc_free_host(hosts[i]);
	}
	platform_device_unregister(dev);
	return err;
}
device_initcall(rnse193_sd_init);
