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

# Start the ADSP as well: it is the audio DSP, and like the modem it probes
# before fw-mount has put adsp.mdt in place, so the first load fails with
# ENOENT and the remoteproc stays offline forever. Without this the audio card
# defers ("MultiMedia1: error getting cpu dai name") and there is no sound.
for r in /sys/class/remoteproc/remoteproc*; do
	[ -e "$r/name" ] || continue
	[ "$(cat "$r/name")" = "adsp" ] || continue
	[ "$(cat "$r/state")" = "running" ] && continue
	i=0
	while [ $i -lt 3 ]; do
		i=$((i + 1))
		echo start > "$r/state" 2>/dev/null || true
		sleep 2
		[ "$(cat "$r/state")" = "running" ] && break
	done
done

# The sound card only appears when the ADSP is up. Retry the route until the
# control actually takes (the card registers a second or two after the ADSP
# reports running, and the mixer is writable from then on), then store it so the
# next boot restores it before this service even runs.
i=0
route=
while [ $i -lt 60 ]; do
	i=$((i + 1))
	if amixer -c 0 cset name='SEC_MI2S_RX Audio Mixer MultiMedia1' 1 >/dev/null 2>&1; then
		route=on
		break
	fi
	sleep 0.5
done
if [ "$route" = on ]; then
	amixer -c 0 cset name='Speaker' 80% unmute >/dev/null 2>&1 || true
	alsactl store -f /var/lib/alsa/asound.state >/dev/null 2>&1 || true
	echo "fw-setup: audio route on (after $i tries)"
else
	echo "fw-setup: audio route could not be set (no card after 30 s)" >&2
fi

echo "fw-setup: $(ls "$FW/image" 2>/dev/null | wc -l) firmware files"
exit 0
