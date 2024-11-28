// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Xukai Wang <kingxukai@zohomail.com>
 * Author: Troy Mitchell <troymitchell988@gmail.com>
 * Kendryte Canaan K230 Clock Drivers
 */
#include <linux/bug.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <dt-bindings/clock/k230-clk.h>

/* PLL control register bits. */
#define K230_PLL_BYPASS_ENABLE		BIT(19)
#define K230_PLL_GATE_ENABLE		BIT(2)
#define K230_PLL_GATE_WRITE_ENABLE	BIT(18)
#define K230_PLL_OD_SHIFT		24
#define K230_PLL_OD_MASK		0xF
#define K230_PLL_R_SHIFT		16
#define K230_PLL_R_MASK			0x3F
#define K230_PLL_F_SHIFT		0
#define K230_PLL_F_MASK			0x1FFFF
#define K230_PLL0_OFFSET_BASE		0x00
#define K230_PLL1_OFFSET_BASE		0x10
#define K230_PLL2_OFFSET_BASE		0x20
#define K230_PLL3_OFFSET_BASE		0x30
#define K230_PLL_DIV_REG_OFFSET		0x00
#define K230_PLL_BYPASS_REG_OFFSET	0x04
#define K230_PLL_GATE_REG_OFFSET	0x08
#define K230_PLL_LOCK_REG_OFFSET	0x0C
/* PLL lock register bits.  */
#define K230_PLL_STATUS_MASK            BIT(0)

/* K230 CLK MACROS */
#define K230_GATE_FORMAT(_reg, _bit, _reverse, _have_gate)                      \
	.gate_reg_off = (_reg),                                                 \
	.gate_bit_enable = (_bit),                                              \
	.gate_bit_reverse = (_reverse),                                         \
	.have_gate = (_have_gate)

#define K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,             \
			_div_min, _div_max, _div_shift, _div_mask,		\
			_reg, _bit, _method, _reg_c, _bit_c,			\
			_mul_min_c, _mul_max_c, _mul_shift_c, _mul_mask_c,      \
			_have_rate, _have_rate_c)                               \
	.rate_mul_min = (_mul_min),                                             \
	.rate_mul_max = (_mul_max),                                             \
	.rate_mul_shift = (_mul_shift),                                         \
	.rate_mul_mask = (_mul_mask),                                           \
	.rate_mul_min_c = (_mul_min_c),                                         \
	.rate_mul_max_c = (_mul_max_c),                                         \
	.rate_mul_shift_c = (_mul_shift_c),                                     \
	.rate_mul_mask_c = (_mul_mask_c),                                       \
	.rate_div_min = (_div_min),                                             \
	.rate_div_max = (_div_max),                                             \
	.rate_div_shift = (_div_shift),                                         \
	.rate_div_mask = (_div_mask),                                           \
	.rate_reg_off = (_reg),                                                 \
	.rate_reg_off_c = (_reg_c),                                             \
	.rate_write_enable_bit = (_bit),                                        \
	.rate_write_enable_bit_c = (_bit_c),                                    \
	.method = (_method),                                                    \
	.have_rate = (_have_rate),                                              \
	.have_rate_c = (_have_rate_c)

#define K230_MUX_FORMAT(_reg, _shift, _mask, _have_mux)                         \
	.mux_reg_off = (_reg),                                                  \
	.mux_reg_shift = (_shift),                                              \
	.mux_reg_mask = (_mask),                                                \
	.have_mux = (_have_mux)

#define K230_GATE_FORMAT_ZERO K230_GATE_FORMAT(0, 0, 0, false)
#define K230_RATE_FORMAT_ZERO K230_RATE_FORMAT(0, 0, 0, 0, 0, 0,                \
						0, 0, 0, 0, 0, 0,		\
						0, 0, 0, 0, 0, false, false)
#define K230_MUX_FORMAT_ZERO K230_MUX_FORMAT(0, 0, 0, false)

struct k230_sysclk;

/* K230 PLLs. */
enum k230_pll_id {
	K230_PLL0, K230_PLL1, K230_PLL2, K230_PLL3, K230_PLL_NUM
};

struct k230_pll {
	enum k230_pll_id id;
	struct k230_sysclk *ksc;
	void __iomem *div, *bypass, *gate, *lock;
	struct clk_hw hw;
};
#define to_k230_pll(_hw)	container_of(_hw, struct k230_pll, hw)

struct k230_pll_cfg {
	u32 reg;
	enum k230_pll_id pll_id;
	const char *name;
};

static struct k230_pll_cfg k230_pll_cfgs[] = {
	[K230_PLL0] = {
		.reg = K230_PLL0_OFFSET_BASE,
		.pll_id = K230_PLL0,
		.name = "pll0",
	},
	[K230_PLL1] = {
		.reg = K230_PLL1_OFFSET_BASE,
		.pll_id = K230_PLL1,
		.name = "pll1",
	},
	[K230_PLL2] = {
		.reg = K230_PLL2_OFFSET_BASE,
		.pll_id = K230_PLL2,
		.name = "pll2",
	},
	[K230_PLL3] = {
		.reg = K230_PLL3_OFFSET_BASE,
		.pll_id = K230_PLL3,
		.name = "pll3",
	},
};

