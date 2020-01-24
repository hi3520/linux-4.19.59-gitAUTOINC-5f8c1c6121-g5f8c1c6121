// SPDX-License-Identifier: GPL-2.0
/**
 * Wrapper driver for SERDES used in J721E
 *
 * Copyright (C) 2019 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <dt-bindings/phy/phy.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mux/consumer.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define WIZ_SERDES_CTRL		0x404
#define WIZ_SERDES_TOP_CTRL	0x408
#define WIZ_SERDES_RST		0x40c
#define WIZ_SERDES_TYPEC	0x410
#define WIZ_LANECTL(n)		(0x480 + (0x40 * (n)))

#define WIZ_MAX_LANES		4
#define WIZ_MUX_NUM_CLOCKS	3
#define WIZ_DIV_NUM_CLOCKS	2

#define WIZ_SERDES_TYPEC_LN10_SWAP	BIT(30)

enum wiz_lane_standard_mode {
	LANE_MODE_GEN1,
	LANE_MODE_GEN2,
	LANE_MODE_GEN3,
	LANE_MODE_GEN4,
};

enum wiz_refclk_mux_sel {
	PLL0_REFCLK,
	PLL1_REFCLK,
	REFCLK_DIG,
};

enum wiz_refclk_div_sel {
	CMN_REFCLK,
	CMN_REFCLK1,
};

static const struct reg_field por_en = REG_FIELD(WIZ_SERDES_CTRL, 31, 31);
static const struct reg_field phy_reset_n = REG_FIELD(WIZ_SERDES_RST, 31, 31);
static const struct reg_field pll1_refclk_mux_sel =
					REG_FIELD(WIZ_SERDES_RST, 29, 29);
static const struct reg_field pll0_refclk_mux_sel =
					REG_FIELD(WIZ_SERDES_RST, 28, 28);
static const struct reg_field refclk_dig_sel =
					REG_FIELD(WIZ_SERDES_RST, 24, 25);
static const struct reg_field pma_cmn_refclk_int_mode =
					REG_FIELD(WIZ_SERDES_TOP_CTRL, 28, 29);
static const struct reg_field pma_cmn_refclk_mode =
					REG_FIELD(WIZ_SERDES_TOP_CTRL, 30, 31);
static const struct reg_field pma_cmn_refclk_dig_div =
					REG_FIELD(WIZ_SERDES_TOP_CTRL, 26, 27);
static const struct reg_field pma_cmn_refclk1_dig_div =
					REG_FIELD(WIZ_SERDES_TOP_CTRL, 24, 25);

static const struct reg_field p_enable[WIZ_MAX_LANES] = {
	REG_FIELD(WIZ_LANECTL(0), 30, 31),
	REG_FIELD(WIZ_LANECTL(1), 30, 31),
	REG_FIELD(WIZ_LANECTL(2), 30, 31),
	REG_FIELD(WIZ_LANECTL(3), 30, 31),
};

static const struct reg_field p_align[WIZ_MAX_LANES] = {
	REG_FIELD(WIZ_LANECTL(0), 29, 29),
	REG_FIELD(WIZ_LANECTL(1), 29, 29),
	REG_FIELD(WIZ_LANECTL(2), 29, 29),
	REG_FIELD(WIZ_LANECTL(3), 29, 29),
};

static const struct reg_field p_raw_auto_start[WIZ_MAX_LANES] = {
	REG_FIELD(WIZ_LANECTL(0), 28, 28),
	REG_FIELD(WIZ_LANECTL(1), 28, 28),
	REG_FIELD(WIZ_LANECTL(2), 28, 28),
	REG_FIELD(WIZ_LANECTL(3), 28, 28),
};

static const struct reg_field p_standard_mode[WIZ_MAX_LANES] = {
	REG_FIELD(WIZ_LANECTL(0), 24, 25),
	REG_FIELD(WIZ_LANECTL(1), 24, 25),
	REG_FIELD(WIZ_LANECTL(2), 24, 25),
	REG_FIELD(WIZ_LANECTL(3), 24, 25),
};

struct wiz_clk_mux {
	struct clk_hw		hw;
	struct regmap_field	*field;
	u32			*table;
	struct clk_init_data	clk_data;
};

#define to_wiz_clk_mux(_hw) container_of(_hw, struct wiz_clk_mux, hw)

struct wiz_clk_divider {
	struct clk_hw		hw;
	struct regmap_field	*field;
	struct clk_div_table	*table;
	struct clk_init_data	clk_data;
};

#define to_wiz_clk_div(_hw) container_of(_hw, struct wiz_clk_divider, hw)

struct wiz_clk_mux_sel {
	struct regmap_field	*field;
	u32			table[4];
	const char		*node_name;
};

struct wiz_clk_div_sel {
	struct regmap_field	*field;
	struct clk_div_table	*table;
	const char		*node_name;
};

static struct wiz_clk_mux_sel clk_mux_sel[] = {
	{
		/*
		 * Mux value to be configured for each of the input clocks
		 * in the order populated in device tree
		 */
		.table = { 1, 0 },
		.node_name = "pll0_refclk",
	},
	{
		.table = { 1, 0 },
		.node_name = "pll1_refclk",
	},
	{
		.table = { 1, 3, 0, 2 },
		.node_name = "refclk_dig",
	},
};

