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

#define K230_CLK_OPS_ID_NONE			0
#define K230_CLK_OPS_ID_GATE_ONLY		1
#define K230_CLK_OPS_ID_RATE_ONLY		2
#define K230_CLK_OPS_ID_RATE_GATE		3
#define K230_CLK_OPS_ID_MUX_ONLY		4
#define K230_CLK_OPS_ID_MUX_GATE		5
#define K230_CLK_OPS_ID_MUX_RATE		6
#define K230_CLK_OPS_ID_ALL			7
#define K230_CLK_OPS_ID_NUM			8

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

#define K230_CLK_OPS_GATE							\
	.enable		= k230_clk_enable,					\
	.disable	= k230_clk_disable,					\
	.is_enabled	= k230_clk_is_enabled

#define K230_CLK_OPS_RATE							\
	.set_rate	= k230_clk_set_rate,					\
	.round_rate	= k230_clk_round_rate,					\
	.recalc_rate	= k230_clk_get_rate

#define K230_CLK_OPS_MUX							\
	.set_parent	= k230_clk_set_parent,					\
	.get_parent	= k230_clk_get_parent,					\
	.determine_rate	= clk_hw_determine_rate_no_reparent

#define K230_GATE_FORMAT(_reg, _bit, _reverse)					\
	.gate_reg_off = (_reg),							\
	.gate_bit_enable = (_bit),						\
	.gate_bit_reverse = (_reverse)

#define K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,		\
			_div_min, _div_max, _div_shift, _div_mask,		\
			_reg, _bit, _method)					\
	.rate_mul_min = (_mul_min),						\
	.rate_mul_max = (_mul_max),						\
	.rate_mul_shift = (_mul_shift),						\
	.rate_mul_mask = (_mul_mask),						\
	.rate_div_min = (_div_min),						\
	.rate_div_max = (_div_max),						\
	.rate_div_shift = (_div_shift),						\
	.rate_div_mask = (_div_mask),						\
	.rate_reg_off = (_reg),							\
	.rate_write_enable_bit = (_bit),					\
	.method = (_method)

#define K230_RATE_C_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,		\
			   _reg, _bit)						\
	.rate_mul_min_c = (_mul_min),						\
	.rate_mul_max_c = (_mul_max),						\
	.rate_mul_shift_c = (_mul_shift),					\
	.rate_mul_mask_c = (_mul_mask),						\
	.rate_reg_off_c = (_reg),						\
	.rate_write_enable_bit_c = (_bit)

#define K230_MUX_FORMAT(_reg, _shift, _mask)					\
	.mux_reg_off = (_reg),							\
	.mux_reg_shift = (_shift),						\
	.mux_reg_mask = (_mask)

#define K230_PLL_DIV_FORMAT(_parent_name, _name, _div)				\
{										\
	.parent_name = _parent_name,						\
	.name = _name,								\
	.div = _div,								\
}

#define K230_CLK_CFG_FORMAT(_name, _read_only, _flags,				\
			    _rate_cfg, _rate_cfg_c,				\
			    _gate_cfg, _mux_cfg)				\
	.name = (_name),							\
	.read_only = (_read_only),						\
	.flags = (_flags),							\
	.rate_cfg = (_rate_cfg),						\
	.rate_cfg_c = (_rate_cfg_c),						\
	.gate_cfg = (_gate_cfg),						\
	.mux_cfg = (_mux_cfg)

#define K230_CLK_FORMAT_C(_var,							\
			  _mul_min, _mul_max, _mul_shift, _mul_mask,		\
			  _div_min, _div_max, _div_shift, _div_mask,		\
			  _reg, _bit, _method,					\
			  cmul_min, cmul_max, cmul_shift, cmul_mask,		\
			  _creg, _cbit,						\
			  greg, gbit, _reverse,					\
			  _read_only, _flags,					\
			  _type, _clk)						\
	static struct k230_clk_rate_cfg k230_##_var##_rate = {			\
		K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,	\
				 _div_min, _div_max, _div_shift, _div_mask,	\
				 _reg, _bit, _method)				\
	};									\
	static struct k230_clk_rate_cfg_c k230_##_var##_rate_c = {		\
		K230_RATE_C_FORMAT(cmul_min, cmul_max, cmul_shift, cmul_mask,	\
				   _creg, _cbit)				\
	};									\
	static struct k230_clk_gate_cfg k230_##_var##_gate = {			\
		K230_GATE_FORMAT(greg, gbit, _reverse)				\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    &k230_##_var##_rate, &k230_##_var##_rate_c,	\
				    &k230_##_var##_gate, NULL),			\
		.parent[0] = {							\
			.type = _type,						\
			.ptr = _clk,						\
		},								\
		.num_parent = 1,						\
	}

#define K230_CLK_FORMAT(_var,							\
			_mul_min, _mul_max, _mul_shift, _mul_mask,		\
			_div_min, _div_max, _div_shift, _div_mask,		\
			_reg, _bit, _method,					\
			greg, gbit, _reverse,					\
			mreg, _mux_shift, _mask,				\
			_read_only, _flags,					\
			count, ...)						\
	_K230_CLK_FORMAT##count(_var,						\
				_mul_min, _mul_max, _mul_shift, _mul_mask,	\
				_div_min, _div_max, _div_shift, _div_mask,	\
				_reg, _bit, _method,				\
				greg, gbit, _reverse,				\
				mreg, _mux_shift, _mask,			\
				_read_only, _flags,				\
				__VA_ARGS__)

#define _K230_CLK_FORMAT2(_var,							\
			  _mul_min, _mul_max, _mul_shift, _mul_mask,		\
			  _div_min, _div_max, _div_shift, _div_mask,		\
			  _reg, _bit, _method,					\
			  greg, gbit, _reverse,					\
			  mreg, _mux_shift, _mask,				\
			  _read_only, _flags,					\
			  type1, clk1, type2, clk2)				\
	static struct k230_clk_rate_cfg k230_##_var##_rate = {			\
		K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,	\
				 _div_min, _div_max, _div_shift, _div_mask,	\
				 _reg, _bit, _method)				\
	};									\
	static struct k230_clk_gate_cfg k230_##_var##_gate = {			\
		K230_GATE_FORMAT(greg, gbit, _reverse)				\
	};									\
	static struct k230_clk_mux_cfg k230_##_var##_mux = {			\
		K230_MUX_FORMAT(mreg, _mux_shift, _mask)			\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    &k230_##_var##_rate, NULL,			\
				    &k230_##_var##_gate, &k230_##_var##_mux),	\
		.num_parent = 2,						\
		.parent[0] = {							\
			.type = type1,						\
			.ptr = clk1,						\
		},								\
		.parent[0] = {							\
			.type = type2,						\
			.ptr = clk2,						\
		},								\
	}

#define _K230_CLK_FORMAT3(_var,							\
			  _mul_min, _mul_max, _mul_shift, _mul_mask,		\
			  _div_min, _div_max, _div_shift, _div_mask,		\
			  _reg, _bit, _method,					\
			  greg, gbit, _reverse,					\
			  mreg, _mux_shift, _mask,				\
			  _read_only, _flags,					\
			  type1, clk1, type2, clk2, type3, clk3)		\
	static struct k230_clk_rate_cfg k230_##_var##_rate = {			\
		K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,	\
				 _div_min, _div_max, _div_shift, _div_mask,	\
				 _reg, _bit, _method)				\
	};									\
	static struct k230_clk_gate_cfg k230_##_var##_gate = {			\
		K230_GATE_FORMAT(greg, gbit, _reverse)				\
	};									\
	static struct k230_clk_mux_cfg k230_##_var##_mux = {			\
		K230_MUX_FORMAT(mreg, _mux_shift, _mask)			\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    &k230_##_var##_rate, NULL,			\
				    &k230_##_var##_gate, &k230_##_var##_mux),	\
		.num_parent = 3,						\
		.parent[0] = {							\
			.type = type1,						\
			.ptr = clk1,						\
		},								\
		.parent[0] = {							\
			.type = type2,						\
			.ptr = clk2,						\
		},								\
		.parent[0] = {							\
			.type = type3,						\
			.ptr = clk3,						\
		},								\
	}

#define K230_CLK_RATE_FORMAT(_var,						\
			     _mul_min, _mul_max, _mul_shift, _mul_mask,		\
			     _div_min, _div_max, _div_shift, _div_mask,		\
			     _reg, _bit, _method,				\
			     _read_only, _flags,				\
			     _type, _clk)					\
	static struct k230_clk_rate_cfg k230_##_var##_rate = {			\
		K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,	\
				 _div_min, _div_max, _div_shift, _div_mask,	\
				 _reg, _bit, _method)				\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    &k230_##_var##_rate, NULL,			\
				    NULL, NULL),				\
		.parent[0] = {							\
			.type = _type,						\
			.ptr = _clk,						\
		},								\
		.num_parent = 1,						\
	}