/* K230 PLL_DIVS. */
enum k230_pll_div_id {
	K230_PLL0_DIV2,
	K230_PLL0_DIV3,
	K230_PLL0_DIV4,
	K230_PLL1_DIV2,
	K230_PLL1_DIV3,
	K230_PLL1_DIV4,
	K230_PLL2_DIV2,
	K230_PLL2_DIV3,
	K230_PLL2_DIV4,
	K230_PLL3_DIV2,
	K230_PLL3_DIV3,
	K230_PLL3_DIV4,
	K230_PLL_DIV_NUM,
};

struct k230_pll_div {
	struct k230_sysclk *ksc;
	struct clk_hw *hw;
};

struct k230_pll_div_cfg {
	const char *parent_name, *name;
	int div;
};

static struct k230_pll_div_cfg k230_pll_div_cfgs[K230_PLL_DIV_NUM] = {
	[K230_PLL0_DIV2] = { "pll0", "pll0_div2", 2},
	[K230_PLL0_DIV3] = { "pll0", "pll0_div3", 3},
	[K230_PLL0_DIV4] = { "pll0", "pll0_div4", 4},
	[K230_PLL1_DIV2] = { "pll1", "pll1_div2", 2},
	[K230_PLL1_DIV3] = { "pll1", "pll1_div3", 3},
	[K230_PLL1_DIV4] = { "pll1", "pll1_div4", 4},
	[K230_PLL2_DIV2] = { "pll2", "pll2_div2", 2},
	[K230_PLL2_DIV3] = { "pll2", "pll2_div3", 3},
	[K230_PLL2_DIV4] = { "pll2", "pll2_div4", 4},
	[K230_PLL3_DIV2] = { "pll3", "pll3_div2", 2},
	[K230_PLL3_DIV3] = { "pll3", "pll3_div3", 3},
	[K230_PLL3_DIV4] = { "pll3", "pll3_div4", 4},
};

/* K230 CLK registers offset */
#define K230_CLK_AUDIO_CLKDIV_OFFSET 0x34
#define K230_CLK_PDM_CLKDIV_OFFSET 0x40
#define K230_CLK_CODEC_ADC_MCLKDIV_OFFSET 0x38
#define K230_CLK_CODEC_DAC_MCLKDIV_OFFSET 0x3c

/* K230 CLKS. */
struct k230_clk {
	int id;
	struct k230_sysclk *ksc;
	struct clk_hw hw;
};
#define to_k230_clk(_hw)	container_of(_hw, struct k230_clk, hw)

enum k230_clk_div_type {
	K230_MUL,
	K230_DIV,
	K230_MUL_DIV,
};

enum k230_clk_parent_type {
	K230_OSC24M,
	K230_PLL,
	K230_PLL_DIV,
	K230_CLK_COMPOSITE,
};

struct k230_clk_parent {
	enum k230_clk_parent_type type;
	union {
		enum k230_pll_div_id pll_div_id;
		enum k230_pll_id pll_id;
		int clk_id;
	};
};

struct k230_clk_cfg {
	/* attr */
	const char *name;
	/* 0-read & write; 1-only read */
	bool read_only;
	struct k230_clk_parent parent1;
	struct k230_clk_parent parent2;
	int flags;

	/* rate reg */
	u32 rate_reg_off;
	u32 rate_reg_off_c;
	void __iomem *rate_reg;
	void __iomem *rate_reg_c;
	/* rate info*/
	u32 rate_write_enable_bit;
	u32 rate_write_enable_bit_c;
	enum k230_clk_div_type method;
	bool have_rate;
	bool have_rate_c;
	/* rate mul */
	u32 rate_mul_min;
	u32 rate_mul_max;
	u32 rate_mul_shift;
	u32 rate_mul_mask;
	/* rate mul-changable */
	u32 rate_mul_min_c;
	u32 rate_mul_max_c;
	u32 rate_mul_shift_c;
	u32 rate_mul_mask_c;
	/* rate div */
	u32 rate_div_min;
	u32 rate_div_max;
	u32 rate_div_shift;
	u32 rate_div_mask;
	/* gate reg */
	u32 gate_reg_off;
	void __iomem *gate_reg;
	/* gate info*/
	bool have_gate;
	u32 gate_bit_enable;
	u32 gate_bit_reverse;

	/* mux reg */
	u32 mux_reg_off;
	void __iomem *mux_reg;
	/* mux info */
	bool have_mux;
	u32 mux_reg_shift;
	u32 mux_reg_mask;
};

/*
 * SRC: K230-SDK-DTS,
 * SEE: /src/little/linux/arch/riscv/boot/dts/kendryte/clock-provider.dtsi
 */
