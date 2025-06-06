// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kendryte Canaan K230 Clock Drivers
 *
 * Author: Xukai Wang <kingxukai@zohomail.com>
 * Author: Troy Mitchell <troymitchell988@gmail.com>
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <dt-bindings/clock/canaan,k230-clk.h>

/* PLL control register bits. */
#define K230_PLL_BYPASS_ENABLE			BIT(19)
#define K230_PLL_GATE_ENABLE			BIT(2)
#define K230_PLL_GATE_WRITE_ENABLE		BIT(18)
#define K230_PLL_OD_SHIFT			24
#define K230_PLL_OD_MASK			0xF
#define K230_PLL_R_SHIFT			16
#define K230_PLL_R_MASK				0x3F
#define K230_PLL_F_SHIFT			0
#define K230_PLL_F_MASK				0x1FFF
#define K230_PLL_DIV_REG_OFFSET			0x00
#define K230_PLL_BYPASS_REG_OFFSET		0x04
#define K230_PLL_GATE_REG_OFFSET		0x08
#define K230_PLL_LOCK_REG_OFFSET		0x0C

/* PLL lock register bits.  */
#define K230_PLL_LOCK_STATUS_MASK		BIT(0)

/* K230 CLK registers offset */
#define K230_CLK_AUDIO_CLKDIV_OFFSET		0x34
#define K230_CLK_PDM_CLKDIV_OFFSET		0x40
#define K230_CLK_CODEC_ADC_MCLKDIV_OFFSET	0x38
#define K230_CLK_CODEC_DAC_MCLKDIV_OFFSET	0x3c

#define K230_CLK_MAX_PARENT_NUM			4

#define K230_FMT(_var)				&k230_##_var

#define K230_PLLX_OFFSET(idx)			((idx) * 0x10)
#define K230_PLLX_BASE(base, idx)		((base) + K230_PLLX_OFFSET(idx))

#define K230_PLLX_DIV_ADDR(base, idx)						\
	(K230_PLL_DIV_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_PLLX_BYPASS_ADDR(base, idx)					\
	(K230_PLL_BYPASS_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_PLLX_GATE_ADDR(base, idx)						\
	(K230_PLL_GATE_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_PLLX_LOCK_ADDR(base, idx)						\
	(K230_PLL_LOCK_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_CLK_OPS_RATE							\
	.set_rate	= k230_clk_set_rate,					\
	.round_rate	= k230_clk_round_rate,					\
	.recalc_rate	= k230_clk_get_rate

#define K230_GATE_FORMAT(_reg, _bit, _gate_flags)				\
{										\
	.gate_reg_off = (_reg),							\
	.gate_bit_enable = (_bit),						\
	.gate_flags = (_gate_flags),						\
}

#define K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,		\
			_div_min, _div_max, _div_shift, _div_mask,		\
			_read_only, _reg, _bit, _reg2, _ops)			\
{										\
	.rate_mul_min = (_mul_min),						\
	.rate_mul_max = (_mul_max),						\
	.rate_mul_shift = (_mul_shift),						\
	.rate_mul_mask = (_mul_mask),						\
	.rate_div_min = (_div_min),						\
	.rate_div_max = (_div_max),						\
	.rate_div_shift = (_div_shift),						\
	.rate_div_mask = (_div_mask),						\
	.rate_reg_off = (_reg),							\
	.rate_reg_off2 = (_reg2),						\
	.rate_write_enable_bit = (_bit),					\
	.ops = _ops,								\
	.read_only = _read_only,						\
}

#define K230_MUX_FORMAT2(_reg, _shift, _width, _mux_flags,			\
			 _name1, _index1, _name2, _index2,			\
			 _def_index)						\
{										\
	.mux_reg_off = (_reg),							\
	.mux_reg_shift = (_shift),						\
	.mux_reg_width = (_width),						\
	.mux_flags = (_mux_flags),						\
	.def_index = (_def_index),						\
	.parent = {								\
		.name[0] = _name1,						\
		.index[0] = _index1,						\
		.name[1] = _name2,						\
		.index[1] = _index2,						\
	}									\
}

#define K230_MUX_FORMAT3(_reg, _shift, _width, _mux_flags,			\
			 _name1, _index1, _name2, _index2, _name3, _index3,	\
			 _def_index)						\
{										\
	.mux_reg_off = (_reg),							\
	.mux_reg_shift = (_shift),						\
	.mux_reg_width = (_width),						\
	.mux_flags = (_mux_flags),						\
	.def_index = (_def_index),						\
	.parent = {								\
		.name[0] = _name1,						\
		.index[0] = _index1,						\
		.name[1] = _name2,						\
		.index[1] = _index2,						\
		.name[2] = _name3,						\
		.index[2] = _index3,						\
	}									\
}

#define K230_PLL_DIV_FORMAT(_parent_name, _name, _div)				\
{										\
	.parent_name = _parent_name,						\
	.name = _name,								\
	.div = _div,								\
}

#define K230_CLK_CFG_FORMAT(_name, _flags, _type)				\
	.name = (_name),							\
	.flags = (_flags),							\
	.type = _type

#define K230_CLK_RATE_FORMAT(_var,						\
			     _mul_min, _mul_max, _mul_shift, _mul_mask,		\
			     _div_min, _div_max, _div_shift, _div_mask,		\
			     _reg, _bit, _method, _reg2,			\
			     _read_only, _flags, 				\
			     _pname, _index)					\
	static struct k230_clk_rate_cfg k230_##_var##_cfg =			\
		K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift,		\
				 _mul_mask, _div_min, _div_max,			\
				 _div_shift, _div_mask, _read_only,		\
				 _reg, _bit, _reg2, &k230_clk_ops_##_method);	\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _flags, K230_RATE),			\
		.parent[0] = {							\
			.name = _pname,						\
			.fw_name = _pname,					\
			.index = _index,					\
		},								\
		.rate_cfg = &k230_##_var##_cfg,					\
	}

#define K230_CLK_GATE_FORMAT(_var,						\
			     _reg, _bit,					\
			     _flags, _gate_flags, 				\
			     _pname, _index)					\
	static struct k230_clk_gate_cfg k230_##_var##_cfg =			\
		K230_GATE_FORMAT(_reg, _bit, _gate_flags);			\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _flags, K230_GATE),			\
		.parent[0] = {							\
			.name = _pname,						\
			.fw_name = _pname,					\
			.index = _index,					\
		},								\
		.gate_cfg = &k230_##_var##_cfg,					\
	}

#define K230_CLK_MUX_FORMAT2(_var,						\
			     _reg, _shift, _width,				\
			     _flags, _mux_flags, _def_index,			\
			     _pname1, _index1, _pname2, _index2)		\
	static struct k230_clk_mux_cfg k230_##_var##_cfg =			\
		K230_MUX_FORMAT2(_reg, _shift, _width, _mux_flags, 		\
				 _pname1, _index1, _pname2, _index2,		\
				 _def_index);					\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _flags | CLK_OPS_PARENT_ENABLE,	\
				    K230_MUX),					\
		.mux_cfg = &k230_##_var##_cfg,					\
	}

#define K230_CLK_MUX_FORMAT3(_var,						\
			     _reg, _shift, _width,				\
			     _flags, _mux_flags, _def_index,			\
			     _pname1, _index1,					\
			     _pname2, _index2,					\
			     _pname3, _index3)					\
	static struct k230_clk_mux_cfg k230_##_var##_cfg =			\
		K230_MUX_FORMAT3(_reg, _shift, _width, _mux_flags,		\
				 _pname1, _index1, _pname2, _index2,		\
				 _pname3, _index3, _def_index);			\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _flags | CLK_OPS_PARENT_ENABLE,	\
				    K230_MUX),					\
		.mux_cfg = &k230_##_var##_cfg,					\
	}

#define K230_CLK_FIXED_FACTOR_FORMAT(_var,					\
				     _mul, _div,				\
			     	     _pname, _index)				\
 	static struct k230_clk_fixed_factor_cfg k230_##_var##_cfg = {		\
		.mul = _mul,							\
		.div = _div,							\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, 0, K230_FIXED_FACTOR),		\
		.parent[0] = {							\
			.name = _pname,						\
			.fw_name = _pname,					\
			.index = _index,					\
		},								\
		.fixed_factor_cfg = &k230_##_var##_cfg,				\
	}

#define K230_CLK_FIXED_RATE_FORMAT(_var,					\
				   _fixed_rate,					\
				   _pname, _index)				\
 	static struct k230_clk_fixed_rate_cfg k230_##_var##_cfg = {		\
		.fixed_rate = _fixed_rate,					\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, 0, K230_FIXED_RATE),			\
		.parent[0] = {							\
			.name = _pname,						\
			.fw_name = _pname,					\
			.index = _index,					\
		},								\
		.fixed_rate_cfg = &k230_##_var##_cfg,				\
	}

struct k230_sysclk;

enum k230_pll_id {
	K230_PLL0,
	K230_PLL1,
	K230_PLL2,
	K230_PLL3,
	K230_PLL_NUM
};

struct k230_pll {
	struct k230_sysclk *ksc;
	const char *name;
	enum k230_pll_id id;
	struct clk_hw hw;
};

#define to_k230_pll(_hw)	container_of(_hw, struct k230_pll, hw)

enum k230_pll_div_id {
	K230_PLL0_DIV2,
	K230_PLL0_DIV3,
	K230_PLL0_DIV4,
	K230_PLL0_DIV16,
	K230_PLL1_DIV2,
	K230_PLL1_DIV3,
	K230_PLL1_DIV4,
	K230_PLL2_DIV2,
	K230_PLL2_DIV3,
	K230_PLL2_DIV4,
	K230_PLL3_DIV2,
	K230_PLL3_DIV3,
	K230_PLL3_DIV4,
	K230_PLL_DIV_NUM
};

struct k230_pll_div {
	struct k230_sysclk *ksc;
	const char *parent_name, *name;
	enum k230_pll_div_id id;
	int div;
	struct clk_hw *hw;
};

enum k230_clk_type {
	K230_RATE,
	K230_GATE,
	K230_MUX,
	K230_FIXED_RATE,
	K230_FIXED_FACTOR,
};

struct k230_clk_rate_cfg {
	u32 rate_reg_off;
	/* second register address */
	u32 rate_reg_off2;
	u32 rate_write_enable_bit;
	u32 rate_mul_min;
	u32 rate_mul_max;
	u32 rate_mul_shift;
	u32 rate_mul_mask;
	u32 rate_div_min;
	u32 rate_div_max;
	u32 rate_div_shift;
	u32 rate_div_mask;
	struct clk_hw hw;
	const struct clk_ops *ops;
	struct k230_sysclk *ksc;
	bool read_only;
};

#define to_k230_rate_cfg(_hw)	container_of(_hw, struct k230_clk_rate_cfg, hw)

struct k230_clk_parent_data {
	int num_parents;
	const char *name[K230_CLK_MAX_PARENT_NUM];
	int index[K230_CLK_MAX_PARENT_NUM];
};

struct k230_clk_gate_cfg {
	u32 gate_reg_off;
	u32 gate_bit_enable;
	int gate_flags;
};

