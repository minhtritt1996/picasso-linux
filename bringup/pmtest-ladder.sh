#!/usr/bin/env bash
# Run the kernel's suspend test ladder on the phone and log it where the log
# survives: the USB console disappears at the "devices" level and its shell dies
# with the hangup, so the results go to the UFS cache partition, not to stdout.
#
# Run it ON the phone (the RAM root filesystem), detached:
#   setsid bringup/pmtest-ladder.sh >/dev/null 2>&1 &
#
# Ladder: freezer -> devices -> platform -> processors -> core, then a real
# s2idle suspend. Each level makes the kernel run the suspend sequence and
# resume immediately (PM: suspend debug: Waiting for 5 second(s)), which names
# the first subsystem that cannot come back.
set -u

LOG=${LOG:-/cache/pmtest.log}
[ -w /sys/power/pm_test ] || { echo "no pm_test" >&2; exit 1; }

mkdir -p "$(dirname "$LOG")" 2>/dev/null || true
: > "$LOG"
say() { echo "$(date +%s.%N) $*" >> "$LOG"; }

say "start: state=$(cat /sys/power/state) mem_sleep=$(cat /sys/power/mem_sleep)"

for level in freezer devices platform processors core; do
	echo "$level" > /sys/power/pm_test
	before=$(date +%s)
	echo mem > /sys/power/state
	rc=$?
	after=$(date +%s)
	say "$level rc=$rc took=$((after - before))s"
	dmesg | grep -iE "PM: suspend|PM: resume|Freezing|platform|devices" | tail -3 >> "$LOG" 2>/dev/null
done

echo none > /sys/power/pm_test
say "ladder done, pm_test=$(cat /sys/power/pm_test)"

# Stats and the wake-capable devices, useful when a level fails.
for f in /sys/power/suspend_stats/*; do
	printf '%s %s\n' "${f##*/}" "$(cat "$f" 2>/dev/null)" >> "$LOG"
done
say "wakeup sources:"
grep -E "^(name|[a-z].*)[[:space:]]" /sys/kernel/debug/wakeup_sources 2>/dev/null | head -20 >> "$LOG"
say "end"
