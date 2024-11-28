/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Author: Bo Wang <Juvenoia@...>
 * Kendryte Canaan K230 Clock Drivers
 */

#ifndef CLOCK_K230_CLK_H
#define CLOCK_K230_CLK_H

/*
 * Kendryte K230 SoC clock identifiers (arbitrary values).
 */
#define K230_CPU0_SRC           0
#define K230_CPU0_ACLK          1
#define K230_CPU0_PLIC          2
#define K230_CPU0_NOC_DDRCP4    3
#define K230_CPU0_PCLK          4
#define K230_PMU_PCLK           5
#define K230_HS_HCLK_HIGN_SRC   6
#define K230_HS_HCLK_HIGN_GATE  7
#define K230_HS_HCLK_SRC        8
#define K230_HS_SD0_HS_AHB_GAT  9
#define K230_HS_SD1_HS_AHB_GAT	10
#define K230_HS_SSI1_HS_AHB_GA	11
#define K230_HS_SSI2_HS_AHB_GA	12
#define K230_HS_USB0_HS_AHB_GA	13
#define K230_HS_USB1_HS_AHB_GA	14
#define K230_HS_SSI0_AXI15	15
#define K230_HS_SSI1		16
#define K230_HS_SSI2		17
#define K230_HS_QSPI_AXI_SRC	18
#define K230_HS_SSI1_ACLK_GATE	19
#define K230_HS_SSI2_ACLK_GATE	20
#define K230_HS_SD_CARD_SRC	21
#define K230_HS_SD0_CARD_TX	22
#define K230_HS_SD1_CARD_TX	23
#define K230_HS_SD_AXI_SRC	24
#define K230_HS_SD0_AXI_GATE	25
#define K230_HS_SD1_AXI_GATE	26
#define K230_HS_SD0_BASE_GATE	27
#define K230_HS_SD1_BASE_GATE	28
#define K230_HS_OSPI_SRC	29
#define K230_HS_USB_REF_50M	30
#define K230_HS_SD_TIMER_SRC	31
#define K230_HS_SD0_TIMER_GATE	32
#define K230_HS_SD1_TIMER_GATE	33
#define K230_HS_USB0_REFERENCE	34
#define K230_HS_USB1_REFERENCE	35

#endif /* CLOCK_K230_CLK_H */
