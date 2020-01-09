// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2013 Boris BREZILLON <b.brezillon@overkiz.com>
 */

#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/clk/at91_pmc.h>
#include <linux/of.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "pmc.h"

DEFINE_SPINLOCK(pmc_pcr_lock);

#define PERIPHERAL_ID_MIN	2
#define PERIPHERAL_ID_MAX	31
#define PERIPHERAL_MASK(id)	(1 << ((id) & PERIPHERAL_ID_MAX))

#define PERIPHERAL_MAX_SHIFT	3

struct clk_peripheral {
	struct clk_hw hw;
	struct regmap *regmap;
	u32 id;
};

#define to_clk_peripheral(hw) container_of(hw, struct clk_peripheral, hw)

struct clk_sam9x5_peripheral {
	struct clk_hw hw;
	struct regmap *regmap;
	struct clk_range range;
	spinlock_t *lock;
	u32 id;
	u32 div;
	const struct clk_pcr_layout *layout;
	bool auto_div;
};

#define to_clk_sam9x5_peripheral(hw) \
	container_of(hw, struct clk_sam9x5_peripheral, hw)

static int clk_peripheral_enable(struct clk_hw *hw)
{
	struct clk_peripheral *periph = to_clk_peripheral(hw);
	int offset = AT91_PMC_PCER;
	u32 id = periph->id;

	if (id < PERIPHERAL_ID_MIN)
		return 0;
	if (id > PERIPHERAL_ID_MAX)
		offset = AT91_PMC_PCER1;
	regmap_write(periph->regmap, offset, PERIPHERAL_MASK(id));

	return 0;
}

static void clk_peripheral_disable(struct clk_hw *hw)
{
	struct clk_peripheral *periph = to_clk_peripheral(hw);
	int offset = AT91_PMC_PCDR;
	u32 id = periph->id;

	if (id < PERIPHERAL_ID_MIN)
		return;
	if (id > PERIPHERAL_ID_MAX)
		offset = AT91_PMC_PCDR1;
	regmap_write(periph->regmap, offset, PERIPHERAL_MASK(id));
}

static int clk_peripheral_is_enabled(struct clk_hw *hw)
{
	struct clk_peripheral *periph = to_clk_peripheral(hw);
	int offset = AT91_PMC_PCSR;
	unsigned int status;
	u32 id = periph->id;

	if (id < PERIPHERAL_ID_MIN)
		return 1;
	if (id > PERIPHERAL_ID_MAX)
		offset = AT91_PMC_PCSR1;
	regmap_read(periph->regmap, offset, &status);

	return status & PERIPHERAL_MASK(id) ? 1 : 0;
}

static const struct clk_ops peripheral_ops = {
	.enable = clk_peripheral_enable,
	.disable = clk_peripheral_disable,
	.is_enabled = clk_peripheral_is_enabled,
};

struct clk_hw * __init
at91_clk_register_peripheral(struct regmap *regmap, const char *name,
			     const char *parent_name, u32 id)
{
	struct clk_peripheral *periph;
	struct clk_init_data init;
	struct clk_hw *hw;
	int ret;

	if (!name || !parent_name || id > PERIPHERAL_ID_MAX)
		return ERR_PTR(-EINVAL);

	periph = kzalloc(sizeof(*periph), GFP_KERNEL);
	if (!periph)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &peripheral_ops;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);
	init.flags = 0;

	periph->id = id;
	periph->hw.init = &init;
	periph->regmap = regmap;

	hw = &periph->hw;
	ret = clk_hw_register(NULL, &periph->hw);
	if (ret) {
		kfree(periph);
		hw = ERR_PTR(ret);
	}

	return hw;
}

static void clk_sam9x5_peripheral_autodiv(struct clk_sam9x5_peripheral *periph)
{
	struct clk_hw *parent;
	unsigned long parent_rate;
	int shift = 0;

	if (!periph->auto_div)
		return;

	if (periph->range.max) {
		parent = clk_hw_get_parent_by_index(&periph->hw, 0);
		parent_rate = clk_hw_get_rate(parent);
		if (!parent_rate)
			return;

		for (; shift < PERIPHERAL_MAX_SHIFT; shift++) {
			if (parent_rate >> shift <= periph->range.max)
				break;
		}
	}

	periph->auto_div = false;
	periph->div = shift;
}

static int clk_sam9x5_peripheral_enable(struct clk_hw *hw)
{
	struct clk_sam9x5_peripheral *periph = to_clk_sam9x5_peripheral(hw);
	unsigned long flags;

	if (periph->id < PERIPHERAL_ID_MIN)
		return 0;

	spin_lock_irqsave(periph->lock, flags);
	regmap_write(periph->regmap, periph->layout->offset,
		     (periph->id & periph->layout->pid_mask));
	regmap_update_bits(periph->regmap, periph->layout->offset,
			   periph->layout->div_mask | periph->layout->cmd |
			   AT91_PMC_PCR_EN,
			   field_prep(periph->layout->div_mask, periph->div) |
			   periph->layout->cmd |
			   AT91_PMC_PCR_EN);
	spin_unlock_irqrestore(periph->lock, flags);

	return 0;
}

static void clk_sam9x5_peripheral_disable(struct clk_hw *hw)
{
	struct clk_sam9x5_peripheral *periph = to_clk_sam9x5_peripheral(hw);
	unsigned long flags;

	if (periph->id < PERIPHERAL_ID_MIN)
		return;

	spin_lock_irqsave(periph->lock, flags);
	regmap_write(periph->regmap, periph->layout->offset,
		     (periph->id & periph->layout->pid_mask));
	regmap_update_bits(periph->regmap, periph->layout->offset,
			   AT91_PMC_PCR_EN | periph->layout->cmd,
			   periph->layout->cmd);
	spin_unlock_irqrestore(periph->lock, flags);
}

