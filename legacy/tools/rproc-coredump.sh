#!/bin/sh
# Enable remoteproc coredumps so a DSP that dies (the modem's DOG, the ADSP's
# sensor PD) leaves its memory behind instead of a bare "crash detected".
# The dump is then readable at /sys/kernel/debug/remoteproc/remoteprocN/coredump
set -u
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
for d in /sys/kernel/debug/remoteproc/remoteproc*; do
	[ -e "$d/coredump" ] || continue
	echo enabled > "$d/coredump" 2>/dev/null && echo "coredump enabled: $d"
done
