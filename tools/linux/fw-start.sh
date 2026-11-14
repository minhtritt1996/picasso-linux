#!/bin/sh
# Start the DSPs now that their firmware is mounted (fw-mount.service) and the
# QRTR services serve the PD map and the EFS (pd-mapper, rmtfs, tqftpserv):
# unbind/bind qcom_q6v5_pas so the second probe finds the firmware, then start
# the modem and wait for it to stay up.  A modem started before those services
# never exposes msm/modem/wlan_pd and its own watchdog kills it.
set -u

# Bring the modem (and with it the WLAN firmware service) up cleanly: the
# kernel starts it at boot before this service has mounted the firmware or the
# QRTR PD services are serving, and a half-started modem never exposes
# msm/modem/wlan_pd.  Stop it, wait, then start it and retry a few times.
for r in /sys/class/remoteproc/remoteproc*; do
	[ -e "$r/name" ] || continue
	[ "$(cat "$r/name")" = "modem" ] || continue

	i=0
	while [ $i -lt 3 ]; do
		i=$((i + 1))
		if [ "$(cat "$r/state")" = "running" ]; then
			echo stop > "$r/state" 2>/dev/null || true
			sleep 2
		fi
		echo start > "$r/state" 2>/dev/null || true
		sleep 6
		[ "$(cat "$r/state")" = "running" ] && break
	done
done

echo "fw-setup: $(ls "$FW/image" 2>/dev/null | wc -l) firmware files"
exit 0
