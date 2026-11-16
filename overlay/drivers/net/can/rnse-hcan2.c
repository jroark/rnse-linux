// SPDX-License-Identifier: GPL-2.0
/*
 * The Audi RNS-E 192's CAN controller as a SocketCAN device, can0.
 *
 * The controller is register-compatible with Renesas' HCAN2 (16-bit
 * registers at 0x07c00000; docs/hardware.md "CAN", src/dev/hcan.rs):
 *
 *   +0x00 MCR   bit 0 reset request, bit 1 halt
 *   +0x02 GSR   status
 *   +0x04 BCR1  TSEG1 - 1 in 15:12, TSEG2 - 1 in 10:8, SJW - 1 in 5:4
 *   +0x06 BCR0  BRP - 1 in 7:0 (a quantum is 2 * BRP clock periods)
 *   +0x08 IRR   interrupt flags (bit 1 a frame received, bit 8 a mailbox
 *               emptied, bit 9 a frame overwritten), +0x0a IMR their mask
 *   +0x0c       error counters: transmit in 15:8, receive in 7:0
 *   +0x20/+0x22 TXPR, +0x28/+0x2a TXCR, +0x30/+0x32 TXACK, +0x38/+0x3a
 *               ABACK, +0x40/+0x42 RXPR, +0x50/+0x52 MBIMR, +0x58/+0x5a
 *               UMSR: a bit a mailbox, 31-16 then 15-0
 *   +0x100 + 0x20 n  mailbox n: identifier << 4, -, control (10:8 the
 *               mailbox's use, 13 overwrite, 3:0 length), -, data, mask
 *
 * As the SDK (sdk/src/hw192.c can_init) uses it: mailbox 0 receives
 * every standard frame, mailbox 1 sends; the controller is not reset
 * (the boot ROM leaves it running) and the transceiver is woken through
 * port A, bits 3 and 2.  The bit timing defaults to the stock software's
 * 100 kbit/s, the infotainment bus's (prescaler 14, 21 quanta: 58.8 MHz
 * / 588), so that `ip link set can0 up` needs nothing else; BusyBox's ip
 * cannot set a CAN bit rate.
 *
 * On a bench with no other node a frame is never acknowledged and the
 * controller sends it for ever: after 100 ms it is cancelled, counted as
 * a transmit error, and the next one may go.  The controller cancels
 * only when the attempt it is making is over, a millisecond or so; a
 * frame given to it before that never went out on a unit, so the next
 * one waits for the cancellation to be flagged.
 *
 * Standard identifiers only; remote and extended frames are not sent.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/timer.h>
#include <linux/netdevice.h>
#include <linux/can/dev.h>
#include <linux/platform_data/rnse.h>

#define MCR	0x00
#define GSR	0x02
#define BCR1	0x04
#define BCR0	0x06
#define IRR	0x08
#define IMR	0x0a
#define TEC_REC	0x0c
#define TXPR0	0x22
#define TXCR0	0x2a
#define TXACK0	0x32
#define ABACK0	0x3a
#define RXPR0	0x42
#define MBIMR1	0x50
#define MBIMR0	0x52
#define UMSR0	0x5a
#define MB(n)	(0x100 + 0x20 * (n))

#define IRR_RX		BIT(1)
#define IRR_EMPTY	BIT(8)
#define IRR_OVERRUN	BIT(9)

#define RX_MB	0
#define TX_MB	1

#define PORT_A		0x12a00001	/* bits 3, 2: the transceiver awake */
#define TX_GIVE_UP_MS	100
#define CANCEL_WAIT_MS	20

struct rnse_can {
	struct can_priv can;	/* first */
	struct net_device *dev;
	void __iomem *base;
	spinlock_t lock;
	struct timer_list tx_timer;
	unsigned long tx_since;
	bool cancelling;
};

static struct rnse_can *the_can;

