// SPDX-License-Identifier: BSD-3-Clause
/*
 * Clock, reset and GDSC driver for the Qualcomm SC7180 / SM7125 GCC (HakiOS, Galaxy A72 port).
 *
 * Register offsets are from Linux drivers/clk/qcom/gcc-sc7180.c. Only what U-Boot uses: UFS, USB and the
 * QUP AHB clocks. Rates are left as ABL programmed them (ABL boots from UFS, so its RCGs are already set).
 */

#include <clk-uclass.h>
#include <dm.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <dt-bindings/clock/qcom,gcc-sc7180.h>

#include "clock-qcom.h"

static ulong sc7180_set_rate(struct clk *clk, ulong rate)
{
	/* keep ABL's rates: the UFS AXI (200 MHz) and UniPro (150 MHz) sources are already running */
	return rate;
}

static const struct gate_clk sc7180_clks[] = {
	GATE_CLK(GCC_AGGRE_UFS_PHY_AXI_CLK, 0x82024, BIT(0)),
	GATE_CLK(GCC_AGGRE_USB3_PRIM_AXI_CLK, 0x8201c, BIT(0)),
	GATE_CLK(GCC_CFG_NOC_USB3_PRIM_AXI_CLK, 0x502c, BIT(0)),
	GATE_CLK(GCC_PRNG_AHB_CLK, 0x52000, BIT(13)),
	GATE_CLK(GCC_QUPV3_WRAP_0_M_AHB_CLK, 0x52008, BIT(6)),
	GATE_CLK(GCC_QUPV3_WRAP_0_S_AHB_CLK, 0x52008, BIT(7)),
	GATE_CLK(GCC_QUPV3_WRAP_1_M_AHB_CLK, 0x52008, BIT(20)),
	GATE_CLK(GCC_QUPV3_WRAP_1_S_AHB_CLK, 0x52008, BIT(21)),
	GATE_CLK(GCC_UFS_MEM_CLKREF_CLK, 0x8c000, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_AHB_CLK, 0x77014, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_AXI_CLK, 0x77038, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_ICE_CORE_CLK, 0x77090, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_PHY_AUX_CLK, 0x77094, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_RX_SYMBOL_0_CLK, 0x7701c, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_TX_SYMBOL_0_CLK, 0x77018, BIT(0)),
	GATE_CLK(GCC_UFS_PHY_UNIPRO_CORE_CLK, 0x7708c, BIT(0)),
	GATE_CLK(GCC_USB30_PRIM_MASTER_CLK, 0xf010, BIT(0)),
	GATE_CLK(GCC_USB30_PRIM_MOCK_UTMI_CLK, 0xf018, BIT(0)),
	GATE_CLK(GCC_USB30_PRIM_SLEEP_CLK, 0xf014, BIT(0)),
	GATE_CLK(GCC_USB3_PRIM_CLKREF_CLK, 0x8c010, BIT(0)),
	GATE_CLK(GCC_USB3_PRIM_PHY_AUX_CLK, 0xf050, BIT(0)),
	GATE_CLK(GCC_USB3_PRIM_PHY_COM_AUX_CLK, 0xf054, BIT(0)),
	GATE_CLK(GCC_USB3_PRIM_PHY_PIPE_CLK, 0xf058, BIT(0)),
};

static int sc7180_enable(struct clk *clk)
{
	struct msm_clk_priv *priv = dev_get_priv(clk->dev);

	if (clk->id >= priv->data->num_clks || !priv->data->clks[clk->id].reg) {
		debug("%s: clk %lu left as ABL set it\n", __func__, clk->id);
		return 0;
	}

	switch (clk->id) {
	case GCC_USB30_PRIM_MASTER_CLK:
		qcom_gate_clk_en(priv, GCC_USB3_PRIM_PHY_AUX_CLK);
		qcom_gate_clk_en(priv, GCC_USB3_PRIM_PHY_COM_AUX_CLK);
		break;
	}

	return qcom_gate_clk_en(priv, clk->id);
}

static const struct qcom_reset_map sc7180_gcc_resets[] = {
	[GCC_QUSB2PHY_PRIM_BCR] = { 0x26000 },
	[GCC_QUSB2PHY_SEC_BCR] = { 0x26004 },
	[GCC_UFS_PHY_BCR] = { 0x77000 },
	[GCC_USB30_PRIM_BCR] = { 0xf000 },
	[GCC_USB3_PHY_PRIM_BCR] = { 0x50000 },
	[GCC_USB3PHY_PHY_PRIM_BCR] = { 0x50004 },
	[GCC_USB3_DP_PHY_PRIM_BCR] = { 0x50008 },
	[GCC_USB_PHY_CFG_AHB2PHY_BCR] = { 0x6a000 },
};

static const struct qcom_power_map sc7180_gdscs[] = {
	[UFS_PHY_GDSC] = { 0x77004 },
	[USB30_PRIM_GDSC] = { 0x0f004 },
};

static struct msm_clk_data sc7180_gcc_data = {
	.resets = sc7180_gcc_resets,
	.num_resets = ARRAY_SIZE(sc7180_gcc_resets),
	.clks = sc7180_clks,
	.num_clks = ARRAY_SIZE(sc7180_clks),
	.power_domains = sc7180_gdscs,
	.num_power_domains = ARRAY_SIZE(sc7180_gdscs),

	.enable = sc7180_enable,
	.set_rate = sc7180_set_rate,
};

static const struct udevice_id gcc_sc7180_of_match[] = {
	{
		.compatible = "qcom,gcc-sc7180",
		.data = (ulong)&sc7180_gcc_data,
	},
	{}
};

U_BOOT_DRIVER(gcc_sc7180) = {
	.name = "gcc_sc7180",
	.id = UCLASS_NOP,
	.of_match = gcc_sc7180_of_match,
	.bind = qcom_cc_bind,
	.flags = DM_FLAG_PRE_RELOC,
};