#define K230_CLK_GATE_FORMAT(_var,						\
			     greg, gbit, _reverse,				\
			     _read_only, _flags,				\
			     _type, _clk)					\
	static struct k230_clk_gate_cfg k230_##_var##_gate = {			\
		K230_GATE_FORMAT(greg, gbit, _reverse)				\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    NULL, NULL,					\
				    &k230_##_var##_gate, NULL),			\
		.parent[0] = {							\
			.type = _type,						\
			.ptr = _clk,						\
		},								\
		.num_parent = 1,						\
	}

#define K230_CLK_RATE_GATE_FORMAT(_var,						\
				  _mul_min, _mul_max, _mul_shift, _mul_mask,	\
				  _div_min, _div_max, _div_shift, _div_mask,	\
				  _reg, _bit, _method,				\
				  greg, gbit, _reverse,				\
				  _read_only, _flags,				\
				  _type, _clk)					\
	static struct k230_clk_rate_cfg k230_##_var##_rate = {			\
		K230_RATE_FORMAT(_mul_min, _mul_max, _mul_shift, _mul_mask,	\
				 _div_min, _div_max, _div_shift, _div_mask,	\
				 _reg, _bit, _method)				\
	};									\
	static struct k230_clk_gate_cfg k230_##_var##_gate = {			\
		K230_GATE_FORMAT(greg, gbit, _reverse)				\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    &k230_##_var##_rate, NULL,			\
				    &k230_##_var##_gate, NULL),			\
		.parent[0] = {							\
			.type = _type,						\
			.ptr = _clk,						\
		},								\
		.num_parent = 1,						\
	}

#define K230_CLK_GATE_MUX_FORMAT(_var,						\
			greg, gbit, _reverse,					\
			mreg, _mux_shift, _mask,				\
			_read_only, _flags,					\
			count, ...)						\
	_K230_CLK_GATE_MUX_FORMAT##count(_var,					\
			greg, gbit, _reverse,					\
			mreg, _mux_shift, _mask,				\
			_read_only, _flags,					\
			__VA_ARGS__)

#define _K230_CLK_GATE_MUX_FORMAT2(_var,					\
				   greg, gbit, _reverse,			\
				   mreg, _mux_shift, _mask,			\
				   _read_only, _flags,				\
				   type0, clk0, type1, clk1)			\
	static struct k230_clk_gate_cfg k230_##_var##_gate = {			\
		K230_GATE_FORMAT(greg, gbit, _reverse)				\
	};									\
	static struct k230_clk_mux_cfg k230_##_var##_mux = {			\
		K230_MUX_FORMAT(mreg, _mux_shift, _mask)			\
	};									\
	static struct k230_clk k230_##_var = {					\
		K230_CLK_CFG_FORMAT(#_var, _read_only, _flags,			\
				    NULL, NULL,					\
				    &k230_##_var##_gate, &k230_##_var##_mux),	\
		.num_parent = 2,						\
		.parent[0] = {							\
			.type = type0,						\
			.ptr = clk0,						\
		},								\
		.parent[1] = {							\
			.type = type1,						\
			.ptr = clk1,						\
		},								\
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

enum k230_clk_div_type {
	K230_MUL,
	K230_DIV,
	K230_MUL_DIV,
};

struct k230_clk_rate_cfg {
	u32 rate_reg_off;
	u32 rate_write_enable_bit;
	enum k230_clk_div_type method;
	u32 rate_mul_min;
	u32 rate_mul_max;
	u32 rate_mul_shift;
	u32 rate_mul_mask;
	u32 rate_div_min;
	u32 rate_div_max;
	u32 rate_div_shift;
	u32 rate_div_mask;
};

struct k230_clk_rate_cfg_c {
	u32 rate_reg_off_c;
	u32 rate_write_enable_bit_c;
	u32 rate_mul_min_c;
	u32 rate_mul_max_c;
	u32 rate_mul_shift_c;
	u32 rate_mul_mask_c;
};

struct k230_clk_gate_cfg {
	u32 gate_reg_off;
	u32 gate_bit_enable;
	bool gate_bit_reverse;
};

struct k230_clk_mux_cfg {
	u32 mux_reg_off;
	u32 mux_reg_shift;
	u32 mux_reg_mask;
};

enum k230_clk_parent_type {
	K230_OSC24M,
	K230_SYSCTL_APB_SRC,
	K230_SHRM_SRAM_DIV2,
	K230_TIMERX_PULSE_IN,
	K230_PLL,
	K230_PLL_DIV,
	K230_CLK_COMPOSITE,
};

struct k230_clk;

struct k230_clk_parent {
	enum k230_clk_parent_type type;
	union {
		struct k230_pll		*pll;
		struct k230_pll_div	*pll_div;
		struct k230_clk		*clk;
		void			*ptr;
	};
};

struct k230_clk {
	const char *name;
	bool read_only;
	int num_parent;
	struct k230_clk_parent parent[K230_CLK_MAX_PARENT_NUM];
	struct k230_sysclk *ksc;
	struct clk_hw hw;
	int flags;
	struct k230_clk_rate_cfg	*rate_cfg;
	struct k230_clk_rate_cfg_c	*rate_cfg_c;
	struct k230_clk_gate_cfg	*gate_cfg;
	struct k230_clk_mux_cfg		*mux_cfg;
};

#define to_k230_clk(_hw)	container_of(_hw, struct k230_clk, hw)

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

/*
 * Single parent clock:
 * osc24m     child: pmu_apb, hs_sd_timer_src, ls_gpio_debounce,
 *                   sysctl_temp_sensor, sysctl_wdtX, display_ref
 * sysctl_apb_src child:
 *                   sysctl_wdtX_apb,sysctl_timer_apb, sysctl_iomux_apb,
 *                   sysctl_mailbox_apb
 * shrm_sram_div2 child:
 *                   shrm_axi_slave
 * pll0       child: dphy_dft
 * pll1       child: usb_480m
 * pll0_div2  child: cpu0_src, vpu_src
 * pll0_div3  child: display_clkext, display_gpu
 * pll0_div4  child: cpu0_apb, cpu1_apb, hs_hclk_high_src, hs_ssi0_axi,
 *                   hs_ss1, hs_ssi2, hs_qspi_axi_src, hs_sd_card_src,
 *                   ls_apb_src, ls_codec_apb, ls_i2c0, ls_i2c1,
 *                   ls_i2c2, ls_i2c3, ls_i2c4, ls_codec_adc, ls_codec_dac,
 *                   ls_audio_dev, ls_pdm, ls_adc, sysctl_hdi, shrm_apb,
 *                   shrm_axi_src, ddrc_apb, display_ahb, display_axi,
 *                   vpu_cfg, sec_apb, usb_100m, spi2axi
 * pll0_div16 child: hs_usb_ref_50m, ls_uartX, ls_jamlinkco_div_src,
 *                   timerX_src,
 * pll1_div4  child: sysctl_time_stamp, display_dpip, display_cfg, sec_fix,
 *                   sec_axi
 * pll2_div4  child: hs_sd_axi_src, ddrc_bypass
 * cpu0_src   child: cpu0_axi, cpu0_plic, cpu0_noc_ddrcp4
 * cpu1_src   child: cpu1_axi, cpu1_src
 * shrm_sram  child: shrm_decompress_axi
 * vpu_src    child: vpu_axi_src
 * ai_src     child: ai_axi
 * vpu_axi_src child:
 *                   vpu_axi, vpu_ddrcp2
 * shrm_axi_src child:
 *                   shrm_nonai2d_axi, shrm_sdma_axi,shrm_pdma_axi
 * hs_hclk_high_src child:
 *                   hs_hclk_high, hs_hclk_src
 * hs_hclk_src child:
 *                   hs_sdX_ahb, hs_ssiX_ahb, hs_usbX_ahb
 * hs_qspi_axi_src child:
 *                   hs_ssiX_axi
 * hs_sd_card_src child:
 *                   hs_sdX_card
 * hs_sd_axi_src child:
 *                   hs_sdX_axi, hs_sdX_base
 * hs_sd_timer_src child:
 *                   hs_sdX_timer
 * ls_apb_src child: ls_uartX_apb, ls_i2cX_apb, ls_gpio_apb, ls_jamlinkX_apb,
 *                   ls_audio_apb, ls_adc_apb, ls_codec_apb
 * ls_jamlinkco_div_src child:
 *                   ls_jamlinkXco
 *
 * Mux clock:
 * hs_ospi_src parents: pll0_div2, pll2_div4
 * hs_usbX_ref parents: osc24m, hs_usb_ref_50m
 * timerX      parents: timerX_pulse_in, timerX_src
 * shrm_sram   parents: pll3_div2, pll0_div2
 * cpu1_src    parents: pll0_div2, pll3, pll0
 * ddrc_src    parents: pll0_div2, pll0_div3, pll2_div4
 * ai_src      parents: pll0_div2, pll3_div2
 * cameraX     parents: pll1_div3, pll1_div4, pll0_div4
 */
K230_CLK_RATE_GATE_FORMAT(cpu0_src,
			  1, 16, 0, 0,
			  16, 16, 1, 0xf,
			  0x0, 31, K230_MUL,
			  0, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2]);

