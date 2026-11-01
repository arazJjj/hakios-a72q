#!/usr/bin/env bash
# HakiOS a72q: fetch the iOS 16.5 IPSW for the identity the loader carries (iPhone X, iPhone10,3 / iPhone10,6)
# through the ipsw.me API, check its SHA-1, then unpack it with sysimage/unpack.sh.
# Run on your own computer: the IPSW is Apple's and is never put in this repository.
#   tools/fetch-ipsw.sh [device] [build]      default: iPhone10,3 20F66 (16.5)
set -euo pipefail
DEVICE="${1:-iPhone10,3}"
BUILD="${2:-20F66}"
OUT="${OUT:-ipsw}"
mkdir -p "$OUT"

need() { command -v "$1" >/dev/null || { echo "need $1" >&2; exit 1; }; }
need curl; need python3

echo "ipsw.me: $DEVICE build $BUILD"
INFO="$(curl -fsSL "https://api.ipsw.me/v4/ipsw/$DEVICE/$BUILD")"
read -r URL SHA1 SIZE VERSION < <(python3 -c '
import json,sys; f=json.loads(sys.argv[1])
print(f["url"], f["sha1sum"], f["filesize"], f["version"])' "$INFO")

FILE="$OUT/$(basename "$URL")"
echo "iOS $VERSION, $((SIZE/1024/1024)) MB"
echo "  $URL"

# -C - resumes a broken download
curl -fL -C - -o "$FILE" "$URL"

echo "checking SHA-1"
GOT="$(sha1sum "$FILE" | cut -d' ' -f1)"
if [ "$GOT" != "$SHA1" ]; then
	echo "SHA-1 mismatch: $GOT, ipsw.me says $SHA1. Delete $FILE and run again." >&2
	exit 1
fi
echo "ok: $FILE"

if [ -n "${NO_UNPACK:-}" ]; then
	exit 0
elif [ -x sysimage/unpack.sh ]; then
	sysimage/unpack.sh "$FILE"
else
	echo "sysimage/unpack.sh not found here: run it from the hakios-xnu-kernel tree with $FILE"
fi