struct k230_clk_mux_cfg {
	u32 mux_reg_off;
	u32 mux_reg_shift;
	u32 mux_reg_width;
	int mux_flags;
	int def_index;
	struct k230_clk_parent_data parent;
};

struct k230_clk_fixed_rate_cfg {
	long fixed_rate;
};

struct k230_clk_fixed_factor_cfg {
	int mul;
	int div;
};

struct k230_clk {
	const char *name;
	enum k230_clk_type type;
	int flags;
	struct clk_parent_data parent[1];
	union {
		struct k230_clk_rate_cfg		*rate_cfg;
		struct k230_clk_gate_cfg		*gate_cfg;
		struct k230_clk_mux_cfg			*mux_cfg;
		struct k230_clk_fixed_rate_cfg		*fixed_rate_cfg;
		struct k230_clk_fixed_factor_cfg	*fixed_factor_cfg;
	};
};

struct k230_sysclk {
	struct platform_device *pdev;
	void __iomem		*regs, *pll_regs;
	spinlock_t		pll_lock, clk_lock;
};

static struct k230_pll k230_plls[] = {
	[K230_PLL0] = { .name = "pll0", .id = K230_PLL0},
	[K230_PLL1] = { .name = "pll1", .id = K230_PLL1},
	[K230_PLL2] = { .name = "pll2", .id = K230_PLL2},
	[K230_PLL3] = { .name = "pll3", .id = K230_PLL3},
};

static struct k230_pll_div k230_pll_divs[] = {
	[K230_PLL0_DIV2]	= K230_PLL_DIV_FORMAT("pll0", "pll0_div2", 2),
	[K230_PLL0_DIV3]	= K230_PLL_DIV_FORMAT("pll0", "pll0_div3", 3),
	[K230_PLL0_DIV4]	= K230_PLL_DIV_FORMAT("pll0", "pll0_div4", 4),
	[K230_PLL0_DIV16]	= K230_PLL_DIV_FORMAT("pll0", "pll0_div16", 16),
	[K230_PLL1_DIV2]	= K230_PLL_DIV_FORMAT("pll1", "pll1_div2", 2),
	[K230_PLL1_DIV3]	= K230_PLL_DIV_FORMAT("pll1", "pll1_div3", 3),
	[K230_PLL1_DIV4]	= K230_PLL_DIV_FORMAT("pll1", "pll1_div4", 4),
	[K230_PLL2_DIV2]	= K230_PLL_DIV_FORMAT("pll2", "pll2_div2", 2),
	[K230_PLL2_DIV3]	= K230_PLL_DIV_FORMAT("pll2", "pll2_div3", 3),
	[K230_PLL2_DIV4]	= K230_PLL_DIV_FORMAT("pll2", "pll2_div4", 4),
	[K230_PLL3_DIV2]	= K230_PLL_DIV_FORMAT("pll3", "pll3_div2", 2),
	[K230_PLL3_DIV3]	= K230_PLL_DIV_FORMAT("pll3", "pll3_div3", 3),
	[K230_PLL3_DIV4]	= K230_PLL_DIV_FORMAT("pll3", "pll3_div4", 4),
};

static int k230_clk_set_rate_mul(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate);
static long k230_clk_round_rate_mul(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate);
static unsigned long k230_clk_get_rate_mul(struct clk_hw *hw,
					   unsigned long parent_rate);
static int k230_clk_set_rate_div(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate);
static long k230_clk_round_rate_div(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate);
static unsigned long k230_clk_get_rate_div(struct clk_hw *hw,
					   unsigned long parent_rate);
static int k230_clk_set_rate_mul_div(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate);
static long k230_clk_round_rate_mul_div(struct clk_hw *hw, unsigned long rate,
					unsigned long *parent_rate);
static unsigned long k230_clk_get_rate_mul_div(struct clk_hw *hw,
					       unsigned long parent_rate);

static struct clk_ops k230_clk_ops_mul = {
	.set_rate	= k230_clk_set_rate_mul,
	.round_rate	= k230_clk_round_rate_mul,
	.recalc_rate	= k230_clk_get_rate_mul,
};

static struct clk_ops k230_clk_ops_div = {
	.set_rate	= k230_clk_set_rate_div,
	.round_rate	= k230_clk_round_rate_div,
	.recalc_rate	= k230_clk_get_rate_div,
};

static struct clk_ops k230_clk_ops_mul_div = {
	.set_rate	= k230_clk_set_rate_mul_div,
	.round_rate	= k230_clk_round_rate_mul_div,
	.recalc_rate	= k230_clk_get_rate_mul_div,
};

K230_CLK_GATE_FORMAT(cpu0_src_gate,
		     0, 0, 0, 0,
		     "pll0_div2", 0);

K230_CLK_RATE_FORMAT(cpu0_src_rate,
		     1, 16, 0, 0,
		     16, 16, 1, 0xf,
		     0x0, 31, mul, 0x0,
		     false, 0,
		     "cpu0_src_gate", 0);

K230_CLK_RATE_FORMAT(cpu0_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x0, 31, div, 0x0,
		     0, 0,
		     "cpu0_src_rate", 0);

K230_CLK_GATE_FORMAT(cpu0_plic_gate,
		     0x0, 9, 0, 0,
		     "cpu0_src_rate", 0);

K230_CLK_RATE_FORMAT(cpu0_plic_rate,
		     1, 1, 0, 0,
		     1, 8, 10, 0x7,
		     0x0, 31, div, 0x0,
		     false, 0,
		     "cpu0_plic_gate", 0);

K230_CLK_GATE_FORMAT(cpu0_noc_ddrcp4_gate,
		     0x60, 7, 0, 0,
		     "cpu0_src_rate", 0);

