#!/bin/sh
# systemd-sleep hook: capture the kernel log immediately before and after a
# suspend. If the resume kills the SoC (PS_HOLD drop on picasso), the "pre"
# dump is what survives and the "post" dump being missing is the signal.
set -u
case "$1" in
	pre|post) ;;
	*) exit 0 ;;
esac
MNT=/mnt/ufs-logs
DEV=/dev/disk/by-partlabel/cache
[ -b "$DEV" ] || exit 0
mkdir -p "$MNT" 2>/dev/null || exit 0
if ! mountpoint -q "$MNT"; then
	mount -t ext4 -o rw,noatime,sync "$DEV" "$MNT" 2>/dev/null || exit 0
	mounted=1
else
	mounted=0
fi
mkdir -p "$MNT/picasso-logs" 2>/dev/null || exit 0
out="$MNT/picasso-logs/sleep-$1-$(date +%Y%m%d-%H%M%S).log"
{ date -Is; cat /sys/power/suspend_stats/success /sys/power/suspend_stats/fail 2>/dev/null; dmesg; } > "$out" 2>&1
sync
[ "$mounted" = 1 ] && umount "$MNT"
exit 0
