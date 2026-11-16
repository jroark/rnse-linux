// SPDX-License-Identifier: GPL-2.0
/*
 * The Audi RNS-E 192's two SD card processors (NEC uPD77115A), one for
 * each slot: the cards, read only, as block devices (rnsesd0, rnsesd1),
 * and sound out of a processor's converter as an ALSA card ("RNSE").
 *
 * The slots belong to the processors, which do nothing until a program
 * is loaded into them; the main processor talks to each through a host
 * port, a 16-bit word at a time (docs/hardware.md, "SD card
 * processors"):
 *
 *   0x15000002, 0x1500004a  host data port of processor 0, 1
 *   0x12400001              status: bits 2, 0 nothing to read (0, 1);
 *                           bits 3, 1 cannot take a word (0, 1)
 *   0x16000004              bits 2, 3: the processors' reset lines, low
 *                           = reset
 *   0x12500001 bit 6, 0x12900001 bit 0: a card in slot 0, 1
 *
 * Cards.  This is sdk/src/sd.c in the kernel: the installed software's
 * program, out of the flash at 0x200000 (it is part of the North
 * American V222 0371 operating system, and stays in the flash when
 * Linux is installed there: only the OS module's first part is
 * replaced), sent as that software's driver sends it, then the card's
 * start and, for every sector, "read it into your memory" and "give me
 * that memory".  A card that the stock software (or the SD loader) has
 * started already is taken as it is, as sd_attach does: nothing reset,
 * a sector asked for, and whether it counts in bytes or sectors found
 * by trying.  That is also the way to a card the SDK's own start fails
 * on.  The size of the disk is what the card's partition table (or,
 * with none, its file system) says: in that case the card's own
 * registers have not been read.
 *
 * Sound.  sdk/src/pcm.c: a program of our own in a processor takes
 * 16-bit words from the host port and gives them to its converter,
 * left then right, at 44.1 kHz from the card board's crystal, and the
 * DMA controller moves them (channel 7 for processor 0, 3 for
 * processor 1), paced by the processor.  The whole ring buffer is one
 * transfer; at its end the interrupt starts the next.  A processor that
 * plays has no card: sound takes the processor of a slot without a disk
 * (or with one nobody has open, which is removed), processor 0 first,
 * or the one the "PCM Processor" control names.  Which converter
 * reaches the amplifier on a unit is not known yet (sdk/examples/pcmtest
 * asks); the amplifier's input itself is the radio processor's
 * (rnse-radio source decoder).  Samples are big-endian, the CPU's.
 *
 * Everything is polled but the end of a sound transfer.  See
 * docs/linux.md.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/hrtimer.h>
#include <linux/dma-mapping.h>
#include <linux/sh_intc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/control.h>
#include <sound/initval.h>
#include <linux/platform_data/rnse.h>

#define STATUS		0x12400001
#define RESET_LATCH	0x16000004
#define FLASH_PROGRAM	0x00200000
#define FLASH_PROGRAM_LEN 0x10000
#define DMAOR		0xffa00040
#define DMA_BASE	0xffa00000

static const u32 port_addr[2] = { 0x15000002, 0x1500004a };
static const u32 present_addr[2] = { 0x12500001, 0x12900001 };
static const u8 present_bit[2] = { 0x40, 0x01 };
static const u8 none_bit[2] = { 4, 1 };	/* nothing to read */
static const u8 full_bit[2] = { 8, 2 };	/* cannot take a word */
static const u32 dma_off[2] = { 0x80, 0x30 };	/* SAR of channel 7, 3 */
#define DMTE0		0x640
static const u32 selector_addr[2] = { 0x12d00007, 0x12d00003 };
static const u8 select_on[2] = { 0x23, 0x24 };

enum { EMPTY, CARD, PCM };

struct slot {
	int n;
	void __iomem *port, *present;
	struct mutex lock;		/* the processor and its port */
	int state;
	bool high_capacity;
	u16 inbox[64];
	u32 in, out;
	struct gendisk *disk;
	struct blk_mq_tag_set tags;
	int failed;			/* card starts that failed in a row */
};

static struct {
	struct platform_device *pdev;
	void __iomem *status, *latch, *program, *dmac, *dmaor;
	void __iomem *selector[2];
	spinlock_t latch_lock;
	struct slot slot[2];
	struct task_struct *thread;
	/* sound */
	struct snd_card *card;
	struct snd_pcm_substream *sub;
	int pcm_proc;			/* the processor playing, -1 */
	int pcm_choice;			/* -1 any, else 0 or 1 */
	struct hrtimer timer;
	snd_pcm_uframes_t last_pos;
	bool running;
	spinlock_t pcm_lock;
	int major;
} m = { .pcm_proc = -1, .pcm_choice = -1 };