static struct clk_div_table clk_div_table[] = {
	{ .val = 0, .div = 1, },
	{ .val = 1, .div = 2, },
	{ .val = 2, .div = 4, },
	{ .val = 3, .div = 8, },
};

static struct wiz_clk_div_sel clk_div_sel[] = {
	{
		.table = clk_div_table,
		.node_name = "cmn_refclk",
	},
	{
		.table = clk_div_table,
		.node_name = "cmn_refclk1",
	},
};

struct wiz {
	struct regmap		*regmap;
	struct wiz_clk_mux_sel	*clk_mux_sel;
	struct wiz_clk_div_sel	*clk_div_sel;
	struct regmap_field	*por_en;
	struct regmap_field	*phy_reset_n;
	struct regmap_field	*p_enable[WIZ_MAX_LANES];
	struct regmap_field	*p_align[WIZ_MAX_LANES];
	struct regmap_field	*p_raw_auto_start[WIZ_MAX_LANES];
	struct regmap_field	*p_standard_mode[WIZ_MAX_LANES];
	struct regmap_field	*pma_cmn_refclk_int_mode;
	struct regmap_field	*pma_cmn_refclk_mode;
	struct regmap_field	*pma_cmn_refclk_dig_div;
	struct regmap_field	*pma_cmn_refclk1_dig_div;

	struct device		*dev;
	u32			num_lanes;
	struct platform_device	*serdes_pdev;
	struct reset_controller_dev wiz_phy_reset_dev;
	struct gpio_desc	*gpio_typec_dir;

	bool used_for_dp;
};

static int wiz_reset(struct wiz *wiz)
{
	int ret;

	ret = regmap_field_write(wiz->por_en, 0x1);
	if (ret)
		return ret;

	mdelay(1);

	ret = regmap_field_write(wiz->por_en, 0x0);
	if (ret)
		return ret;

	return 0;
}

static int wiz_mode_select(struct wiz *wiz)
{
	u32 num_lanes = wiz->num_lanes;
	int ret;
	int i;

	for (i = 0; i < num_lanes; i++) {
		ret = regmap_field_write(wiz->p_standard_mode[i],
					 LANE_MODE_GEN4);
		if (ret)
			return ret;
	}

	return 0;
}

static int wiz_init_raw_interface(struct wiz *wiz, bool enable)
{
	u32 num_lanes = wiz->num_lanes;
	int i;
	int ret;

	for (i = 0; i < num_lanes; i++) {
		ret = regmap_field_write(wiz->p_align[i], enable);
		if (ret)
			return ret;

		ret = regmap_field_write(wiz->p_raw_auto_start[i], enable);
		if (ret)
			return ret;
	}

	return 0;
}

