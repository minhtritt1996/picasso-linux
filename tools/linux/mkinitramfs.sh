#!/bin/bash
# Build the picasso mainline ramdisk (initramfs): static init + empty dirs.
# The root filesystem is always this ramdisk, packed as entry "initrd" and
# handed over through /chosen/linux,initrd-*.
#
#   tools/linux/mkinitramfs.sh [OUT]   default: build/out/initrd.cpio.gz
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
OUT=${1:-$ROOT/build/out/initrd.cpio.gz}
CC=${CC:-aarch64-linux-gnu-gcc}
TMP="$ROOT/build/initramfs"

command -v "$CC" >/dev/null || { echo "mkinitramfs: $CC not found" >&2; exit 1; }
command -v cpio >/dev/null || { echo "mkinitramfs: cpio not found" >&2; exit 1; }

rm -rf "$TMP"
mkdir -p "$TMP/dev" "$TMP/proc" "$TMP/sys" "$TMP/tmp" "$TMP/run"
"$CC" -static -Os -s -o "$TMP/init" "$ROOT/tools/linux/init.c"
chmod 755 "$TMP/init"

# prebuilt static helpers (rmtfs, pd-mapper): see tools/linux/bin/README
mkdir -p "$TMP/bin" "$TMP/run/rmtfs"
if compgen -G "$ROOT/tools/linux/bin/*" >/dev/null; then
	cp "$ROOT"/tools/linux/bin/* "$TMP/bin/"
	chmod 755 "$TMP"/bin/*
fi

mkdir -p "$(dirname "$OUT")"
(cd "$TMP" && find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | gzip -9) > "$OUT"
ls -l "$OUT"
