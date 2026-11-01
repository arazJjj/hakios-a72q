# Galaxy A72 (a72q): hardware map for the HakiOS drivers

The values here come from the mainline device tree `sm7125-samsung-common.dtsi` and `sm7125-samsung-a72q.dts` in
map220v/sm7125-mainline, branch a72q-6.10 (GPL-2.0). They are facts read out of those files. Before a driver relies on
one, check it against the phone's own tree or its boot logs.

| Part | What | Where |
|---|---|---|
| SoC | SM7125 (atoll), close to SC7180 (`sm7125.dtsi` includes `sc7180.dtsi`) | msm-id 443, board-id 0x22/0x5 |
| PMICs | PM6150 + PM6150L (RPMh regulators) | not the PM8350C the POCO has |
| Framebuffer | UEFI/bootloader framebuffer in reserved memory | 0x9c000000 |
| Touch | STMicro FTS5CU56A (`st,stmfts_fts5cu56a`) on **I2C** (QUP `i2c7`, 400 kHz), address 0x49 | IRQ TLMM GPIO 9; vdd = LDO10A 1.8 V, avdd = LDO7C 3.0 V |
| Volume up | gpio-keys, PM6150L GPIO 8, active low, pull-up | power and volume down come through the PMIC's PON block (to confirm) |
| Hall sensor | PM6150 GPIO 10, active low (SW_LID) | |
| Display | DSI0, 4 lanes; panel reset TLMM GPIO 8 | vddio LDO13A 1.8 V, vci LDO18A 3.0 V |
| UFS | ufs_mem_hc | vcc LDO19A 2.85 V, vccq2 LDO12A 1.8 V |
| Wi-Fi | WCN3988-family over SNOC (Linux: ath10k_snoc), **not** the POCO's WCN6750/ath11k | LDO9A 0.8 V, LDO1C 1.8 V, LDO2C 1.3 V, LDO10C/LDO11C 3.3 V |
| Bluetooth | `qcom,wcn3988-bt` on UART3 | swctrl TLMM GPIO 73 |
| GPU | Adreno 618 | |

## What this means for the port

- **Touch**: the POCO's `hakios.goodix.c` does not apply. Its chip, bus and PMIC are all different. The A72 needs
  a new `drivers/input/hakios.stmfts.c` in this repository that brings up the GENI serial engine as I2C rather than SPI,
  switches LDO10A and LDO7C through RPMh, and polls GPIO 9 the way the POCO driver polls its own line.
- **Buttons**: volume up is a PM6150L GPIO, so `hakios.buttons.c` needs the PM6150L's SPMI address and pin 8.
- **Wi-Fi**: needs an ath10k-snoc port under `wifi/lx` beside the ath11k one, plus `fw-wifi/` pulled from the phone.
- **Calls**: missing on every HakiOS device.