K230_CLK_RATE_FORMAT(cpu0_axi,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x0, 31, K230_DIV,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(cpu0_src));

K230_CLK_RATE_GATE_FORMAT(cpu0_plic,
			  1, 1, 0, 0,
			  1, 8, 10, 0x7,
			  0x0, 31, K230_DIV,
			  0x0, 9, false,
			  false, 0,
			  K230_CLK_COMPOSITE, K230_FMT(cpu0_src));

K230_CLK_GATE_FORMAT(cpu0_noc_ddrcp4,
		     0x60, 7, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(cpu0_src));

K230_CLK_RATE_GATE_FORMAT(cpu0_apb,
			  1, 1, 0, 0,
			  1, 8, 15, 0x7,
			  0x0, 31, K230_DIV,
			  0x0, 13, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_FORMAT(cpu1_src,
		1, 1, 0, 0,
		1, 8, 3, 0x7,
		0x4, 31, K230_DIV,
		0x4, 0, false,
		0x4, 1, 0x3,
		false, 0,
		3,
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2],
		K230_PLL, &k230_plls[K230_PLL3],
		K230_PLL, &k230_plls[K230_PLL0]);

K230_CLK_RATE_FORMAT(cpu1_axi,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x4, 31, K230_DIV,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(cpu1_src));

K230_CLK_RATE_GATE_FORMAT(cpu1_plic,
			  1, 1, 0, 0,
			  1, 8, 16, 0x7,
			  0x4, 31, K230_DIV,
			  0x4, 15, false,
			  false, 0,
			  K230_CLK_COMPOSITE, K230_FMT(cpu1_src));

K230_CLK_RATE_GATE_FORMAT(cpu1_apb,
			  1, 1, 0, 0,
			  1, 8, 15, 0x7,
			  0x0, 31, K230_DIV,
			  0x4, 19, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(pmu_apb,
		     0x10, 0, false,
		     false, 0,
		     K230_OSC24M, NULL);

K230_CLK_RATE_FORMAT(hs_hclk_high_src,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x1C, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(hs_hclk_high,
		     0x18, 1, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_high_src));

K230_CLK_RATE_GATE_FORMAT(hs_hclk_src,
			  1, 1, 0, 0,
			  1, 8, 3, 0x7,
			  0x1C, 31, K230_DIV,
			  0x18, 1, false,
			  false, 0,
			  K230_CLK_COMPOSITE, K230_FMT(hs_hclk_high_src));

K230_CLK_GATE_FORMAT(hs_sd0_ahb,
		     0x18, 2, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_src));

K230_CLK_GATE_FORMAT(hs_sd1_ahb,
		     0x18, 3, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_src));

K230_CLK_GATE_FORMAT(hs_ssi1_ahb,
		     0x18, 7, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_src));

K230_CLK_GATE_FORMAT(hs_ssi2_ahb,
		     0x18, 8, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_src));

K230_CLK_GATE_FORMAT(hs_usb0_ahb,
		     0x18, 4, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_src));

K230_CLK_GATE_FORMAT(hs_usb1_ahb,
		     0x18, 5, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_hclk_src));

K230_CLK_RATE_GATE_FORMAT(hs_ssi0_axi,
			  1, 1, 0, 0,
			  1, 8, 9, 0x7,
			  0x20, 31, K230_DIV,
			  0x18, 27, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(hs_ssi1,
			  1, 1, 0, 0,
			  1, 8, 3, 0x7,
			  0x20, 31, K230_DIV,
			  0x18, 25, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(hs_ssi2,
			  1, 1, 0, 0,
			  1, 8, 6, 0x7,
			  0x20, 31, K230_DIV,
			  0x18, 26, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(hs_qspi_axi_src,
			  1, 1, 0, 0,
			  1, 8, 12, 0x7,
			  0x20, 31, K230_DIV,
			  0x18, 28, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(hs_ssi1_axi,
		     0x18, 29, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_qspi_axi_src));

K230_CLK_GATE_FORMAT(hs_ssi2_axi,
		     0x18, 30, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_qspi_axi_src));

K230_CLK_RATE_GATE_FORMAT(hs_sd_card_src,
			  1, 1, 0, 0,
			  2, 8, 12, 0x7,
			  0x1C, 31, K230_DIV,
			  0x18, 11, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(hs_sd0_card,
		     0x18, 15, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_card_src));

K230_CLK_GATE_FORMAT(hs_sd1_card,
		     0x18, 19, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_card_src));

K230_CLK_RATE_GATE_FORMAT(hs_sd_axi_src,
			  1, 1, 0, 0,
			  1, 8, 6, 0x7,
			  0x1C, 31, K230_DIV,
			  0x18, 9, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL2_DIV4]);

K230_CLK_GATE_FORMAT(hs_sd0_axi,
		     0x18, 13, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_axi_src));

K230_CLK_GATE_FORMAT(hs_sd1_axi,
		     0x18, 17, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_axi_src));

K230_CLK_GATE_FORMAT(hs_sd0_base,
		     0x18, 14, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_axi_src));

K230_CLK_GATE_FORMAT(hs_sd1_base,
		     0x18, 18, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_axi_src));

K230_CLK_GATE_MUX_FORMAT(hs_ospi_src,
			 0x18, 24, false,
			 0x20, 18, 0x1,
			 false, 0,
			 2,
			 K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2],
			 K230_PLL_DIV, &k230_pll_divs[K230_PLL2_DIV4]);

K230_CLK_RATE_FORMAT(hs_usb_ref_50m,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x20, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_GATE_FORMAT(hs_sd_timer_src,
			  1, 1, 0, 0,
			  24, 32, 15, 0x1F,
			  0x1C, 31, K230_DIV,
			  0x18, 12, false,
			  false, 0,
			  K230_OSC24M, NULL);

K230_CLK_GATE_FORMAT(hs_sd0_timer,
		     0x18, 16, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_timer_src));

K230_CLK_GATE_FORMAT(hs_sd1_timer,
		     0x18, 20, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(hs_sd_timer_src));

K230_CLK_GATE_MUX_FORMAT(hs_usb0_ref,
			 0x18, 21, false,
			 0x18, 23, 0x1,
			 false, 0,
			 2,
			 K230_OSC24M, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(hs_usb_ref_50m));

K230_CLK_GATE_MUX_FORMAT(hs_usb1_ref,
			 0x18, 22, false,
			 0x18, 23, 0x1,
			 false, 0,
			 2,
			 K230_OSC24M, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(hs_usb_ref_50m));

K230_CLK_RATE_GATE_FORMAT(ls_apb_src,
			  1, 1, 0, 0,
			  1, 8, 0, 0x7,
			  0x30, 31, K230_DIV,
			  0x24, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(ls_uart0_apb,
		     0x24, 1, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_uart1_apb,
		     0x24, 2, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_uart2_apb,
		     0x24, 3, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_uart3_apb,
		     0x24, 4, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_uart4_apb,
		     0x24, 5, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_i2c0_apb,
		     0x24, 6, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_i2c1_apb,
		     0x24, 7, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_i2c2_apb,
		     0x24, 8, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_i2c3_apb,
		     0x24, 9, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_i2c4_apb,
		     0x24, 10, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_gpio_apb,
		     0x24, 11, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_pwm_apb,
		     0x24, 12, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_jamlink0_apb,
		     0x28, 4, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_jamlink1_apb,
		     0x28, 5, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_jamlink2_apb,
		     0x28, 6, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_jamlink3_apb,
		     0x28, 7, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_audio_apb,
		     0x24, 13, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_adc_apb,
		     0x24, 15, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_apb_src));

