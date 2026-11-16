#!/bin/sh
# Persist this boot's systemd + kernel log to the phone's UFS, so it survives a
# power cycle (RAM, ramoops and the journal in RAM do not).
#
# Target: the Android "cache" partition (ext4, ~380 MB, wiped by Android as it
# pleases); fallback "metadata" (ext4, 16 MB).  "userdata" is NOT usable: it is
# metadata-encrypted (dm-default-key), so f2fs finds no superblock there.
#
# Runs from log-to-ufs.timer every 30 s: journal + dmesg + cmdline, then the
# partition is unmounted again so nothing else sees it mounted.
set -u

pick_dev() {
	for p in cache metadata; do
		d=/dev/disk/by-partlabel/$p
		[ -b "$d" ] && { echo "$d"; return 0; }
	done
	return 1
}

DEV=$(pick_dev) || exit 0
MNT=/mnt/ufs-logs
DIR=$MNT/picasso-logs

mkdir -p "$MNT" || exit 0
if ! mountpoint -q "$MNT"; then
	mount -t ext4 -o rw,noatime,sync "$DEV" "$MNT" 2>/dev/null || exit 0
	mounted=1
else
	mounted=0
fi
mkdir -p "$DIR" 2>/dev/null || { [ "$mounted" = 1 ] && umount "$MNT"; exit 0; }

STAMP=$(date +%Y%m%d-%H%M%S)
journalctl -b --no-pager -o short-monotonic > "$DIR/journal-$STAMP.log" 2>/dev/null || true
dmesg > "$DIR/dmesg-$STAMP.log" 2>/dev/null || true
# A resume that kills the SoC leaves no post-log, so also keep one rolling copy
# that a sleep hook can append to without a new mount.
dmesg > "$DIR/dmesg-last.log" 2>/dev/null || true
cp /proc/cmdline "$DIR/cmdline-$STAMP.txt" 2>/dev/null || true
sync
# keep the newest 20 boots, and never fill the partition
ls -1t "$DIR"/journal-*.log 2>/dev/null | tail -n +21 | xargs -r rm -f
ls -1t "$DIR"/dmesg-*.log 2>/dev/null | tail -n +21 | xargs -r rm -f
[ "$mounted" = 1 ] && umount "$MNT"
exit 0