static u8 status(void)
{
	return __raw_readb(m.status);
}

static void reset_line(int n, int run)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&m.latch_lock, flags);
	v = __raw_readl(m.latch);
	v = run ? v | (4u << n) : v & ~(4u << n);
	__raw_writel(v, m.latch);
	spin_unlock_irqrestore(&m.latch_lock, flags);
}

static bool card_present(struct slot *s)
{
	return __raw_readb(s->present) & present_bit[s->n];
}

/* ---- The host port ------------------------------------------------------ */

static int put(struct slot *s, u16 w)
{
	unsigned long end = jiffies + msecs_to_jiffies(3000);

	for (;;) {
		u8 st = status();

		/* Words the processor says while the host waits to say one
		 * are taken and kept (sd.c, "inbox"). */
		if (!(st & none_bit[s->n]) && s->in - s->out < 64) {
			s->inbox[s->in++ % 64] = __raw_readw(s->port);
			continue;
		}
		if (!(st & full_bit[s->n]))
			break;
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		cond_resched();
	}
	/* The program fetches the words after a message's head itself with
	 * its interrupt masked: the next word not at once. */
	udelay(20);
	__raw_writew(w, s->port);
	return 0;
}

static int get(struct slot *s, u16 *w, unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);

	if (s->in != s->out) {
		*w = s->inbox[s->out++ % 64];
		return 0;
	}
	while (status() & none_bit[s->n]) {
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		cond_resched();
	}
	*w = __raw_readw(s->port);
	return 0;
}

static int answer(struct slot *s, u16 *out, int max, unsigned int ms)
{
	u16 head, w;
	int i;

	if (get(s, &head, ms))
		return -ETIMEDOUT;
	for (i = 0; i < (head & 0xff); i++) {
		if (get(s, &w, 300))
			return -ETIMEDOUT;
		if (i < max)
			out[i] = w;
	}
	return head;
}

static int say(struct slot *s, const u16 *msg, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (put(s, msg[i]))
			return -ETIMEDOUT;
	return answer(s, NULL, 0, 1000) == 0xf000 ? 0 : -EIO;
}

static int poke(struct slot *s, u16 space, u16 addr, u16 value)
{
	u16 msg[4] = { 0x2303, space, addr, 1 };

	if (say(s, msg, 4))
		return -EIO;
	return put(s, value);
}

static int command(struct slot *s, u16 head, u16 reg, u32 arg, u16 *r, int expect)
{
	u16 msg[4] = { head, reg, arg >> 16, arg };
	int got;

	memset(r, 0, 9 * sizeof(*r));
	if (say(s, msg, 4))
		return -EIO;
	if (!expect)
		return 0;
	got = answer(s, r, 9, 2000);
	return got < 0 || (got >> 8) == 0xee ? -EIO : 0;
}

/* ---- Cards ---------------------------------------------------------------- */

enum { FROM_FLASH, ZEROES, LITERAL, ANSWER };
static const u16 literal[] = {
	0x0001, 0x0e00, 0x0c00, 0x0002, 0x1c6f, 0x8000, 0x00f6, 0x9f00, 0x0004,
	0x0002, 0x0bfb, 0x0000, 0x0000, 0x026c, 0x2994, 0x0467, 0x4900, 0x0001,
	0x4ef2, 0x0001, 0x0002, 0x061d, 0x29e3, 0x0f4f, 0x4000,
};
/* How the stock driver's download is put together from the flash
 * (sdk/src/sd.c; tools/dsp-recipe.py made it from a trace). */
static const struct step { u8 kind; u32 at; u16 count; } recipe[] = {
	{ LITERAL, 0x000000, 3 }, { FROM_FLASH, 0x20000c, 7168 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x000003, 3 }, { FROM_FLASH, 0x203818, 14558 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x000006, 2 }, { FROM_FLASH, 0x20a9dc, 492 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x000008, 5 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x00000d, 2 }, { FROM_FLASH, 0x20adcc, 620 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x00000f, 2 }, { FROM_FLASH, 0x20b2ac, 1127 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x000011, 3 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x000014, 3 }, { ZEROES, 0, 58 }, { FROM_FLASH, 0x20bc04, 1507 }, { ANSWER, 0, 0 },
	{ LITERAL, 0x000017, 2 }, { ZEROES, 0, 31 }, { FROM_FLASH, 0x20c810, 3888 }, { ANSWER, 0, 0 },
};

