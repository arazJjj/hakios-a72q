# HakiOS on the Samsung Galaxy A72 (a72q)

Port of [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) to the SM-A725F (SM7125). See README.txt for state.

## Build iOS 16.5 without Linux: GitHub Actions (easiest)
1. Actions tab, Build iOS images, Run workflow.
2. About 30 min later, the Releases page has root.apfs.zst and system.apfs.zst (or parts) to download.
3. Keep this repo private: the images come from Apple's IPSW.

## Or: GitHub Codespaces
1. On this repo: Code, Codespaces, Create codespace on main.
2. In its terminal: `bash codespace-build.sh` (about 20 min, downloads the IPSW through ipsw.me).
3. Download `out/root.apfs.zst` and `out/system.apfs.zst` (or their parts) and run them from Windows: see the end of the script's output.

No Apple files are in this repository; they are fetched into the Codespace and stay out of git (.gitignore).

## Flashing
- `fastboot boot device/hakios/hakios-a72q-boot.img` (RAM only), or Odin: `tools/odin-pack.sh`, see ODIN.txt.