static int clk_sam9x5_peripheral_is_enabled(struct clk_hw *hw)
{
	struct clk_sam9x5_peripheral *periph = to_clk_sam9x5_peripheral(hw);
	unsigned long flags;
	unsigned int status;

	if (periph->id < PERIPHERAL_ID_MIN)
		return 1;

	spin_lock_irqsave(periph->lock, flags);
	regmap_write(periph->regmap, periph->layout->offset,
		     (periph->id & periph->layout->pid_mask));
	regmap_read(periph->regmap, periph->layout->offset, &status);
	spin_unlock_irqrestore(periph->lock, flags);

	return status & AT91_PMC_PCR_EN ? 1 : 0;
}

static unsigned long
clk_sam9x5_peripheral_recalc_rate(struct clk_hw *hw,
				  unsigned long parent_rate)
{
	struct clk_sam9x5_peripheral *periph = to_clk_sam9x5_peripheral(hw);
	unsigned long flags;
	unsigned int status;

	if (periph->id < PERIPHERAL_ID_MIN)
		return parent_rate;

	spin_lock_irqsave(periph->lock, flags);
	regmap_write(periph->regmap, periph->layout->offset,
		     (periph->id & periph->layout->pid_mask));
	regmap_read(periph->regmap, periph->layout->offset, &status);
	spin_unlock_irqrestore(periph->lock, flags);

	if (status & AT91_PMC_PCR_EN) {
		periph->div = field_get(periph->layout->div_mask, status);
		periph->auto_div = false;
	} else {
		clk_sam9x5_peripheral_autodiv(periph);
	}

	return parent_rate >> periph->div;
}

static long clk_sam9x5_peripheral_round_rate(struct clk_hw *hw,
					     unsigned long rate,
					     unsigned long *parent_rate)
{
	int shift = 0;
	unsigned long best_rate;
	unsigned long best_diff;
	unsigned long cur_rate = *parent_rate;
	unsigned long cur_diff;
	struct clk_sam9x5_peripheral *periph = to_clk_sam9x5_peripheral(hw);

	if (periph->id < PERIPHERAL_ID_MIN || !periph->range.max)
		return *parent_rate;

	if (periph->range.max) {
		for (; shift <= PERIPHERAL_MAX_SHIFT; shift++) {
			cur_rate = *parent_rate >> shift;
			if (cur_rate <= periph->range.max)
				break;
		}
	}

	if (rate >= cur_rate)
		return cur_rate;

	best_diff = cur_rate - rate;
	best_rate = cur_rate;
	for (; shift <= PERIPHERAL_MAX_SHIFT; shift++) {
		cur_rate = *parent_rate >> shift;
		if (cur_rate < rate)
			cur_diff = rate - cur_rate;
		else
			cur_diff = cur_rate - rate;

		if (cur_diff < best_diff) {
			best_diff = cur_diff;
			best_rate = cur_rate;
		}

		if (!best_diff || cur_rate < rate)
			break;
	}

	return best_rate;
}

static int clk_sam9x5_peripheral_set_rate(struct clk_hw *hw,
					  unsigned long rate,
					  unsigned long parent_rate)
{
	int shift;
	struct clk_sam9x5_peripheral *periph = to_clk_sam9x5_peripheral(hw);
	if (periph->id < PERIPHERAL_ID_MIN || !periph->range.max) {
		if (parent_rate == rate)
			return 0;
		else
			return -EINVAL;
	}

	if (periph->range.max && rate > periph->range.max)
		return -EINVAL;

	for (shift = 0; shift <= PERIPHERAL_MAX_SHIFT; shift++) {
		if (parent_rate >> shift == rate) {
			periph->auto_div = false;
			periph->div = shift;
			return 0;
		}
	}

	return -EINVAL;
}

static const struct clk_ops sam9x5_peripheral_ops = {
	.enable = clk_sam9x5_peripheral_enable,
	.disable = clk_sam9x5_peripheral_disable,
	.is_enabled = clk_sam9x5_peripheral_is_enabled,
	.recalc_rate = clk_sam9x5_peripheral_recalc_rate,
	.round_rate = clk_sam9x5_peripheral_round_rate,
	.set_rate = clk_sam9x5_peripheral_set_rate,
};

struct clk_hw * __init
at91_clk_register_sam9x5_peripheral(struct regmap *regmap, spinlock_t *lock,
				    const struct clk_pcr_layout *layout,
				    const char *name, const char *parent_name,
				    u32 id, const struct clk_range *range)
{
	struct clk_sam9x5_peripheral *periph;
	struct clk_init_data init;
	struct clk_hw *hw;
	int ret;

	if (!name || !parent_name)
		return ERR_PTR(-EINVAL);

	periph = kzalloc(sizeof(*periph), GFP_KERNEL);
	if (!periph)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &sam9x5_peripheral_ops;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);
	init.flags = 0;

	periph->id = id;
	periph->hw.init = &init;
	periph->div = 0;
	periph->regmap = regmap;
	periph->lock = lock;
	if (layout->div_mask)
		periph->auto_div = true;
	periph->layout = layout;
	periph->range = *range;

	hw = &periph->hw;
	ret = clk_hw_register(NULL, &periph->hw);
	if (ret) {
		kfree(periph);
		hw = ERR_PTR(ret);
	} else {
		clk_sam9x5_peripheral_autodiv(periph);
		pmc_register_id(id);
	}

	return hw;
}