static u16 flash_word(u32 at)
{
	const void __iomem *p = m.program + (at - FLASH_PROGRAM);

	return __raw_readb(p) | __raw_readb(p + 1) << 8;
}

static bool program_known(void)
{
	return flash_word(0x200000) == 1 && flash_word(0x200004) == 0x3800 &&
	       flash_word(0x20380c) == 2 && flash_word(0x203810) == 0x71bc;
}

static int download(struct slot *s)
{
	u32 i, k;
	u16 w;

	for (i = 0; i < ARRAY_SIZE(recipe); i++) {
		const struct step *st = &recipe[i];

		if (st->kind == ANSWER) {
			if (get(s, &w, 2000))
				return -ETIMEDOUT;
			continue;
		}
		for (k = 0; k < st->count; k++) {
			w = st->kind == FROM_FLASH ? flash_word(st->at + 2 * k) :
			    st->kind == LITERAL ? literal[st->at + k] : 0;
			if (put(s, w))
				return -ETIMEDOUT;
		}
	}
	return 0;
}

/* The stock driver leaves 150 ms between two steps of a card's start. */
static void pause(void)
{
	msleep(150);
}

/* sd_init: the processor reset, the program sent, the card started. */
static int card_start(struct slot *s)
{
	static const u16 start[] = { 0x1202, 0xd762, 0x0034 };
	static const u16 a[] = { 0x7301, 0x0002 }, b[] = { 0x3301, 0x0001 }, c[] = { 0x3101, 0x0001 };
	static const u16 d[] = { 0x3201, 0x0000 }, e[] = { 0x0301, 0x0000 }, f[] = { 0x0301, 0x0001 };
	u16 r[9], rca;
	u32 ocr = 0, hcs, i;
	unsigned long t;
	int mmc = 0;

	s->in = s->out = 0;
	s->high_capacity = false;
	reset_line(s->n, 0);
	msleep(20);
	reset_line(s->n, 1);
	msleep(20);
	if (download(s))
		return -3;
	msleep(300);
	if (say(s, start, 3) || say(s, a, 2) || say(s, b, 2) || say(s, c, 2))
		return -4;
	if (put(s, d[0]) || put(s, d[1]) || answer(s, r, 9, 1000) < 0)
		return -4;
	pause();
	if (poke(s, 0, 0x3814, 0x3802))
		return -5;
	pause();
	if (poke(s, 1, 0x4fb1, 0x0100))
		return -5;
	pause();
	if (say(s, e, 2))
		return -5;
	pause();
	if (command(s, 0x0b03, 0x4800, 0, r, 0))		/* CMD0 */
		return -6;
	pause();
	hcs = command(s, 0x0b03, 0x4c08, 0x000001aa, r, 1) ? 0 : 0x40000000;	/* CMD8 */
	for (i = 0;; i++) {
		pause();
		if (!command(s, 0x0b03, 0x4c37, 0, r, 1))	/* CMD55 */
			break;
		if (i == 6)
			return -7;
		pause();
		if (command(s, 0x0b03, 0x4800, 0, r, 0))	/* CMD0 */
			return -6;
		pause();
		if (!command(s, 0x0b03, 0x4c01, 0x00ff0000, r, 1)) {	/* CMD1 */
			mmc = 1;
			break;
		}
	}
	t = jiffies;
	for (i = 0;; i++) {
		if (mmc) {
			if (i) {
				msleep(300);
				if (command(s, 0x0b03, 0x4c01, 0x00ff0000, r, 1))
					return -7;
			}
		} else {
			if (i) {
				pause();
				if (command(s, 0x0b03, 0x4c37, 0, r, 1))
					return -7;
			}
			pause();
			if (command(s, 0x0b03, 0x4c29, hcs | 0x00ff0000, r, 1))	/* ACMD41 */
				return -7;
		}
		ocr = (u32)(r[0] & 0xff) << 24 | (u32)r[1] << 8 | r[2] >> 8;
		if (ocr & 0x80000000)
			break;
		if (time_after(jiffies, t + msecs_to_jiffies(6000)))
			return -8;
		if (!mmc) {
			pause();
			if (poke(s, 0, 0x3814, 0x3802))
				return -5;
		}
	}
	s->high_capacity = !mmc && (ocr & 0x40000000);
	if (!mmc && hcs && !s->high_capacity) {
		pause();
		if (command(s, 0x0b03, 0x4c37, 0, r, 1))
			return -7;
		pause();
		if (command(s, 0x0b03, 0x4c29, 0x00ff0000, r, 1))
			return -7;
	}
	pause();
	if (poke(s, 0, 0x3814, 0x3802))
		return -5;
	pause();
	if (command(s, 0x0b03, 0x4c42, 0, r, 1))		/* CMD2 */
		return -9;
	pause();
	if (command(s, 0x0b03, 0x4c03, 0, r, 1))		/* CMD3 */
		return -9;
	rca = mmc ? 0 : (r[0] & 0xff) << 8 | r[1] >> 8;
	pause();
	if (command(s, 0x0b03, 0x4c49, (u32)rca << 16, r, 1))	/* CMD9 */
		return -9;
	pause();
	if (command(s, 0x0b03, 0x4c4a, (u32)rca << 16, r, 1))	/* CMD10 */
		return -9;
	pause();
	if (poke(s, 0, 0x3814, 0x0c02))
		return -5;
	pause();
	if (poke(s, 1, 0x4fb1, 0x0004))
		return -5;
	pause();
	if (command(s, 0x0b03, 0x4c07, (u32)rca << 16, r, 1))	/* CMD7 */
		return -10;
	pause();
	if (command(s, 0x0b03, 0x4c10, 0x200, r, 1))		/* CMD16 */
		return -10;
	pause();
	if (say(s, f, 2))
		return -11;
	pause();
	if (put(s, 0x7400) || answer(s, r, 9, 1000) < 0)
		return -11;
	return 0;
}