static int wiz_init(struct wiz *wiz)
{
	struct device *dev = wiz->dev;
	int ret;

	ret = wiz_reset(wiz);
	if (ret) {
		dev_err(dev, "WIZ reset failed\n");
		return ret;
	}

	ret = wiz_mode_select(wiz);
	if (ret) {
		dev_err(dev, "WIZ mode select failed\n");
		return ret;
	}

	ret = wiz_init_raw_interface(wiz, true);
	if (ret) {
		dev_err(dev, "WIZ interface initialization failed\n");
		return ret;
	}

	/* INIT HACK to get DP working. Values from Brian */
	if (wiz->used_for_dp) {
		regmap_write(wiz->regmap, 0x408, 0x30000000);
		regmap_write(wiz->regmap, 0x40c, 0x39000000);
		regmap_write(wiz->regmap, 0x480, 0x70000000);
		regmap_write(wiz->regmap, 0x4c0, 0x80000000);
		regmap_write(wiz->regmap, 0x500, 0x80000000);
		regmap_write(wiz->regmap, 0x540, 0x80000000);
		regmap_write(wiz->regmap, 0x484, 0x10001);
		regmap_write(wiz->regmap, 0x4c4, 0x10001);
		regmap_write(wiz->regmap, 0x504, 0x10001);
		regmap_write(wiz->regmap, 0x544, 0x10001);
		regmap_write(wiz->regmap, 0x5FC, 0x00000);
	}

	return 0;
}

static int wiz_regfield_init(struct wiz *wiz)
{
	struct wiz_clk_mux_sel *clk_mux_sel;
	struct wiz_clk_div_sel *clk_div_sel;
	struct regmap *regmap = wiz->regmap;
	int num_lanes = wiz->num_lanes;
	struct device *dev = wiz->dev;
	int i;

	wiz->por_en = devm_regmap_field_alloc(dev, regmap, por_en);
	if (IS_ERR(wiz->por_en)) {
		dev_err(dev, "POR_EN reg field init failed\n");
		return PTR_ERR(wiz->por_en);
	}

	wiz->phy_reset_n = devm_regmap_field_alloc(dev, regmap,
						   phy_reset_n);
	if (IS_ERR(wiz->phy_reset_n)) {
		dev_err(dev, "PHY_RESET_N reg field init failed\n");
		return PTR_ERR(wiz->phy_reset_n);
	}

	wiz->pma_cmn_refclk_int_mode =
		devm_regmap_field_alloc(dev, regmap, pma_cmn_refclk_int_mode);
	if (IS_ERR(wiz->pma_cmn_refclk_int_mode)) {
		dev_err(dev, "PMA_CMN_REFCLK_INT_MODE reg field init failed\n");
		return PTR_ERR(wiz->pma_cmn_refclk_int_mode);
	}

	wiz->pma_cmn_refclk_mode =
		devm_regmap_field_alloc(dev, regmap, pma_cmn_refclk_mode);
	if (IS_ERR(wiz->pma_cmn_refclk_mode)) {
		dev_err(dev, "PMA_CMN_REFCLK_MODE reg field init failed\n");
		return PTR_ERR(wiz->pma_cmn_refclk_mode);
	}

	clk_div_sel = &wiz->clk_div_sel[CMN_REFCLK];
	clk_div_sel->field = devm_regmap_field_alloc(dev, regmap,
						     pma_cmn_refclk_dig_div);
	if (IS_ERR(clk_div_sel->field)) {
		dev_err(dev, "PMA_CMN_REFCLK_DIG_DIV reg field init failed\n");
		return PTR_ERR(clk_div_sel->field);
	}

	clk_div_sel = &wiz->clk_div_sel[CMN_REFCLK1];
	clk_div_sel->field = devm_regmap_field_alloc(dev, regmap,
						     pma_cmn_refclk1_dig_div);
	if (IS_ERR(clk_div_sel->field)) {
		dev_err(dev, "PMA_CMN_REFCLK1_DIG_DIV reg field init failed\n");
		return PTR_ERR(clk_div_sel->field);
	}

	clk_mux_sel = &wiz->clk_mux_sel[PLL0_REFCLK];
	clk_mux_sel->field = devm_regmap_field_alloc(dev, regmap,
						     pll0_refclk_mux_sel);
	if (IS_ERR(clk_mux_sel->field)) {
		dev_err(dev, "PLL0_REFCLK_SEL reg field init failed\n");
		return PTR_ERR(clk_mux_sel->field);
	}

	clk_mux_sel = &wiz->clk_mux_sel[PLL1_REFCLK];
	clk_mux_sel->field = devm_regmap_field_alloc(dev, regmap,
						     pll1_refclk_mux_sel);
	if (IS_ERR(clk_mux_sel->field)) {
		dev_err(dev, "PLL1_REFCLK_SEL reg field init failed\n");
		return PTR_ERR(clk_mux_sel->field);
	}

	clk_mux_sel = &wiz->clk_mux_sel[REFCLK_DIG];
	clk_mux_sel->field = devm_regmap_field_alloc(dev, regmap,
						     refclk_dig_sel);
	if (IS_ERR(clk_mux_sel->field)) {
		dev_err(dev, "REFCLK_DIG_SEL reg field init failed\n");
		return PTR_ERR(clk_mux_sel->field);
	}

	for (i = 0; i < num_lanes; i++) {
		wiz->p_enable[i] = devm_regmap_field_alloc(dev, regmap,
							   p_enable[i]);
		if (IS_ERR(wiz->p_enable[i])) {
			dev_err(dev, "P%d_ENABLE reg field init failed\n", i);
			return PTR_ERR(wiz->p_enable[i]);
		}

		wiz->p_align[i] = devm_regmap_field_alloc(dev, regmap,
							  p_align[i]);
		if (IS_ERR(wiz->p_align[i])) {
			dev_err(dev, "P%d_ALIGN reg field init failed\n", i);
			return PTR_ERR(wiz->p_align[i]);
		}

		wiz->p_raw_auto_start[i] =
		  devm_regmap_field_alloc(dev, regmap, p_raw_auto_start[i]);
		if (IS_ERR(wiz->p_raw_auto_start[i])) {
			dev_err(dev, "P%d_RAW_AUTO_START reg field init fail\n",
				i);
			return PTR_ERR(wiz->p_raw_auto_start[i]);
		}

		wiz->p_standard_mode[i] =
		  devm_regmap_field_alloc(dev, regmap, p_standard_mode[i]);
		if (IS_ERR(wiz->p_standard_mode[i])) {
			dev_err(dev, "P%d_STANDARD_MODE reg field init fail\n",
				i);
			return PTR_ERR(wiz->p_standard_mode[i]);
		}
	}

	return 0;
}

