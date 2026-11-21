#!/bin/bash
# Build the picasso mainline ramdisk (initramfs): static init + empty dirs.
# The root filesystem is always this ramdisk, packed as entry "initrd" and
# handed over through /chosen/linux,initrd-*.
#
#   scripts/mkinitramfs.sh [OUT]   default: build/out/initrd.cpio.gz
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
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
"$CC" -static -Os -s -o "$TMP/init" "$ROOT/initramfs/init.c"
chmod 755 "$TMP/init"

# GPU firmware (vendor blobs, kept out of git): zap shader + GMU + SQE
if [ -d "$ROOT/prebuilt/fw-gpu" ]; then
	mkdir -p "$TMP/lib/firmware/qcom"
	cp "$ROOT"/prebuilt/fw-gpu/* "$TMP/lib/firmware/qcom/"
	echo "    [OK] gpu firmware: $(ls "$ROOT"/prebuilt/fw-gpu | wc -l) files"
fi

# Novatek touchscreen firmware (vendor blob, kept out of git)
if [ -d "$ROOT/prebuilt/fw-touch" ]; then
	mkdir -p "$TMP/lib/firmware/novatek"
	cp "$ROOT"/prebuilt/fw-touch/* "$TMP/lib/firmware/novatek/"
	echo "    [OK] touch firmware: $(ls "$ROOT"/prebuilt/fw-touch | wc -l) files"
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

# ath10k WCN3990 metadata container (60 bytes; modem supplies WLAN firmware via TFTP)
if [ -f "$ROOT/prebuilt/fw-wcn3990/firmware-5.bin" ]; then
	mkdir -p "$TMP/lib/firmware/ath10k/WCN3990/hw1.0"
	cp "$ROOT/prebuilt/fw-wcn3990/firmware-5.bin" "$TMP/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin"
elif [ -f /lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin ]; then
	mkdir -p "$TMP/lib/firmware/ath10k/WCN3990/hw1.0"
	cp /lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin "$TMP/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin"
fi

# prebuilt static helpers (rmtfs, pd-mapper): see initramfs/bin/README.md
mkdir -p "$TMP/bin" "$TMP/run/rmtfs"
"$CC" -static -Os -s -o "$TMP/bin/tone" "$ROOT/initramfs/tone/tone.c"
"$CC" -static -Os -s -Wall -Wextra -Werror -o "$TMP/bin/picasso-gnss" "$ROOT/initramfs/gnss/picasso-gnss.c"
if compgen -G "$ROOT/initramfs/bin/*" >/dev/null; then
	for f in "$ROOT"/initramfs/bin/*; do
		case "$f" in *.md) continue ;; esac
		cp "$f" "$TMP/bin/"
	done
	chmod 755 "$TMP"/bin/*
fi

compgen -G "$ROOT/initramfs/bin/*" | grep -qv 'README' || echo "    [WARN] initramfs/bin has no helpers (see initramfs/bin/README.md): modem services will be missing" >&2

# Device camera profiles/calibrations (braun) & PipeWire speaker DSP config
# Staged into /share inside initramfs and bind-mounted onto /newroot at boot.
if compgen -G "$ROOT/initramfs/braun/sensors/*.toml" >/dev/null; then
	mkdir -p "$TMP/share/braun/sensors"
	cp "$ROOT"/initramfs/braun/sensors/*.toml "$TMP/share/braun/sensors/"
	chmod 0644 "$TMP"/share/braun/sensors/*.toml
fi
if compgen -G "$ROOT/initramfs/braun/calibrations/*.toml" >/dev/null; then
	mkdir -p "$TMP/share/braun/calibrations"
	cp "$ROOT"/initramfs/braun/calibrations/*.toml "$TMP/share/braun/calibrations/"
	chmod 0644 "$TMP"/share/braun/calibrations/*.toml
fi
if compgen -G "$ROOT/initramfs/pipewire/*.conf" >/dev/null; then
	mkdir -p "$TMP/share/pipewire"
	cp "$ROOT"/initramfs/pipewire/*.conf "$TMP/share/pipewire/"
	chmod 0644 "$TMP"/share/pipewire/*.conf
fi

mkdir -p "$(dirname "$OUT")"
if command -v xz >/dev/null; then
	(cd "$TMP" && find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | xz -9 --check=crc32 -c) > "$OUT"
else
	(cd "$TMP" && find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | gzip -9) > "$OUT"
fi
ls -l "$OUT"