static u16 rd(struct rnse_can *p, int r)
{
	return __raw_readw(p->base + r);
}

static void wr(struct rnse_can *p, int r, u16 v)
{
	__raw_writew(v, p->base + r);
}

static const struct can_bittiming_const rnse_can_bittiming = {
	.name = "rnse-hcan2",
	.tseg1_min = 4, .tseg1_max = 16,
	.tseg2_min = 2, .tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1, .brp_max = 256, .brp_inc = 1,
};

static void set_timing(struct rnse_can *p)
{
	const struct can_bittiming *bt = &p->can.bittiming;
	u32 tseg1 = bt->prop_seg + bt->phase_seg1;

	wr(p, BCR0, (bt->brp - 1) & 0xff);
	wr(p, BCR1, (tseg1 - 1) << 12 | (bt->phase_seg2 - 1) << 8 | (bt->sjw - 1) << 4);
}

/* The transmit mailbox is free again: a frame acknowledged (sent) or
 * cancelled (an error). */
static void tx_done(struct rnse_can *p, bool sent)
{
	struct net_device_stats *st = &p->dev->stats;

	timer_delete(&p->tx_timer);
	p->cancelling = false;
	if (sent) {
		st->tx_bytes += can_get_echo_skb(p->dev, 0, NULL);
		st->tx_packets++;
	} else {
		can_free_echo_skb(p->dev, 0, NULL);
		st->tx_errors++;
		st->tx_aborted_errors++;
	}
	netif_wake_queue(p->dev);
}

static void rx_one(struct rnse_can *p)
{
	struct net_device_stats *st = &p->dev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;
	int i, len;

	skb = alloc_can_skb(p->dev, &cf);
	if (!skb) {
		st->rx_dropped++;
		wr(p, RXPR0, BIT(RX_MB));
		return;
	}
	cf->can_id = rd(p, MB(RX_MB)) >> 4 & CAN_SFF_MASK;
	len = rd(p, MB(RX_MB) + 4) & 0xf;
	cf->len = len > 8 ? 8 : len;
	for (i = 0; i < cf->len; i++)
		cf->data[i] = __raw_readb(p->base + MB(RX_MB) + 8 + i);
	wr(p, RXPR0, BIT(RX_MB));
	st->rx_packets++;
	st->rx_bytes += cf->len;
	netif_rx(skb);
}

static irqreturn_t rnse_can_irq(int irq, void *data)
{
	struct rnse_can *p = data;
	irqreturn_t r = IRQ_NONE;
	u16 irr;

	spin_lock(&p->lock);
	irr = rd(p, IRR);
	if (rd(p, UMSR0) & BIT(RX_MB)) {
		wr(p, UMSR0, BIT(RX_MB));
		p->dev->stats.rx_over_errors++;
		p->dev->stats.rx_errors++;
		r = IRQ_HANDLED;
	}
	while (rd(p, RXPR0) & BIT(RX_MB)) {
		rx_one(p);
		r = IRQ_HANDLED;
	}
	if (rd(p, TXACK0) & BIT(TX_MB)) {
		wr(p, TXACK0, BIT(TX_MB));
		tx_done(p, true);
		r = IRQ_HANDLED;
	}
	if (rd(p, ABACK0) & BIT(TX_MB)) {
		wr(p, ABACK0, BIT(TX_MB));
		tx_done(p, false);
		r = IRQ_HANDLED;
	}
	/* The other flags (reset, error states) are taken back here and
	 * left to the error counters. */
	if (irr & ~(IRR_RX | IRR_EMPTY | IRR_OVERRUN)) {
		wr(p, IRR, irr & ~(IRR_RX | IRR_EMPTY | IRR_OVERRUN));
		r = IRQ_HANDLED;
	}
	spin_unlock(&p->lock);
	return r;
}

/* No acknowledgement within TX_GIVE_UP_MS: cancel, and wait for the
 * cancellation (or a late acknowledgement) to be flagged, CANCEL_WAIT_MS
 * at most. */