static u8 wiz_clk_mux_get_parent(struct clk_hw *hw)
{
	struct wiz_clk_mux *mux = to_wiz_clk_mux(hw);
	struct regmap_field *field = mux->field;
	unsigned int val;

	regmap_field_read(field, &val);
	return clk_mux_val_to_index(hw, mux->table, 0, val);
}

static int wiz_clk_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct wiz_clk_mux *mux = to_wiz_clk_mux(hw);
	struct regmap_field *field = mux->field;
	int val;

	val = mux->table[index];
	return regmap_field_write(field, val);
}

static const struct clk_ops wiz_clk_mux_ops = {
	.set_parent = wiz_clk_mux_set_parent,
	.get_parent = wiz_clk_mux_get_parent,
};

static int wiz_mux_clk_register(struct wiz *wiz, struct device_node *node,
				struct regmap_field *field, u32 *table)
{
	struct device *dev = wiz->dev;
	struct clk_init_data *init;
	const char **parent_names;
	unsigned int num_parents;
	struct wiz_clk_mux *mux;
	const char *clk_name;
	struct clk *clk;
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	num_parents = of_clk_get_parent_count(node);
	if (num_parents < 2) {
		dev_err(dev, "SERDES clock must have parents\n");
		return -EINVAL;
	}

	parent_names = devm_kzalloc(dev, (sizeof(char *) * num_parents),
				    GFP_KERNEL);
	if (!parent_names)
		return -ENOMEM;

	of_clk_parent_fill(node, parent_names, num_parents);

	ret = of_property_read_string(node, "clock-output-names", &clk_name);
	if (ret) {
		dev_err(dev, "Unable to read clock-output-names DT property\n");
		return ret;
	}

	init = &mux->clk_data;

	init->ops = &wiz_clk_mux_ops;
	init->flags = CLK_SET_RATE_NO_REPARENT;
	init->parent_names = parent_names;
	init->num_parents = num_parents;
	init->name = clk_name;

