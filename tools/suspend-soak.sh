#!/usr/bin/env bash
# Sleep/wake soak for a phone whose only wake source is a key (no RTC alarm):
# each cycle suspends, waits for the wake, and records the timing and whether the
# radios came back. Run it ON the phone, detached, so a console hangup during the
# cycle cannot kill it:
#
#   setsid tools/suspend-soak.sh 10 /mnt/ufs-logs/soak.log VOLUME-DOWN >/dev/null 2>&1 &
#
# The person holding the phone presses VOLUME-UP once per cycle. The script says
# so on the panel (console) before each suspend.
set -u
N=${1:-10}
LOG=${2:-/mnt/ufs-logs/soak.log}
KEY=${3:-VOLUME-DOWN}
mkdir -p "$(dirname "$LOG")" 2>/dev/null

say() { printf '%s %s\n' "$(date -Is)" "$*" | tee -a "$LOG" > /dev/tty1 2>/dev/null || true; }

say "soak: $N cycles, wake each one with $KEY"
ok=0
for i in $(seq 1 "$N"); do
	before=$(cat /sys/power/suspend_stats/success 2>/dev/null)
	t0=$(date +%s)
	echo "PM: soak cycle $i/$N, press $KEY to wake" > /dev/tty1 2>/dev/null || true
	echo mem > /sys/power/state
	t1=$(date +%s)
	after=$(cat /sys/power/suspend_stats/success 2>/dev/null)
	leas=$(ip -4 -br addr show wlan0 2>/dev/null | awk '{print $3}')
	[ "$after" != "$before" ] && ok=$((ok + 1))
	say "cycle $i: ${t1}s measured $((t1 - t0))s | success $before -> $after | wlan0=${leas:-down}"
	dmesg | grep -E "PM: resume devices took|PM: suspend exit" | tail -2 >> "$LOG" 2>/dev/null
	sleep 2
done
say "soak done: $ok/$N cycles counted by suspend_stats (wake key $KEY)"