static void rnse_can_tx_timeout(struct timer_list *t)
{
	struct rnse_can *p = timer_container_of(p, t, tx_timer);
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	if (!(rd(p, TXPR0) & BIT(TX_MB)) && !p->cancelling) {
		/* Done meanwhile; the interrupt has it. */
	} else if (!p->cancelling) {
		wr(p, TXCR0, BIT(TX_MB));
		p->cancelling = true;
		p->tx_since = jiffies;
		mod_timer(&p->tx_timer, jiffies + msecs_to_jiffies(CANCEL_WAIT_MS));
	} else {
		/* Not flagged in time: free the mailbox all the same. */
		wr(p, ABACK0, BIT(TX_MB));
		tx_done(p, false);
	}
	spin_unlock_irqrestore(&p->lock, flags);
}

static netdev_tx_t rnse_can_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rnse_can *p = netdev_priv(dev);
	struct can_frame *cf = (struct can_frame *)skb->data;
	unsigned long flags;
	int i;

	if (can_dev_dropped_skb(dev, skb))
		return NETDEV_TX_OK;
	if (cf->can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG)) {
		dev->stats.tx_dropped++;
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	netif_stop_queue(dev);
	spin_lock_irqsave(&p->lock, flags);
	wr(p, MB(TX_MB), (cf->can_id & CAN_SFF_MASK) << 4);
	wr(p, MB(TX_MB) + 2, 0);
	for (i = 0; i < cf->len; i++)
		__raw_writeb(cf->data[i], p->base + MB(TX_MB) + 8 + i);
	wr(p, MB(TX_MB) + 4, 0x2000 | cf->len);
	can_put_echo_skb(skb, dev, 0, 0);
	p->cancelling = false;
	mod_timer(&p->tx_timer, jiffies + msecs_to_jiffies(TX_GIVE_UP_MS));
	wr(p, TXPR0, BIT(TX_MB));
	spin_unlock_irqrestore(&p->lock, flags);
	return NETDEV_TX_OK;
}

static void mailboxes(struct rnse_can *p)
{
	int n, i;

	for (n = 0; n < 32; n++) {
		for (i = 0; i < 0x14; i += 2)
			wr(p, MB(n) + i, 0);
		wr(p, MB(n) + 4, 0x0700);	/* not in use */
	}
	wr(p, MB(RX_MB) + 4, 0x2300);		/* receives, new over unread */
	wr(p, MB(RX_MB) + 0x10, 0x7ff0);	/* whatever the identifier */
	wr(p, MB(TX_MB) + 4, 0x2000);		/* sends */
}

static int rnse_can_open(struct net_device *dev)
{
	struct rnse_can *p = netdev_priv(dev);
	void __iomem *porta;
	unsigned long flags;
	int r;

	r = open_candev(dev);
	if (r)
		return r;
	spin_lock_irqsave(&p->lock, flags);
	wr(p, MCR, 0);			/* run */
	mailboxes(p);
	set_timing(p);
	wr(p, IRR, 0x0001);		/* reset flag taken back */
	wr(p, TXACK0, 0xffff);
	wr(p, ABACK0, 0xffff);
	wr(p, UMSR0, 0xffff);
	wr(p, MBIMR1, 0xffff);
	wr(p, MBIMR0, (u16)~(BIT(RX_MB) | BIT(TX_MB)));
	wr(p, IMR, (u16)~(IRR_RX | IRR_EMPTY | IRR_OVERRUN));
	spin_unlock_irqrestore(&p->lock, flags);

	/* The transceiver awake, as the loader and the stock software
	 * set it: nothing gets out of a sleeping one. */
	porta = ioremap(PORT_A, 1);
	if (porta) {
		__raw_writeb(__raw_readb(porta) | 0x0c, porta);
		iounmap(porta);
	}

	r = request_irq(dev->irq, rnse_can_irq, 0, dev->name, p);
	if (r) {
		close_candev(dev);
		return r;
	}
	p->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_start_queue(dev);
	return 0;
}