	mux->field = field;
	mux->table = table;
	mux->hw.init = init;

	clk = devm_clk_register(dev, &mux->hw);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ret = of_clk_add_provider(node, of_clk_src_simple_get, clk);
	if (ret)
		dev_err(dev, "Failed to add clock provider: %s\n", clk_name);

	return ret;
}

static unsigned long wiz_clk_div_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct wiz_clk_divider *div = to_wiz_clk_div(hw);
	struct regmap_field *field = div->field;
	int val;

	regmap_field_read(field, &val);

	return divider_recalc_rate(hw, parent_rate, val, div->table, 0x0, 2);
}

static long wiz_clk_div_round_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long *prate)
{
	struct wiz_clk_divider *div = to_wiz_clk_div(hw);

	return divider_round_rate(hw, rate, prate, div->table, 2, 0x0);
}

static int wiz_clk_div_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct wiz_clk_divider *div = to_wiz_clk_div(hw);
	struct regmap_field *field = div->field;
	int val;

	val = divider_get_val(rate, parent_rate, div->table, 2, 0x0);
	if (val < 0)
		return val;

	return regmap_field_write(field, val);
}

static const struct clk_ops wiz_clk_div_ops = {
	.recalc_rate = wiz_clk_div_recalc_rate,
	.round_rate = wiz_clk_div_round_rate,
	.set_rate = wiz_clk_div_set_rate,
};

static int wiz_div_clk_register(struct wiz *wiz, struct device_node *node,
				struct regmap_field *field,
				struct clk_div_table *table)
{
	struct device *dev = wiz->dev;
	struct wiz_clk_divider *div;
	struct clk_init_data *init;
	const char **parent_names;
	const char *clk_name;
	struct clk *clk;
	int ret;

	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return -ENOMEM;

	ret = of_property_read_string(node, "clock-output-names", &clk_name);
	if (ret) {
		dev_err(dev, "Unable to read clock-output-names DT property\n");
		return ret;
	}

	parent_names = devm_kzalloc(dev, sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		return -ENOMEM;

	of_clk_parent_fill(node, parent_names, 1);

	init = &div->clk_data;

	init->ops = &wiz_clk_div_ops;
	init->flags = 0;
	init->parent_names = parent_names;
	init->num_parents = 1;
	init->name = clk_name;

	div->field = field;
	div->table = table;
	div->hw.init = init;

	clk = devm_clk_register(dev, &div->hw);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ret = of_clk_add_provider(node, of_clk_src_simple_get, clk);
	if (ret)
		dev_err(dev, "Failed to add clock provider: %s\n", clk_name);

	return ret;
}

static void wiz_clock_cleanup(struct wiz *wiz, struct device_node *node)
{
	struct wiz_clk_mux_sel *clk_mux_sel = wiz->clk_mux_sel;
	struct device_node *clk_node;
	int i;

	for (i = 0; i < WIZ_MUX_NUM_CLOCKS; i++) {
		clk_node = of_get_child_by_name(node, clk_mux_sel[i].node_name);
		of_clk_del_provider(clk_node);
		of_node_put(clk_node);
	}
}

static int wiz_clock_init(struct wiz *wiz, struct device_node *node)
{
	struct wiz_clk_mux_sel *clk_mux_sel = wiz->clk_mux_sel;
	struct device *dev = wiz->dev;
	struct device_node *clk_node;
	const char *node_name;
	unsigned long rate;
	struct clk *clk;
	int ret;
	int i;

	clk = devm_clk_get(dev, "core_ref_clk");
	if (IS_ERR(clk)) {
		dev_err(dev, "core_ref_clk clock not found\n");
		ret = PTR_ERR(clk);
		return ret;
	}

	rate = clk_get_rate(clk);
	if (rate >= 100000000)
		regmap_field_write(wiz->pma_cmn_refclk_int_mode, 0x1);
	else
		regmap_field_write(wiz->pma_cmn_refclk_int_mode, 0x3);

	clk = devm_clk_get(dev, "ext_ref_clk");
	if (IS_ERR(clk)) {
		dev_err(dev, "ext_ref_clk clock not found\n");
		ret = PTR_ERR(clk);
		return ret;
	}

	rate = clk_get_rate(clk);
	if (rate >= 100000000)
		regmap_field_write(wiz->pma_cmn_refclk_mode, 0x0);
	else
		regmap_field_write(wiz->pma_cmn_refclk_mode, 0x2);

	for (i = 0; i < WIZ_MUX_NUM_CLOCKS; i++) {
		node_name = clk_mux_sel[i].node_name;
		clk_node = of_get_child_by_name(node, node_name);
		if (!clk_node) {
			dev_err(dev, "Unable to get %s node\n", node_name);
			ret = -EINVAL;
			goto err;
		}

		ret = wiz_mux_clk_register(wiz, clk_node, clk_mux_sel[i].field,
					   clk_mux_sel[i].table);
		if (ret) {
			dev_err(dev, "Failed to register %s clock\n",
				node_name);
			of_node_put(clk_node);
			goto err;
		}

		of_node_put(clk_node);
	}

	for (i = 0; i < WIZ_DIV_NUM_CLOCKS; i++) {
		node_name = clk_div_sel[i].node_name;
		clk_node = of_get_child_by_name(node, node_name);
		if (!clk_node) {
			dev_err(dev, "Unable to get %s node\n", node_name);
			ret = -EINVAL;
			goto err;
		}

		ret = wiz_div_clk_register(wiz, clk_node, clk_div_sel[i].field,
					   clk_div_sel[i].table);
		if (ret) {
			dev_err(dev, "Failed to register %s clock\n",
				node_name);
			of_node_put(clk_node);
			goto err;
		}

		of_node_put(clk_node);
	}

	return 0;
err:
	wiz_clock_cleanup(wiz, node);

	return ret;
}

static int wiz_phy_reset_assert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct device *dev = rcdev->dev;
	struct wiz *wiz = dev_get_drvdata(dev);
	int ret = 0;

