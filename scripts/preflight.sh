#!/usr/bin/env bash
# Agent preflight (gate 1) and CI entry point for picasso-linux.
#
# Runs the same checks the `policy` CI job runs, so a green local preflight
# means the policy job is green. It never touches a phone and never writes to a
# device: the hardware gate is a separate runner.
#
# Usage:
#   scripts/preflight.sh           documents, policy, series, syntax, whitespace
#   scripts/preflight.sh --full    also ./build.sh check (needs ./upstream)
#   scripts/preflight.sh --ci      no colour/no working-tree listing
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

full=0
ci=0
for arg in "$@"; do
	case "$arg" in
		--full) full=1 ;;
		--ci) ci=1 ;;
		-h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "usage: $0 [--full] [--ci]" >&2; exit 2 ;;
	esac
done

step() { printf '\n== %s\n' "$*"; }
die() { printf 'preflight: %s\n' "$*" >&2; exit 1; }

step "documents"
for doc in AGENTS.md README.md docs/hardware-status.md docs/changelog/boot-history.md patches/linux/UPSTREAM; do
	[ -s "$doc" ] || die "$doc is missing or empty"
	printf '  %-34s %6s bytes  %s\n' "$doc" "$(stat -c%s "$doc")" "$(date -r "$doc" '+%Y-%m-%d %H:%M')"
done

step "policy scan"
bash scripts/policy-scan.sh --root "$root"

step "patch series"
mapfile -t series < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' patches/linux/series)
for entry in "${series[@]}"; do
	entry=$(printf '%s' "$entry" | tr -d '[:space:]')
	[ -s "patches/linux/$entry" ] || die "patches/linux/series names a missing patch: $entry"
done
printf '  %s patches listed and present (%s picasso, %s vendor)\n' \
	"${#series[@]}" \
	"$(printf '%s\n' "${series[@]}" | grep -c '^picasso/' || true)" \
	"$(printf '%s\n' "${series[@]}" | grep -c '^vendor/' || true)"

step "shell and python syntax"
mapfile -t shells < <(git ls-files '*.sh')
for f in "${shells[@]}"; do bash -n "$f" || die "$f does not parse"; done
mapfile -t pys < <(git ls-files '*.py')
for f in "${pys[@]}"; do
	python3 -B - "$f" <<'PY' || die "$f does not compile"
import sys
compile(open(sys.argv[1], 'rb').read(), sys.argv[1], 'exec')
PY
done
printf '  %s shell, %s python files parse\n' "${#shells[@]}" "${#pys[@]}"

step "whitespace"
git diff --check || die "whitespace errors in the working tree"
git diff --cached --check || die "whitespace errors in the index"

step "working tree"
if [ "$ci" -eq 1 ]; then
	printf '  %s changed path(s)\n' "$(git status --porcelain | wc -l)"
else
	git status --short || true
	printf '\n'
	for up in linux avicii-mainline hakios-xnu-kernel; do
		printf '  upstream/%-20s %s\n' "$up" \
			"$([ -d "upstream/$up/.git" ] && git -C "upstream/$up" rev-parse --short HEAD || echo 'not provisioned')"
	done
fi

if [ "$full" -eq 1 ]; then
	step "repository check (./build.sh check)"
	./build.sh check
fi

printf '\npreflight: OK\n'