static struct k230_clk_cfg k230_clk_cfgs[] = {
	[K230_CPU0_SRC] = {
		.name = "cpu0_src",
		.read_only = false,
		.flags = 0,
		.parent1 = {
			.type = K230_PLL_DIV,
			.pll_div_id = K230_PLL0_DIV2,
		},
		K230_RATE_FORMAT(1, 16, 0, 0,
				16, 16, 1, 0xF,
				0x0, 31, K230_MUL, 0, 0,
				0, 0, 0, 0,
				true, false),
		K230_GATE_FORMAT(0, 0, 0, true),
		K230_MUX_FORMAT_ZERO,
	},
	[K230_CPU0_ACLK] = {
		.name = "cpu0_aclk",
		.read_only = false,
		.flags = 0,
		.parent1 = {
			.type = K230_CLK_COMPOSITE,
			.clk_id = K230_CPU0_SRC,
		},
		K230_RATE_FORMAT(1, 1, 0, 0,
				1, 8, 7, 0x7,
				0x0, 31, K230_MUL, 0, 0,
				0, 0, 0, 0,
				true, false),
		K230_GATE_FORMAT_ZERO,
		K230_MUX_FORMAT_ZERO,
	},
	[K230_CPU0_PLIC] = {
		.name = "cpu0_plic",
		.read_only = false,
		.flags = 0,
		.parent1 = {
			.type = K230_CLK_COMPOSITE,
			.clk_id = K230_CPU0_SRC,
		},
		K230_RATE_FORMAT(1, 1, 0, 0,
				1, 8, 10, 0x7,
				0x0, 31, K230_DIV, 0, 0,
				0, 0, 0, 0,
				true, false),
		K230_GATE_FORMAT(0, 9, 0, true),
		K230_MUX_FORMAT_ZERO,
	},
	[K230_CPU0_NOC_DDRCP4] = {
		.name = "cpu0_noc_ddrcp4",
		.read_only = false,
		.flags = 0,
		.parent1 = {
			.type = K230_CLK_COMPOSITE,
			.clk_id = K230_CPU0_SRC,
		},
		K230_RATE_FORMAT_ZERO,
		K230_GATE_FORMAT(0x60, 7, 0, true),
		K230_MUX_FORMAT_ZERO,
	},
	[K230_CPU0_PCLK] = {
		.name = "cpu0_pclk",
		.read_only = false,
		.flags = 0,
		.parent1 = {
			.type = K230_PLL_DIV,
			.pll_div_id = K230_PLL0_DIV4,
		},
		K230_RATE_FORMAT(1, 1, 0, 0,
				1, 8, 15, 0x7,
				0x0, 31, K230_DIV, 0, 0,
				0, 0, 0, 0,
				true, false),
		K230_GATE_FORMAT(0, 13, 0, true),
		K230_MUX_FORMAT_ZERO,
	},
};
#define K230_NUM_CLKS ARRAY_SIZE(k230_clk_cfgs)

struct k230_sysclk {
	/* just for print */
	struct device_node *np;
	void __iomem			*pll_regs, *regs;
	spinlock_t			pll_lock, clk_lock;
	struct k230_pll			plls[K230_PLL_NUM];
	struct k230_clk			clks[K230_NUM_CLKS];
	struct k230_pll_div		dclks[K230_PLL_DIV_NUM];
} clksrc;

static void k230_init_pll(void __iomem *regs, enum k230_pll_id pllid,
			struct k230_pll *pll)
{
	void __iomem *base;

	pll->id = pllid;
	base = regs + k230_pll_cfgs[pllid].reg;
	pll->div = base + K230_PLL_DIV_REG_OFFSET;
	pll->bypass = base + K230_PLL_BYPASS_REG_OFFSET;
	pll->gate = base + K230_PLL_GATE_REG_OFFSET;
	pll->lock = base + K230_PLL_LOCK_REG_OFFSET;
}

static int k230_pll_prepare(struct clk_hw *hw)
{
	struct k230_pll *pll = to_k230_pll(hw);
	u32 reg;
	int ret;

	/* wait for PLL lock until it reachs lock status */
	ret = readl_poll_timeout(pll->lock, reg,
				 (reg & K230_PLL_STATUS_MASK) == K230_PLL_STATUS_MASK,
				 400, 0);
	/* this will not happen actually */
	if (ret)
		pr_err("%pOFP: PLL timeout! \n", pll->ksc->np);

	return ret;
}

static bool k230_pll_hw_is_enabled(struct k230_pll *pll)
{
	return (readl(pll->gate) & K230_PLL_GATE_ENABLE) == K230_PLL_GATE_ENABLE;
}

static void k230_pll_enable_hw(void __iomem *regs, struct k230_pll *pll)
{
	u32 reg;

	if (k230_pll_hw_is_enabled(pll))
		return;

	/* Set PLL factors */
	reg = readl(pll->gate);
	reg |= (K230_PLL_GATE_ENABLE | K230_PLL_GATE_WRITE_ENABLE);
	writel(reg, pll->gate);
}

static int k230_pll_enable(struct clk_hw *hw)
{
	struct k230_pll *pll = to_k230_pll(hw);
	struct k230_sysclk *ksc = pll->ksc;

	spin_lock(&ksc->pll_lock);
	k230_pll_enable_hw(ksc->regs, pll);
	spin_unlock(&ksc->pll_lock);

	return 0;
}

static void k230_pll_disable(struct clk_hw *hw)
{
	struct k230_pll *pll = to_k230_pll(hw);
	struct k230_sysclk *ksc = pll->ksc;
	u32 reg;

	spin_lock(&ksc->pll_lock);
	reg = readl(pll->gate);

	reg &= ~(K230_PLL_GATE_ENABLE);
	reg |= (K230_PLL_GATE_WRITE_ENABLE);

	writel(reg, pll->gate);
	spin_unlock(&ksc->pll_lock);
}

