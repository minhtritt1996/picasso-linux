#!/bin/sh
# Mount the phone's Android firmware partitions read-only and build the single
# firmware directory the kernel drivers and pd-mapper look at:
#
#   /lib/firmware/qcom/sm7250/xiaomi/picasso/image -> /run/fw-image  (tmpfs)
#
# The image directory is a tmpfs of symlinks rather than the mounted ext4
# itself because pd-mapper only scans that directory, and this port has to add
# PD maps the vendor's files do not carry (see the tms/pddump_disabled block).
set -u

FW=/lib/firmware/qcom/sm7250/xiaomi/picasso
IMG=/run/fw-image
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

mkdir -p "$FW" "$IMG" "$BT" /lib/firmware/ath10k/picasso /lib/firmware/ath10k/WCN3990/hw1.0

for p in modem dsp; do
	mount_ro "$p" "/usr/lib/firmware-android-$p" || continue
	for f in "/usr/lib/firmware-android-$p/image"/*; do
		[ -f "$f" ] || continue
		ln -sf "$f" "$IMG/$(basename "$f")"
	done
done

if mount_ro bluetooth /usr/lib/firmware-android-bt; then
	for f in /usr/lib/firmware-android-bt/* /usr/lib/firmware-android-bt/image/*; do
		[ -f "$f" ] || continue
		ln -sf "$f" "$BT/$(basename "$f")"
	done
fi

# the WCN3990 board data: ath10k looks in ath10k/WCN3990/hw1.0/board.bin
[ -e "$IMG/bdf_g7a.bin" ] && ln -sf "$IMG/bdf_g7a.bin" /lib/firmware/ath10k/WCN3990/hw1.0/board.bin
for f in "$IMG"/bdwlan* "$IMG"/bdf_*.bin; do
	[ -e "$f" ] || continue
	ln -sf "$f" "/lib/firmware/ath10k/picasso/$(basename "$f")"
done

# The Xiaomi PD maps carry tms/pdr_enabled and tms/servreg but *not*
# tms/pddump_disabled, which the modem asks for right after boot: pd-mapper
# answers 0 domains and the modem's PD watchdog then fires
# (dog_hal_common.c:180).  Declare the missing service for each root PD.
mk_tms() {
	sub=$1; inst=$2; dom=$3
	cat > "$IMG/tms-pddump-$dom-$sub.jsn" <<JSON
{
	"sr_domain": {
		"soc": "msm",
		"domain": "$dom",
		"subdomain": "$sub",
		"qmi_instance_id": $inst
	},
	"sr_service": [
		{ "provider": "tms", "service": "pddump_disabled" }
	]
}
JSON
}
mk_tms root_pd 180 modem
mk_tms audio_pd 74 adsp
mk_tms root_pd  76 cdsp

# expose the tmpfs image dir where the drivers expect the mounted partition
rm -rf "$FW/image"
ln -sfn "$IMG" "$FW/image"

# tqftpserv serves the same files to the modem
mkdir -p /var/lib/tqftpserv/readonly /var/lib/tqftpserv/readwrite
for f in "$IMG"/*; do
	[ -e "$f" ] || continue
	ln -sf "$f" "/var/lib/tqftpserv/readonly/$(basename "$f")"
	ln -sf "$f" "/var/lib/tqftpserv/readwrite/$(basename "$f")"
done

echo "fw-mount: $(ls "$IMG" | wc -l) firmware files (mounted image dir: $(readlink "$FW/image"))"
exit 0