/* A sector: where in its memory the processor is to put it and how many
 * words, the card's CMD17, then that memory.  The step that failed, as
 * a negative number. */
static int read_sector_once(struct slot *s, u32 sector, u8 *out)
{
	static const u16 fetch[] = { 0x2203, 0x0001, 0x5000, 0x0100 };
	u16 r[9], w;
	int i;

	if (poke(s, 1, 0x4fb6, 0x5000) || poke(s, 1, 0x4fb5, 0x0100))
		return -1;
	if (command(s, 0x0503, 0x4d11, s->high_capacity ? sector : sector << 9, r, 1))
		return -2;
	for (i = 0; i < 4; i++)
		if (put(s, fetch[i]))
			return -3;
	if (get(s, &w, 1000) || w != 0xd301 || get(s, &w, 300))
		return -4;
	for (i = 0; i < 256; i++) {
		if (get(s, &w, 300))
			return -5;
		out[2 * i] = w >> 8;
		out[2 * i + 1] = w;
	}
	return 0;
}

/* Three tries: after one that failed, whatever the processor still has
 * to say is taken and thrown away, so that the next starts in step. */
static int read_sector(struct slot *s, u32 sector, u8 *out)
{
	int try, r = 0, n;
	u16 w;

	for (try = 0; try < 3; try++) {
		r = read_sector_once(s, sector, out);
		if (!r)
			return 0;
		pr_warn_ratelimited("rnse: slot %d, sector %u: step %d failed%s\n",
				    s->n, sector, -r, try < 2 ? ", again" : "");
		s->in = s->out = 0;
		for (n = 0; n < 600 && get(s, &w, 20) == 0; n++)
			;
	}
	return -EIO;
}

static u32 le16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 le32(const u8 *p) { return le16(p) | le16(p + 2) << 16; }

static bool is_volume(const u8 *sec)
{
	return sec[510] == 0x55 && sec[511] == 0xaa && le16(sec + 11) == 512 && sec[13];
}

static u32 volume_sectors(const u8 *sec)
{
	return le16(sec + 19) ? le16(sec + 19) : le32(sec + 32);
}

/* What the card holds, in sectors, by its partition table or (with
 * none) its file system; and whether it counts in sectors.  sd_attach
 * finds the second by trying. */
static int card_size(struct slot *s, u8 *sec, sector_t *size)
{
	u32 first, end = 0;
	int i;

	if (read_sector(s, 0, sec))
		return -EIO;
	if (sec[510] != 0x55 || sec[511] != 0xaa)
		return -ENODEV;
	if (is_volume(sec)) {
		*size = volume_sectors(sec);
		return 0;
	}
	for (i = 0; i < 4; i++) {
		const u8 *e = sec + 446 + 16 * i;

		if (e[4] && le32(e + 8) + le32(e + 12) > end)
			end = le32(e + 8) + le32(e + 12);
	}
	first = le32(sec + 446 + 8);
	if (!first || !end)
		return -ENODEV;
	*size = end;
	if (!read_sector(s, first, sec) && is_volume(sec))
		return 0;
	s->high_capacity = !s->high_capacity;
	if (!read_sector(s, first, sec) && is_volume(sec))
		return 0;
	return -ENODEV;
}

/* A card started by somebody else: words still meant for that one
 * thrown away, and a sector asked for. */
