#!/usr/bin/env bash
# HakiOS a72q: pack the HakiOS boot image (U-Boot + loader) as an Odin AP file.
#   tools/odin-pack.sh [boot.img] [out.tar.md5]
# Needs an unlocked bootloader (OEM unlock) and vbmeta with verification off,
# or Odin/the phone refuses it ("Only official released binaries are allowed").
set -euo pipefail
IMG="${1:-device/hakios/hakios-a72q-boot.img}"
OUT="${2:-AP_hakios_a72q.tar.md5}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp "$IMG" "$TMP/boot.img"
# Odin wants a plain ustar archive, then the md5 of that archive appended as text
tar -H ustar -C "$TMP" -cf "$TMP/ap.tar" boot.img
( cd "$TMP" && md5sum ap.tar ) | sed "s/ap.tar/$(basename "${OUT%.md5}")/" >> "$TMP/ap.tar"
mv "$TMP/ap.tar" "$OUT"
echo "Odin: load $OUT in AP, untick Auto Reboot, Start."
echo "Then hold Volume Down as it boots for the HakiOS developer menu."