static int k230_pll_is_enabled(struct clk_hw *hw)
{
	return k230_pll_hw_is_enabled(to_k230_pll(hw));
}

static int k230_pll_init(struct clk_hw *hw)
{
	if (k230_pll_is_enabled(hw))
		return clk_prepare_enable(hw->clk);

	return 0;
}

static unsigned long k230_pll_get_rate(struct clk_hw *hw,
				       unsigned long parent_rate)
{
	struct k230_pll *pll = to_k230_pll(hw);
	u32 reg;
	u32 r, f, od;

	reg = readl(pll->bypass);
	if (reg & K230_PLL_BYPASS_ENABLE)
		return parent_rate;

	reg = readl(pll->lock);
	if (!(reg & (K230_PLL_STATUS_MASK))) { /* unlocked */
		pr_err("%pOFP: %s is unlock.", pll->ksc->np, clk_hw_get_name(hw));
		return 0;
	}
	reg = readl(pll->div);

	r = ((reg >> K230_PLL_R_SHIFT) & K230_PLL_R_MASK) + 1;
	f = ((reg >> K230_PLL_F_SHIFT) & K230_PLL_F_MASK) + 1;
	od = ((reg >> K230_PLL_OD_SHIFT) & K230_PLL_OD_MASK) + 1;

	return div_u64((u64)parent_rate * f, r * od);
}

static const struct clk_ops k230_pll_ops = {
	.init		= k230_pll_init,
	.prepare	= k230_pll_prepare,
	.enable	        = k230_pll_enable,
	.disable	= k230_pll_disable,
	.is_enabled	= k230_pll_is_enabled,
	.recalc_rate	= k230_pll_get_rate,
};

static int k230_register_pll(struct device_node *np,
				    struct k230_sysclk *ksc,
				    enum k230_pll_id pllid, const char *name,
				    int num_parents, const struct clk_ops *ops)
{
	struct k230_pll *pll = &ksc->plls[pllid];
	struct clk_init_data init = {};
	int ret;
	/* pll0-3 are son of osc24m. */
	const struct clk_parent_data parent_data[] = {
		{ .fw_name = "osc24m" },
	};

	init.name = name;
	init.parent_data = parent_data;
	init.num_parents = num_parents;
	init.ops = ops;

	pll->hw.init = &init;
	pll->ksc = ksc;

	ret = of_clk_hw_register(np, &pll->hw);
	if (ret) {
		pll->id = -1;
		goto out;
	}

out:
	return ret;
}

static int k230_register_plls(struct device_node *np, struct k230_sysclk *ksc)
{
	int i, ret;
	struct k230_pll_cfg *cfg;

	for (i = 0; i < K230_PLL_NUM; i++) {
		cfg = &k230_pll_cfgs[i];

		k230_init_pll(ksc->pll_regs, i, &ksc->plls[i]);

		ret = k230_register_pll(np, ksc, cfg->pll_id, cfg->name, 1, &k230_pll_ops);
		if (ret) {
			pr_err("%pOFP: register %s failed\n", np, cfg->name);
			goto out;
		}
	}

out:
	if (i != K230_PLL_NUM)
		for (i--; i >= 0; i--) {
			cfg = &k230_pll_cfgs[i];

			clk_hw_unregister(&ksc->plls[cfg->pll_id].hw);
			pr_warn("%pOFP: Unregistered PLL: %s\n", np, cfg->name);
		}

	return ret;
}

static void k230_register_pll_divs(struct device_node *np, struct k230_sysclk *ksc)
{
	for (int i = 0; i < K230_PLL_DIV_NUM; i++) {
		ksc->dclks[i].hw = clk_hw_register_fixed_factor(NULL, k230_pll_div_cfgs[i].name,
								k230_pll_div_cfgs[i].parent_name,
								0, 1, k230_pll_div_cfgs[i].div);
		ksc->dclks[i].ksc = ksc;
	}
}

static int k230_clk_enable(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u32 reg;

	if (!cfg->have_gate) {
		pr_err("%pOFP: This clock doesn't have gate", ksc->np);
		return -EINVAL;
	}

	spin_lock_irqsave(&ksc->clk_lock, flags);
	reg = readl(cfg->gate_reg);
	if (cfg->gate_bit_reverse)
		reg &= ~BIT(cfg->gate_bit_enable);
	else
		reg |= BIT(cfg->gate_bit_enable);
	writel(reg, cfg->gate_reg);
	spin_unlock_irqrestore(&ksc->clk_lock, flags);

	return 0;
}

static void k230_clk_disable(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u32 reg;

	if (!cfg->have_gate) {
		pr_err("%pOFP: This clock doesn't have gate", ksc->np);
		return;
	}

	spin_lock_irqsave(&ksc->clk_lock, flags);
	reg = readl(cfg->gate_reg);

	if (cfg->gate_bit_reverse)
		reg |= BIT(cfg->gate_bit_enable);
	else
		reg &= ~BIT(cfg->gate_bit_enable);

	writel(reg, cfg->gate_reg);
	spin_unlock_irqrestore(&ksc->clk_lock, flags);
}

