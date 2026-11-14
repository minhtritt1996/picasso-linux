#!/bin/sh
# Mount the phone's Android firmware partitions read-only and link the files the
# kernel drivers ask for.  This must happen *before* pd-mapper starts: it reads
# its PD maps from /lib/firmware, and a first start with nothing mounted leaves
# it without maps (it then restarts, too late for the modem).
set -u

FW=/lib/firmware/qcom/sm7250/xiaomi/picasso
BT=/lib/firmware/qca

# udev creates /dev/disk/by-partlabel asynchronously; wait for it
i=0
while [ $i -lt 40 ] && [ ! -e /dev/disk/by-partlabel/modem ]; do
	sleep 0.5
	i=$((i + 1))
done

mount_ro() {
	dev=/dev/disk/by-partlabel/$1
	mp=$2

	[ -b "$dev" ] || return 1
	mkdir -p "$mp" || return 1
	mountpoint -q "$mp" && return 0
	mount -t ext4 -o ro,noatime "$dev" "$mp" 2>/dev/null && return 0
	mount -t vfat -o ro,noatime "$dev" "$mp" 2>/dev/null && return 0
	return 1
}

mkdir -p "$FW/image" "$BT"
# ath10k_snoc asks for <dir>/<board_name>/<file> with board_name = the DT's
# "firmware-name" ("picasso"), and the WCN3990 board/firmware files live on the
# modem partition next to wlanmdsp.mbn
mkdir -p /lib/firmware/ath10k/picasso /lib/firmware/ath10k/WCN3990/hw1.0

for p in modem dsp; do
	if mount_ro "$p" "/usr/lib/firmware-android-$p"; then
		if [ -d "/usr/lib/firmware-android-$p/image" ]; then
			for f in "/usr/lib/firmware-android-$p/image"/*; do
				[ -f "$f" ] || continue
				ln -sf "$f" "$FW/image/$(basename "$f")"
			done
		fi
	fi
done

if mount_ro bluetooth /usr/lib/firmware-android-bt; then
	for f in /usr/lib/firmware-android-bt/*; do
		[ -f "$f" ] || continue
		ln -sf "$f" "$BT/$(basename "$f")"
	done
	[ -d /usr/lib/firmware-android-bt/image ] && for f in /usr/lib/firmware-android-bt/image/*; do
		[ -f "$f" ] || continue
		ln -sf "$f" "$BT/$(basename "$f")"
	done
fi

for f in "$FW/image"/bdwlan* "$FW/image"/bdf_*.bin; do
	[ -e "$f" ] || continue
	ln -sf "$f" "/lib/firmware/ath10k/picasso/$(basename "$f")"
done
# the WCN3990 board data: ath10k looks in ath10k/WCN3990/hw1.0/board.bin;
# the Android bdf for this panel is bdf_g7a.bin (G7A) on the modem partition
[ -e "$FW/image/bdf_g7a.bin" ] && \
	ln -sf "$FW/image/bdf_g7a.bin" /lib/firmware/ath10k/WCN3990/hw1.0/board.bin

for drv in qcom_q6v5_pas ath10k_snoc; do
	[ -d /sys/bus/platform/drivers/$drv ] || continue
	for d in /sys/bus/platform/drivers/$drv/*.*; do
		[ -L "$d" ] || continue
		base=$(basename "$d")
		echo "$base" > /sys/bus/platform/drivers/$drv/unbind 2>/dev/null || true
		sleep 0.2
		echo "$base" > /sys/bus/platform/drivers/$drv/bind 2>/dev/null || true
	done
done


# The modem-hosted WLAN fetches its board/firmware files from the AP over
# TFTP (tqftpserv), which serves <statedir>/readonly and .../readwrite.
mkdir -p /var/lib/tqftpserv/readonly /var/lib/tqftpserv/readwrite
for f in "$FW/image"/*; do
	[ -f "$f" ] || continue
	ln -sf "$f" "/var/lib/tqftpserv/readonly/$(basename "$f")"
	ln -sf "$f" "/var/lib/tqftpserv/readwrite/$(basename "$f")"
done

echo "fw-mount: $(ls "$FW/image" 2>/dev/null | wc -l) firmware files, $(ls /var/lib/tqftpserv/readonly 2>/dev/null | wc -l) for tqftpserv"
exit 0
