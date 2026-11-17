#!/usr/bin/env bash
# Hardware gate: one RAM-boot cycle on a picasso that is already in the loader.
#
# What it does (and nothing else): arm the resident ramdisk, stage the root
# filesystem and the payload with verified transfers, `oem run`, then collect
# the console output. It never flashes, never erases, never writes a block
# device: `tools/policy-scan.sh` enforces that on this file too.
#
# Environment:
#   ROOTFS   resident root filesystem image   (default build/out/fedora-rootfs.cpio.gz)
#   PAYLOAD  XNUP payload                     (default build/out/payload-linux.bin)
#   CAPTURE  seconds of console to log        (default 30)
#   REBOOT   1 to reboot a running kernel first (default 0)
#
# Usage: tools/ramboot-cycle.sh [--reboot] [--capture SECONDS]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

ROOTFS=${ROOTFS:-build/out/fedora-rootfs.cpio.gz}
PAYLOAD=${PAYLOAD:-build/out/payload-linux.bin}
CAPTURE=${CAPTURE:-30}
REBOOT=${REBOOT:-0}

while [ $# -gt 0 ]; do
	case "$1" in
		--reboot) REBOOT=1; shift ;;
		--capture) CAPTURE=$2; shift 2 ;;
		*) echo "usage: $0 [--reboot] [--capture SECONDS]" >&2; exit 2 ;;
	esac
done

for f in "$ROOTFS" "$PAYLOAD"; do
	[ -s "$f" ] || { echo "ramboot: $f is missing" >&2; exit 1; }
done
outdir=build/logs
mkdir -p "$outdir"

say() { printf 'ramboot: %s\n' "$*"; }

product=$(timeout 10 fastboot getvar product 2>&1 | head -n 1 || true)
case "$product" in
	*xnuloader*) say "loader present ($product)" ;;
	*)
		if [ "$REBOOT" -eq 1 ]; then
			say "no loader; asking the running kernel to reboot (console or ssh)"
			# ssh first (fast when the link is up), console as the fallback
			ssh -i "$HOME/.ssh/picasso_key" -o BatchMode=yes -o ConnectTimeout=4 \
				-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
				root@192.168.7.2 'reboot -f' >/dev/null 2>&1 ||
				python3 tools/console_cmd.py --reboot >/dev/null 2>&1 || true
			for _ in $(seq 1 80); do
				# a missing device makes fastboot fail, not the script
				p=$(timeout 2 fastboot getvar product 2>&1 | head -n 1 || true)
				case "$p" in *xnuloader*) say "loader up"; break ;; esac
				sleep 3
			done
		fi
		;;
esac
product=$(timeout 10 fastboot getvar product 2>&1 | head -n 1 || true)
case "$product" in
	*xnuloader*) ;;
	*) say "refusing to run: the phone is not in the RAM loader ($product)"; exit 1 ;;
esac

stage() { # FILE LABEL
	local o rc
	o=$(timeout 900 fastboot stage "$1" 2>&1); rc=$?
	printf '%s\n' "$o" | tail -n 2
	if [ $rc -ne 0 ] || ! printf '%s' "$o" | grep -qiE 'total time|OKAY' || printf '%s' "$o" | grep -qi 'FAILED'; then
		say "staging $2 failed"; return 1
	fi
	say "staged $2 ($(stat -c%s "$1") bytes)"
}

say "arming the resident ramdisk"
timeout 15 fastboot oem ramdisk >/dev/null
sz=$(timeout 5 fastboot getvar max-download-size 2>&1 | grep -o '0x[0-9a-f]*' | head -n 1)
say "max-download-size now $sz (0xffffffff means the resident channel is armed)"

stage "$ROOTFS" rootfs
stage "$PAYLOAD" payload

say "oem run"
timeout 30 fastboot oem run >/dev/null
for _ in $(seq 1 120); do [ -e /dev/ttyACM0 ] && break; sleep 2; done
if [ ! -e /dev/ttyACM0 ]; then
	say "no USB console after 240 s; check the panel"
	exit 1
fi
say "console enumerated; capturing ${CAPTURE}s to $outdir/console.log"
CAPTURE="$CAPTURE" OUT="$root/$outdir/console.log" tools/console_cmd.py --capture
say "done; log in $outdir/console.log"