K230_CLK_GATE_FORMAT(ls_codec_apb,
		     0x24, 14, false,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_i2c0,
			  1, 1, 0, 0,
			  1, 8, 15, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 21, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_i2c1,
			  1, 1, 0, 0,
			  1, 8, 18, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 22, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_i2c2,
			  1, 1, 0, 0,
			  1, 8, 21, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 23, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_i2c3,
			  1, 1, 0, 0,
			  1, 8, 24, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 24, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_i2c4,
			  1, 1, 0, 0,
			  1, 8, 27, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 25, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_codec_adc,
			  0x10, 0x1B9, 14, 0x1FFF,
			  0xC35, 0x3D09, 0, 0x3FFF,
			  0x38, 31, K230_MUL_DIV,
			  0x24, 29, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_codec_dac,
			  0x10, 0x1B9, 14, 0x1FFF,
			  0xC35, 0x3D09, 0, 0x3FFF,
			  0x3C, 31, K230_MUL_DIV,
			  0x24, 30, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_audio_dev,
			  0x4, 0x1B9, 16, 0x7FFF,
			  0xC35, 0xF424, 0, 0xFFFF,
			  0x34, 31, K230_MUL_DIV,
			  0x24, 28, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_FORMAT_C(ls_pdm,
		  0, 0, 0, 0,
		  0xC35, 0x1E848, 0, 0x1FFFF,
		  0x40, 0, K230_MUL_DIV,
		  0x2, 0x1B9, 0, 0xFFFF,
		  0x44, 31,
		  0x24, 31, false,
		  false, 0,
		  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_adc,
			  1, 1, 0, 0,
			  1, 1024, 3, 0x3FF,
			  0x30, 31, K230_DIV,
			  0x24, 26, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ls_uart0,
			  1, 1, 0, 0,
			  1, 8, 0, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 16, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_GATE_FORMAT(ls_uart1,
			  1, 1, 0, 0,
			  1, 8, 3, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 17, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_GATE_FORMAT(ls_uart2,
			  1, 1, 0, 0,
			  1, 8, 6, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 18, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_GATE_FORMAT(ls_uart3,
			  1, 1, 0, 0,
			  1, 8, 9, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 19, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_GATE_FORMAT(ls_uart4,
			  1, 1, 0, 0,
			  1, 8, 12, 0x7,
			  0x2C, 31, K230_DIV,
			  0x24, 20, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_FORMAT(ls_jamlinkco_div_src,
		     1, 1, 0, 0,
		     2, 512, 23, 0xFF,
		     0x30, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_GATE_FORMAT(ls_jamlink0co,
		     0x28, 0, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_jamlinkco_div_src));

K230_CLK_GATE_FORMAT(ls_jamlink1co,
		     0x28, 1, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_jamlinkco_div_src));

K230_CLK_GATE_FORMAT(ls_jamlink2co,
		     0x28, 2, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_jamlinkco_div_src));

K230_CLK_GATE_FORMAT(ls_jamlink3co,
		     0x28, 3, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ls_jamlinkco_div_src));

K230_CLK_RATE_GATE_FORMAT(ls_gpio_debounce,
			  1, 1, 0, 0,
			  1, 1024, 13, 0x3FF,
			  0x30, 31, K230_DIV,
			  0x24, 27, false,
			  false, 0,
			  K230_OSC24M, NULL);

K230_CLK_GATE_FORMAT(sysctl_wdt0_apb,
		     0x50, 1, false,
		     false, 0,
		     K230_SYSCTL_APB_SRC, NULL);

K230_CLK_GATE_FORMAT(sysctl_wdt1_apb,
		     0x50, 2, false,
		     false, 0,
		     K230_SYSCTL_APB_SRC, NULL);

K230_CLK_GATE_FORMAT(sysctl_timer_apb,
		     0x50, 3, false,
		     false, 0,
		     K230_SYSCTL_APB_SRC, NULL);

K230_CLK_GATE_FORMAT(sysctl_iomux_apb,
		     0x50, 20, false,
		     false, 0,
		     K230_SYSCTL_APB_SRC, NULL);

K230_CLK_GATE_FORMAT(sysctl_mailbox_apb,
		     0x50, 4, false,
		     false, 0,
		     K230_SYSCTL_APB_SRC, NULL);

K230_CLK_RATE_GATE_FORMAT(sysctl_hdi,
			  1, 1, 0, 0,
			  1, 8, 28, 0x7,
			  0x58, 31, K230_DIV,
			  0x50, 21, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(sysctl_time_stamp,
			  1, 1, 0, 0,
			  1, 32, 15, 0x1F,
			  0x58, 31, K230_DIV,
			  0x50, 19, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4]);

K230_CLK_RATE_FORMAT(sysctl_temp_sensor,
		     1, 1, 0, 0,
		     1, 256, 20, 0xFF,
		     0x58, 31, K230_DIV,
		     false, 0,
		     K230_OSC24M, NULL);

K230_CLK_RATE_GATE_FORMAT(sysctl_wdt0,
			  1, 1, 0, 0,
			  1, 64, 3, 0x3F,
			  0x58, 31, K230_DIV,
			  0x50, 4, false,
			  false, 0,
			  K230_OSC24M, NULL);

K230_CLK_RATE_GATE_FORMAT(sysctl_wdt1,
			  1, 1, 0, 0,
			  1, 64, 3, 0x3F,
			  0x58, 31, K230_DIV,
			  0x50, 4, false,
			  false, 0,
			  K230_OSC24M, NULL);

K230_CLK_RATE_FORMAT(timer0_src,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x54, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_FORMAT(timer1_src,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x54, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_FORMAT(timer2_src,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x54, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_FORMAT(timer3_src,
		     1, 1, 0, 0,
		     1, 8, 9, 0x7,
		     0x54, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_FORMAT(timer4_src,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x54, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_RATE_FORMAT(timer5_src,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x54, 31, K230_DIV,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV16]);

K230_CLK_GATE_MUX_FORMAT(timer0,
			 0x50, 13, false,
			 0x50, 7, 0x1,
			 false, 0,
			 2,
			 K230_TIMERX_PULSE_IN, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(timer0_src));

K230_CLK_GATE_MUX_FORMAT(timer1,
			 0x50, 14, false,
			 0x50, 8, 0x1,
			 false, 0,
			 2,
			 K230_TIMERX_PULSE_IN, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(timer1_src));

K230_CLK_GATE_MUX_FORMAT(timer2,
			 0x50, 15, false,
			 0x50, 9, 0x1,
			 false, 0,
			 2,
			 K230_TIMERX_PULSE_IN, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(timer2_src));

K230_CLK_GATE_MUX_FORMAT(timer3,
			 0x50, 16, false,
			 0x50, 10, 0x1,
			 false, 0,
			 2,
			 K230_TIMERX_PULSE_IN, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(timer3_src));

K230_CLK_GATE_MUX_FORMAT(timer4,
			 0x50, 17, false,
			 0x50, 11, 0x1,
			 false, 0,
			 2,
			 K230_TIMERX_PULSE_IN, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(timer4_src));

K230_CLK_GATE_MUX_FORMAT(timer5,
			 0x50, 18, false,
			 0x50, 12, 0x1,
			 false, 0,
			 2,
			 K230_TIMERX_PULSE_IN, NULL,
			 K230_CLK_COMPOSITE, K230_FMT(timer5_src));

K230_CLK_RATE_GATE_FORMAT(shrm_apb,
			  1, 1, 0, 0,
			  1, 8, 18, 0x7,
			  0x5C, 31, K230_DIV,
			  0x5C, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(shrm_axi_src,
		     0x5C, 12, false,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(shrm_axi_slave,
		     0x5C, 11, false,
		     false, 0,
		     K230_SHRM_SRAM_DIV2, NULL);

K230_CLK_GATE_FORMAT(shrm_nonai2d_axi,
		     0x5C, 9, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(shrm_axi_src));

K230_CLK_GATE_MUX_FORMAT(shrm_sram,
			 0x5c, 10, false,
			 0x50, 14, 0x1,
			 false, 0,
			 2,
			 K230_PLL_DIV, &k230_pll_divs[K230_PLL3_DIV2],
			 K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2]);

K230_CLK_GATE_FORMAT(shrm_decompress_axi,
		     0x5C, 7, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(shrm_sram));

K230_CLK_GATE_FORMAT(shrm_sdma_axi,
		     0x5C, 5, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(shrm_axi_src));

K230_CLK_GATE_FORMAT(shrm_pdma_axi,
		     0x5C, 3, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(shrm_axi_src));

K230_CLK_FORMAT(ddrc_src,
		1, 1, 0, 0,
		1, 16, 10, 0xF,
		0x60, 31, K230_DIV,
		0x60, 2, false,
		0x60, 0, 0x3,
		false, 0,
		3,
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV3],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL2_DIV4]);

K230_CLK_GATE_FORMAT(ddrc_bypass,
		     0x60, 8, false,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL2_DIV4]);

K230_CLK_RATE_GATE_FORMAT(ddrc_apb,
			  1, 1, 0, 0,
			  1, 16, 14, 0xF,
			  0x60, 31, K230_DIV,
			  0x60, 9, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(display_ahb,
			  1, 1, 0, 0,
			  1, 8, 0, 0x7,
			  0x78, 31, K230_DIV,
			  0x74, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_GATE_FORMAT(display_axi,
		     0x74, 1, false,
		     false, 0,
		     K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(display_clkext,
			  1, 1, 0, 0,
			  1, 16, 16, 0xF,
			  0x78, 31, K230_DIV,
			  0x74, 5, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV3]);

K230_CLK_RATE_GATE_FORMAT(display_gpu,
			  1, 1, 0, 0,
			  1, 16, 20, 0xF,
			  0x78, 31, K230_DIV,
			  0x74, 6, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV3]);

K230_CLK_RATE_GATE_FORMAT(display_dpip,
			  1, 1, 0, 0,
			  1, 256, 3, 0xFF,
			  0x78, 31, K230_DIV,
			  0x74, 2, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4]);

K230_CLK_RATE_GATE_FORMAT(display_cfg,
			  1, 1, 0, 0,
			  1, 32, 11, 0x1F,
			  0x78, 31, K230_DIV,
			  0x74, 4, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4]);

K230_CLK_GATE_FORMAT(display_ref,
		     0x74, 3, false,
		     false, 0,
		     K230_OSC24M, NULL);

K230_CLK_RATE_GATE_FORMAT(vpu_src,
			  1, 16, 0, 0,
			  16, 16, 1, 0xF,
			  0xC, 31, K230_MUL,
			  0xC, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2]);

K230_CLK_RATE_FORMAT(vpu_axi_src,
		     1, 1, 0, 0,
		     1, 16, 6, 0xF,
		     0xC, 31, K230_DIV,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(vpu_src));

K230_CLK_GATE_FORMAT(vpu_axi,
		     0xC, 5, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(vpu_axi_src));

K230_CLK_GATE_FORMAT(vpu_ddrcp2,
		     0x60, 5, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(vpu_axi_src));

K230_CLK_RATE_GATE_FORMAT(vpu_cfg,
			  1, 1, 0, 0,
			  1, 16, 11, 0xF,
			  0xC, 31, K230_DIV,
			  0xC, 10, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(sec_apb,
			  1, 1, 0, 0,
			  1, 8, 1, 0x7,
			  0x80, 31, K230_DIV,
			  0x80, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(sec_fix,
			  1, 1, 0, 0,
			  1, 32, 6, 0x1F,
			  0x80, 31, K230_DIV,
			  0x80, 5, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4]);

K230_CLK_RATE_GATE_FORMAT(sec_axi,
			  1, 1, 0, 0,
			  1, 8, 11, 0x3,
			  0x80, 31, K230_DIV,
			  0x80, 4, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4]);

K230_CLK_RATE_GATE_FORMAT(usb_480m,
			  1, 1, 0, 0,
			  1, 8, 1, 0x7,
			  0x100, 31, K230_DIV,
			  0x100, 0, false,
			  false, 0,
			  K230_PLL, &k230_plls[K230_PLL1]);

K230_CLK_RATE_GATE_FORMAT(usb_100m,
			  1, 1, 0, 0,
			  1, 8, 4, 0x7,
			  0x100, 31, K230_DIV,
			  0x100, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_RATE_GATE_FORMAT(dphy_dft,
			  1, 1, 0, 0,
			  1, 16, 1, 0xF,
			  0x104, 31, K230_DIV,
			  0x100, 0, false,
			  false, 0,
			  K230_PLL, &k230_plls[K230_PLL0]);

K230_CLK_RATE_GATE_FORMAT(spi2axi,
			  1, 1, 0, 0,
			  1, 8, 1, 0x7,
			  0x108, 31, K230_DIV,
			  0x108, 0, false,
			  false, 0,
			  K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_FORMAT(ai_src,
		1, 1, 0, 0,
		1, 8, 3, 0x7,
		0x8, 31, K230_DIV,
		0x8, 0, false,
		0x8, 2, 0x1,
		false, 0,
		2,
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV2],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL3_DIV2]);

K230_CLK_GATE_FORMAT(ai_axi,
		     0x8, 10, false,
		     false, 0,
		     K230_CLK_COMPOSITE, K230_FMT(ai_src));

K230_CLK_FORMAT(camera0,
		1, 1, 0, 0,
		1, 32, 5, 0x1f,
		0x6C, 31, K230_DIV,
		0x6C, 0, false,
		0x6C, 3, 0x3,
		false, 0,
		3,
		K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV3],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_FORMAT(camera1,
		1, 1, 0, 0,
		1, 32, 12, 0x1f,
		0x6C, 31, K230_DIV,
		0x6C, 1, false,
		0x6C, 10, 0x3,
		false, 0,
		3,
		K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV3],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

K230_CLK_FORMAT(camera2,
		1, 1, 0, 0,
		1, 32, 19, 0x1f,
		0x6C, 31, K230_DIV,
		0x6C, 2, false,
		0x6C, 17, 0x3,
		false, 0,
		3,
		K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV3],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL1_DIV4],
		K230_PLL_DIV, &k230_pll_divs[K230_PLL0_DIV4]);

static struct k230_clk *k230_clks[] = {
	[K230_CPU0_SRC]			=	K230_FMT(cpu0_src),
	[K230_CPU0_AXI]			=	K230_FMT(cpu0_axi),
	[K230_CPU0_PLIC]		=	K230_FMT(cpu0_plic),
	[K230_CPU0_NOC_DDRCP4]		=	K230_FMT(cpu0_noc_ddrcp4),
	[K230_CPU0_APB]			=	K230_FMT(cpu0_apb),
	[K230_CPU1_SRC]			=	K230_FMT(cpu1_src),
	[K230_CPU1_AXI]			=	K230_FMT(cpu1_axi),
	[K230_CPU1_PLIC]		=	K230_FMT(cpu1_plic),
	[K230_CPU1_APB]			=	K230_FMT(cpu1_apb),
	[K230_PMU_APB]			=	K230_FMT(pmu_apb),
	[K230_HS_HCLK_HIGH_SRC]		=	K230_FMT(hs_hclk_high_src),
	[K230_HS_HCLK_HIGH]		=	K230_FMT(hs_hclk_high),
	[K230_HS_HCLK_SRC]		=	K230_FMT(hs_hclk_src),
	[K230_HS_SD0_AHB]		=	K230_FMT(hs_sd0_ahb),
	[K230_HS_SD1_AHB]		=	K230_FMT(hs_sd1_ahb),
	[K230_HS_SSI1_AHB]		=	K230_FMT(hs_ssi1_ahb),
	[K230_HS_SSI2_AHB]		=	K230_FMT(hs_ssi2_ahb),
	[K230_HS_USB0_AHB]		=	K230_FMT(hs_usb0_ahb),
	[K230_HS_USB1_AHB]		=	K230_FMT(hs_usb1_ahb),
	[K230_HS_SSI0_AXI]		=	K230_FMT(hs_ssi0_axi),
	[K230_HS_SSI1]			=	K230_FMT(hs_ssi1),
	[K230_HS_SSI2]			=	K230_FMT(hs_ssi2),
	[K230_HS_QSPI_AXI_SRC]		=	K230_FMT(hs_qspi_axi_src),
	[K230_HS_SSI1_AXI]		=	K230_FMT(hs_ssi1_axi),
	[K230_HS_SSI2_AXI]		=	K230_FMT(hs_ssi2_axi),
	[K230_HS_SD_CARD_SRC]		=	K230_FMT(hs_sd_card_src),
	[K230_HS_SD0_CARD_TX]		=	K230_FMT(hs_sd0_card),
	[K230_HS_SD1_CARD_TX]		=	K230_FMT(hs_sd1_card),
	[K230_HS_SD_AXI_SRC]		=	K230_FMT(hs_sd_axi_src),
	[K230_HS_SD0_AXI]		=	K230_FMT(hs_sd0_axi),
	[K230_HS_SD1_AXI]		=	K230_FMT(hs_sd1_axi),
	[K230_HS_SD0_BASE]		=	K230_FMT(hs_sd0_base),
	[K230_HS_SD1_BASE]		=	K230_FMT(hs_sd1_base),
	[K230_HS_OSPI_SRC]		=	K230_FMT(hs_ospi_src),
	[K230_HS_USB_REF_50M]		=	K230_FMT(hs_usb_ref_50m),
	[K230_HS_SD_TIMER_SRC]		=	K230_FMT(hs_sd_timer_src),
	[K230_HS_SD0_TIMER]		=	K230_FMT(hs_sd0_timer),
	[K230_HS_SD1_TIMER]		=	K230_FMT(hs_sd1_timer),
	[K230_HS_USB0_REFERENCE]	=	K230_FMT(hs_usb0_ref),
	[K230_HS_USB1_REFERENCE]	=	K230_FMT(hs_usb1_ref),
	[K230_LS_APB_SRC]		=	K230_FMT(ls_apb_src),
	[K230_LS_UART0_APB]		=	K230_FMT(ls_uart0_apb),
	[K230_LS_UART1_APB]		=	K230_FMT(ls_uart1_apb),
	[K230_LS_UART2_APB]		=	K230_FMT(ls_uart2_apb),
	[K230_LS_UART3_APB]		=	K230_FMT(ls_uart3_apb),
	[K230_LS_UART4_APB]		=	K230_FMT(ls_uart4_apb),
	[K230_LS_I2C0_APB]		=	K230_FMT(ls_i2c0_apb),
	[K230_LS_I2C1_APB]		=	K230_FMT(ls_i2c1_apb),
	[K230_LS_I2C2_APB]		=	K230_FMT(ls_i2c2_apb),
	[K230_LS_I2C3_APB]		=	K230_FMT(ls_i2c3_apb),
	[K230_LS_I2C4_APB]		=	K230_FMT(ls_i2c4_apb),
	[K230_LS_GPIO_APB]		=	K230_FMT(ls_gpio_apb),
	[K230_LS_PWM_APB]		=	K230_FMT(ls_pwm_apb),
	[K230_LS_JAMLINK0_APB]		=	K230_FMT(ls_jamlink0_apb),
	[K230_LS_JAMLINK1_APB]		=	K230_FMT(ls_jamlink1_apb),
	[K230_LS_JAMLINK2_APB]		=	K230_FMT(ls_jamlink2_apb),
	[K230_LS_JAMLINK3_APB]		=	K230_FMT(ls_jamlink3_apb),
	[K230_LS_AUDIO_APB]		=	K230_FMT(ls_audio_apb),
	[K230_LS_ADC_APB]		=	K230_FMT(ls_adc_apb),
	[K230_LS_CODEC_APB]		=	K230_FMT(ls_codec_apb),
	[K230_LS_I2C0]			=	K230_FMT(ls_i2c0),
	[K230_LS_I2C1]			=	K230_FMT(ls_i2c1),
	[K230_LS_I2C2]			=	K230_FMT(ls_i2c2),
	[K230_LS_I2C3]			=	K230_FMT(ls_i2c3),
	[K230_LS_I2C4]			=	K230_FMT(ls_i2c4),
	[K230_LS_CODEC_ADC]		=	K230_FMT(ls_codec_adc),
	[K230_LS_CODEC_DAC]		=	K230_FMT(ls_codec_dac),
	[K230_LS_AUDIO_DEV]		=	K230_FMT(ls_audio_dev),
	[K230_LS_PDM]			=	K230_FMT(ls_pdm),
	[K230_LS_ADC]			=	K230_FMT(ls_adc),
	[K230_LS_UART0]			=	K230_FMT(ls_uart0),
	[K230_LS_UART1]			=	K230_FMT(ls_uart1),
	[K230_LS_UART2]			=	K230_FMT(ls_uart2),
	[K230_LS_UART3]			=	K230_FMT(ls_uart3),
	[K230_LS_UART4]			=	K230_FMT(ls_uart4),
	[K230_LS_JAMLINKCO_DIV_SRC]	=	K230_FMT(ls_jamlinkco_div_src),
	[K230_LS_JAMLINK0CO]		=	K230_FMT(ls_jamlink0co),
	[K230_LS_JAMLINK1CO]		=	K230_FMT(ls_jamlink1co),
	[K230_LS_JAMLINK2CO]		=	K230_FMT(ls_jamlink2co),
	[K230_LS_JAMLINK3CO]		=	K230_FMT(ls_jamlink3co),
	[K230_LS_GPIO_DEBOUNCE]		=	K230_FMT(ls_gpio_debounce),
	[K230_SYSCTL_WDT0_APB]		=	K230_FMT(sysctl_wdt0_apb),
	[K230_SYSCTL_WDT1_APB]		=	K230_FMT(sysctl_wdt1_apb),
	[K230_SYSCTL_TIMER_APB]		=	K230_FMT(sysctl_timer_apb),
	[K230_SYSCTL_IOMUX_APB]		=	K230_FMT(sysctl_iomux_apb),
	[K230_SYSCTL_MAILBOX_APB]	=	K230_FMT(sysctl_mailbox_apb),
	[K230_SYSCTL_HDI]		=	K230_FMT(sysctl_hdi),
	[K230_SYSCTL_TIME_STAMP]	=	K230_FMT(sysctl_time_stamp),
	[K230_SYSCTL_TEMP_SENSOR]	=	K230_FMT(sysctl_temp_sensor),
	[K230_SYSCTL_WDT0]		=	K230_FMT(sysctl_wdt0),
	[K230_SYSCTL_WDT1]		=	K230_FMT(sysctl_wdt1),
	[K230_TIMER0_SRC]		=	K230_FMT(timer0_src),
	[K230_TIMER1_SRC]		=	K230_FMT(timer1_src),
	[K230_TIMER2_SRC]		=	K230_FMT(timer2_src),
	[K230_TIMER3_SRC]		=	K230_FMT(timer3_src),
	[K230_TIMER4_SRC]		=	K230_FMT(timer4_src),
	[K230_TIMER5_SRC]		=	K230_FMT(timer5_src),
	[K230_TIMER0]			=	K230_FMT(timer0),
	[K230_TIMER1]			=	K230_FMT(timer1),
	[K230_TIMER2]			=	K230_FMT(timer2),
	[K230_TIMER3]			=	K230_FMT(timer3),
	[K230_TIMER4]			=	K230_FMT(timer4),
	[K230_TIMER5]			=	K230_FMT(timer5),
	[K230_SHRM_APB]			=	K230_FMT(shrm_apb),
	[K230_SHRM_AXI_SRC]		=	K230_FMT(shrm_axi_src),
	[K230_SHRM_AXI_SLAVE]		=	K230_FMT(shrm_axi_slave),
	[K230_SHRM_NONAI2D_AXI]		=	K230_FMT(shrm_nonai2d_axi),
	[K230_SHRM_SRAM]		=	K230_FMT(shrm_sram),
	[K230_SHRM_DECOMPRESS_AXI]	=	K230_FMT(shrm_decompress_axi),
	[K230_SHRM_SDMA_AXI]		=	K230_FMT(shrm_sdma_axi),
	[K230_SHRM_PDMA_AXI]		=	K230_FMT(shrm_pdma_axi),
	[K230_DDRC_SRC]			=	K230_FMT(ddrc_src),
	[K230_DDRC_BYPASS]		=	K230_FMT(ddrc_bypass),
	[K230_DDRC_APB]			=	K230_FMT(ddrc_apb),
	[K230_DISPLAY_AHB]		=	K230_FMT(display_ahb),
	[K230_DISPLAY_AXI]		=	K230_FMT(display_axi),
	[K230_DISPLAY_CLKEXT]		=	K230_FMT(display_clkext),
	[K230_DISPLAY_GPU]		=	K230_FMT(display_gpu),
	[K230_DISPLAY_DPIP]		=	K230_FMT(display_dpip),
	[K230_DISPLAY_CFG]		=	K230_FMT(display_cfg),
	[K230_DISPLAY_REF]		=	K230_FMT(display_ref),
	[K230_VPU_SRC]			=	K230_FMT(vpu_src),
	[K230_VPU_AXI_SRC]		=	K230_FMT(vpu_axi_src),
	[K230_VPU_AXI]			=	K230_FMT(vpu_axi),
	[K230_VPU_DDRCP2]		=	K230_FMT(vpu_ddrcp2),
	[K230_VPU_CFG]			=	K230_FMT(vpu_cfg),
	[K230_SEC_APB]			=	K230_FMT(sec_apb),
	[K230_SEC_FIX]			=	K230_FMT(sec_fix),
	[K230_SEC_AXI]			=	K230_FMT(sec_axi),
	[K230_USB_480M]			=	K230_FMT(usb_480m),
	[K230_USB_100M]			=	K230_FMT(usb_100m),
	[K230_DPHY_DFT]			=	K230_FMT(dphy_dft),
	[K230_SPI2AXI]			=	K230_FMT(spi2axi),
	[K230_AI_SRC]			=	K230_FMT(ai_src),
	[K230_AI_AXI]			=	K230_FMT(ai_axi),
	[K230_CAMERA0]			=	K230_FMT(camera0),
	[K230_CAMERA1]			=	K230_FMT(camera1),
	[K230_CAMERA2]			=	K230_FMT(camera2),
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

static int k230_pll_init(struct clk_hw *hw)
{
	if (k230_pll_is_enabled(hw))
		return clk_prepare_enable(hw->clk);

	return 0;
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
	.init		= k230_pll_init,
	.prepare	= k230_pll_prepare,
	.enable	        = k230_pll_enable,
	.disable	= k230_pll_disable,
	.is_enabled	= k230_pll_is_enabled,
	.recalc_rate	= k230_pll_get_rate,
};

static int k230_register_pll(struct platform_device *pdev,
			     struct k230_sysclk *ksc,
			     enum k230_pll_id pll_id,
			     const char *name,
			     int num_parents,
			     const struct clk_ops *ops)
{
	struct k230_pll *pll = &k230_plls[pll_id];
	struct clk_init_data init = {};
	struct device *dev = &pdev->dev;
	int ret;
	const struct clk_parent_data parent_data[] = {
		{ .index = 0, },
	};

	init.name = name;
	init.parent_data = parent_data;
	init.num_parents = num_parents;
	init.ops = ops;

	pll->hw.init = &init;
	pll->ksc = ksc;

	ret = devm_clk_hw_register(dev, &pll->hw);
	if (ret)
		return ret;

	return 0;
}

static int k230_register_plls(struct platform_device *pdev, struct k230_sysclk *ksc)
{
	int i, ret;
	const struct k230_pll *pll;

	for (i = 0; i < K230_PLL_NUM; i++) {
		pll = &k230_plls[i];

		ret = k230_register_pll(pdev, ksc, i, pll->name, 1, &k230_pll_ops);
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

	for (int i = 0; i < K230_PLL_DIV_NUM; i++) {
		hw = devm_clk_hw_register_fixed_factor(dev, k230_pll_divs[i].name,
						       k230_pll_divs[i].parent_name,
						       0, 1, k230_pll_divs[i].div);
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		pll_div = &k230_pll_divs[i];
		pll_div->hw = hw;
		pll_div->ksc = ksc;
		k230_pll_divs[i].id = i;
	}

	return 0;
}

static int k230_clk_enable(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_gate_cfg *gate_cfg = clk->gate_cfg;
	u32 reg;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + gate_cfg->gate_reg_off);
	if (gate_cfg->gate_bit_reverse)
		reg &= ~BIT(gate_cfg->gate_bit_enable);
	else
		reg |= BIT(gate_cfg->gate_bit_enable);
	writel(reg, ksc->regs + gate_cfg->gate_reg_off);

	return 0;
}

static void k230_clk_disable(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_gate_cfg *gate_cfg = clk->gate_cfg;
	u32 reg;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + gate_cfg->gate_reg_off);
	if (gate_cfg->gate_bit_reverse)
		reg |= BIT(gate_cfg->gate_bit_enable);
	else
		reg &= ~BIT(gate_cfg->gate_bit_enable);
	writel(reg, ksc->regs + gate_cfg->gate_reg_off);
}

static int k230_clk_is_enabled(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_gate_cfg *gate_cfg = clk->gate_cfg;
	u32 reg;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + gate_cfg->gate_reg_off);
	if (gate_cfg->gate_bit_reverse)
		return BIT(gate_cfg->gate_bit_enable) & reg ? 1 : 0;

	return BIT(gate_cfg->gate_bit_enable) & ~reg ? 1 : 0;
}