static int card_attach(struct slot *s, u8 *sec, sector_t *size)
{
	u16 w;
	int i;

	s->in = s->out = 0;
	s->high_capacity = false;
	for (i = 0; i < 600 && get(s, &w, 20) == 0; i++)
		;
	return card_size(s, sec, size);
}

/* ---- The disks ------------------------------------------------------------ */

static blk_status_t rnse_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct slot *s = hctx->queue->queuedata;
	struct req_iterator iter;
	struct bio_vec bv;
	sector_t sector = blk_rq_pos(rq);
	blk_status_t st = BLK_STS_OK;

	blk_mq_start_request(rq);
	if (req_op(rq) != REQ_OP_READ) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}
	mutex_lock(&s->lock);
	rq_for_each_segment(bv, rq, iter) {
		u8 *buf = bvec_kmap_local(&bv);
		unsigned int off;

		for (off = 0; off < bv.bv_len; off += 512, sector++) {
			if (s->state != CARD || read_sector(s, sector, buf + off)) {
				st = BLK_STS_IOERR;
				break;
			}
		}
		kunmap_local(buf);
		if (st)
			break;
	}
	mutex_unlock(&s->lock);
	blk_mq_end_request(rq, st);
	return BLK_STS_OK;
}

static const struct blk_mq_ops rnse_mq_ops = {
	.queue_rq = rnse_queue_rq,
};

static const struct block_device_operations rnse_bdops = {
	.owner = THIS_MODULE,
};

static int disk_add(struct slot *s, sector_t size)
{
	struct queue_limits lim = {
		.logical_block_size = 512,
		.max_hw_sectors = 64,
		.max_segments = 32,
	};
	struct gendisk *disk;
	int r;

	memset(&s->tags, 0, sizeof(s->tags));
	s->tags.ops = &rnse_mq_ops;
	s->tags.nr_hw_queues = 1;
	s->tags.queue_depth = 4;
	s->tags.numa_node = NUMA_NO_NODE;
	s->tags.flags = BLK_MQ_F_BLOCKING;
	r = blk_mq_alloc_tag_set(&s->tags);
	if (r)
		return r;
	disk = blk_mq_alloc_disk(&s->tags, &lim, s);
	if (IS_ERR(disk)) {
		blk_mq_free_tag_set(&s->tags);
		return PTR_ERR(disk);
	}
	disk->major = m.major;
	disk->first_minor = 8 * s->n;
	disk->minors = 8;
	disk->fops = &rnse_bdops;
	disk->private_data = s;
	snprintf(disk->disk_name, sizeof(disk->disk_name), "rnsesd%d", s->n);
	set_capacity(disk, size);
	set_disk_ro(disk, true);
	r = add_disk(disk);
	if (r) {
		put_disk(disk);
		blk_mq_free_tag_set(&s->tags);
		return r;
	}
	s->disk = disk;
	pr_info("rnse: card in slot %d, %llu sectors%s\n", s->n,
		(unsigned long long)size, s->high_capacity ? ", high capacity" : "");
	return 0;
}

static void disk_remove(struct slot *s)
{
	if (!s->disk)
		return;
	del_gendisk(s->disk);
	put_disk(s->disk);
	blk_mq_free_tag_set(&s->tags);
	s->disk = NULL;
	pr_info("rnse: card in slot %d gone\n", s->n);
}

/* ---- The thread: cards in and out ----------------------------------------- */

static void slot_check(struct slot *s, u8 *sec, bool first)
{
	sector_t size;
	int r;

	if (s->state == PCM)
		return;
	if (!card_present(s)) {
		if (s->state == CARD) {
			mutex_lock(&s->lock);
			s->state = EMPTY;
			mutex_unlock(&s->lock);
			disk_remove(s);
		}
		s->failed = 0;
		return;
	}
	if (s->state == CARD || s->failed >= 3)
		return;
	mutex_lock(&s->lock);
	r = -ENODEV;
	/* At the first look a card may be running already, the SD loader's
	 * or the stock software's; not when the boot ROM started Linux
	 * from the flash, which leaves the processors in reset. */
	if (first && !rnse_started_from_flash())
		r = card_attach(s, sec, &size);
	if (r) {
		r = card_start(s);
		if (!r)
			r = card_size(s, sec, &size);
	}
	if (!r)
		s->state = CARD;
	mutex_unlock(&s->lock);
	if (r) {
		s->failed++;
		pr_warn("rnse: the card in slot %d does not start (%d)\n", s->n, r);
		return;
	}
	if (disk_add(s, size)) {
		mutex_lock(&s->lock);
		s->state = EMPTY;
		mutex_unlock(&s->lock);
	}
}

