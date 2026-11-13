#!/bin/bash
# mainline-linux-picasso build driver.
#
# Builds mainline Linux (pinned v7.3-rc7) with the vendored avicii SM7250 port plus this
# device's patches, from read-only ./upstream into ./build. Never touches upstream/.
#
#   ./build.sh linux [--clean]     Image + sm7250-xiaomi-picasso.dtb -> build/out/
#   ./build.sh export-patches      regenerate patches/linux/picasso/*.patch from build/linux
#   ./build.sh check               apply the series to a fresh tree, build, run dtbs_check
#   ./build.sh clean               remove build/
set -euo pipefail

ROOT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
UP="$ROOT/upstream"
BUILD="$ROOT/build"
WS="$BUILD/linux"
KB="$WS/out"
OUT="$BUILD/out"
PATCHES="$ROOT/patches/linux"
LINUX_UP="$UP/linux"
REF_UP="$UP/avicii-mainline"

LINUX_REV="24a19b953e2a3d0ea5a8c4951a0ab8f32720c907"
REF_REV="71944476a1394d0e6faa6ee95fb157a29dce6943"

CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
MAKE=(make -C "$WS" O="$KB" ARCH=arm64 CROSS_COMPILE="$CROSS")

die() { echo "ERROR: $*" >&2; exit 1; }

