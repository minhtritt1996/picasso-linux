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
# service state: tqftpserv refuses to start without its readwrite directory,
# pd-mapper keeps its PD map here
mkdir -p "$TMP/var/lib/tqftpserv/readonly" "$TMP/var/lib/tqftpserv/readwrite" \
	 "$TMP/var/lib/pd-mapper"
"$CC" -static -Os -s -o "$TMP/init" "$ROOT/tools/linux/init.c"
chmod 755 "$TMP/init"

# GPU firmware (vendor blobs, kept out of git): zap shader + GMU + SQE
if [ -d "$ROOT/prebuilt/fw-gpu" ]; then
	mkdir -p "$TMP/lib/firmware/qcom"
	cp "$ROOT"/prebuilt/fw-gpu/* "$TMP/lib/firmware/qcom/"
	echo "    [OK] gpu firmware: $(ls "$ROOT"/prebuilt/fw-gpu | wc -l) files"
fi

# kernel modules needed at runtime (audio, wifi), loaded by init from load.order
KMOD="$ROOT/build/linux/out"
if [ -f "$KMOD/modules.order" ]; then
	mkdir -p "$TMP/lib/modules"
	: > "$TMP/lib/modules/load.order"
	while read -r m; do
		case "$m" in
		*snd-soc-qdsp6*|*snd-soc-sm8250*|*snd-soc-qcom*|*snd-soc-wcd938x*|*snd-soc-wsa881x*|*snd-soc-tfa9874*|*soundwire*|*ath10k*)
			b=$(basename "$m")
			if [ -f "$KMOD/$b" ]; then
				cp "$KMOD/$b" "$TMP/lib/modules/$b"
				echo "$b" >> "$TMP/lib/modules/load.order"
			fi
			;;
		esac
	done < "$KMOD/modules.order"
	echo "    [OK] modules: $(wc -l < "$TMP/lib/modules/load.order") packed"
fi

# prebuilt static helpers (rmtfs, pd-mapper): see tools/linux/bin/README
mkdir -p "$TMP/bin" "$TMP/run/rmtfs"
"$CC" -static -Os -s -o "$TMP/bin/tone" "$ROOT/tools/linux/tone/tone.c"
if compgen -G "$ROOT/tools/linux/bin/*" >/dev/null; then
	cp "$ROOT"/tools/linux/bin/* "$TMP/bin/"
	chmod 755 "$TMP"/bin/*
fi

mkdir -p "$(dirname "$OUT")"
(cd "$TMP" && find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | gzip -9) > "$OUT"
ls -l "$OUT"