static int k230_clk_is_enabled(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u32 reg;
	int ret;

	if (!cfg->have_gate) {
		pr_err("%pOFP: This clock doesn't have gate", ksc->np);
		return -EINVAL;
	}

	spin_lock_irqsave(&ksc->clk_lock, flags);
	reg = readl(cfg->gate_reg);

	/* Check gate bit condition based on configuration and then set ret */
	if (cfg->gate_bit_reverse)
		ret = (BIT(cfg->gate_bit_enable) & reg) ? 1 : 0;
	else
		ret = (BIT(cfg->gate_bit_enable) & ~reg) ? 1 : 0;

	spin_unlock_irqrestore(&ksc->clk_lock, flags);

	return ret;
}

static int k230_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u8 reg;

	if (!cfg->have_mux) {
		pr_err("%pOFP: This clock doesn't have mux", ksc->np);
		return -EINVAL;
	}

	spin_lock_irqsave(&ksc->clk_lock, flags);
	reg = (cfg->mux_reg_mask & index) << cfg->mux_reg_shift;
	writeb(reg, cfg->mux_reg);
	spin_unlock_irqrestore(&ksc->clk_lock, flags);

	return 0;
}

static u8 k230_clk_get_parent(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u8 reg;

	if (!cfg->have_mux) {
		pr_err("%pOFP: This clock doesn't have mux", ksc->np);
		return -EINVAL;
	}

	spin_lock_irqsave(&ksc->clk_lock, flags);
	reg = readb(cfg->mux_reg);
	spin_unlock_irqrestore(&ksc->clk_lock, flags);

	return reg;
}

static unsigned long k230_clk_get_rate(struct clk_hw *hw,
				       unsigned long parent_rate)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u32 mul, div;

	if (!cfg->have_rate) /* no divider, return parents' clk */
		return parent_rate;

	switch (cfg->method) {
	/*
	 * K230_MUL: div_mask+1/div_max...
	 * K230_DIV: mul_max/div_mask+1
	 * K230_MUL_DIV: mul_mask/div_mask...
	 */
	case K230_MUL:
		spin_lock_irqsave(&clk->ksc->clk_lock, flags);
		div = cfg->rate_div_max;
		mul = (readl(cfg->rate_reg) >> cfg->rate_div_shift)
			& cfg->rate_div_mask;
		mul++;
		spin_unlock_irqrestore(&clk->ksc->clk_lock, flags);
		break;
	case K230_DIV:
		spin_lock_irqsave(&clk->ksc->clk_lock, flags);
		mul = cfg->rate_mul_max;
		div = (readl(cfg->rate_reg) >> cfg->rate_div_shift)
			& cfg->rate_div_mask;
		div++;
		spin_unlock_irqrestore(&clk->ksc->clk_lock, flags);
		break;
	case K230_MUL_DIV:
		if (!cfg->have_rate_c) {
			spin_lock_irqsave(&clk->ksc->clk_lock, flags);
			mul = (readl(cfg->rate_reg) >> cfg->rate_mul_shift)
				& cfg->rate_mul_mask;
			div = (readl(cfg->rate_reg) >> cfg->rate_div_shift)
				& cfg->rate_div_mask;
			spin_unlock_irqrestore(&clk->ksc->clk_lock, flags);
		} else {
			mul = (readl(cfg->rate_reg_c) >> cfg->rate_mul_shift_c)
				& cfg->rate_mul_mask_c;
			div = (readl(cfg->rate_reg_c) >> cfg->rate_div_shift)
				& cfg->rate_div_mask;
		}
		break;
	default:
		return 0;
	}

	return div_u64((u64)parent_rate * mul, div);
}