static void quiet(struct rnse_can *p)
{
	wr(p, IMR, 0xffff);
	wr(p, MBIMR0, 0xffff);
	if (rd(p, TXPR0) & BIT(TX_MB))
		wr(p, TXCR0, BIT(TX_MB));
}

static int rnse_can_stop(struct net_device *dev)
{
	struct rnse_can *p = netdev_priv(dev);
	unsigned long flags;

	netif_stop_queue(dev);
	spin_lock_irqsave(&p->lock, flags);
	quiet(p);
	spin_unlock_irqrestore(&p->lock, flags);
	free_irq(dev->irq, p);
	timer_delete_sync(&p->tx_timer);
	can_free_echo_skb(dev, 0, NULL);
	p->can.state = CAN_STATE_STOPPED;
	close_candev(dev);
	return 0;
}

/* Before the way back to the SD loader: no interrupt, nothing pending. */
void rnse_can_quiesce(void)
{
	if (the_can)
		quiet(the_can);
}

static int rnse_can_set_mode(struct net_device *dev, enum can_mode mode)
{
	return mode == CAN_MODE_START ? 0 : -EOPNOTSUPP;
}

static int rnse_can_berr(const struct net_device *dev, struct can_berr_counter *bec)
{
	struct rnse_can *p = netdev_priv(dev);
	u16 v = rd(p, TEC_REC);

	bec->txerr = v >> 8;
	bec->rxerr = v & 0xff;
	return 0;
}

static const struct net_device_ops rnse_can_ops = {
	.ndo_open = rnse_can_open,
	.ndo_stop = rnse_can_stop,
	.ndo_start_xmit = rnse_can_start_xmit,
};

static int rnse_can_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct rnse_can *p;
	struct can_bittiming *bt;
	int irq, r;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	dev = alloc_candev(sizeof(*p), 1);
	if (!dev)
		return -ENOMEM;
	p = netdev_priv(dev);
	p->dev = dev;
	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base)) {
		r = PTR_ERR(p->base);
		goto fail;
	}
	spin_lock_init(&p->lock);
	timer_setup(&p->tx_timer, rnse_can_tx_timeout, 0);
	dev->irq = irq;
	dev->netdev_ops = &rnse_can_ops;
	dev->flags |= IFF_ECHO;
	/* A quantum is two clock periods times the prescaler. */
	p->can.clock.freq = 58800000 / 2;
	p->can.bittiming_const = &rnse_can_bittiming;
	p->can.do_set_mode = rnse_can_set_mode;
	p->can.do_get_berr_counter = rnse_can_berr;
	/* 100 kbit/s, the stock software's timing, as the default. */
	bt = &p->can.bittiming;
	bt->bitrate = 100000;
	bt->brp = 14;
	bt->prop_seg = 8;
	bt->phase_seg1 = 8;
	bt->phase_seg2 = 4;
	bt->sjw = 1;
	bt->tq = 1000000000UL / (p->can.clock.freq / bt->brp);
	bt->sample_point = 800;
	SET_NETDEV_DEV(dev, &pdev->dev);
	platform_set_drvdata(pdev, dev);
	r = register_candev(dev);
	if (r)
		goto fail;
	the_can = p;
	dev_info(&pdev->dev, "HCAN2 at %p, irq %d, 100 kbit/s\n", p->base, irq);
	return 0;
fail:
	free_candev(dev);
	return r;
}

static struct platform_driver rnse_can_driver = {
	.driver = { .name = "rnse-hcan2" },
	.probe = rnse_can_probe,
};
builtin_platform_driver(rnse_can_driver);
MODULE_DESCRIPTION("Audi RNS-E 192 CAN controller");
MODULE_LICENSE("GPL");
