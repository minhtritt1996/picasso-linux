#!/bin/bash
# Pack a userspace tree into the gzip cpio the kernel takes as an initramfs,
# for the loader's resident-ramdisk channel:
#
#   tools/pack-rootfs.sh /tmp/armroot2 build/out/fedora.cpio.gz
#   fastboot oem ramdisk
#   fastboot stage build/out/fedora.cpio.gz
#   fastboot stage build/out/payload-linux.bin     (PAYLOAD_NO_INITRD=1)
#   fastboot oem run
#
# Ownership comes from -R 0:0, so no root privilege is needed as long as every
# file in the tree is world-readable (dnf --installroot makes them so).
set -euo pipefail
SRC=${1:?usage: pack-rootfs.sh TREE OUT.cpio.gz}
OUT=${2:?usage: pack-rootfs.sh TREE OUT.cpio.gz}
[ -d "$SRC" ] || { echo "pack-rootfs: $SRC is not a directory" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
# -1: a 1.5 GiB tree at gzip -9 takes many minutes for a few percent
(cd "$SRC" && find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | gzip -1) > "$OUT"
ls -l "$OUT"
echo "pack-rootfs: $(du -sh --exclude=var/cache "$SRC" 2>/dev/null | cut -f1) tree -> $(stat -c%s "$OUT") bytes"