static int k230_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_mux_cfg *mux_cfg = clk->mux_cfg;
	u8 reg;

	guard(spinlock)(&ksc->clk_lock);

	reg = (readl(ksc->regs + mux_cfg->mux_reg_off) & index) << mux_cfg->mux_reg_shift;
	writel(reg, ksc->regs + mux_cfg->mux_reg_off);

	return 0;
}

static u8 k230_clk_get_parent(struct clk_hw *hw)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_mux_cfg *mux_cfg = clk->mux_cfg;

	guard(spinlock)(&ksc->clk_lock);

	return readl(ksc->regs + mux_cfg->mux_reg_off);
}

static unsigned long k230_clk_get_rate(struct clk_hw *hw,
				       unsigned long parent_rate)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_rate_cfg *rate_cfg = clk->rate_cfg;
	struct k230_clk_rate_cfg_c *rate_cfg_c = clk->rate_cfg_c;
	u32 mul, div;

	/* no divider, return parents' clk */
	if (!rate_cfg)
		return parent_rate;

	guard(spinlock)(&ksc->clk_lock);

	switch (rate_cfg->method) {
	/*
	 * K230_MUL: div_mask+1/div_max...
	 * K230_DIV: mul_max/div_mask+1
	 * K230_MUL_DIV: mul_mask/div_mask...
	 */
	case K230_MUL:
		div = rate_cfg->rate_div_max;
		mul = (readl(ksc->regs + rate_cfg->rate_reg_off) >> rate_cfg->rate_div_shift)
			& rate_cfg->rate_div_mask;
		mul++;
		break;
	case K230_DIV:
		mul = rate_cfg->rate_mul_max;
		div = (readl(ksc->regs + rate_cfg->rate_reg_off) >> rate_cfg->rate_div_shift)
			& rate_cfg->rate_div_mask;
		div++;
		break;
	case K230_MUL_DIV:
		if (!rate_cfg_c) {
			mul = (readl(ksc->regs + rate_cfg->rate_reg_off)
				>> rate_cfg->rate_mul_shift)
				& rate_cfg->rate_mul_mask;
			div = (readl(ksc->regs + rate_cfg->rate_reg_off)
				>> rate_cfg->rate_div_shift)
				& rate_cfg->rate_div_mask;
		} else {
			mul = (readl(ksc->regs + rate_cfg_c->rate_reg_off_c)
				>> rate_cfg_c->rate_mul_shift_c)
				& rate_cfg_c->rate_mul_mask_c;
			div = (readl(ksc->regs + rate_cfg->rate_reg_off)
				>> rate_cfg->rate_div_shift)
				& rate_cfg->rate_div_mask;
		}
		break;
	}

	return mul_u64_u32_div(parent_rate, mul, div);
}

