#!/usr/bin/env bash
# Suspend test for the RAM-booted phone: arm an RTC wake FIRST, verify it, then
# suspend. An s2idle suspend with no armed wake source hangs the phone until
# someone holds the power key, which powers it off - so this script refuses to
# suspend unless it can read back a wakealarm.
#
# Usage:
#   tools/suspend-test.sh [--seconds N] [--pm-test LEVEL] [--dry-run]
#
#   --seconds N     RTC alarm in N seconds (default 30)
#   --pm-test LVL   write LVL to /sys/power/pm_test and keep it for the run;
#                   requires the kernel to expose that file
#                   (CONFIG_PM_SLEEP_DEBUG). The ladder is
#                   freezer -> devices -> platform -> processors -> core
#   --dry-run       only arm + verify + disarm the alarm, never suspend
#
# After the wake it prints /sys/power/suspend_stats/*, the last PM dmesg lines
# and whether the USB console and Wi-Fi came back.
set -euo pipefail

RTC=/sys/class/rtc/rtc0
SECS=30
PM_TEST=
DRY=0
while [ $# -gt 0 ]; do
	case "$1" in
		--seconds) SECS=${2:?}; shift 2 ;;
		--pm-test) PM_TEST=${2:?}; shift 2 ;;
		--dry-run) DRY=1; shift ;;
		-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "usage: $0 [--seconds N] [--pm-test LEVEL] [--dry-run]" >&2; exit 2 ;;
	esac
done

say() { printf 'suspend-test: %s\n' "$*"; }
die() { printf 'suspend-test: %s\n' "$*" >&2; exit 1; }

[ -w /sys/power/state ] || die "no /sys/power/state"
say "states: $(cat /sys/power/state)  mem_sleep: $(cat /sys/power/mem_sleep 2>/dev/null)"
say "wakeup devices: $(grep -c . /sys/kernel/debug/wakeup_sources 2>/dev/null || echo '?')"

if [ -n "$PM_TEST" ]; then
	[ -w /sys/power/pm_test ] || die "no /sys/power/pm_test (need CONFIG_PM_SLEEP_DEBUG)"
	echo "$PM_TEST" > /sys/power/pm_test
	say "pm_test=$(cat /sys/power/pm_test) (returns after each level, safe)"
fi

# Arm and verify the wake alarm. rtcwake writes the alarm; we read it back,
# because an unarmed alarm is exactly the failure that hangs the phone.
say "arming RTC alarm in ${SECS}s"
rtcwake -m no -d /dev/rtc0 -s "$SECS" >/dev/null || die "rtcwake could not arm the alarm"
alarm=$(cat "$RTC/wakealarm" 2>/dev/null || true)
[ -n "$alarm" ] || die "wakealarm is empty after arming: refusing to suspend"
say "wakealarm=$alarm (now $(date +%s))"

if [ "$DRY" -eq 1 ]; then
	rtcwake -m disable -d /dev/rtc0 >/dev/null || true
	say "dry run: alarm armed and verified, disarmed, not suspending"
	exit 0
fi

before=$(date +%s)
say "suspending (mem); the RTC wakes it in ${SECS}s"
if ! rtcwake -m mem -d /dev/rtc0 -s "$SECS"; then
	say "rtcwake returned an error; check dmesg"
fi
after=$(date +%s)
say "awake again after $((after - before))s"

say "suspend_stats:"
for f in "$RTC"/../../power/suspend_stats/*; do :; done 2>/dev/null || true
for f in /sys/power/suspend_stats/*; do printf '  %-18s %s\n' "${f##*/}" "$(cat "$f" 2>/dev/null)"; done
say "last PM lines:"
dmesg | grep -iE "PM: |suspend|resume|s2idle" | tail -12 || true
[ -e /dev/ttyGS0 ] && say "USB gadget: present" || say "USB gadget: MISSING"
ip -br addr show wlan0 2>/dev/null | sed 's/^/  /' || true