static int media_thread(void *unused)
{
	u8 *sec = kmalloc(512, GFP_KERNEL);
	bool first = true;

	if (!sec)
		return -ENOMEM;
	while (!kthread_should_stop()) {
		slot_check(&m.slot[0], sec, first);
		slot_check(&m.slot[1], sec, first);
		first = false;
		msleep_interruptible(500);
	}
	kfree(sec);
	return 0;
}

/* ---- Sound ------------------------------------------------------------------ */

/* sdk/src/pcm.c: from the host port to the converter, for ever. */
static const u32 pcm_program[] = {
	0x2c002000, 0, 0, 0,
	0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
	0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
	0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
	0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
	0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
	0x38010c00, 0x48103807, 0x3801d762, 0x48103802, 0x38010042, 0x48103801,
	0x48113806, 0x48103800, 0x48113806, 0x48103800, 0x2c7ffe00,
};

static int pcm_put_word(struct slot *s, u16 w)
{
	unsigned long end = jiffies + msecs_to_jiffies(200);

	while (status() & full_bit[s->n])
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
	__raw_writew(w, s->port);
	return 0;
}

static int pcm_load(struct slot *s)
{
	u32 i, count = ARRAY_SIZE(pcm_program);

	reset_line(s->n, 0);
	msleep(20);
	reset_line(s->n, 1);
	msleep(20);
	if (pcm_put_word(s, 1) || pcm_put_word(s, count) || pcm_put_word(s, 0x0c00))
		return -EIO;
	for (i = 0; i < count; i++)
		if (pcm_put_word(s, pcm_program[i]) || pcm_put_word(s, pcm_program[i] >> 16))
			return -EIO;
	return 0;
}

static void __iomem *dma_reg(int proc, int r)
{
	return m.dmac + dma_off[proc] + r;
}

static void dma_stop(int proc)
{
	if (__raw_readl(dma_reg(proc, 12)) & 1) {
		__raw_writel(0x00080020, dma_reg(proc, 12));
		__raw_writeb(__raw_readb(m.selector[proc]) | 0x1f, m.selector[proc]);
		__raw_writeb(__raw_readb(m.selector[proc]) | 0x40, m.selector[proc]);
	}
}

static void dma_start(int proc, dma_addr_t from, u32 words)
{
	__raw_writel(0x00080020, dma_reg(proc, 12));	/* end flag cleared */
	__raw_writel(__raw_readl(m.dmaor) | 0xc301, m.dmaor);
	__raw_writel(from, dma_reg(proc, 0));
	__raw_writel(port_addr[proc], dma_reg(proc, 4));
	__raw_writel(words, dma_reg(proc, 8));
	__raw_writeb(select_on[proc], m.selector[proc]);
	/* 16-bit words, the source counting up, the destination fixed, on
	 * the processor's request; the end's interrupt. */
	__raw_writel(0x000a1025, dma_reg(proc, 12));
}

static irqreturn_t pcm_dma_irq(int irq, void *data)
{
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	int proc;

	spin_lock_irqsave(&m.pcm_lock, flags);
	for (proc = 0; proc < 2; proc++) {
		struct snd_pcm_substream *sub = m.sub;
		u32 chcr = __raw_readl(dma_reg(proc, 12));

		/* Ours: enabled, with its end interrupt, and ended. */
		if ((chcr & 7) != 7)
			continue;
		ret = IRQ_HANDLED;
		if (m.running && sub && m.pcm_proc == proc) {
			struct snd_pcm_runtime *rt = sub->runtime;

			/* Round again at once: the processor holds two samples. */
			dma_start(proc, rt->dma_addr, frames_to_bytes(rt, rt->buffer_size) / 2);
		} else {
			__raw_writel(0x00080020, dma_reg(proc, 12));
		}
	}
	spin_unlock_irqrestore(&m.pcm_lock, flags);
	return ret;
}

static snd_pcm_uframes_t pcm_position(struct snd_pcm_runtime *rt)
{
	u32 left, words = frames_to_bytes(rt, rt->buffer_size) / 2;

	if (m.pcm_proc < 0 || !(__raw_readl(dma_reg(m.pcm_proc, 12)) & 1))
		return 0;
	left = __raw_readl(dma_reg(m.pcm_proc, 8));
	if (left > words)
		left = words;
	return bytes_to_frames(rt, (words - left) * 2) % rt->buffer_size;
}

/* A quarter of a period, in nanoseconds. */
static ktime_t period_tick(struct snd_pcm_runtime *rt)
{
	return ns_to_ktime((u32)rt->period_size * (1000000000u / 44100u / 4));
}

