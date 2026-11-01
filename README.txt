HakiOS port for Samsung Galaxy A72 (a72q, SM-A725F, SM7125) - state 2026-11-01
Base: github.com/oosexgang/hakios-xnu-kernel (HakiOS) + U-Boot 2026.07.
patches/u-boot-a72q.diff   : U-Boot: SC7180/SM7125 GCC clocks, TLMM pinctrl, UFS PHY, geni, hakmenu (graphical menu)
patches/hakios-loader-a72q-resflash.diff : xnuloader: platform_a72q + "resflash" (read system image from partition darwin)
new-files/u-boot/          : new U-Boot source files (clock-sc7180.c, pinctrl-sc7180.c, cmd/hakmenu.c, a72q dts/defconfig)
device/                    : a72q device repo scaffold + source copies
No Apple files are included: build the iOS 16.5 system from your own IPSW (sysimage/unpack.sh).
Status: U-Boot, screen, UFS and XNU kernel boot on the phone. iOS user space from userdata: in progress (v6).