K230_CLK_GATE_FORMAT(cpu0_apb_gate,
		     0x0, 13, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(cpu0_apb_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x0, 31, div, 0x0,
		     false, 0,
		     "cpu0_apb_gate", 0);

K230_CLK_MUX_FORMAT3(cpu1_src_mux,
		     0x4, 1, 2,
		     0, 0, 1,
		     "pll0_div2", 0,
		     "pll3", 0,
		     "pll0", 0);

K230_CLK_GATE_FORMAT(cpu1_src_gate,
		     0x4, 0, 0, 0,
		     "cpu1_src_mux", 0);

K230_CLK_RATE_FORMAT(cpu1_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x4, 31, div, 0x0,
		     false, 0,
		     "cpu1_src_gate", 0);

K230_CLK_RATE_FORMAT(cpu1_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x4, 31, div, 0x0,
		     false, 0,
		     "cpu1_src_rate", 0);

K230_CLK_GATE_FORMAT(cpu1_plic_gate,
		     0x4, 15, 0, 0,
		     "cpu1_src_rate", 0);

K230_CLK_RATE_FORMAT(cpu1_plic_rate,
		     1, 1, 0, 0,
		     1, 8, 16, 0x7,
		     0x4, 31, div, 0x0,
		     false, 0,
		     "cpu1_plic_gate", 0);

K230_CLK_GATE_FORMAT(cpu1_apb_gate,
		     0x4, 19, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(cpu1_apb_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x0, 31, div, 0x0,
		     false, 0,
		     "cpu1_apb_gate", 0);

K230_CLK_GATE_FORMAT(pmu_apb_gate,
		     0x10, 0, 0, 0,
		     "osc24m", 0);

K230_CLK_RATE_FORMAT(hs_hclk_high_src_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     "pll0_div4", 0);

K230_CLK_GATE_FORMAT(hs_hclk_high_gate,
		     0x18, 1, 0, 0,
		     "hs_hclk_high_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_hclk_src_gate,
		     0x18, 1, 0, 0,
		     "hs_hclk_high_src_rate", 0);

K230_CLK_RATE_FORMAT(hs_hclk_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     "hs_hclk_src_gate", 0);

K230_CLK_GATE_FORMAT(hs_sd0_ahb_gate,
		     0x18, 2, 0, 0,
		     "hs_hclk_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_sd1_ahb_gate,
		     0x18, 3, 0, 0,
		     "hs_hclk_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_ssi1_ahb_gate,
		     0x18, 7, 0, 0,
		     "hs_hclk_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_ssi2_ahb_gate,
		     0x18, 8, 0, 0,
		     "hs_hclk_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_usb0_ahb_gate,
		     0x18, 4, 0, 0,
		     "hs_hclk_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_usb1_ahb_gate,
		     0x18, 5, 0, 0,
		     "hs_hclk_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_ssi0_axi_gate,
		     0x18, 27, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(hs_ssi0_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 9, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     "hs_ssi0_axi_gate", 0);

K230_CLK_GATE_FORMAT(hs_ssi1_gate,
		     0x18, 25, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(hs_ssi1_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     "hs_ssi1_gate", 0);

K230_CLK_GATE_FORMAT(hs_ssi2_gate,
		     0x18, 26, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(hs_ssi2_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     "hs_ssi_gate", 0);

K230_CLK_GATE_FORMAT(hs_qspi_axi_src_gate,
		     0x18, 28, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(hs_qspi_axi_src_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     "hs_qspi_axi_src_gate", 0);

K230_CLK_GATE_FORMAT(hs_ssi1_axi_gate,
		     0x18, 29, false, 0,
		     "hs_qspi_axi_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_ssi2_axi_gate,
		     0x18, 30, 0, 0,
		     "hs_qspi_axi_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_sd_card_src_gate,
		     0x18, 11, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(hs_sd_card_src_rate,
		     1, 1, 0, 0,
		     2, 8, 12, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     "pll0_div4", 0);

K230_CLK_GATE_FORMAT(hs_sd0_card_gate,
		     0x18, 15, 0, 0,
		     "hs_sd_card_src", 0);

K230_CLK_GATE_FORMAT(hs_sd1_card_gate,
		     0x18, 19, 0, 0,
		     "hs_sd_card_src", 0);

K230_CLK_GATE_FORMAT(hs_sd_axi_src_gate,
			  0x18, 9, 0, 0,
			  "k230_pll2_div4", 0);

K230_CLK_RATE_FORMAT(hs_sd_axi_src_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     "hs_sd_axi_src_gate", 0);

K230_CLK_GATE_FORMAT(hs_sd0_axi_gate,
		     0x18, 13, 0, 0,
		     "hs_sd_axi_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_sd1_axi_gate,
		     0x18, 17, 0, 0,
		     "hs_sd_axi_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_sd0_base_gate,
		     0x18, 14, 0, 0,
		     "hs_sd_axi_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_sd1_base_gate,
		     0x18, 18, false, 0,
		     "hs_sd_axi_src_rate", 0);

K230_CLK_MUX_FORMAT2(hs_ospi_src_mux,
		     0x20, 18, 1,
		     0, 0, 0,
		     "pll0_div2", 0,
		     "pll2_div4", 0);

K230_CLK_GATE_FORMAT(hs_ospi_src_gate,
		     0x18, 24, 0, 0,
		     "hs_ospi_src_mux", 0);

K230_CLK_RATE_FORMAT(hs_usb_ref_50m_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_GATE_FORMAT(hs_sd_timer_src_gate,
			  0x18, 12, 0, 0,
			  "osc24m", 0);

K230_CLK_RATE_FORMAT(hs_sd_timer_src_rate,
			  1, 1, 0, 0,
			  24, 32, 15, 0x1F,
			  0x1C, 31, div, 0x0,
			  false, 0,
			  "osc24m", 0);

K230_CLK_GATE_FORMAT(hs_sd0_timer_gate,
		     0x18, 16, 0, 0,
		     "hs_sd_timer_src_rate", 0);

K230_CLK_GATE_FORMAT(hs_sd1_timer_gate,
		     0x18, 20, 0, 0,
		     "hs_sd_timer_src_rate", 0);

K230_CLK_MUX_FORMAT2(hs_usb0_ref_mux,
			  0x18, 23, 1,
			  0, 0, 0,
			  "osc24m", 0,
			  "hs_usb_ref_50m_rate", 0);

K230_CLK_GATE_FORMAT(hs_usb0_ref_gate,
			  0x18, 21, 0, 0,
			  "hs_usb0_ref_mux", 0);

K230_CLK_MUX_FORMAT2(hs_usb1_ref_mux,
			  0x18, 23, 1,
			  0, 0, 0,
			  "osc24m", 0,
			  "hs_usb_ref_50m_rate", 0);

K230_CLK_GATE_FORMAT(hs_usb1_ref_gate,
			  0x18, 22, 0, 0,
			  "hs_usb1_ref_mux", 0);

K230_CLK_GATE_FORMAT(ls_apb_src_gate,
			  0x24, 0, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_apb_src_rate,
			  1, 1, 0, 0,
			  1, 8, 0, 0x7,
			  0x30, 31, div, 0x0,
			  false, 0,
			  "ls_apb_src_gate", 0);

K230_CLK_GATE_FORMAT(ls_uart0_apb_gate,
		     0x24, 1, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_uart1_apb_gate,
		     0x24, 2, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_uart2_apb_gate,
		     0x24, 3, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_uart3_apb_gate,
		     0x24, 4, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_uart4_apb_gate,
		     0x24, 5, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_i2c0_apb_gate,
		     0x24, 6, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_i2c1_apb_gate,
		     0x24, 7, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_i2c2_apb_gate,
		     0x24, 8, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_i2c3_apb_gate,
		     0x24, 9, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_i2c4_apb_gate,
		     0x24, 10, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_gpio_apb_gate,
		     0x24, 11, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_pwm_apb_gate,
		     0x24, 12, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink0_apb_gate,
		     0x28, 4, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink1_apb_gate,
		     0x28, 5,
		     false, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink2_apb_gate,
		     0x28, 6, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink3_apb_gate,
		     0x28, 7, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_audio_apb_gate,
		     0x24, 13, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_adc_apb_gate,
		     0x24, 15, 0, 0,
		     "ls_apb_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_codec_apb_gate,
		     0x24, 14, 0, 0,
		     "pll0_div4", 0);

K230_CLK_GATE_FORMAT(ls_i2c0_gate,
			  0x24, 21, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_i2c0_rate,
			  1, 1, 0, 0,
			  1, 8, 15, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_i2c0_gate", 0);

K230_CLK_GATE_FORMAT(ls_i2c1_gate,
			  0x24, 22, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_i2c1_rate,
			  1, 1, 0, 0,
			  1, 8, 18, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_i2c1_gate", 0);

K230_CLK_GATE_FORMAT(ls_i2c2_gate,
			  0x24, 23, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_i2c2_rate,
			  1, 1, 0, 0,
			  1, 8, 21, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_i2c2_gate", 0);

K230_CLK_GATE_FORMAT(ls_i2c3_gate,
			  0x24, 24, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_i2c3_rate,
			  1, 1, 0, 0,
			  1, 8, 24, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_i2c3_gate", 0);

K230_CLK_GATE_FORMAT(ls_i2c4_gate,
			  0x24, 25, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_i2c4_rate,
			  1, 1, 0, 0,
			  1, 8, 27, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_i2c_gate", 0);

K230_CLK_GATE_FORMAT(ls_codec_adc_gate,
			  0x24, 29, false, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_codec_adc_rate,
			  0x10, 0x1B9, 14, 0x1FFF,
			  0xC35, 0x3D09, 0, 0x3FFF,
			  0x38, 31, mul_div, 0x0,
			  false, 0,
			  "ls_codec_adc_gate", 0);

K230_CLK_GATE_FORMAT(ls_codec_dac_gate,
			  0x24, 30, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_codec_dac_rate,
			  0x10, 0x1B9, 14, 0x1FFF,
			  0xC35, 0x3D09, 0, 0x3FFF,
			  0x3C, 31, mul_div, 0x0,
			  false, 0,
			  "ls_codec_dac_gate", 0);

K230_CLK_GATE_FORMAT(ls_audio_dev_gate,
			  0x24, 28, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_audio_dev_rate,
			  0x4, 0x1B9, 16, 0x7FFF,
			  0xC35, 0xF424, 0, 0xFFFF,
			  0x34, 31, mul_div, 0x0,
			  false, 0,
			  "pll0_div4", 0);

K230_CLK_GATE_FORMAT(ls_pdm_gate,
		  0x24, 31, false, 0,
		  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_pdm_rate,
		  0x2, 0x1B9, 0, 0xFFFF,
		  0xC35, 0x1E848, 0, 0x1FFFF,
		  0x40, 0, mul_div, 0x44,
		  false, 0,
		  "ls_pdm_gate", 0);

K230_CLK_GATE_FORMAT(ls_adc_gate,
			  0x24, 26, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ls_adc_rate,
			  1, 1, 0, 0,
			  1, 1024, 3, 0x3FF,
			  0x30, 31, div, 0x0,
			  false, 0,
			  "ls_adc_gate", 0);

K230_CLK_GATE_FORMAT(ls_uart0_gate,
			  0x24, 16, 0, 0,
			  "pll0_div16", 0);

K230_CLK_RATE_FORMAT(ls_uart0_rate,
			  1, 1, 0, 0,
			  1, 8, 0, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_uart0_gate", 0);

K230_CLK_GATE_FORMAT(ls_uart1_gate,
			  0x24, 17, 0, 0,
			  "pll0_div16", 0);

K230_CLK_RATE_FORMAT(ls_uart1_rate,
			  1, 1, 0, 0,
			  1, 8, 3, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_uart1_gate", 0);

K230_CLK_GATE_FORMAT(ls_uart2_gate,
			  0x24, 18, 0, 0,
			  "pll0_div16", 0);

K230_CLK_RATE_FORMAT(ls_uart2_rate,
			  1, 1, 0, 0,
			  1, 8, 6, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "la_uart2_gate", 0);

K230_CLK_GATE_FORMAT(ls_uart3_gate,
			  0x24, 19, 0, 0,
			  "pll0_div16", 0);

K230_CLK_RATE_FORMAT(ls_uart3_rate,
			  1, 1, 0, 0,
			  1, 8, 9, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_uart3_gate", 0);

K230_CLK_GATE_FORMAT(ls_uart4_gate,
			  0x24, 20, 0, 0,
			  "pll0_div16", 0);

K230_CLK_RATE_FORMAT(ls_uart4_rate,
			  1, 1, 0, 0,
			  1, 8, 12, 0x7,
			  0x2C, 31, div, 0x0,
			  false, 0,
			  "ls_uart4_gate", 0);

K230_CLK_RATE_FORMAT(ls_jamlinkco_src_rate,
		     1, 1, 0, 0,
		     2, 512, 23, 0xFF,
		     0x30, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_GATE_FORMAT(ls_jamlink0co_gate,
		     0x28, 0, 0, 0,
		     "ls_jamlinkco_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink1co_gate,
		     0x28, 1, 0, 0,
		     "ls_jamlinkco_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink2co_gate,
		     0x28, 2, 0, 0,
		     "ls_jamlinkco_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_jamlink3co_gate,
		     0x28, 3, 0, 0,
		     "ls_jamlinkco_src_rate", 0);

K230_CLK_GATE_FORMAT(ls_gpio_debounce_gate,
			  0x24, 27, 0, 0,
			  "osc24m", 0);

K230_CLK_RATE_FORMAT(ls_gpio_debounce_rate,
			  1, 1, 0, 0,
			  1, 1024, 13, 0x3FF,
			  0x30, 31, div, 0x0,
			  false, 0,
			  "osc24m", 0);

K230_CLK_FIXED_RATE_FORMAT(sysctl_apb_src,
			   100000000, NULL, 0);

K230_CLK_GATE_FORMAT(sysctl_wdt0_apb_gate,
		     0x50, 1, 0, 0,
		     "sysctl_apb_src", 0);

K230_CLK_GATE_FORMAT(sysctl_wdt1_apb_gate,
		     0x50, 2, 0, 0,
		     "sysctl_apb_src", 0);

K230_CLK_GATE_FORMAT(sysctl_timer_apb_gate,
		     0x50, 3, 0, 0,
		     "sysctl_apb_src", 0);

K230_CLK_GATE_FORMAT(sysctl_iomux_apb_gate,
		     0x50, 20, 0, 0,
		     "sysctl_apb_src", 0);

K230_CLK_GATE_FORMAT(sysctl_mailbox_apb_gate,
		     0x50, 4, 0, 0,
		     "sysctl_apb_src", 0);

K230_CLK_GATE_FORMAT(sysctl_hdi_gate,
			  0x50, 21, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(sysctl_hdi_rate,
			  1, 1, 0, 0,
			  1, 8, 28, 0x7,
			  0x58, 31, div, 0x0,
			  false, 0,
			  "sysctl_hdi_gate", 0);

K230_CLK_GATE_FORMAT(sysctl_time_stamp_gate,
			  0x50, 19, 0, 0,
			  "pll1_div4", 0);

K230_CLK_RATE_FORMAT(sysctl_time_stamp_rate,
			  1, 1, 0, 0,
			  1, 32, 15, 0x1F,
			  0x58, 31, div, 0x0,
			  false, 0,
			  "sysctl_time_stamp_gate", 0);

K230_CLK_RATE_FORMAT(sysctl_temp_sensor_rate,
		     1, 1, 0, 0,
		     1, 256, 20, 0xFF,
		     0x58, 31, div, 0x0,
		     false, 0,
		     "osc24m", 0);

K230_CLK_GATE_FORMAT(sysctl_wdt0_gate,
			  0x50, 4, 0, 0,
			  "osc24m", 0);

K230_CLK_RATE_FORMAT(sysctl_wdt0_rate,
			  1, 1, 0, 0,
			  1, 64, 3, 0x3F,
			  0x58, 31, div, 0x0,
			  false, 0,
			  "sysctl_wdt0_gate", 0);

K230_CLK_GATE_FORMAT(sysctl_wdt1_gate,
			  0x50, 4, 0, 0,
			  "osc24m", 0);

K230_CLK_RATE_FORMAT(sysctl_wdt1_rate,
			  1, 1, 0, 0,
			  1, 64, 3, 0x3F,
			  0x58, 31, div, 0x0,
			  false, 0,
			  "sysctl_wdt1_gate", 0);

K230_CLK_RATE_FORMAT(timer0_src_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_RATE_FORMAT(timer1_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_RATE_FORMAT(timer2_src_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_RATE_FORMAT(timer3_src_rate,
		     1, 1, 0, 0,
		     1, 8, 9, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_RATE_FORMAT(timer4_src_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_RATE_FORMAT(timer5_src_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     "pll0_div16", 0);

K230_CLK_MUX_FORMAT2(timer0_mux,
			  0x50, 7, 1,
			  0, 0, 1,
			  "timer0_src_rate", 0,
			  "timer-pulse-in", 1);

K230_CLK_GATE_FORMAT(timer0_gate,
			  0x50, 13, 0, 0,
			  "timer0_mux", 0);

K230_CLK_MUX_FORMAT2(timer1_mux,
			  0x50, 8, 1,
			  0, 0, 1,
			  "timer1_src", 0,
			  "timer-pulse-in", 1);

K230_CLK_GATE_FORMAT(timer1_gate,
			  0x50, 14, 0, 0,
			  "timer1_mux", 0);

K230_CLK_MUX_FORMAT2(timer2_mux,
			  0x50, 9, 1,
			  0, 0, 1,
			  "timer2_src", 0,
			  "timer-pulse-in", 1);

K230_CLK_GATE_FORMAT(timer2_gate,
			  0x50, 15, false, 0,
			  "timer2_mux", 0);

K230_CLK_MUX_FORMAT2(timer3_mux,
			  0x50, 10, 1,
			  0, 0, 1,
			  "timer3_gate", 0,
			  "timer-pulse-in", 1);

K230_CLK_GATE_FORMAT(timer3_gate,
			  0x50, 16, 0, 0,
			  "timer3_mux", 0);

K230_CLK_MUX_FORMAT2(timer4_mux,
			  0x50, 11, 1,
			  0, 0, 1,
			  "timer4_src", 0,
			  "timer-pulse-in", 1);

K230_CLK_GATE_FORMAT(timer4_gate,
			  0x50, 17, 0, 0,
			  "timer4_mux", 0);

K230_CLK_MUX_FORMAT2(timer5_mux,
			  0x50, 12, 1,
			  0, 0, 1,
			  "timer5_src", 0,
			  "timer-pulse-in", 1);

K230_CLK_GATE_FORMAT(timer5_gate,
			  0x50, 18, false, 0,
			  "timer5_mux", 0);

K230_CLK_GATE_FORMAT(shrm_apb_gate,
			  0x5C, 0, false, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(shrm_apb_rate,
			  1, 1, 0, 0,
			  1, 8, 18, 0x7,
			  0x5C, 31, div, 0x0,
			  false, 0,
			  "shrm_apb_gate", 0);

K230_CLK_GATE_FORMAT(shrm_sram_gate,
			  0x5c, 10, 0, 0,
			  "shrm_sram_mux", 0);

K230_CLK_FIXED_FACTOR_FORMAT(shrm_sram_div2,
			     1, 2,
			     "shrm_sram_gate", 0);

K230_CLK_GATE_FORMAT(shrm_axi_slave_gate,
		     0x5C, 11, 0, 0,
		     "shrm_sram_div2", 0);

K230_CLK_GATE_FORMAT(shrm_axi_gate,
		     0x5C, 12, 0, 0,
		     "pll0_div4", 0);

K230_CLK_GATE_FORMAT(shrm_nonai2d_axi_gate,
		     0x5C, 9, 0, 0,
		     "shrm_axi_gate", 0);

K230_CLK_MUX_FORMAT2(shrm_sram_mux,
			  0x50, 14, 1,
			  0, 0, 1,
			  "pll3_div2", 0, "pll0_div2", 0);

K230_CLK_GATE_FORMAT(shrm_decompress_axi_gate,
		     0x5C, 7, 0, 0,
		     "shrm_sram_gate", 0);

K230_CLK_GATE_FORMAT(shrm_sdma_axi_gate,
		     0x5C, 5, 0, 0,
		     "shrm_axi_gate", 0);

K230_CLK_GATE_FORMAT(shrm_pdma_axi_gate,
		     0x5C, 3, 0, 0,
		     "shrm_axi_gate", 0);

K230_CLK_MUX_FORMAT3(ddrc_src_mux,
		 0x60, 0, 2,
		 0, 0, 1,
		 "pll0_div2", 0,
		 "pll0_div3", 0,
		 "pll2_div4", 0);

K230_CLK_GATE_FORMAT(ddrc_src_gate,
		 0x60, 2, 0, 0,
		 "ddrc_src_mux", 0);

K230_CLK_RATE_FORMAT(ddrc_src_rate,
		 1, 1, 0, 0,
		 1, 16, 10, 0xF,
		 0x60, 31, div, 0x0,
		 false, 0,
		 "ddrc_src_gate", 0);

K230_CLK_GATE_FORMAT(ddrc_bypass_gate,
		     0x60, 8, 0, 0,
		     "pll2_div4", 0);

K230_CLK_GATE_FORMAT(ddrc_apb_gate,
			  0x60, 9, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(ddrc_apb_rate,
			  1, 1, 0, 0,
			  1, 16, 14, 0xF,
			  0x60, 31, div, 0x0,
			  false, 0,
			  "pll0_div4", 0);

K230_CLK_GATE_FORMAT(display_ahb_gate,
			  0x74, 0, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(display_ahb_rate,
			  1, 1, 0, 0,
			  1, 8, 0, 0x7,
			  0x78, 31, div, 0x0,
			  false, 0,
			  "pll0_div4", 0);

K230_CLK_GATE_FORMAT(display_axi_gate,
		     0x74, 1, 0, 0,
		     "pll0_div4", 0);

K230_CLK_RATE_FORMAT(display_clkext_rate,
			  1, 1, 0, 0,
			  1, 16, 16, 0xF,
			  0x78, 31, div, 0x0,
			  false, 0,
			  "display_axi_gate", 0);

K230_CLK_GATE_FORMAT(display_gpu_gate,
			  0x74, 6, false, 0,
			  "pll0_div3", 0);

K230_CLK_RATE_FORMAT(display_gpu_rate,
			  1, 1, 0, 0,
			  1, 16, 20, 0xF,
			  0x78, 31, div, 0x0,
			  false, 0,
			  "display_gpu_gate", 0);

K230_CLK_GATE_FORMAT(display_dpip_gate,
			  0x74, 2, 0, 0,
			  "pll1_div4", 0);

K230_CLK_RATE_FORMAT(display_dpip_rate,
			  1, 1, 0, 0,
			  1, 256, 3, 0xFF,
			  0x78, 31, div, 0x0,
			  false, 0,
			  "display_dpip_gate", 0);

K230_CLK_GATE_FORMAT(display_cfg_gate,
			  0x74, 4, 0, 0,
			  "pll1_div4", 0);

K230_CLK_RATE_FORMAT(display_cfg_rate,
			  1, 1, 0, 0,
			  1, 32, 11, 0x1F,
			  0x78, 31, div, 0x0,
			  false, 0,
			  "display_cfg_gate", 0);

K230_CLK_GATE_FORMAT(display_ref_gate,
		     0x74, 3, 0, 0,
		     "osc24m", 0);

K230_CLK_GATE_FORMAT(vpu_src_gate,
			  0xC, 0, 0, 0,
			  "pll0_div2", 0);

K230_CLK_RATE_FORMAT(vpu_src_rate,
			  1, 16, 0, 0,
			  16, 16, 1, 0xF,
			  0xC, 31, mul, 0x0,
			  false, 0,
			  "vpu_src_gate", 0);

K230_CLK_RATE_FORMAT(vpu_axi_src_rate,
		     1, 1, 0, 0,
		     1, 16, 6, 0xF,
		     0xC, 31, div, 0x0,
		     false, 0,
		     "vpu_src", 0);

K230_CLK_GATE_FORMAT(vpu_axi_gate,
		     0xC, 5, 0, 0,
		     "vpu_axi_src_rate", 0);

K230_CLK_GATE_FORMAT(vpu_ddrcp2_gate,
		     0x60, 5, 0, 0,
		     "vpu_axi_src_gate", 0);

K230_CLK_GATE_FORMAT(vpu_cfg_gate,
			  0xC, 10, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(vpu_cfg_rate,
			  1, 1, 0, 0,
			  1, 16, 11, 0xF,
			  0xC, 31, div, 0x0,
			  false, 0,
			  "vpu_cfg_gate", 0);

K230_CLK_GATE_FORMAT(sec_apb_gate,
			  0x80, 0, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(sec_apb_rate,
			  1, 1, 0, 0,
			  1, 8, 1, 0x7,
			  0x80, 31, div, 0x0,
			  false, 0,
			  "sec_apb_gate", 0);

K230_CLK_GATE_FORMAT(sec_fix_gate,
			  0x80, 5, 0, 0,
			  "pll1_div4", 0);

K230_CLK_RATE_FORMAT(sec_fix_rate,
			  1, 1, 0, 0,
			  1, 32, 6, 0x1F,
			  0x80, 31, div, 0x0,
			  false, 0,
			  "sec_fix_gate", 0);

K230_CLK_GATE_FORMAT(sec_axi_gate,
			  0x80, 4, 0, 0,
			  "pll1_div4", 0);

K230_CLK_RATE_FORMAT(sec_axi_rate,
			  1, 1, 0, 0,
			  1, 8, 11, 0x3,
			  0x80, 31, div, 0,
			  false, 0,
			  "sec_axi_rate", 0);

K230_CLK_GATE_FORMAT(usb_480m_gate,
			  0x100, 0, 0, 0,
			  "pll1", 0);

K230_CLK_RATE_FORMAT(usb_480m_rate,
			  1, 1, 0, 0,
			  1, 8, 1, 0x7,
			  0x100, 31, div, 0,
			  false, 0,
			  "usb_480m_gate", 0);

K230_CLK_GATE_FORMAT(usb_100m_gate,
			  0x100, 0, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(usb_100m_rate,
			  1, 1, 0, 0,
			  1, 8, 4, 0x7,
			  0x100, 31, div, 0,
			  false, 0,
			  "usb_100m_gate", 0);

K230_CLK_GATE_FORMAT(dphy_dft_gate,
			  0x100, 0, 0, 0,
			  "pll0", 0);

K230_CLK_RATE_FORMAT(dphy_dft_rate,
			  1, 1, 0, 0,
			  1, 16, 1, 0xF,
			  0x104, 31, div, 0,
			  false, 0,
			  "dphy_dft_gate", 0);

K230_CLK_GATE_FORMAT(spi2axi_gate,
			  0x108, 0, 0, 0,
			  "pll0_div4", 0);

K230_CLK_RATE_FORMAT(spi2axi_rate,
			  1, 1, 0, 0,
			  1, 8, 1, 0x7,
			  0x108, 31, div, 0x0,
			  false, 0,
			  "pll0_div4", 0);

K230_CLK_MUX_FORMAT2(ai_src_mux,
		 0x8, 2, 1,
		 0, 0, 1,
		 "pll0_div2", 0,
		 "pll3_div2", 0);

K230_CLK_GATE_FORMAT(ai_src_gate,
		 0x8, 0, 0, 0,
		 "ai_src_mux", 0);

K230_CLK_RATE_FORMAT(ai_src_rate,
		 1, 1, 0, 0,
		 1, 8, 3, 0x7,
		 0x8, 31, div, 0x0,
		 false, 0,
		 "ai_src_gate", 0);

K230_CLK_GATE_FORMAT(ai_axi_gate,
		     0x8, 10, 0, 0,
		     "ai_src", 0);

K230_CLK_MUX_FORMAT3(camera0_mux,
		 0x6C, 3, 2,
		 0, 0, 1,
		 "pll1_div3", 0,
		 "pll1_div4", 0,
		 "pll0_div4", 0);

K230_CLK_GATE_FORMAT(camera0_gate,
		 0x6C, 0, 0, 0,
		 "camera0_mux", 0);

K230_CLK_RATE_FORMAT(camera0_rate,
		 1, 1, 0, 0,
		 1, 32, 5, 0x1f,
		 0x6C, 31, div, 0x0,
		 false, 0,
		 "camera0_gate", 0);

K230_CLK_MUX_FORMAT3(camera1_mux,
		 0x6C, 10, 2,
		 0, 0, 1,
		 "pll1_div3", 0,
		 "pll1_div4", 0,
		 "pll0_div4", 0);

K230_CLK_GATE_FORMAT(camera1_gate,
		 0x6C, 1, 0, 0,
		 "camera1_mux", 0);

K230_CLK_RATE_FORMAT(camera1_rate,
		 1, 1, 0, 0,
		 1, 32, 12, 0x1f,
		 0x6C, 31, div, 0x0,
		 false, 0,
		 "camera1_gate", 0);

K230_CLK_MUX_FORMAT3(camera2_mux,
		 0x6C, 17, 2,
		 0, 0, 1,
		 "pll1_div3", 0,
		 "pll1_div4", 0,
		 "pll0_div4", 0);

K230_CLK_GATE_FORMAT(camera2_gate,
		 0x6C, 2, 0, 0,
		 "camera2_mux", 0);

K230_CLK_RATE_FORMAT(camera2_rate,
		 1, 1, 0, 0,
		 1, 32, 19, 0x1f,
		 0x6C, 31, div, 0x0,
		 false, 0,
		 "camera2_gate", 0);

static struct k230_clk *k230_clks[] = {
	[K230_CPU0_SRC_GATE]		=	K230_FMT(cpu0_src_gate),
	[K230_CPU0_SRC_RATE]		=	K230_FMT(cpu0_src_rate),
	[K230_CPU0_AXI_RATE]		=	K230_FMT(cpu0_axi_rate),
	[K230_CPU0_PLIC_GATE]		=	K230_FMT(cpu0_plic_gate),
	[K230_CPU0_PLIC_RATE]		=	K230_FMT(cpu0_plic_rate),
	[K230_CPU0_NOC_DDRCP4_GATE]	=	K230_FMT(cpu0_noc_ddrcp4_gate),
	[K230_CPU0_APB_GATE]		=	K230_FMT(cpu0_apb_gate),
	[K230_CPU0_APB_RATE]		=	K230_FMT(cpu0_apb_rate),
	[K230_CPU1_SRC_MUX]		=	K230_FMT(cpu1_src_mux),
	[K230_CPU1_SRC_GATE]		=	K230_FMT(cpu1_src_gate),
	[K230_CPU1_SRC_RATE]		=	K230_FMT(cpu1_src_rate),
	[K230_CPU1_AXI_RATE]		=	K230_FMT(cpu1_axi_rate),
	[K230_CPU1_PLIC_GATE]		=	K230_FMT(cpu1_plic_gate),
	[K230_CPU1_PLIC_RATE]		=	K230_FMT(cpu1_plic_rate),
	[K230_CPU1_APB_GATE]		=	K230_FMT(cpu1_apb_gate),
	[K230_CPU1_APB_RATE]		=	K230_FMT(cpu1_apb_rate),
	[K230_PMU_APB_GATE]		=	K230_FMT(pmu_apb_gate),
	[K230_HS_HCLK_HIGH_SRC_RATE]	=	K230_FMT(hs_hclk_high_src_rate),
	[K230_HS_HCLK_HIGH_GATE]	=	K230_FMT(hs_hclk_high_gate),
	[K230_HS_HCLK_SRC_GATE]		=	K230_FMT(hs_hclk_src_gate),
	[K230_HS_HCLK_SRC_RATE]		=	K230_FMT(hs_hclk_src_rate),
	[K230_HS_SD0_AHB_GATE]		=	K230_FMT(hs_sd0_ahb_gate),
	[K230_HS_SD1_AHB_GATE]		=	K230_FMT(hs_sd1_ahb_gate),
	[K230_HS_SSI1_AHB_GATE]		=	K230_FMT(hs_ssi1_ahb_gate),
	[K230_HS_SSI2_AHB_GATE]		=	K230_FMT(hs_ssi2_ahb_gate),
	[K230_HS_USB0_AHB_GATE]		=	K230_FMT(hs_usb0_ahb_gate),
	[K230_HS_USB1_AHB_GATE]		=	K230_FMT(hs_usb1_ahb_gate),
	[K230_HS_SSI0_AXI_GATE]		=	K230_FMT(hs_ssi0_axi_gate),
	[K230_HS_SSI0_AXI_RATE]		=	K230_FMT(hs_ssi0_axi_rate),
	[K230_HS_SSI1_GATE]		=	K230_FMT(hs_ssi1_gate),
	[K230_HS_SSI1_RATE]		=	K230_FMT(hs_ssi1_rate),
	[K230_HS_SSI2_GATE]		=	K230_FMT(hs_ssi2_gate),
	[K230_HS_SSI2_RATE]		=	K230_FMT(hs_ssi2_rate),
	[K230_HS_QSPI_AXI_SRC_GATE]	=	K230_FMT(hs_qspi_axi_src_gate),
	[K230_HS_QSPI_AXI_SRC_RATE]	=	K230_FMT(hs_qspi_axi_src_rate),
	[K230_HS_SSI1_AXI_GATE]		=	K230_FMT(hs_ssi1_axi_gate),
	[K230_HS_SSI2_AXI_GATE]		=	K230_FMT(hs_ssi2_axi_gate),
	[K230_HS_SD_CARD_SRC_GATE]	=	K230_FMT(hs_sd_card_src_gate),
	[K230_HS_SD_CARD_SRC_RATE]	=	K230_FMT(hs_sd_card_src_rate),
	[K230_HS_SD0_CARD_GATE]		=	K230_FMT(hs_sd0_card_gate),
	[K230_HS_SD1_CARD_GATE]		=	K230_FMT(hs_sd1_card_gate),
	[K230_HS_SD_AXI_SRC_GATE]	=	K230_FMT(hs_sd_axi_src_gate),
	[K230_HS_SD0_AXI_GATE]		=	K230_FMT(hs_sd0_axi_gate),
	[K230_HS_SD1_AXI_GATE]		=	K230_FMT(hs_sd1_axi_gate),
	[K230_HS_SD0_BASE_GATE]		=	K230_FMT(hs_sd0_base_gate),
	[K230_HS_SD1_BASE_GATE]		=	K230_FMT(hs_sd1_base_gate),
#if 0
	[K230_HS_OSPI_SRC_MUX]		=	K230_FMT(hs_ospi_src_mux),
	[K230_HS_OSPI_SRC_GATE]		=	K230_FMT(hs_ospi_src_gate),
	[K230_HS_USB_REF_50M_RATE]	=	K230_FMT(hs_usb_ref_50m_rate),
	[K230_HS_SD_TIMER_SRC_GATE]	=	K230_FMT(hs_sd_timer_src_gate),
	[K230_HS_SD_TIMER_SRC_RATE]	=	K230_FMT(hs_sd_timer_src_rate),
	[K230_HS_SD0_TIMER_GATE]	=	K230_FMT(hs_sd0_timer_gate),
	[K230_HS_SD1_TIMER_GATE]	=	K230_FMT(hs_sd1_timer_gate),
	[K230_HS_USB0_REF_MUX]		=	K230_FMT(hs_usb0_ref_mux),
	[K230_HS_USB0_REF_GATE]		=	K230_FMT(hs_usb0_ref_gate),
	[K230_HS_USB1_REF_MUX]		=	K230_FMT(hs_usb1_ref_mux),
	[K230_HS_USB1_REF_GATE]		=	K230_FMT(hs_usb1_ref_gate),
	[K230_LS_APB_SRC_GATE]		=	K230_FMT(ls_apb_src_gate),
	[K230_LS_APB_SRC_RATE]		=	K230_FMT(ls_apb_src_rate),
	[K230_LS_UART0_APB_GATE]	=	K230_FMT(ls_uart0_apb_gate),
	[K230_LS_UART1_APB_GATE]	=	K230_FMT(ls_uart1_apb_gate),
	[K230_LS_UART2_APB_GATE]	=	K230_FMT(ls_uart2_apb_gate),
	[K230_LS_UART3_APB_GATE]	=	K230_FMT(ls_uart3_apb_gate),
	[K230_LS_UART4_APB_GATE]	=	K230_FMT(ls_uart4_apb_gate),
	[K230_LS_I2C0_APB_GATE]		=	K230_FMT(ls_i2c0_apb_gate),
	[K230_LS_I2C1_APB_GATE]		=	K230_FMT(ls_i2c1_apb_gate),
	[K230_LS_I2C2_APB_GATE]		=	K230_FMT(ls_i2c2_apb_gate),
	[K230_LS_I2C3_APB_GATE]		=	K230_FMT(ls_i2c3_apb_gate),
	[K230_LS_I2C4_APB_GATE]		=	K230_FMT(ls_i2c4_apb_gate),
	[K230_LS_GPIO_APB_GATE]		=	K230_FMT(ls_gpio_apb_gate),
	[K230_LS_PWM_APB_GATE]		=	K230_FMT(ls_pwm_apb_gate),
	[K230_LS_JAMLINK0_APB_GATE]	=	K230_FMT(ls_jamlink0_apb_gate),
	[K230_LS_JAMLINK1_APB_GATE]	=	K230_FMT(ls_jamlink1_apb_gate),
	[K230_LS_JAMLINK2_APB_GATE]	=	K230_FMT(ls_jamlink2_apb_gate),
	[K230_LS_JAMLINK3_APB_GATE]	=	K230_FMT(ls_jamlink3_apb_gate),
	[K230_LS_AUDIO_APB_GATE]	=	K230_FMT(ls_audio_apb_gate),
	[K230_LS_ADC_APB_GATE]		=	K230_FMT(ls_adc_apb_gate),
	[K230_LS_CODEC_APB_GATE]	=	K230_FMT(ls_codec_apb_gate),
	[K230_LS_I2C0_GATE]		=	K230_FMT(ls_i2c0_gate),
	[K230_LS_I2C0_RATE]		=	K230_FMT(ls_i2c0_rate),
	[K230_LS_I2C1_GATE]		=	K230_FMT(ls_i2c1_gate),
	[K230_LS_I2C1_RATE]		=	K230_FMT(ls_i2c1_rate),
	[K230_LS_I2C2_GATE]		=	K230_FMT(ls_i2c2_gate),
	[K230_LS_I2C2_RATE]		=	K230_FMT(ls_i2c2_rate),
	[K230_LS_I2C3_GATE]		=	K230_FMT(ls_i2c3_gate),
	[K230_LS_I2C3_RATE]		=	K230_FMT(ls_i2c3_rate),
	[K230_LS_I2C4_GATE]		=	K230_FMT(ls_i2c4_gate),
	[K230_LS_I2C4_RATE]		=	K230_FMT(ls_i2c4_rate),
	[K230_LS_CODEC_ADC_GATE]	=	K230_FMT(ls_codec_adc_gate),
	[K230_LS_CODEC_ADC_RATE]	=	K230_FMT(ls_codec_adc_rate),
	[K230_LS_CODEC_DAC_GATE]	=	K230_FMT(ls_codec_dac_gate),
	[K230_LS_CODEC_DAC_RATE]	=	K230_FMT(ls_codec_dac_rate),
	[K230_LS_AUDIO_DEV_GATE]	=	K230_FMT(ls_audio_dev_gate),
	[K230_LS_AUDIO_DEV_RATE]	=	K230_FMT(ls_audio_dev_rate),
	[K230_LS_PDM_GATE]		=	K230_FMT(ls_pdm_gate),
	[K230_LS_PDM_RATE]		=	K230_FMT(ls_pdm_rate),
	[K230_LS_ADC_GATE]		=	K230_FMT(ls_adc_gate),
	[K230_LS_ADC_RATE]		=	K230_FMT(ls_adc_rate),
	[K230_LS_UART0_GATE]		=	K230_FMT(ls_uart0_gate),
	[K230_LS_UART0_RATE]		=	K230_FMT(ls_uart0_rate),
	[K230_LS_UART1_GATE]		=	K230_FMT(ls_uart1_gate),
	[K230_LS_UART1_RATE]		=	K230_FMT(ls_uart1_rate),
	[K230_LS_UART2_GATE]		=	K230_FMT(ls_uart2_gate),
	[K230_LS_UART2_RATE]		=	K230_FMT(ls_uart2_rate),
	[K230_LS_UART3_GATE]		=	K230_FMT(ls_uart3_gate),
	[K230_LS_UART3_RATE]		=	K230_FMT(ls_uart3_rate),
	[K230_LS_UART4_GATE]		=	K230_FMT(ls_uart4_gate),
	[K230_LS_UART4_RATE]		=	K230_FMT(ls_uart4_rate),
	[K230_LS_JAMLINKCO_SRC_RATE]	=	K230_FMT(ls_jamlinkco_src_rate),
	[K230_LS_JAMLINK0CO_GATE]	=	K230_FMT(ls_jamlink0co_gate),
	[K230_LS_JAMLINK1CO_GATE]	=	K230_FMT(ls_jamlink1co_gate),
	[K230_LS_JAMLINK2CO_GATE]	=	K230_FMT(ls_jamlink2co_gate),
	[K230_LS_JAMLINK3CO_GATE]	=	K230_FMT(ls_jamlink3co_gate),
	[K230_LS_GPIO_DEBOUNCE_GATE]	=	K230_FMT(ls_gpio_debounce_gate),
	[K230_LS_GPIO_DEBOUNCE_RATE]	=	K230_FMT(ls_gpio_debounce_rate),
	[K230_SYSCTL_APB_SRC]		=	K230_FMT(sysctl_apb_src),
	[K230_SYSCTL_WDT0_APB_GATE]	=	K230_FMT(sysctl_wdt0_apb_gate),
	[K230_SYSCTL_WDT1_APB_GATE]	=	K230_FMT(sysctl_wdt1_apb_gate),
	[K230_SYSCTL_TIMER_APB_GATE]	=	K230_FMT(sysctl_timer_apb_gate),
	[K230_SYSCTL_IOMUX_APB_GATE]	=	K230_FMT(sysctl_iomux_apb_gate),
	[K230_SYSCTL_MAILBOX_APB_GATE]	=	K230_FMT(sysctl_mailbox_apb_gate),
	[K230_SYSCTL_HDI_GATE]		=	K230_FMT(sysctl_hdi_gate),
	[K230_SYSCTL_HDI_RATE]		=	K230_FMT(sysctl_hdi_rate),
	[K230_SYSCTL_TIME_STAMP_GATE]	=	K230_FMT(sysctl_time_stamp_gate),
	[K230_SYSCTL_TIME_STAMP_RATE]	=	K230_FMT(sysctl_time_stamp_rate),
	[K230_SYSCTL_TEMP_SENSOR_RATE]	=	K230_FMT(sysctl_temp_sensor_rate),
	[K230_SYSCTL_WDT0_GATE]		=	K230_FMT(sysctl_wdt0_gate),
	[K230_SYSCTL_WDT0_RATE]		=	K230_FMT(sysctl_wdt0_rate),
	[K230_SYSCTL_WDT1_GATE]		=	K230_FMT(sysctl_wdt1_gate),
	[K230_SYSCTL_WDT1_RATE]		=	K230_FMT(sysctl_wdt1_rate),
	[K230_TIMER0_SRC_RATE]		=	K230_FMT(timer0_src_rate),
	[K230_TIMER1_SRC_RATE]		=	K230_FMT(timer1_src_rate),
	[K230_TIMER2_SRC_RATE]		=	K230_FMT(timer2_src_rate),
	[K230_TIMER3_SRC_RATE]		=	K230_FMT(timer3_src_rate),
	[K230_TIMER4_SRC_RATE]		=	K230_FMT(timer4_src_rate),
	[K230_TIMER5_SRC_RATE]		=	K230_FMT(timer5_src_rate),
	[K230_TIMER0_MUX]		=	K230_FMT(timer0_mux),
	[K230_TIMER0_GATE]		=	K230_FMT(timer0_gate),
	[K230_TIMER1_MUX]		=	K230_FMT(timer1_mux),
	[K230_TIMER1_GATE]		=	K230_FMT(timer1_gate),
	[K230_TIMER2_MUX]		=	K230_FMT(timer2_mux),
	[K230_TIMER2_GATE]		=	K230_FMT(timer2_gate),
	[K230_TIMER3_MUX]		=	K230_FMT(timer3_mux),
	[K230_TIMER3_GATE]		=	K230_FMT(timer3_gate),
	[K230_TIMER4_MUX]		=	K230_FMT(timer4_mux),
	[K230_TIMER4_GATE]		=	K230_FMT(timer4_gate),
	[K230_TIMER5_MUX]		=	K230_FMT(timer5_mux),
	[K230_TIMER5_GATE]		=	K230_FMT(timer5_gate),
	[K230_SHRM_APB_GATE]		=	K230_FMT(shrm_apb_gate),
	[K230_SHRM_AXI_GATE]		=	K230_FMT(shrm_axi_gate),
	[K230_SHRM_AXI_SLAVE_GATE]	=	K230_FMT(shrm_axi_slave_gate),
	[K230_SHRM_NONAI2D_AXI_GATE]	=	K230_FMT(shrm_nonai2d_axi_gate),
	[K230_SHRM_SRAM_MUX]		=	K230_FMT(shrm_sram_mux),
	[K230_SHRM_SRAM_GATE]		=	K230_FMT(shrm_sram_gate),
	[K230_SHRM_SRAM_DIV2]		=	K230_FMT(shrm_sram_div2),
	[K230_SHRM_DECOMPRESS_AXI_GATE]	=	K230_FMT(shrm_decompress_axi_gate),
	[K230_SHRM_SDMA_AXI_GATE]	=	K230_FMT(shrm_sdma_axi_gate),
	[K230_SHRM_PDMA_AXI_GATE]	=	K230_FMT(shrm_pdma_axi_gate),
	[K230_DDRC_SRC_MUX]		=	K230_FMT(ddrc_src_mux),
	[K230_DDRC_SRC_GATE]		=	K230_FMT(ddrc_src_gate),
	[K230_DDRC_SRC_RATE]		=	K230_FMT(ddrc_src_rate),
	[K230_DDRC_BYPASS_GATE]		=	K230_FMT(ddrc_bypass_gate),
	[K230_DDRC_APB_GATE]		=	K230_FMT(ddrc_apb_gate),
	[K230_DDRC_APB_RATE]		=	K230_FMT(ddrc_apb_rate),
	[K230_DISPLAY_AHB_GATE]		=	K230_FMT(display_ahb_gate),
	[K230_DISPLAY_AHB_RATE]		=	K230_FMT(display_ahb_rate),
	[K230_DISPLAY_AXI_GATE]		=	K230_FMT(display_axi_gate),
	[K230_DISPLAY_CLKEXT_RATE]	=	K230_FMT(display_clkext_rate),
	[K230_DISPLAY_GPU_GATE]		=	K230_FMT(display_gpu_gate),
	[K230_DISPLAY_GPU_RATE]		=	K230_FMT(display_gpu_rate),
	[K230_DISPLAY_DPIP_GATE]	=	K230_FMT(display_dpip_gate),
	[K230_DISPLAY_DPIP_RATE]	=	K230_FMT(display_dpip_rate),
	[K230_DISPLAY_CFG_GATE]		=	K230_FMT(display_cfg_gate),
	[K230_DISPLAY_CFG_RATE]		=	K230_FMT(display_cfg_rate),
	[K230_DISPLAY_REF_GATE]		=	K230_FMT(display_ref_gate),
	[K230_VPU_SRC_GATE]		=	K230_FMT(vpu_src_gate),
	[K230_VPU_SRC_RATE]		=	K230_FMT(vpu_src_rate),
	[K230_VPU_AXI_SRC_RATE]		=	K230_FMT(vpu_axi_src_rate),
	[K230_VPU_AXI_GATE]		=	K230_FMT(vpu_axi_gate),
	[K230_VPU_DDRCP2_GATE]		=	K230_FMT(vpu_ddrcp2_gate),
	[K230_VPU_CFG_GATE]		=	K230_FMT(vpu_cfg_gate),
	[K230_VPU_CFG_RATE]		=	K230_FMT(vpu_cfg_rate),
	[K230_SEC_APB_GATE]		=	K230_FMT(sec_apb_gate),
	[K230_SEC_APB_RATE]		=	K230_FMT(sec_apb_rate),
	[K230_SEC_FIX_GATE]		=	K230_FMT(sec_fix_gate),
	[K230_SEC_FIX_RATE]		=	K230_FMT(sec_fix_rate),
	[K230_SEC_AXI_GATE]		=	K230_FMT(sec_axi_gate),
	[K230_SEC_AXI_RATE]		=	K230_FMT(sec_axi_rate),
	[K230_USB_480M_GATE]		=	K230_FMT(usb_480m_gate),
	[K230_USB_480M_RATE]		=	K230_FMT(usb_480m_rate),
	[K230_USB_100M_GATE]		=	K230_FMT(usb_100m_gate),
	[K230_USB_100M_RATE]		=	K230_FMT(usb_100m_rate),
	[K230_DPHY_DFT_GATE]		=	K230_FMT(dphy_dft_gate),
	[K230_DPHY_DFT_RATE]		=	K230_FMT(dphy_dft_rate),
	[K230_SPI2AXI_GATE]		=	K230_FMT(spi2axi_gate),
	[K230_SPI2AXI_RATE]		=	K230_FMT(spi2axi_rate),
	[K230_AI_SRC_MUX]		=	K230_FMT(ai_src_mux),
	[K230_AI_SRC_GATE]		=	K230_FMT(ai_src_gate),
	[K230_AI_SRC_RATE]		=	K230_FMT(ai_src_rate),
	[K230_AI_AXI_GATE]		=	K230_FMT(ai_axi_gate),
	[K230_CAMERA0_MUX]		=	K230_FMT(camera0_mux),
	[K230_CAMERA0_GATE]		=	K230_FMT(camera0_gate),
	[K230_CAMERA0_RATE]		=	K230_FMT(camera0_rate),
	[K230_CAMERA1_MUX]		=	K230_FMT(camera1_mux),
	[K230_CAMERA1_GATE]		=	K230_FMT(camera1_gate),
	[K230_CAMERA1_RATE]		=	K230_FMT(camera1_rate),
	[K230_CAMERA2_MUX]		=	K230_FMT(camera2_mux),
	[K230_CAMERA2_GATE]		=	K230_FMT(camera2_gate),
	[K230_CAMERA2_RATE]		=	K230_FMT(camera2_rate),
#endif
};

#define K230_CLK_NUM	ARRAY_SIZE(k230_clks)

static int k230_pll_prepare(struct clk_hw *hw)
{
	struct k230_pll *pll = to_k230_pll(hw);
	struct k230_sysclk *ksc = pll->ksc;
	u32 reg;

	/* wait for PLL lock until it reaches lock status */
	return readl_poll_timeout(K230_PLLX_LOCK_ADDR(ksc->pll_regs, pll->id), reg,
				  reg & K230_PLL_LOCK_STATUS_MASK,
				  400, 0);
}

static inline bool k230_pll_hw_is_enabled(struct k230_pll *pll)
{
	struct k230_sysclk *ksc = pll->ksc;

	return !!(readl(K230_PLLX_GATE_ADDR(ksc->pll_regs, pll->id)) & K230_PLL_GATE_ENABLE);
}

static void k230_pll_enable_hw(struct k230_pll *pll)
{
	struct k230_sysclk *ksc = pll->ksc;
	u32 reg;

	if (k230_pll_hw_is_enabled(pll))
		return;

	/* Set PLL factors */
	reg = readl(K230_PLLX_GATE_ADDR(ksc->pll_regs, pll->id));
	reg |= K230_PLL_GATE_ENABLE | K230_PLL_GATE_WRITE_ENABLE;
	writel(reg, K230_PLLX_GATE_ADDR(ksc->pll_regs, pll->id));
}

static int k230_pll_enable(struct clk_hw *hw)
{
	struct k230_pll *pll = to_k230_pll(hw);
	struct k230_sysclk *ksc = pll->ksc;

	guard(spinlock)(&ksc->pll_lock);

	k230_pll_enable_hw(pll);

	return 0;
}

static void k230_pll_disable(struct clk_hw *hw)
{
	struct k230_pll *pll = to_k230_pll(hw);
	struct k230_sysclk *ksc = pll->ksc;
	u32 reg;

	guard(spinlock)(&ksc->pll_lock);

	reg = readl(K230_PLLX_GATE_ADDR(ksc->pll_regs, pll->id));
	reg &= ~(K230_PLL_GATE_ENABLE);
	reg |= (K230_PLL_GATE_WRITE_ENABLE);
	writel(reg, K230_PLLX_GATE_ADDR(ksc->pll_regs, pll->id));
}

static int k230_pll_is_enabled(struct clk_hw *hw)
{
	return k230_pll_hw_is_enabled(to_k230_pll(hw));
}

static unsigned long k230_pll_get_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct k230_pll *pll = to_k230_pll(hw);
	struct k230_sysclk *ksc = pll->ksc;
	u32 reg;
	u32 r, f, od;

	guard(spinlock)(&ksc->pll_lock);

	reg = readl(K230_PLLX_BYPASS_ADDR(ksc->pll_regs, pll->id));
	if (reg & K230_PLL_BYPASS_ENABLE)
		return parent_rate;

	reg = readl(K230_PLLX_LOCK_ADDR(ksc->pll_regs, pll->id));
	if (!(reg & (K230_PLL_LOCK_STATUS_MASK))) {
		dev_err(&ksc->pdev->dev, "%s is unlock.\n", clk_hw_get_name(hw));
		return 0;
	}

	reg = readl(K230_PLLX_DIV_ADDR(ksc->pll_regs, pll->id));
	r = ((reg >> K230_PLL_R_SHIFT) & K230_PLL_R_MASK) + 1;
	f = ((reg >> K230_PLL_F_SHIFT) & K230_PLL_F_MASK) + 1;
	od = ((reg >> K230_PLL_OD_SHIFT) & K230_PLL_OD_MASK) + 1;

	return mul_u64_u32_div(parent_rate, f, r * od);
}

static const struct clk_ops k230_pll_ops = {
	.prepare	= k230_pll_prepare,
	.enable	        = k230_pll_enable,
	.disable	= k230_pll_disable,
	.is_enabled	= k230_pll_is_enabled,
	.recalc_rate	= k230_pll_get_rate,
};

static int k230_register_plls(struct platform_device *pdev, struct k230_sysclk *ksc)
{
	int i, ret;
	struct k230_pll *pll;
	struct clk_init_data init = {};
	const struct clk_parent_data parent_data[] = {
		{
			.index = 0,
			.name = "osc24m",
		},
	};

	for (i = 0; i < K230_PLL_NUM; i++) {
		pll = &k230_plls[i];

		init.name = pll->name;
		init.parent_data = parent_data;
		init.num_parents = 1;
		init.ops = &k230_pll_ops;
		init.flags = CLK_IS_CRITICAL;

		pll->hw.init = &init;
		pll->ksc = ksc;

		ret = devm_clk_hw_register(&pdev->dev, &pll->hw);
		if (ret)
			return ret;
	}

	return 0;
}

static int k230_register_pll_divs(struct platform_device *pdev, struct k230_sysclk *ksc)
{
	struct device *dev = &pdev->dev;
	struct k230_pll_div *pll_div;
	struct clk_hw *hw;
	int ret;

	for (int i = 0; i < K230_PLL_DIV_NUM; i++) {
		pll_div = &k230_pll_divs[i];

		hw = devm_clk_hw_register_fixed_factor(dev, pll_div->name,
						       pll_div->parent_name,
						       0, 1, pll_div->div);
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		ret = devm_clk_hw_register_clkdev(&pdev->dev, hw, pll_div->name, NULL);
		if (ret)
			return ret;

		pll_div->hw = hw;
		pll_div->ksc = ksc;
		pll_div->id = i;
	}

	return 0;
}

static unsigned long k230_clk_get_rate_mul(struct clk_hw *hw,
				       unsigned long parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	struct k230_sysclk *ksc = rate_cfg->ksc;
	u32 mul = 1, div;

	/* no divider, return parents' clk */
	if (!rate_cfg)
		return parent_rate;

	guard(spinlock)(&ksc->clk_lock);

	div = rate_cfg->rate_div_max;
	mul += (readl(ksc->regs + rate_cfg->rate_reg_off) >> rate_cfg->rate_div_shift)
		& rate_cfg->rate_div_mask;

	return mul_u64_u32_div(parent_rate, mul, div);
}

static unsigned long k230_clk_get_rate_div(struct clk_hw *hw,
				       unsigned long parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	struct k230_sysclk *ksc = rate_cfg->ksc;
	u32 mul, div = 1;

	/* no divider, return parents' clk */
	if (!rate_cfg)
		return parent_rate;

	guard(spinlock)(&ksc->clk_lock);

	mul = rate_cfg->rate_mul_max;
	div += (readl(ksc->regs + rate_cfg->rate_reg_off) >> rate_cfg->rate_div_shift)
		& rate_cfg->rate_div_mask;

	return mul_u64_u32_div(parent_rate, mul, div);
}

static unsigned long k230_clk_get_rate_mul_div(struct clk_hw *hw,
				       unsigned long parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	struct k230_sysclk *ksc = rate_cfg->ksc;
	u32 mul, div, reg_off, reg_off2;

	guard(spinlock)(&ksc->clk_lock);

	reg_off = rate_cfg->rate_reg_off;
	reg_off2 = rate_cfg->rate_reg_off2 ?
		   rate_cfg->rate_reg_off2 : reg_off;

	mul = (readl(ksc->regs + reg_off2)
		>> rate_cfg->rate_mul_shift)
		& rate_cfg->rate_mul_mask;

	div = (readl(ksc->regs + reg_off)
		>> rate_cfg->rate_div_shift)
		& rate_cfg->rate_div_mask;

	return mul_u64_u32_div(parent_rate, mul, div);
}

static int k230_clk_find_approximate_mul(u32 mul_min, u32 mul_max,
					 u32 div_min, u32 div_max,
					 unsigned long rate, unsigned long parent_rate,
					 u32 *div, u32 *mul)
{
	long abs_min;
	long abs_current;
	long perfect_divide;

	if (!rate || !parent_rate || !mul_min || !mul_max)
		return -EINVAL;

	perfect_divide = (long)((parent_rate * 1000) / rate);
	abs_min = abs(perfect_divide -
		     (long)(((long)div_max * 1000) / (long)mul_min));
	*mul = mul_min;

	for (u32 i = mul_min + 1; i <= mul_max; i++) {
		abs_current = abs(perfect_divide -
				(long)((long)((long)div_max * 1000) / (long)i));
		if (abs_min > abs_current) {
			abs_min = abs_current;
			*mul = i;
		}
	}

	*div = div_max;

	return 0;
}

static int k230_clk_find_approximate_div(u32 mul_min, u32 mul_max,
					 u32 div_min, u32 div_max,
				     	 unsigned long rate, unsigned long parent_rate,
				     	 u32 *div, u32 *mul)
{
	long abs_min;
	long abs_current;
	long perfect_divide;

	if (!rate || !parent_rate || !mul_min || !mul_max)
		return -EINVAL;

	perfect_divide = (long)((parent_rate * 1000) / rate);
	abs_min = abs(perfect_divide -
		     (long)(((long)div_min * 1000) / (long)mul_max));
	*div = div_min;

	for (u32 i = div_min + 1; i <= div_max; i++) {
		abs_current = abs(perfect_divide -
				 (long)((long)((long)i * 1000) / (long)mul_max));
		if (abs_min > abs_current) {
			abs_min = abs_current;
			*div = i;
		}
	}

	*mul = mul_max;

	return 0;
}

static int k230_clk_find_approximate_mul_div(struct k230_clk_rate_cfg *rate_cfg,
					     u32 mul_min, u32 mul_max,
					     u32 div_min, u32 div_max,
					     unsigned long rate, unsigned long parent_rate,
					     u32 *div, u32 *mul)
{
	const u32 codec_clk[9] = {
		2048000,
		3072000,
		4096000,
		6144000,
		8192000,
		11289600,
		12288000,
		24576000,
		49152000
	};

	const u32 codec_div[9][2] = {
		{3125, 16},
		{3125, 24},
		{3125, 32},
		{3125, 48},
		{3125, 64},
		{15625, 441},
		{3125, 96},
		{3125, 192},
		{3125, 384}
	};

	const u32 pdm_clk[20] = {
		128000,
		192000,
		256000,
		384000,
		512000,
		768000,
		1024000,
		1411200,
		1536000,
		2048000,
		2822400,
		3072000,
		4096000,
		5644800,
		6144000,
		8192000,
		11289600,
		12288000,
		24576000,
		49152000
	};

	const u32 pdm_div[20][2] = {
		{3125, 1},
		{6250, 3},
		{3125, 2},
		{3125, 3},
		{3125, 4},
		{3125, 6},
		{3125, 8},
		{125000, 441},
		{3125, 12},
		{3125, 16},
		{62500, 441},
		{3125, 24},
		{3125, 32},
		{31250, 441},
		{3125, 48},
		{3125, 64},
		{15625, 441},
		{3125, 96},
		{3125, 192},
		{3125, 384}
	};

	if (!rate || !parent_rate || !mul_min || !mul_max)
		return -EINVAL;

	if (rate_cfg->rate_reg_off == K230_CLK_CODEC_ADC_MCLKDIV_OFFSET ||
	    rate_cfg->rate_reg_off == K230_CLK_CODEC_DAC_MCLKDIV_OFFSET) {
		for (int i = 0; i < 9; i++) {
			if (rate == codec_clk[i]) {
				*div = codec_div[i][0];
				*mul = codec_div[i][1];
			}
		}
	} else if (rate_cfg->rate_reg_off == K230_CLK_AUDIO_CLKDIV_OFFSET ||
		   rate_cfg->rate_reg_off == K230_CLK_PDM_CLKDIV_OFFSET) {
		for (int i = 0; i < 20; i++) {
			if (rate == pdm_clk[i]) {
				*div = pdm_div[i][0];
				*mul = pdm_div[i][1];
			}
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static long k230_clk_round_rate_mul(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	u32 div, mul;

	if (k230_clk_find_approximate_mul(rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
					  rate_cfg->rate_div_min, rate_cfg->rate_div_max,
				      	  rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static long k230_clk_round_rate_div(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	u32 div, mul;

	if (k230_clk_find_approximate_div(rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
					  rate_cfg->rate_div_min, rate_cfg->rate_div_max,
					  rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static long k230_clk_round_rate_mul_div(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	u32 div, mul;

	if (k230_clk_find_approximate_mul_div(rate_cfg,
					      rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
				      	      rate_cfg->rate_div_min, rate_cfg->rate_div_max,
				      	      rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static int k230_clk_set_rate_mul(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	struct k230_sysclk *ksc = rate_cfg->ksc;
	u32 div, mul, reg;

	if (rate > parent_rate) {
		dev_err(&ksc->pdev->dev, "rate should be smaller than parent rate\n");
		return -EINVAL;
	}

	if (rate_cfg->read_only)
		return 0;

	if (k230_clk_find_approximate_mul(
				      rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
				      rate_cfg->rate_div_min, rate_cfg->rate_div_max,
				      rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + rate_cfg->rate_reg_off);
	reg &= ~((rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift));
	reg |= ((mul - 1) & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
	reg |= BIT(rate_cfg->rate_write_enable_bit);
	writel(reg, ksc->regs + rate_cfg->rate_reg_off);

	return 0;
}

static int k230_clk_set_rate_div(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	struct k230_sysclk *ksc = rate_cfg->ksc;
	u32 div, mul, reg;

	if (rate > parent_rate) {
		dev_err(&ksc->pdev->dev, "rate should be smaller than parent rate\n");
		return -EINVAL;
	}

	if (rate_cfg->read_only)
		return 0;

	if (k230_clk_find_approximate_div(rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
				      rate_cfg->rate_div_min, rate_cfg->rate_div_max,
				      rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + rate_cfg->rate_reg_off);
	reg &= ~((rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift));
	reg &= ~((rate_cfg->rate_mul_mask) << (rate_cfg->rate_mul_shift));
	reg |= ((div - 1) & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
	reg |= BIT(rate_cfg->rate_write_enable_bit);
	writel(reg, ksc->regs + rate_cfg->rate_reg_off);

	return 0;
}

static int k230_clk_set_rate_mul_div(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct k230_clk_rate_cfg *rate_cfg = to_k230_rate_cfg(hw);
	struct k230_sysclk *ksc = rate_cfg->ksc;
	u32 div, mul, reg, reg_c, reg_off;

	if (rate > parent_rate) {
		dev_err(&ksc->pdev->dev, "rate should be smaller than parent rate\n");
		return -EINVAL;
	}

	if (rate_cfg->read_only)
		return 0;

	if (k230_clk_find_approximate_mul_div(rate_cfg,
					      rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
					      rate_cfg->rate_div_min, rate_cfg->rate_div_max,
					      rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + reg_off);
	reg &= ~((rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift));
	if (!rate_cfg->rate_reg_off2) {
		reg |= (mul & rate_cfg->rate_mul_mask) << (rate_cfg->rate_mul_shift);
		reg |= (div & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
		reg |= BIT(rate_cfg->rate_write_enable_bit);
	} else {
		reg_c = readl(ksc->regs + rate_cfg->rate_reg_off);
		reg_c &= ~((rate_cfg->rate_mul_mask) << (rate_cfg->rate_mul_shift));
		reg_c |= BIT(rate_cfg->rate_write_enable_bit);
		reg_c |= (mul & rate_cfg->rate_mul_mask) << (rate_cfg->rate_mul_shift);
		writel(reg_c, ksc->regs + rate_cfg->rate_reg_off2);
	}
	reg |= (div & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
	writel(reg, ksc->regs + rate_cfg->rate_reg_off);

	return 0;
}

static int k230_register_clks(struct platform_device *pdev,
			      struct clk_hw_onecell_data *hw_data,
			      struct k230_sysclk *ksc)
{
	struct clk_hw *hw;
	struct clk_init_data init = {};
	struct k230_clk *clk;
	struct k230_clk_parent_data *parent;
	int ret, i, flags;
	u32 reg, bit, shift, width;

	for (i = 0; i < K230_CLK_NUM; i++) {
		clk = k230_clks[i];
		if (!clk)
			continue;

		switch (clk->type) {
		case K230_FIXED_RATE:
			hw = devm_clk_hw_register_fixed_rate(&pdev->dev, clk->name,
								  clk->parent[0].name,
								  clk->flags,
								  clk->fixed_rate_cfg->fixed_rate);
			if (IS_ERR(hw))
				return PTR_ERR(hw);

			break;
		case K230_FIXED_FACTOR:
			hw = devm_clk_hw_register_fixed_factor(&pdev->dev, clk->name,
							       clk->parent[0].name,
							       clk->flags,
							       clk->fixed_factor_cfg->mul,
							       clk->fixed_factor_cfg->div);
			if (IS_ERR(hw))
				return PTR_ERR(hw);

			break;
		case K230_GATE:
			reg = clk->gate_cfg->gate_reg_off;
			bit = clk->gate_cfg->gate_bit_enable;
			flags = clk->gate_cfg->gate_flags;

			hw = devm_clk_hw_register_gate(&pdev->dev, clk->name,
						       clk->parent[0].name,
						       clk->flags,
						       reg + ksc->regs,
						       bit, flags,
						       &ksc->clk_lock);
			if (IS_ERR(hw))
				return PTR_ERR(hw);

			break;
		case K230_MUX:
			reg = clk->mux_cfg->mux_reg_off;
			shift = clk->mux_cfg->mux_reg_shift;
			width = clk->mux_cfg->mux_reg_width;
			flags = clk->mux_cfg->mux_flags;
			parent = &clk->mux_cfg->parent;

			hw = devm_clk_hw_register_mux(&pdev->dev, clk->name,
						      parent->name,
						      parent->num_parents,
						      clk->flags,
						      reg + ksc->regs,
						      shift, width, flags,
						      &ksc->clk_lock);
			if (IS_ERR(hw))
				return PTR_ERR(hw);

			break;
		default:
			init.name = clk->name;
			init.flags = clk->flags;
			init.parent_data = clk->parent;
			init.num_parents = 1;
			init.ops = clk->rate_cfg->ops;

			hw = &clk->rate_cfg->hw;
			hw->init = &init;

			clk->rate_cfg->ksc = ksc;

			ret = devm_clk_hw_register(&pdev->dev, hw);
			if (ret)
				return ret;
		}
		hw_data->hws[i] = hw;
	}

	return 0;
}

static int k230_clk_init_plls(struct platform_device *pdev,
			      struct k230_sysclk *ksc)
{
	int ret;

	spin_lock_init(&ksc->pll_lock);

	ksc->pll_regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ksc->pll_regs))
		return PTR_ERR(ksc->pll_regs);

	ret = k230_register_plls(pdev, ksc);
	if (ret)
		return ret;

	ret = k230_register_pll_divs(pdev, ksc);
	if (ret)
		return ret;

	return 0;
}

static int k230_clk_init_clks(struct platform_device *pdev,
			      struct clk_hw_onecell_data *hw_data,
			      struct k230_sysclk *ksc)
{
	int ret;

	spin_lock_init(&ksc->clk_lock);

	hw_data->num = K230_CLK_NUM;

	ksc->regs = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ksc->regs))
		return PTR_ERR(ksc->regs);

	ret = k230_register_clks(pdev, hw_data, ksc);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, hw_data);
}

static int k230_clk_probe(struct platform_device *pdev)
{
	int ret;
	struct k230_sysclk *ksc;
	struct clk_hw_onecell_data *hw_data;

	ksc = devm_kzalloc(&pdev->dev, sizeof(*ksc), GFP_KERNEL);
	if (!ksc)
		return -ENOMEM;

	hw_data = devm_kzalloc(&pdev->dev, struct_size(hw_data, hws, K230_CLK_NUM),
			       GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	ret = k230_clk_init_plls(pdev, ksc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "init plls failed\n");

	ret = k230_clk_init_clks(pdev, hw_data, ksc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "init clks failed\n");

	return 0;
}

static const struct of_device_id k230_clk_ids[] = {
	{ .compatible = "canaan,k230-clk" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, k230_clk_ids);

static struct platform_driver k230_clk_driver = {
	.driver = {
		.name = "k230_clock_controller",
		.of_match_table = k230_clk_ids,
	},
	.probe = k230_clk_probe,
};
builtin_platform_driver(k230_clk_driver);