static enum hrtimer_restart pcm_tick(struct hrtimer *t)
{
	struct snd_pcm_substream *sub = m.sub;
	struct snd_pcm_runtime *rt;
	snd_pcm_uframes_t pos;

	if (!m.running || !sub)
		return HRTIMER_NORESTART;
	rt = sub->runtime;
	pos = pcm_position(rt);
	if (pos / rt->period_size != m.last_pos / rt->period_size)
		snd_pcm_period_elapsed(sub);
	m.last_pos = pos;
	hrtimer_forward_now(t, period_tick(rt));
	return HRTIMER_RESTART;
}

static const struct snd_pcm_hardware rnse_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_44100,
	.rate_min = 44100,
	.rate_max = 44100,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 128 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 32 * 1024,
	.periods_min = 2,
	.periods_max = 64,
};

/* A processor for sound: the one asked for, else one whose slot has no
 * disk, else one whose disk nobody has open (which is then removed). */
static int pcm_claim(void)
{
	int order[2] = { 0, 1 }, i;

	if (m.pcm_choice >= 0) {
		order[0] = m.pcm_choice;
		order[1] = -1;
	}
	for (i = 0; i < 2; i++) {
		struct slot *s;

		if (order[i] < 0)
			continue;
		s = &m.slot[order[i]];
		mutex_lock(&s->lock);
		if (s->state == EMPTY) {
			s->state = PCM;
			mutex_unlock(&s->lock);
			return s->n;
		}
		mutex_unlock(&s->lock);
	}
	for (i = 0; i < 2; i++) {
		struct slot *s;

		if (order[i] < 0)
			continue;
		s = &m.slot[order[i]];
		if (s->disk && !disk_openers(s->disk)) {
			mutex_lock(&s->lock);
			s->state = PCM;
			mutex_unlock(&s->lock);
			disk_remove(s);
			return s->n;
		}
	}
	return -EBUSY;
}

static void pcm_release(int proc)
{
	struct slot *s = &m.slot[proc];
	int i;

	dma_stop(proc);
	/* Silence before the reset (sdk/src/pcm.c pcm_stop). */
	for (i = 0; i < 8; i++)
		if (pcm_put_word(s, 0))
			break;
	reset_line(proc, 0);
	mutex_lock(&s->lock);
	s->state = EMPTY;
	s->failed = 0;		/* its card may be started anew */
	mutex_unlock(&s->lock);
}

static int rnse_pcm_open(struct snd_pcm_substream *sub)
{
	int proc, r;

	if (m.sub)
		return -EBUSY;
	proc = pcm_claim();
	if (proc < 0)
		return proc;
	mutex_lock(&m.slot[proc].lock);
	r = pcm_load(&m.slot[proc]);
	mutex_unlock(&m.slot[proc].lock);
	if (r) {
		reset_line(proc, 0);
		m.slot[proc].state = EMPTY;
		return r;
	}
	m.pcm_proc = proc;
	m.sub = sub;
	sub->runtime->hw = rnse_pcm_hw;
	return 0;
}

static int rnse_pcm_close(struct snd_pcm_substream *sub)
{
	int proc = m.pcm_proc;

	m.running = false;
	hrtimer_cancel(&m.timer);
	m.sub = NULL;
	m.pcm_proc = -1;
	if (proc >= 0)
		pcm_release(proc);
	return 0;
}

static int rnse_pcm_prepare(struct snd_pcm_substream *sub)
{
	m.last_pos = 0;
	return 0;
}

static int rnse_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct snd_pcm_runtime *rt = sub->runtime;
	unsigned long flags;

	spin_lock_irqsave(&m.pcm_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		m.running = true;
		m.last_pos = 0;
		dma_start(m.pcm_proc, rt->dma_addr, frames_to_bytes(rt, rt->buffer_size) / 2);
		hrtimer_start(&m.timer, period_tick(rt),
			      HRTIMER_MODE_REL);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		m.running = false;
		dma_stop(m.pcm_proc);
		break;
	default:
		spin_unlock_irqrestore(&m.pcm_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&m.pcm_lock, flags);
	return 0;
}

static snd_pcm_uframes_t rnse_pcm_pointer(struct snd_pcm_substream *sub)
{
	return pcm_position(sub->runtime);
}

static const struct snd_pcm_ops rnse_pcm_ops = {
	.open = rnse_pcm_open,
	.close = rnse_pcm_close,
	.prepare = rnse_pcm_prepare,
	.trigger = rnse_pcm_trigger,
	.pointer = rnse_pcm_pointer,
};

/* "PCM Processor": Auto, 0 or 1. */
static int proc_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	static const char *const names[] = { "Auto", "0", "1" };

	return snd_ctl_enum_info(info, 1, 3, names);
}