	if (id == 0) {
		ret = regmap_field_write(wiz->phy_reset_n, false);
		return ret;
	}

	ret = regmap_field_write(wiz->p_enable[id - 1], false);
	return ret;
}

static int wiz_phy_reset_deassert(struct reset_controller_dev *rcdev,
				  unsigned long id)
{
	struct device *dev = rcdev->dev;
	struct wiz *wiz = dev_get_drvdata(dev);
	int ret;

	/* if typec-dir gpio was specified, set LN10 SWAP bit based on that */
	if (id == 0 && wiz->gpio_typec_dir) {
		u32 reg;

		if (gpiod_get_value_cansleep(wiz->gpio_typec_dir)) {
			regmap_update_bits(wiz->regmap, WIZ_SERDES_TYPEC,
					   WIZ_SERDES_TYPEC_LN10_SWAP,
					   WIZ_SERDES_TYPEC_LN10_SWAP);
		} else {
			regmap_update_bits(wiz->regmap, WIZ_SERDES_TYPEC,
					   WIZ_SERDES_TYPEC_LN10_SWAP, 0);
		}
	}

	if (id == 0) {
		ret = regmap_field_write(wiz->phy_reset_n, true);
		return ret;
	}

	ret = regmap_field_write(wiz->p_enable[id - 1], true);
	return ret;
}

static const struct reset_control_ops wiz_phy_reset_ops = {
	.assert = wiz_phy_reset_assert,
	.deassert = wiz_phy_reset_deassert,
};

static struct regmap_config wiz_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.fast_io = true,
};

static const struct of_device_id wiz_id_table[] = {
	{
		.compatible = "ti,j721e-wiz",
	},
	{}
};
MODULE_DEVICE_TABLE(of, wiz_id_table);

static void wiz_check_dp_usage(struct wiz *wiz, struct device_node *child_node)
{
	const char *compat;

	if (of_property_read_string(child_node, "compatible", &compat) == 0)
		wiz->used_for_dp = !strcmp("cdns,dp-phy", compat);
}