static int k230_clk_find_approximate(struct k230_clk *clk,
				     u32 mul_min,
				     u32 mul_max,
				     u32 div_min,
				     u32 div_max,
				     enum k230_clk_div_type method,
				     unsigned long rate,
				     unsigned long parent_rate,
				     u32 *div,
				     u32 *mul)
{
	long abs_min;
	long abs_current;
	long perfect_divide;
	struct k230_clk_rate_cfg *rate_cfg = clk->rate_cfg;

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

	switch (method) {
	/* Only mul can be changed: 1/12, 2/12, 3/12, ... */
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
	/* Only div can be changeable: 1/1, 1/2, 1/3, ... */
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
	/* mul and div can be changed */
	case K230_MUL_DIV:
		if (rate_cfg->rate_reg_off == K230_CLK_CODEC_ADC_MCLKDIV_OFFSET ||
		    rate_cfg->rate_reg_off == K230_CLK_CODEC_DAC_MCLKDIV_OFFSET) {
			for (u32 j = 0; j < 9; j++) {
				if (rate == codec_clk[j]) {
					*div = codec_div[j][0];
					*mul = codec_div[j][1];
				}
			}
		} else if (rate_cfg->rate_reg_off == K230_CLK_AUDIO_CLKDIV_OFFSET ||
			   rate_cfg->rate_reg_off == K230_CLK_PDM_CLKDIV_OFFSET) {
			for (u32 j = 0; j < 20; j++) {
				if (rate == pdm_clk[j]) {
					*div = pdm_div[j][0];
					*mul = pdm_div[j][1];
				}
			}
		} else {
			return -EINVAL;
		}
		break;
	}

	return 0;
}

