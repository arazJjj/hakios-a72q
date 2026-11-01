#!/usr/bin/env bash
# HakiOS a72q: build the iOS 16.5 images in a GitHub Codespace (Ubuntu in the browser).
#   bash codespace-build.sh
# Leaves out/root.apfs.zst and out/system.apfs.zst (split in 1.5 GB parts if bigger) to download.
# The IPSW comes from Apple's CDN through ipsw.me, into this Codespace only; it is never committed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HOME/hakios}"
OUT="$HERE/out"
mkdir -p "$OUT"

echo "== tools"
sudo apt-get update -qq
sudo apt-get install -y -qq 7zip clang lld lz4 python3 git make curl zstd

echo "== HakiOS common repository"
if [ ! -d "$WORK/.git" ]; then
	git clone --depth 1 https://github.com/oosexgang/hakios-xnu-kernel.git "$WORK"
fi
cd "$WORK"
cp "$HERE/tools/fetch-ipsw.sh" tools/fetch-ipsw.sh
chmod +x tools/fetch-ipsw.sh
tools/doctor.sh || true

echo "== iOS 16.5 IPSW (ipsw.me), about 6.2 GB"
OUT_IPSW="$WORK/ipsw"
NO_UNPACK=1 OUT="$OUT_IPSW" tools/fetch-ipsw.sh iPhone10,3 20F66
IPSW="$(ls "$OUT_IPSW"/*.ipsw | head -1)"

echo "== unpack (about 5 min)"
sysimage/unpack.sh "$IPSW"

echo "== build root.apfs + system.apfs (about 4 min)"
PREBUILT=1 REST=1 SYSTEM=1 userland/build.sh

echo "== free the disk: IPSW removed"
rm -f "$IPSW"

echo "== pack for download"
cd userland
for f in root.apfs system.apfs; do
	zstd -T0 -10 -f "$f" -o "$OUT/$f.zst"
	if [ "$(stat -c %s "$OUT/$f.zst")" -gt 1610612736 ]; then
		split -b 1500M -d "$OUT/$f.zst" "$OUT/$f.zst.part"
		rm "$OUT/$f.zst"
	fi
done
( cd "$OUT" && sha256sum * > SHA256SUMS )
ls -lh "$OUT"
cat <<MSG

Done. In the file list on the left, open out/, right-click each file, Download.
On Windows (Git Bash):
  cat system.apfs.zst.part* > system.apfs.zst     # only if it was split
  zstd -d root.apfs.zst ; zstd -d system.apfs.zst
Then put both in userland/ of your Windows HakiOS checkout and:
  fastboot boot boot.img
  ./xl ramdisk userland/system.apfs
  XL_SECS=600 ./xl run
Stop the Codespace afterwards (github.com/codespaces) to keep your free hours.
MSG
