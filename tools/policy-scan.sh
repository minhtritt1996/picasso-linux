#!/usr/bin/env bash
# Policy scan for picasso-linux (gate 1 of the four-gate workflow).
#
# The invariants it enforces, from AGENTS.md:
#   - RAM only: no `fastboot flash`/`erase`, no `dd … of=/dev/…`, no `mkfs … /dev/…`
#     in anything that runs. `fastboot boot|stage|oem ramdisk|oem run` stay legal.
#   - no machine-specific absolute path (/home/…, /Users/…) in scripts, configs
#     or CI. Documentation is exempt: AGENTS.md has to be able to state the rule.
#   - upstream/ is a read-only clone and is never tracked.
#   - patches/linux/UPSTREAM is the single source of truth for the pins and must
#     agree with build.sh; patches/linux/series must name existing patches.
#
# A line that must keep a listed pattern for a real reason carries
#     # policy-allow: <why>
# and is skipped.
#
# Usage: tools/policy-scan.sh [--root DIR]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
while [ $# -gt 0 ]; do
	case "$1" in
		--root) root=$(cd "$2" && pwd); shift 2 ;;
		*) echo "usage: $0 [--root DIR]" >&2; exit 2 ;;
	esac
done
cd "$root"

fail=0
note() { printf 'policy-scan: %s\n' "$*" >&2; }

exec_files=$(git ls-files '*.sh' '*.py')
path_files=$(git ls-files '*.sh' '*.py' '*.conf' '*.config' '*.service' '*.network' '*.yml' '*.yaml' '*.nmconnection' 'patches/linux/UPSTREAM')

# 1. Destructive operations in code. `fastboot boot/stage/oem` are the only
#    allowed fastboot verbs; anything that writes flash or a raw block device is
#    a violation.
if [ -n "$exec_files" ]; then
	hits=$(grep -nHE 'fastboot[[:space:]]+(flash|erase|format)|dd[[:space:]][^#]*of=/dev/|mkfs\.[a-z0-9]+[[:space:]]+/dev/|of=/dev/(sd|mmcblk|disk|block)' $exec_files \
		| grep -v 'policy-allow:' || true)
	if [ -n "$hits" ]; then
		printf '%s\n' "$hits" >&2
		note 'destructive flash/erase/raw-write command in code'
		fail=1
	fi
fi

# 2. Machine-specific absolute paths.
if [ -n "$path_files" ]; then
	# policy-allow: this line states the pattern it searches for
	hits=$(grep -nHE '(/home/[A-Za-z0-9._-]+|/Users/[A-Za-z0-9._-]+)' $path_files \
		| grep -v 'policy-allow:' || true)
	if [ -n "$hits" ]; then
		printf '%s\n' "$hits" >&2
		note 'machine-specific absolute path'
		fail=1
	fi
fi

# 3. upstream/ is read-only and never tracked.
if [ -n "$(git ls-files 'upstream/*' 'upstream/**' 2>/dev/null || true)" ]; then
	note 'upstream/ has tracked files: it is a read-only clone, never committed'
	fail=1
fi

# 4. The documents every change is supposed to have read.
for doc in AGENTS.md README.md STATUS.md docs/luma-port.md docs/mainline-linux-plan.md; do
	[ -s "$doc" ] || { note "$doc is missing or empty"; fail=1; }
done

# 5. Pins: patches/linux/UPSTREAM must agree with build.sh.
upstream_pins=patches/linux/UPSTREAM
if [ -s "$upstream_pins" ]; then
	for repo in linux avicii-mainline hakios-xnu-kernel; do
		want=$(awk -v r="$repo:" '$1 == r {found=1} found && /revision:/ {print $2; exit}' "$upstream_pins")
		[ -n "${want:-}" ] || continue
		got=$(grep -oE "^[A-Z_]*_REV=\"$want\"" build.sh | head -n 1)
		[ -n "$got" ] || { note "build.sh does not pin $repo at $want (patches/linux/UPSTREAM)"; fail=1; }
	done
else
	note "$upstream_pins is missing"
	fail=1
fi

# 6. Patch series entries point at real, non-empty patches.
if [ -s patches/linux/series ]; then
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue ;; esac
		line=$(printf '%s' "${line%%#*}" | tr -d '[:space:]')
		[ -n "$line" ] || continue
		[ -s "patches/linux/$line" ] || { note "patches/linux/series names a missing patch: $line"; fail=1; }
	done < patches/linux/series
else
	note 'patches/linux/series is missing'
	fail=1
fi

# 7. Every picasso patch is either in the series or deliberately not (vendor
#    series is applied first and is not listed).
while IFS= read -r p; do
	base=${p#patches/linux/}
	grep -qxF "$base" patches/linux/series || { note "$p is not listed in patches/linux/series"; fail=1; }
done < <(git ls-files 'patches/linux/picasso/*.patch')

if [ "$fail" -ne 0 ]; then
	note 'FAILED'
	exit 1
fi
printf 'policy-scan: OK (%s code files, %s path files)\n' \
	"$(printf '%s' "$exec_files" | grep -c . || true)" \
	"$(printf '%s' "$path_files" | grep -c . || true)"