static int wiz_probe(struct platform_device *pdev)
{
	struct reset_controller_dev *phy_reset_dev;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct platform_device *serdes_pdev;
	struct device_node *child_node;
	struct regmap *regmap;
	struct resource res;
	void __iomem *base;
	struct wiz *wiz;
	u32 num_lanes;
	int ret;

	wiz = devm_kzalloc(dev, sizeof(*wiz), GFP_KERNEL);
	if (!wiz)
		return -ENOMEM;

	child_node = of_get_child_by_name(node, "serdes");
	if (!child_node) {
		dev_err(dev, "Failed to get SERDES child DT node\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(child_node, 0, &res);
	if (ret) {
		dev_err(dev, "Failed to get memory resource\n");
		goto err_addr_to_resource;
	}

	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (IS_ERR(base))
		goto err_addr_to_resource;

	regmap = devm_regmap_init_mmio(dev, base, &wiz_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to initialize regmap\n");
		ret = PTR_ERR(regmap);
		goto err_addr_to_resource;
	}

	ret = of_property_read_u32(node, "num-lanes", &num_lanes);
	if (ret) {
		dev_err(dev, "Failed to read num-lanes property\n");
		goto err_addr_to_resource;
	}

	if (num_lanes > WIZ_MAX_LANES) {
		dev_err(dev, "Cannot support %d lanes\n", num_lanes);
		goto err_addr_to_resource;
	}

	wiz->gpio_typec_dir = devm_gpiod_get_optional(dev, "typec-dir",
						      GPIOD_IN);
	if (IS_ERR(wiz->gpio_typec_dir)) {
		ret = PTR_ERR(wiz->gpio_typec_dir);
		dev_err(dev, "Failed to request typec-dir gpio: %d\n", ret);
		return ret;
	}

	wiz->dev = dev;
	wiz->regmap = regmap;
	wiz->num_lanes = num_lanes;
	wiz->clk_mux_sel = clk_mux_sel;
	wiz->clk_div_sel = clk_div_sel;

	platform_set_drvdata(pdev, wiz);

	ret = wiz_regfield_init(wiz);
	if (ret) {
		dev_err(dev, "Failed to initialize regfields\n");
		goto err_addr_to_resource;
	}

	phy_reset_dev = &wiz->wiz_phy_reset_dev;
	phy_reset_dev->dev = dev;
	phy_reset_dev->ops = &wiz_phy_reset_ops,
	phy_reset_dev->owner = THIS_MODULE,
	phy_reset_dev->of_node = node;
	/* Reset for each of the lane and one for the entire SERDES */
	phy_reset_dev->nr_resets = num_lanes + 1;

	ret = devm_reset_controller_register(dev, phy_reset_dev);
	if (ret < 0) {
		dev_warn(dev, "Failed to register reset controller\n");
		goto err_addr_to_resource;
	}

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_get_sync;
	}

	ret = wiz_clock_init(wiz, node);
	if (ret < 0) {
		dev_warn(dev, "Failed to initialize clocks\n");
		goto err_get_sync;
	}

	serdes_pdev = of_platform_device_create(child_node, NULL, dev);
	if (!serdes_pdev) {
		dev_WARN(dev, "Unable to create SERDES platform device\n");
		goto err_pdev_create;
	}
	wiz->serdes_pdev = serdes_pdev;

	wiz_check_dp_usage(wiz, child_node);

	ret = wiz_init(wiz);
	if (ret) {
		dev_err(dev, "WIZ initialization failed\n");
		goto err_wiz_init;
	}

	of_node_put(child_node);
	return 0;

err_wiz_init:
	of_platform_device_destroy(&serdes_pdev->dev, NULL);

err_pdev_create:
	wiz_clock_cleanup(wiz, node);

err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

err_addr_to_resource:
	of_node_put(child_node);

	return ret;
}

static int wiz_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct platform_device *serdes_pdev;
	struct wiz *wiz;

	wiz = dev_get_drvdata(dev);
	serdes_pdev = wiz->serdes_pdev;

	of_platform_device_destroy(&serdes_pdev->dev, NULL);
	wiz_clock_cleanup(wiz, node);
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return 0;
}

static struct platform_driver wiz_driver = {
	.probe		= wiz_probe,
	.remove		= wiz_remove,
	.driver		= {
		.name	= "wiz",
		.of_match_table = wiz_id_table,
	},
};
module_platform_driver(wiz_driver);

MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("TI J721E WIZ driver");
MODULE_LICENSE("GPL v2");