usage() {
	sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

verify_upstream() {
	local rev
	[ -d "$LINUX_UP/.git" ] || die "missing $LINUX_UP (git clone --branch v7.3-rc7 --depth 1 https://github.com/torvalds/linux.git upstream/linux)"
	[ -d "$REF_UP/.git" ] || die "missing $REF_UP (git clone --depth 1 https://github.com/lazybrar/avicii-mainline.git upstream/avicii-mainline)"
	rev=$(git -C "$LINUX_UP" rev-parse HEAD)
	[ "$rev" = "$LINUX_REV" ] || die "upstream/linux HEAD is $rev, expected $LINUX_REV"
	[ -z "$(git -C "$LINUX_UP" status --porcelain)" ] || die "upstream/linux is modified"
	rev=$(git -C "$REF_UP" rev-parse HEAD)
	[ "$rev" = "$REF_REV" ] || die "upstream/avicii-mainline HEAD is $rev, expected $REF_REV"
	[ -z "$(git -C "$REF_UP" status --porcelain)" ] || die "upstream/avicii-mainline is modified"
}

series() {
	grep -vE '^[[:space:]]*(#|$)' "$PATCHES/series"
}

apply_series() {
	local target=$1 p n=0
	while read -r p; do
		[ -f "$PATCHES/$p" ] || die "missing patch $p"
		git -C "$target" apply --whitespace=nowarn "$PATCHES/$p" || die "failed to apply $p"
		n=$((n + 1))
	done < <(series)
	echo "    [OK] $n patches applied"
}

prepare() {
	mkdir -p "$BUILD" "$OUT"
	if [ ! -d "$WS/.git" ]; then
		echo "==> Cloning upstream/linux into $WS"
		git clone -q "$LINUX_UP" "$WS"
		git -C "$WS" checkout -q "$LINUX_REV"
		echo "==> Applying patches/linux/series"
		apply_series "$WS"
		# Baseline for export-patches: the series as applied, before any local work.
		git -C "$WS" -c user.name=build -c user.email=build@localhost \
			commit -qam "mainline-linux-picasso: vendor + picasso series" || true
	else
		echo "==> Reusing existing $WS (use --clean to rebuild from upstream)"
	fi
	mkdir -p "$KB"
}

configure() {
	echo "==> Configuring (defconfig + configs/linux/*.config)"
	local frags=()
	for f in "$ROOT"/configs/linux/*.config; do frags+=("$f"); done
	"${MAKE[@]}" defconfig
	ARCH=arm64 CROSS_COMPILE="$CROSS" "$WS/scripts/kconfig/merge_config.sh" \
		-m -O "$KB" "$KB/.config" "${frags[@]}" >/dev/null
	"${MAKE[@]}" olddefconfig
}

build_kernel() {
	prepare
	configure
	echo "==> Building Image + sm7250-xiaomi-picasso.dtb (-j$JOBS)"
	"${MAKE[@]}" -j"$JOBS" Image qcom/sm7250-xiaomi-picasso.dtb
	cp -f "$KB/arch/arm64/boot/Image" "$OUT/Image"
	cp -f "$KB/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dtb" "$OUT/sm7250-xiaomi-picasso.dtb"
	echo "==> Artifacts:"
	ls -lh "$OUT/Image" "$OUT/sm7250-xiaomi-picasso.dtb"
}

export_patches() {
	[ -d "$WS/.git" ] || die "nothing to export: run ./build.sh linux first"
	local p1="picasso/0001-arm64-dts-qcom-sm7250-move-board-reserved-memory.patch"
	local p2="picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch"
	git -C "$WS" add -N arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts 2>/dev/null || true
	echo "==> Exporting $p1"
	git -C "$WS" diff HEAD -- \
		arch/arm64/boot/dts/qcom/sm7250.dtsi \
		arch/arm64/boot/dts/qcom/sm7250-oneplus-avicii.dts > "$PATCHES/$p1"
	echo "==> Exporting $p2"
	git -C "$WS" diff HEAD -- \
		arch/arm64/boot/dts/qcom/Makefile \
		arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts \
		Documentation/devicetree/bindings/arm/qcom.yaml > "$PATCHES/$p2"
	for f in "$PATCHES/$p1" "$PATCHES/$p2"; do
		[ -s "$f" ] || die "empty patch: $f"
	done
	echo "    [OK] regenerated; run ./build.sh check"
}

run_check() {
	verify_upstream
	local tmp rc=0 check_dtbs=1 log="$OUT/check.log" diags
	mkdir -p "$OUT"
	if [ -x "$ROOT/.venv-dtschema/bin/dt-doc-validate" ]; then
		PATH="$ROOT/.venv-dtschema/bin:$PATH"
	elif ! command -v dt-doc-validate >/dev/null; then
		echo "    [WARN] dt-doc-validate not found: skipping dtbs_check"
		echo "           ./scripts/setup-dtschema.sh"
		check_dtbs=0
	fi
	tmp=$(mktemp -d)
	echo "==> Checking series on a fresh tree ($tmp)"
	git clone -q "$LINUX_UP" "$tmp/linux"
	apply_series "$tmp/linux"
	chmod -R u+w "$tmp"
	(
		cd "$tmp/linux"
		make -s O="$tmp/out" ARCH=arm64 CROSS_COMPILE="$CROSS" defconfig
		ARCH=arm64 CROSS_COMPILE="$CROSS" scripts/kconfig/merge_config.sh \
			-m -O "$tmp/out" "$tmp/out/.config" "$ROOT"/configs/linux/*.config >/dev/null
		make -s O="$tmp/out" ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig
		make O="$tmp/out" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS" \
			CHECK_DTBS="$check_dtbs" qcom/sm7250-xiaomi-picasso.dtb
	) >"$log" 2>&1 || rc=$?
	rm -rf "$tmp"
	[ "$rc" -eq 0 ] || { tail -20 "$log" >&2; die "check failed (log: $log)"; }
	echo "    [OK] series applies to a clean tree and the DTB builds"
	if [ "$check_dtbs" = 1 ]; then
		diags=$(grep -c 'from schema \$id:' "$log" || true)
		echo "    dtbs_check: ${diags:-0} schema diagnostics from the vendor series (detail: $log)"
	fi
}

case "${1:-}" in
	linux)
		verify_upstream
		if [ "${2:-}" = "--clean" ]; then
			rm -rf "$WS" "$KB"
		fi
		build_kernel
		;;
	export-patches)
		export_patches
		;;
	check)
		run_check
		;;
	clean)
		rm -rf "$BUILD"
		echo "removed $BUILD"
		;;
	*)
		usage
		;;
esac
