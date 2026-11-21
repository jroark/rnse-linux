// SPDX-License-Identifier: GPL-2.0
/*
 * The R8A77730 (Audi RNS-E 193): fixed clocks.
 *
 * Where this chip keeps its frequency control is not known, so nothing
 * is read: the peripheral clock is CONFIG_SH_PCLK_FREQ, 49.95 MHz, the
 * rate the stock software's 1 ms tick implies (12488 counts at a quarter
 * of it; rnse's docs/hardware-193.md, "Clocks and time"), and the bus
 * and module clocks are taken to be the same.  The CPU's rate is only a
 * figure here (the kernel calibrates its delays against the timer).
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <asm/clock.h>

static unsigned long same_as_parent(struct clk *clk)
{
	return clk->parent->rate;
}

static unsigned long cpu_clk_recalc(struct clk *clk)
{
	return clk->parent->rate * 8;
}

static struct sh_clk_ops r8a77730_master_clk_ops = {
};

static struct sh_clk_ops r8a77730_module_clk_ops = {
	.recalc		= same_as_parent,
};

static struct sh_clk_ops r8a77730_bus_clk_ops = {
	.recalc		= same_as_parent,
};

static struct sh_clk_ops r8a77730_cpu_clk_ops = {
	.recalc		= cpu_clk_recalc,
};

static struct sh_clk_ops *r8a77730_clk_ops[] = {
	&r8a77730_master_clk_ops,
	&r8a77730_module_clk_ops,
	&r8a77730_bus_clk_ops,
	&r8a77730_cpu_clk_ops,
};

void __init arch_init_clk_ops(struct sh_clk_ops **ops, int idx)
{
	if (idx < ARRAY_SIZE(r8a77730_clk_ops))
		*ops = r8a77730_clk_ops[idx];
}
