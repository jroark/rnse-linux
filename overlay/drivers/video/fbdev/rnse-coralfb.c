// SPDX-License-Identifier: GPL-2.0
/*
 * Framebuffer of the Audi RNS-E 192: one layer of its Fujitsu MB86293
 * ("Coral") graphics chip, 400x235 pixels at 8 bits through a palette.
 *
 * The chip is little-endian and sits on the SH-4's big-endian bus without
 * byte lanes swapped, so pixel N of a surface is the byte at bus address
 * N ^ 3: a little-endian framebuffer to a big-endian CPU.  The fbdev
 * drawing code handles that itself (FBINFO_FOREIGN_ENDIAN, which wants
 * CONFIG_FB_FOREIGN_ENDIAN), and so this driver draws nothing of its own.
 *
 * The display's timing, the chip's host interface and the layer itself
 * are set up as the rnse SDK's board_init sets them (sdk/src/hw192.c),
 * which the linuxboot loader has done before it starts the kernel; this
 * driver sets the layer again (lower-right layer L4/"BL", 448 bytes a
 * line at 0x110000 in video memory), clears it and turns it on.  The
 * screen's light is not the chip's but the faceplate controller's.
 *
 * Reverse-engineered: see docs/hardware.md ("Display") and src/dev/coral.rs
 * in the rnse repository.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#define CORAL_DISP	0x1fd0000	/* display controller, from the chip's base */
#define DCE		0x02		/* 16-bit, bus address offset ^ 2 = 0x00 */
#define L4M		0x70		/* layer L4: mode (stride, format) */
#define L4OA		0x74		/* origin address */
#define L4DA		0x78		/* display address */
#define L4_PALETTE	0x800		/* 256 words, 6 bits a colour, 0x00RRGGBB */

#define SURFACE		0x110000
#define WIDTH		400
#define HEIGHT		235
#define STRIDE		448		/* the mode's line length field: 7 * 64 */
#define LINES		HEIGHT

struct coralfb {
	void __iomem *disp;
	u32 pseudo_palette[16];
};

static int coralfb_setcolreg(unsigned int regno, unsigned int red,
			     unsigned int green, unsigned int blue,
			     unsigned int transp, struct fb_info *info)
{
	struct coralfb *par = info->par;

	if (regno >= 256)
		return 1;
	__raw_writel(((red >> 8) & 0xfc) << 16 | ((green >> 8) & 0xfc) << 8 |
		     ((blue >> 8) & 0xfc), par->disp + L4_PALETTE + 4 * regno);
	return 0;
}

static const struct fb_ops coralfb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_setcolreg	= coralfb_setcolreg,
};

static int coralfb_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct fb_info *info;
	struct coralfb *par;
	void __iomem *vram;
	int i, ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	info = framebuffer_alloc(sizeof(*par), &pdev->dev);
	if (!info)
		return -ENOMEM;
	par = info->par;

	vram = devm_ioremap(&pdev->dev, res->start + SURFACE, STRIDE * LINES);
	par->disp = devm_ioremap(&pdev->dev, res->start + CORAL_DISP, 0x1000);
	if (!vram || !par->disp) {
		ret = -ENOMEM;
		goto fail;
	}

	strscpy(info->fix.id, "rnse-coral", sizeof(info->fix.id));
	info->fix.smem_start = res->start + SURFACE;
	info->fix.smem_len = STRIDE * LINES;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_PSEUDOCOLOR;
	info->fix.line_length = STRIDE;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = WIDTH;
	info->var.yres = HEIGHT;
	info->var.xres_virtual = WIDTH;
	info->var.yres_virtual = LINES;
	info->var.bits_per_pixel = 8;
	info->var.red.length = 6;
	info->var.green.length = 6;
	info->var.blue.length = 6;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;
	info->var.vmode = FB_VMODE_NONINTERLACED;

	info->fbops = &coralfb_ops;
	info->screen_base = vram;
	info->screen_size = STRIDE * LINES;
	info->pseudo_palette = par->pseudo_palette;
	info->flags = FBINFO_FOREIGN_ENDIAN;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		goto fail;

	/* A black picture, then the layer that shows it, on. */
	for (i = 0; i < STRIDE * LINES; i += 4)
		__raw_writel(0, vram + i);
	__raw_writel(0x00070400, par->disp + L4M);
	__raw_writel(SURFACE, par->disp + L4OA);
	__raw_writel(SURFACE, par->disp + L4DA);
	__raw_writew(0x8008, par->disp + (DCE ^ 2));

	ret = register_framebuffer(info);
	if (ret)
		goto fail_cmap;
	platform_set_drvdata(pdev, info);
	fb_info(info, "%dx%d, 8 bpp, Coral layer L4 at 0x%lx\n",
		WIDTH, HEIGHT, info->fix.smem_start);
	return 0;

fail_cmap:
	fb_dealloc_cmap(&info->cmap);
fail:
	framebuffer_release(info);
	return ret;
}

static struct platform_driver coralfb_driver = {
	.probe = coralfb_probe,
	.driver = {
		.name = "rnse-coralfb",
	},
};
builtin_platform_driver(coralfb_driver);