static int proc_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.enumerated.item[0] = m.pcm_choice + 1;
	return 0;
}

static int proc_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	int c = v->value.enumerated.item[0];

	if (c > 2)
		return -EINVAL;
	if (c - 1 == m.pcm_choice)
		return 0;
	m.pcm_choice = c - 1;
	return 1;
}

static const struct snd_kcontrol_new proc_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "PCM Processor",
	.info = proc_info,
	.get = proc_get,
	.put = proc_put,
};

static int sound_setup(struct platform_device *pdev)
{
	struct snd_pcm *pcm;
	int r;

	r = snd_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1, "RNSE", THIS_MODULE, 0, &m.card);
	if (r)
		return r;
	strscpy(m.card->driver, "rnse-media", sizeof(m.card->driver));
	strscpy(m.card->shortname, "RNS-E", sizeof(m.card->shortname));
	strscpy(m.card->longname, "Audi RNS-E 192 card processor converter", sizeof(m.card->longname));
	r = snd_pcm_new(m.card, "RNS-E PCM", 0, 1, 0, &pcm);
	if (r)
		goto fail;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &rnse_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &pdev->dev, 64 * 1024, 128 * 1024);
	strscpy(pcm->name, "RNS-E PCM", sizeof(pcm->name));
	r = snd_ctl_add(m.card, snd_ctl_new1(&proc_control, NULL));
	if (r)
		goto fail;
	/* The DMA controller's eight end interrupts all arrive as the first
	 * one's (drivers/sh/intc/core.c redirects vectors of one source). */
	r = devm_request_irq(&pdev->dev, evt2irq(DMTE0), pcm_dma_irq, IRQF_SHARED,
			     "rnse-pcm", &m);
	if (r)
		goto fail;
	r = snd_card_register(m.card);
	if (r)
		goto fail;
	return 0;
fail:
	snd_card_free(m.card);
	m.card = NULL;
	return r;
}

/* ---- The way back to the SD loader ------------------------------------------- */

void rnse_media_quiesce(void)
{
	int proc = m.pcm_proc;

	if (m.thread) {
		kthread_stop(m.thread);
		m.thread = NULL;
	}
	m.running = false;
	if (proc >= 0) {
		hrtimer_cancel(&m.timer);
		dma_stop(proc);
		/* The processor held in reset: its slot's card is to be
		 * started anew by whoever comes next. */
		reset_line(proc, 0);
	}
}

/* ---- Probe -------------------------------------------------------------------- */

static int rnse_media_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int i, r;

	m.pdev = pdev;
	spin_lock_init(&m.latch_lock);
	spin_lock_init(&m.pcm_lock);
	m.status = devm_ioremap(dev, STATUS, 1);
	m.latch = devm_ioremap(dev, RESET_LATCH, 4);
	m.program = devm_ioremap(dev, FLASH_PROGRAM, FLASH_PROGRAM_LEN);
	m.dmac = devm_ioremap(dev, DMA_BASE, 0x100);
	m.selector[0] = devm_ioremap(dev, selector_addr[0], 1);
	m.selector[1] = devm_ioremap(dev, selector_addr[1], 1);
	if (!m.status || !m.latch || !m.program || !m.dmac || !m.selector[0] || !m.selector[1])
		return -ENOMEM;
	m.dmaor = m.dmac + (DMAOR - DMA_BASE);
	for (i = 0; i < 2; i++) {
		struct slot *s = &m.slot[i];

		s->n = i;
		mutex_init(&s->lock);
		s->port = devm_ioremap(dev, port_addr[i], 2);
		s->present = devm_ioremap(dev, present_addr[i], 1);
		if (!s->port || !s->present)
			return -ENOMEM;
	}
	hrtimer_setup(&m.timer, pcm_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	m.major = register_blkdev(0, "rnsesd");
	if (m.major < 0)
		return m.major;
	r = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(29));
	if (r)
		return r;
	r = sound_setup(pdev);
	if (r)
		dev_warn(dev, "no sound (%d)\n", r);
	if (!program_known()) {
		dev_warn(dev, "no card processor program in the flash at 0x200000: no cards\n");
		return 0;
	}
	m.thread = kthread_run(media_thread, NULL, "rnse-media");
	return PTR_ERR_OR_ZERO(m.thread);
}

static struct platform_driver rnse_media_driver = {
	.driver = { .name = "rnse-media" },
	.probe = rnse_media_probe,
};
builtin_platform_driver(rnse_media_driver);
MODULE_DESCRIPTION("Audi RNS-E 192 SD card processors");
MODULE_LICENSE("GPL");