static long k230_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_clk_rate_cfg *rate_cfg = clk->rate_cfg;
	u32 div = 0, mul = 0;

	if (k230_clk_find_approximate(clk,
				      rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
				      rate_cfg->rate_div_min, rate_cfg->rate_div_max,
				      rate_cfg->method, rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static int k230_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct k230_clk *clk = to_k230_clk(hw);
	struct k230_sysclk *ksc = clk->ksc;
	struct k230_clk_rate_cfg *rate_cfg = clk->rate_cfg;
	struct k230_clk_rate_cfg_c *rate_cfg_c = clk->rate_cfg_c;
	u32 div, mul, reg, reg_c;

	if (rate > parent_rate) {
		dev_err(&ksc->pdev->dev, "rate should be smaller than parent rate\n");
		return -EINVAL;
	}

	if (clk->read_only) {
		dev_err(&ksc->pdev->dev, "This clk rate is read only\n");
		return -EPERM;
	}

	if (k230_clk_find_approximate(clk,
				      rate_cfg->rate_mul_min, rate_cfg->rate_mul_max,
				      rate_cfg->rate_div_min, rate_cfg->rate_div_max,
				      rate_cfg->method, rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(&ksc->clk_lock);

	reg = readl(ksc->regs + rate_cfg->rate_reg_off);
	if (!rate_cfg_c) {
		reg &= ~((rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift));

		if (rate_cfg->method == K230_DIV) {
			reg &= ~((rate_cfg->rate_mul_mask) << (rate_cfg->rate_mul_shift));
			reg |= ((div - 1) & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
		} else if (rate_cfg->method == K230_MUL) {
			reg |= ((mul - 1) & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
		} else {
			reg |= (mul & rate_cfg->rate_mul_mask) << (rate_cfg->rate_mul_shift);
			reg |= (div & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
		}
		reg |= BIT(rate_cfg->rate_write_enable_bit);
	} else {
		reg_c = readl(ksc->regs + rate_cfg_c->rate_reg_off_c);
		reg_c &= ~((rate_cfg_c->rate_mul_mask_c) << (rate_cfg_c->rate_mul_shift_c));
		reg_c |= BIT(rate_cfg_c->rate_write_enable_bit_c);
		reg_c |= (mul & rate_cfg_c->rate_mul_mask_c) << (rate_cfg_c->rate_mul_shift_c);
		writel(reg_c, ksc->regs + rate_cfg_c->rate_reg_off_c);

		reg &= ~((rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift));
		reg |= (div & rate_cfg->rate_div_mask) << (rate_cfg->rate_div_shift);
	}
	writel(reg, ksc->regs + rate_cfg->rate_reg_off);

	return 0;
}

static const struct clk_ops k230_clk_ops_arr[K230_CLK_OPS_ID_NUM] = {
	[K230_CLK_OPS_ID_NONE] = {
		/* Sentinel */
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

static int k230_register_clk(struct platform_device *pdev,
			     struct k230_sysclk *ksc,
			     int id,
			     const struct clk_parent_data *parent_data,
			     u8 num_parents,
			     unsigned long flags)
{
	struct k230_clk *clk = k230_clks[id];
	struct clk_hw_onecell_data *hw_data = platform_get_drvdata(pdev);
	struct clk_init_data init = {};
	int clk_id = 0;
	int ret;

	if (clk->rate_cfg)
		clk_id += K230_CLK_OPS_ID_RATE_ONLY;

	if (clk->mux_cfg)
		clk_id += K230_CLK_OPS_ID_MUX_ONLY;

	if (clk->gate_cfg)
		clk_id += K230_CLK_OPS_ID_GATE_ONLY;

	init.name = k230_clks[id]->name;
	init.flags = flags;
	init.parent_data = parent_data;
	init.num_parents = num_parents;
	init.ops = &k230_clk_ops_arr[clk_id];

	clk->ksc = ksc;
	clk->hw.init = &init;

	ret = devm_clk_hw_register(&pdev->dev, &clk->hw);
	if (ret)
		return ret;

	hw_data->hws[id] = &clk->hw;
	return 0;
}

static inline int k230_register_mux_clk(struct platform_device *pdev,
					struct k230_sysclk *ksc,
					struct clk_parent_data *parent_data,
					int num_parent,
					int id)
{
	return k230_register_clk(pdev, ksc, id, parent_data, num_parent, 0);
}

static inline int k230_register_fixed_child(struct platform_device *pdev,
					    struct k230_sysclk *ksc,
					    enum k230_clk_parent_type clk_type,
					    int id)
{
	const struct clk_parent_data parent_data = {
		.index = clk_type,
	};

	return k230_register_clk(pdev, ksc, id, &parent_data, 1, 0);
}

static inline int k230_register_pll_child(struct platform_device *pdev,
					  struct k230_sysclk *ksc,
					  int id,
					  struct clk_hw *parent_hw,
					  unsigned long flags)
{
	const struct clk_parent_data parent_data = {
		.hw = parent_hw,
	};

	return k230_register_clk(pdev, ksc, id, &parent_data, 1, flags);
}

static inline int k230_register_pll_div_child(struct platform_device *pdev,
					      struct k230_sysclk *ksc,
					      int id,
					      struct clk_hw *parent_hw,
					      unsigned long flags)
{
	const struct clk_parent_data parent_data = {
		.hw = parent_hw,
	};

	return k230_register_clk(pdev, ksc, id, &parent_data, 1, flags);
}

static inline int k230_register_clk_child(struct platform_device *pdev,
					  struct k230_sysclk *ksc,
					  int id,
					  struct clk_hw *parent_hw)
{
	const struct clk_parent_data parent_data = {
		.hw = parent_hw,
	};

	return k230_register_clk(pdev, ksc, id, &parent_data, 1, 0);
}

static int k230_clk_get_parent_data(struct k230_clk_parent *pclk,
				    struct clk_parent_data *parent_data)
{
	switch (pclk->type) {
	case K230_PLL:
		parent_data->hw = &pclk->pll->hw;
		break;
	case K230_PLL_DIV:
		parent_data->hw = pclk->pll_div->hw;
		break;
	case K230_CLK_COMPOSITE:
		parent_data->hw = &pclk->clk->hw;
		break;
	default:
		parent_data->index = pclk->type;
		return 0;
	}

	return parent_data->hw ? 0 : -EINVAL;
}

static int k230_clk_mux_get_parent_data(struct k230_clk *clk,
					struct clk_parent_data *parent_data)
{
	int ret;
	struct k230_clk_parent *pclk = clk->parent;

	for (int i = 0; i < clk->num_parent; i++) {
		memset(&parent_data[i], 0, sizeof(*parent_data));
		ret = k230_clk_get_parent_data(&pclk[i], &parent_data[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int k230_register_clks(struct platform_device *pdev, struct k230_sysclk *ksc)
{
	struct k230_clk *clk;
	struct k230_clk_parent *pclk;
	struct clk_parent_data parent_data[K230_CLK_MAX_PARENT_NUM];
	int ret, i;

	for (i = 0; i < K230_CLK_NUM; i++) {
		clk = k230_clks[i];
		if (!clk)
			continue;

		if (clk->mux_cfg) {
			ret = k230_clk_mux_get_parent_data(clk, parent_data);
			if (ret)
				return ret;

			ret = k230_register_mux_clk(pdev, ksc, parent_data,
						    clk->num_parent, i);
		} else {
			pclk = clk->parent;

			switch (pclk->type) {
			case K230_PLL:
				ret = k230_register_pll_child(pdev, ksc, i,
							      &pclk->pll->hw,
							      clk->flags);
				break;
			case K230_PLL_DIV:
				ret = k230_register_pll_div_child(pdev, ksc, i,
								  pclk->pll_div->hw,
								  clk->flags);
				break;
			case K230_CLK_COMPOSITE:
				ret = k230_register_clk_child(pdev, ksc, i,
							      &pclk->clk->hw);
				break;
			default:
				ret = k230_register_fixed_child(pdev, ksc,
								pclk->type, i);
			}
		}
		if (ret)
			return ret;
	}

	return 0;
}

static int k230_clk_init_plls(struct platform_device *pdev, struct k230_sysclk *ksc)
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

	for (int i = 0; i < K230_PLL_DIV_NUM; i++) {
		ret = devm_clk_hw_register_clkdev(&pdev->dev, k230_pll_divs[i].hw,
						  k230_pll_divs[i].name, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static int k230_clk_init_clks(struct platform_device *pdev, struct k230_sysclk *ksc)
{
	int ret;

	struct clk_hw_onecell_data *hw_data = platform_get_drvdata(pdev);

	hw_data->num = K230_CLK_NUM;

	spin_lock_init(&ksc->clk_lock);

	ksc->regs = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ksc->regs))
		return PTR_ERR(ksc->regs);

	ret = k230_register_clks(pdev, ksc);
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

	ksc->pdev = pdev;
	platform_set_drvdata(pdev, hw_data);

	ret = k230_clk_init_plls(pdev, ksc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "init plls failed\n");

	ret = k230_clk_init_clks(pdev, ksc);
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