static int k230_clk_find_approximate(struct k230_clk *clk,
					u32 mul_min, u32 mul_max, u32 div_min, u32 div_max,
					enum k230_clk_div_type method,
					unsigned long rate, unsigned long parent_rate,
					u32 *div, u32 *mul)
{
	long abs_min;
	long abs_current;
	/* we used integer to instead of float */
	long perfect_divide;

	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];

	uint32_t codec_clk[9] = {
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
	uint32_t codec_div[9][2] = {
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

	uint32_t pdm_clk[20] = {
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
	uint32_t pdm_div[20][2] = {
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

	switch (method) {
	/* only mul can be changeable 1/12,2/12,3/12...*/
	case K230_MUL:
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
		break;
	/* only div can be changeable, 1/1,1/2,1/3...*/
	case K230_DIV:
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
		break;
	/* mul and div can be changeable. */
	case K230_MUL_DIV:
		if (cfg->rate_reg_off == K230_CLK_CODEC_ADC_MCLKDIV_OFFSET ||
			cfg->rate_reg_off == K230_CLK_CODEC_DAC_MCLKDIV_OFFSET) {
			for (u32 j = 0; j < 9; j++) {
				if (0 == (rate - codec_clk[j])) {
					*div = codec_div[j][0];
					*mul = codec_div[j][1];
				}
			}
		} else if (cfg->rate_reg_off == K230_CLK_AUDIO_CLKDIV_OFFSET ||
			cfg->rate_reg_off == K230_CLK_PDM_CLKDIV_OFFSET) {
			for (u32 j = 0; j < 20; j++) {
				if (0 == (rate - pdm_clk[j])) {
					*div = pdm_div[j][0];
					*mul = pdm_div[j][1];
				}
			}
		} else {
			return -EINVAL;
		}
		break;

	default:
		BUG();
		return -EPERM;
	}
	return 0;
}

static long k230_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	u32 div = 0, mul = 0;

	if (k230_clk_find_approximate(clk,
				      cfg->rate_mul_min, cfg->rate_mul_max,
				      cfg->rate_div_min, cfg->rate_div_max,
				      cfg->method, rate, *parent_rate, &div, &mul)) {
		pr_err("%pOFP: [%s]: clk %s round rate error!\n", clk->ksc->np, __func__, clk_hw_get_name(hw));
		return -EINVAL;
	}

	return div_u64((u64)(*parent_rate) * mul, div);
}

static int k230_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[clk->id];
	unsigned long flags;
	u32 div = 0, mul = 0, reg = 0, reg_c = 0;

	if ((!cfg->have_rate) || (!cfg->rate_reg)) {
		pr_err("%pOFP: This clock may have no rate", clk->ksc->np);
		return -EINVAL;
	}

	if ((rate > parent_rate) || (rate == 0) || (parent_rate == 0)) {
		pr_err("%pOFP: rate or parent_rate error", clk->ksc->np);
		return -EINVAL;
	}

	if (cfg->read_only) {
		pr_err("%pOFP: This clk rate is read only", clk->ksc->np);
		return -EPERM;
	}

	if (k230_clk_find_approximate(clk,
				      cfg->rate_mul_min, cfg->rate_mul_max,
				      cfg->rate_div_min, cfg->rate_div_max,
				      cfg->method, rate, parent_rate, &div, &mul)) {
		pr_err("%pOFP: [%s]: clk %s set rate error!\n", clk->ksc->np, __func__, clk_hw_get_name(hw));
		return -EINVAL;
	}

	spin_lock_irqsave(&clk->ksc->clk_lock, flags);
	if (!cfg->have_rate_c) {
		reg = readl(cfg->rate_reg);
		reg &= ~((cfg->rate_div_mask) << (cfg->rate_div_shift));

		if (cfg->method == K230_DIV) {
			reg &= ~((cfg->rate_mul_mask) << (cfg->rate_mul_shift));
			reg |= ((div - 1) & cfg->rate_div_mask) << (cfg->rate_div_shift);
		} else if (cfg->method == K230_MUL) {
			reg |= ((mul - 1) & cfg->rate_div_mask) << (cfg->rate_div_shift);
		} else {
			reg |= (mul & cfg->rate_mul_mask) << (cfg->rate_mul_shift);
			reg |= (div & cfg->rate_div_mask) << (cfg->rate_div_shift);
		}
		reg |= BIT(cfg->rate_write_enable_bit);
	} else {
		reg = readl(cfg->rate_reg);
		reg_c = readl(cfg->rate_reg_c);
		reg &= ~((cfg->rate_div_mask) << (cfg->rate_div_shift));
		reg_c &= ~((cfg->rate_mul_mask_c) << (cfg->rate_mul_shift_c));
		reg_c |= BIT(cfg->rate_write_enable_bit_c);

		reg_c |= (mul & cfg->rate_mul_mask_c) << (cfg->rate_mul_shift_c);
		reg |= (div & cfg->rate_div_mask) << (cfg->rate_div_shift);

		writel(reg_c, cfg->rate_reg_c);
	}
	writel(reg, cfg->rate_reg);
	spin_unlock_irqrestore(&clk->ksc->clk_lock, flags);

	rate = k230_clk_get_rate(hw, parent_rate);
	return 0;
}

#define K230_CLK_OPS_GATE				\
	.enable		= k230_clk_enable,		\
	.disable	= k230_clk_disable,		\
	.is_enabled	= k230_clk_is_enabled

#define K230_CLK_OPS_RATE				\
	.set_rate    = k230_clk_set_rate,		\
	.round_rate  = k230_clk_round_rate,		\
	.recalc_rate = k230_clk_get_rate

#define K230_CLK_OPS_MUX				\
	.set_parent	 = k230_clk_set_parent,		\
	.get_parent	 = k230_clk_get_parent

#define K230_CLK_OPS_ID_NONE				0
#define K230_CLK_OPS_ID_GATE_ONLY			1
#define K230_CLK_OPS_ID_RATE_ONLY			2
#define K230_CLK_OPS_ID_RATE_GATE			3
#define K230_CLK_OPS_ID_MUX_ONLY			4
#define K230_CLK_OPS_ID_MUX_GATE			5
#define K230_CLK_OPS_ID_MUX_RATE			6
#define K230_CLK_OPS_ID_ALL				7
#define K230_CLK_OPS_ID_NUM				8

static const  struct clk_ops k230_clk_ops_arr[K230_CLK_OPS_ID_NUM] = {
	[K230_CLK_OPS_ID_NONE] = {

	},
	[K230_CLK_OPS_ID_GATE_ONLY] = {
		K230_CLK_OPS_GATE,
	},
	[K230_CLK_OPS_ID_RATE_ONLY] = {
		K230_CLK_OPS_RATE,
	},
	[K230_CLK_OPS_ID_RATE_GATE] = {
		K230_CLK_OPS_RATE,
		K230_CLK_OPS_GATE,
	},
	[K230_CLK_OPS_ID_MUX_ONLY] = {
		K230_CLK_OPS_MUX,
	},
	[K230_CLK_OPS_ID_MUX_GATE] = {
		K230_CLK_OPS_MUX,
		K230_CLK_OPS_GATE,
	},
	[K230_CLK_OPS_ID_MUX_RATE] = {
		K230_CLK_OPS_MUX,
		K230_CLK_OPS_RATE,
	},
	[K230_CLK_OPS_ID_ALL] = {
		K230_CLK_OPS_MUX,
		K230_CLK_OPS_RATE,
		K230_CLK_OPS_GATE,
	},
};

static int k230_register_clk(struct device_node *np, struct k230_sysclk *ksc,
				int id, const struct clk_parent_data *parent_data,
				u8 num_parents, unsigned long flags)
{
	struct k230_clk *clk = &ksc->clks[id];
	struct k230_clk_cfg *cfg = &k230_clk_cfgs[id];
	struct clk_init_data init = {};
	int clk_id = 0;
	int ret = 0;

	if (cfg->have_rate) {
		cfg->rate_reg = (clksrc.regs + cfg->rate_reg_off);
		clk_id += K230_CLK_OPS_ID_RATE_ONLY;
	}

	if (cfg->have_mux) {
		cfg->mux_reg = (clksrc.regs + cfg->mux_reg_off);
		clk_id += K230_CLK_OPS_ID_MUX_ONLY;

		/* mux clock doesn't match the case that num_parents less than 2 */
		if (num_parents < 2) {
			ret = -EINVAL;
			goto out;
		}
	}

	if (cfg->have_gate) {
		cfg->gate_reg = (clksrc.regs + cfg->gate_reg_off);
		clk_id += K230_CLK_OPS_ID_GATE_ONLY;
	}

	if (cfg->have_rate_c)
		cfg->rate_reg_c = (clksrc.regs + cfg->rate_reg_off_c);

	init.name = k230_clk_cfgs[id].name;
	init.flags = flags;
	init.parent_data = parent_data;
	init.num_parents = num_parents;
	init.ops = &k230_clk_ops_arr[clk_id];

	clk->id = id;
	clk->ksc = ksc;
	clk->hw.init = &init;

	ret = of_clk_hw_register(np, &clk->hw);
	if (ret) {
		pr_err("%pOFP: register clock %s failed\n", np, k230_clk_cfgs[id].name);
		clk->id = -1;
		goto out;
	}

out:
	return ret;
}

static int k230_register_mux_clk(struct device_node *np,
					struct k230_sysclk *ksc,
					struct clk_hw *hw1, struct clk_hw *hw2,
					int id)
{
	const struct clk_parent_data parent_data[] = {
		{ .hw = hw1 },
		{ .hw = hw2 }
	};
	return k230_register_clk(np, ksc, id, parent_data, 2, 0);
}

static int k230_register_osc24m_child(struct device_node *np,
				      struct k230_sysclk *ksc,
				      int id)
{
	const struct clk_parent_data parent_data = {
		/* .index = 0 for osc24m.. */
	};
	return k230_register_clk(np, ksc, id, &parent_data, 1, 0);
}

static int k230_register_pll_child(struct device_node *np,
				   struct k230_sysclk *ksc,
				   int id,
				   enum k230_pll_id pllid,
				   unsigned long flags)
{
	const struct clk_parent_data parent_data = {
		.hw = &ksc->plls[pllid].hw,
	};
	return k230_register_clk(np, ksc, id, &parent_data, 1, flags);
}

static int k230_register_pll_div_child(struct device_node *np,
				       struct k230_sysclk *ksc,
				       int id,
				       enum k230_pll_div_id pll_div_id,
				       unsigned long flags)
{
	const struct clk_parent_data parent_data = {
		.hw = ksc->dclks[pll_div_id].hw,
	};
	return k230_register_clk(np, ksc, id, &parent_data, 1, flags);
}

static int k230_register_clk_child(struct device_node *np,
				   struct k230_sysclk *ksc,
				   int id,
				   int parent_id)
{
	const struct clk_parent_data parent_data = {
		.hw = &ksc->clks[parent_id].hw,
	};
	return k230_register_clk(np, ksc, id, &parent_data, 1, 0);
}

static int _k230_clk_mux_get_hw(struct k230_sysclk *ksc,
				struct k230_clk_parent *pclk,
				struct clk_hw **hw)
{
	switch (pclk->type) {
	case K230_OSC24M:
		*hw = NULL;
		break;
	case K230_PLL:
		*hw = &ksc->plls[pclk->pll_id].hw;
		break;
	case K230_PLL_DIV:
		*hw = ksc->dclks[pclk->pll_div_id].hw;
		break;
	case K230_CLK_COMPOSITE:
		*hw = &ksc->clks[pclk->clk_id].hw;
		break;
	default:
		BUG();
		return -EPERM;
	}
	return 0;
}

static int k230_clk_mux_get_hw(struct k230_sysclk *ksc,
			       struct k230_clk_cfg *cfg,
			       struct clk_hw **hw1,
			       struct clk_hw **hw2)
{
	int ret = 0;
	struct k230_clk_parent *pclk1, *pclk2;

	pclk1 = &cfg->parent1;
	pclk2 = &cfg->parent2;

	ret = _k230_clk_mux_get_hw(ksc, pclk1, hw1);
	if (ret)
		goto out;

	ret = _k230_clk_mux_get_hw(ksc, pclk2, hw2);
	if (ret)
		goto out;
out:
	return ret;
}

static int k230_register_clks(struct device_node *np, struct k230_sysclk *ksc)
{
	struct k230_clk_cfg *cfg;
	struct k230_clk_parent *pclk;
	struct clk_hw *hw1, *hw2;
	int ret, i;

	/*
	 *  pll0_div2 sons: CPU0_SRC
	 *  pll0_div4 sons: CPU0_PCLK
	 *  CPU0_SRC sons: CPU0_ACLK, CPU0_PLIC, CPU0_NOC_DDRCP4
	 */
	for (i = 0; i < K230_NUM_CLKS; i++) {
		cfg = &k230_clk_cfgs[i];
		if (cfg->have_mux) {
			ret = k230_clk_mux_get_hw(ksc, cfg, &hw1, &hw2);
			if (ret)
				goto out;

			ret = k230_register_mux_clk(np, ksc, hw1, hw2, i);
			if (ret)
				goto out;
		} else {
			pclk = &cfg->parent1;
			switch (pclk->type) {
			case K230_OSC24M:
				ret = k230_register_osc24m_child(np, ksc, i);
				break;
			case K230_PLL:
				ret = k230_register_pll_child(np, ksc, i,
						pclk->pll_id, cfg->flags);
				break;
			case K230_PLL_DIV:
				ret = k230_register_pll_div_child(np, ksc, i,
						pclk->pll_div_id, cfg->flags);
				break;
			case K230_CLK_COMPOSITE:
				ret = k230_register_clk_child(np, ksc, i, pclk->clk_id);
				break;
			default:
				pr_err("%pOFP: Invalid type", np);
				ret = -EINVAL;
				goto out;
			}
		}
		if (ret) {
			pr_err("%pOFP: register child id %d failed", np, i);
			goto out;
		}
	}

out:
	if (i != K230_NUM_CLKS)
		for (i--; i >= 0; i--) {
			cfg = &k230_clk_cfgs[i];

			clk_hw_unregister(&ksc->clks[i].hw);
			pr_warn("%pOFP: Unregistered clk: %s\n", np, cfg->name);
		}
	return ret;
}

static struct clk_hw *k230_clk_hw_onecell_get(struct of_phandle_args *clkspec, void *data)
{
	struct k230_sysclk *ksc;
	unsigned int idx;

	if (clkspec->args_count != 1)
		return ERR_PTR(-EINVAL);
	idx = clkspec->args[0];

	if (!data)
		return ERR_PTR(-EINVAL);

	ksc = (struct k230_sysclk *)data;

	if (idx >= K230_NUM_CLKS)
		return ERR_PTR(-EINVAL);

	return &ksc->clks[idx].hw;
}

static int k230_clk_init_plls(struct device_node *np)
{
	struct k230_sysclk *ksc = &clksrc;
	int ret = 0;

	if (!np) {
		pr_err("%pOFP: device node pointer is NULL", np);
		return -EINVAL;
	}

	spin_lock_init(&ksc->pll_lock);

	ksc->pll_regs = of_iomap(np, 0);
	if (!ksc->pll_regs) {
		pr_err("%pOFP: failed to map registers\n", np);
		return -ENOMEM;
	}

	ret = k230_register_plls(np, ksc);
	if (ret) {
		pr_err("%pOFP: register plls failed %d", np, ret);
		goto out_unmap;
	}

	k230_register_pll_divs(np, ksc);

out_unmap:
	if (ret)
		iounmap(ksc->pll_regs);
	return ret;
}

static int k230_clk_init_sysclk(struct device_node *np)
{
	struct k230_sysclk *ksc = &clksrc;
	int ret = 0;

	if (!np) {
		pr_err("%pOFP: device node pointer is NULL", np);
		return -EINVAL;
	}

	spin_lock_init(&ksc->clk_lock);

	ksc->regs = of_iomap(np, 1);
	if (!ksc->regs) {
		pr_err("%pOFP: failed to map registers\n", np);
		return -ENOMEM;
	}

	ret = k230_register_clks(np, ksc);
	if (ret) {
		pr_err("%pOFP: register clock provider failed %d", np, ret);
		goto out_unmap;
	}

	ret = of_clk_add_hw_provider(np, k230_clk_hw_onecell_get, ksc);
	if (ret)
		pr_err("%pOFP: add clock provider failed %d\n", np, ret);

out_unmap:
	if (ret)
		iounmap(ksc->regs);
	return ret;
}

static void __init k230_clk_init_clks(struct device_node *np)
{
	struct k230_sysclk *ksc = &clksrc;
	int ret = 0;

	ksc->np = np;

	ret = k230_clk_init_plls(np);
	if (ret) {
		pr_err("%pOFP: init clk plls failed %d", np, ret);
		return;
	}
	pr_info("%pOFP: success to init plls", np);

	ret = k230_clk_init_sysclk(np);
	if (ret) {
		pr_err("%pOFP: init clk clks failed %d", np, ret);
		return;
	}
	pr_info("%pOFP: success to init clks", np);
}
CLK_OF_DECLARE_DRIVER(k230_clk, "canaan,k230-clk", k230_clk_init_clks);
