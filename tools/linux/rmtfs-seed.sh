#!/bin/sh
# rmtfs serves the modem's EFS. Fedora's unit runs it without any storage, so
# the modem reads garbage and its watchdog kills it a few seconds after boot.
# Seed the four blobs from the phone's own partitions, exactly like the small
# initramfs does: modemst1/modemst2 (8 MiB), fsc (64 KiB), fsg (8 MiB).
set -u

DIR=/run/rmtfs
mkdir -p "$DIR"

# udev may not have created the partlabel links yet
i=0
while [ $i -lt 40 ] && [ ! -e /dev/disk/by-partlabel/modemst1 ]; do
	sleep 0.5
	i=$((i + 1))
done

seed() {
	part=$1; out=$2; max=$3
	dev=/dev/disk/by-partlabel/$part

	[ -b "$dev" ] || return 0
	# never read past the real partition
	sectors=$(cat /sys/class/block/$(basename $(readlink -f "$dev"))/size 2>/dev/null || echo 0)
	[ "$sectors" -gt 0 ] 2>/dev/null && [ $((sectors * 512)) -lt "$max" ] && max=$((sectors * 512))
	dd if="$dev" of="$DIR/$out" bs=64k count=$(( (max + 65535) / 65536 )) 2>/dev/null || true
	echo "rmtfs-seed: $part -> $DIR/$out ($(stat -c%s "$DIR/$out" 2>/dev/null || echo 0) bytes)"
}

seed modemst1 modem_fs1 $((8 * 1024 * 1024))
seed modemst2 modem_fs2 $((8 * 1024 * 1024))
seed fsc       modem_fsc $((64 * 1024))
seed fsg       modem_fsg $((8 * 1024 * 1024))
