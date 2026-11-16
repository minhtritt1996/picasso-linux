#!/bin/bash
# mainline-linux-picasso build driver.
#
# Builds mainline Linux (pinned v7.3-rc7) with the vendored avicii SM7250 port plus this
# device's patches, from read-only ./upstream into ./build. Never touches upstream/.
#
#   ./build.sh linux [--clean]     Image + sm7250-xiaomi-picasso.dtb -> build/out/
#   ./build.sh loader              linux.efi (patched XnuLoader) -> build/out/
#   ./build.sh bootimg             boot-linux.img + payload-linux.bin (needs UEFI_FD,
#                                  BOOTSHIM, UEFI_DTB; CMDLINE optional)
#   ./build.sh bootimg-luma        boot-luma.img: Android boot v2 image for the stock
#                                  bootloader (kernel, initramfs, DTB; LUMA_CMDLINE)
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
LOADER_UP="$UP/hakios-xnu-kernel"
LOADER_WS="$BUILD/loader"

LINUX_REV="24a19b953e2a3d0ea5a8c4951a0ab8f32720c907"
REF_REV="71944476a1394d0e6faa6ee95fb157a29dce6943"
LOADER_REV="96ffbc532e970f3540885755d457e401e04d4707"

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
	local target=$1 prefix=${2:-} p n=0
	while read -r p; do
		if [ -n "$prefix" ] && [ "${p#"$prefix"}" = "$p" ]; then
			continue
		fi
		[ -f "$PATCHES/$p" ] || die "missing patch $p"
		git -C "$target" apply --whitespace=nowarn "$PATCHES/$p" || die "failed to apply $p"
		n=$((n + 1))
	done < <(series)
	echo "    [OK] $n patches applied${prefix:+ ($prefix)}"
}

prepare() {
	mkdir -p "$BUILD" "$OUT"
	if [ ! -d "$WS/.git" ]; then
		echo "==> Cloning upstream/linux into $WS"
		git clone -q "$LINUX_UP" "$WS"
		git -C "$WS" checkout -q "$LINUX_REV"
		echo "==> Applying vendor patches (avicii port)"
		apply_series "$WS" "vendor/"
		# Baseline for export-patches: HEAD must contain the vendor series only.
		git -C "$WS" add -A
		git -C "$WS" -c user.name=build -c user.email=build@localhost \
			commit -qm "vendor: avicii-mainline SM7250 port" || true
		echo "==> Applying picasso patches (uncommitted)"
		apply_series "$WS" "picasso/"
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

verify_loader_upstream() {
	[ -d "$LOADER_UP/.git" ] || die "missing $LOADER_UP (git clone --depth 1 https://github.com/oosexgang/hakios-xnu-kernel.git upstream/hakios-xnu-kernel)"
	local rev
	rev=$(git -C "$LOADER_UP" rev-parse HEAD)
	[ "$rev" = "$LOADER_REV" ] || die "upstream/hakios-xnu-kernel HEAD is $rev, expected $LOADER_REV"
	[ -z "$(git -C "$LOADER_UP" status --porcelain)" ] || die "upstream/hakios-xnu-kernel is modified"
}

build_loader() {
	verify_loader_upstream
	mkdir -p "$BUILD" "$OUT"
	if [ ! -d "$LOADER_WS/.git" ]; then
		echo "==> Cloning $LOADER_UP into $LOADER_WS"
		git clone -q "$LOADER_UP" "$LOADER_WS"
		git -C "$LOADER_WS" checkout -q "$LOADER_REV"
		while read -r p; do
			git -C "$LOADER_WS" apply --whitespace=nowarn "$ROOT/patches/loader/$p" ||
				die "failed to apply loader patch $p"
		done < <(grep -vE '^[[:space:]]*(#|$)' "$ROOT/patches/loader/series")
		echo "    [OK] loader patch series applied"
	fi

	local idh="${IDENTITY_H:-$ROOT/prebuilt/identity.h}"
	[ -f "$idh" ] || die "identity.h not found: export IDENTITY_H=/path/to/identity.h (generated by xnuloader/mkidentity.sh from an IPSW)"
	cp -f "$idh" "$LOADER_WS/xnuloader/identity.h"

	local clang="${CLANG:-clang-15}" lld="${LD:-lld-link-15}"
	command -v "$clang" >/dev/null || die "$clang not found (clang-14 miscompiles this loader)"
	command -v "$lld" >/dev/null || die "$lld not found"
	echo "==> Compiling linux.efi ($clang / $lld)"
	(
		cd "$LOADER_WS/xnuloader"
		"$clang" --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector \
			-fshort-wchar -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter \
			-c loader.c -o loader.obj
		"$lld" /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
			/out:loader.efi loader.obj
	)
	cp -f "$LOADER_WS/xnuloader/loader.efi" "$OUT/linux.efi"
	echo "    [OK] $OUT/linux.efi"
	ls -l "$OUT/linux.efi"
}

build_bootimg() {
	local fd="${UEFI_FD:-}" shim="${BOOTSHIM:-}" udtb="${UEFI_DTB:-}" extra=() initrd_arg=()
	[ -f "$OUT/Image" ] || die "run ./build.sh linux first"
	[ -f "$OUT/linux.efi" ] || die "run ./build.sh loader first"
	[ -f "$OUT/sm7250-xiaomi-picasso.dtb" ] || die "run ./build.sh linux first"
	[ -n "${PAYLOAD_NO_INITRD:-}" ] || [ -f "$OUT/initrd.cpio.gz" ] || "$ROOT/tools/linux/mkinitramfs.sh" >/dev/null
	[ -f "$fd" ] || die "set UEFI_FD=/path/to/SILICIUM_UEFI.fd"
	[ -f "$shim" ] || die "set BOOTSHIM=/path/to/BootShim.bin"
	[ -f "$udtb" ] || die "set UEFI_DTB=/path/to/picasso.dtb"
	# A payload without an explicit command line silently inherits the DTB's
	# /chosen/bootargs, which once produced a kernel with no rdinit and panic=0
	# (it panicked "unable to mount root fs" and stayed dead). Always pass one.
	: "${CMDLINE:=console=tty0 loglevel=7 picasso_pinmux.mode=write \
rdinit=/usr/lib/systemd/systemd systemd.show_status=1 \
clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10}"
	extra=(--cmdline "$CMDLINE")
	# PAYLOAD_NO_INITRD=1: no initrd in the container; Linux boots the resident
	# image sent with "fastboot oem ramdisk" (the loader patches /chosen)
	[ -n "${PAYLOAD_NO_INITRD:-}" ] || initrd_arg=(--initrd "$OUT/initrd.cpio.gz")
	python3 "$ROOT/tools/linux/build-bootimg.py" \
		-o "$OUT/boot-linux.img" --payload-out "$OUT/payload-linux.bin" \
		--loader "$OUT/linux.efi" --image "$OUT/Image" \
		"${initrd_arg[@]}" \
		--dtb "$OUT/sm7250-xiaomi-picasso.dtb" \
		--uefi-fd "$fd" --bootshim "$shim" --uefi-dtb "$udtb" "${extra[@]}"
}

# The image the stock bootloader loads from `boot` (or `fastboot boot` loads into
# RAM): Android boot v2 with the kernel, the initramfs and the DTB, at the load
# addresses proven on this phone. RAM-only until the owner allows a flash.
build_bootimg_luma() {
	[ -f "$OUT/Image" ] || die "run ./build.sh linux first"
	[ -f "$OUT/sm7250-xiaomi-picasso.dtb" ] || die "run ./build.sh linux first"
	[ -f "$OUT/initrd.cpio.gz" ] || "$ROOT/tools/linux/mkinitramfs.sh" >/dev/null
	python3 "$ROOT/tools/linux/build-luma-bootimg.py" \
		--image "$OUT/Image" --initrd "$OUT/initrd.cpio.gz" \
		--dtb "$OUT/sm7250-xiaomi-picasso.dtb" -o "$OUT/boot-luma.img" \
		${LUMA_CMDLINE:+--cmdline "$LUMA_CMDLINE"}
}

export_patches() {
	[ -d "$WS/.git" ] || die "nothing to export: run ./build.sh linux first"
	local p1="picasso/0001-arm64-dts-qcom-sm7250-move-board-reserved-memory.patch"
	local p2="picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch"
	local p3="picasso/0003-printk-ram-console.patch"
	local p4="picasso/0004-base-dd-probe-trace.patch"
	local p5="picasso/0005-pinctrl-msm-skip-unmapped-tiles.patch"
	local p6="picasso/0006-drm-panel-xiaomi-g7a.patch"
	local t
	git -C "$WS" add -N arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts 2>/dev/null || true
	t=$(mktemp)
	echo "==> Exporting $p1"
	git -C "$WS" diff HEAD -- \
		arch/arm64/boot/dts/qcom/sm7250.dtsi \
		arch/arm64/boot/dts/qcom/sm7250-oneplus-avicii.dts > "$t"
	[ -s "$t" ] || { rm -f "$t"; die "empty patch: $p1 (already applied/committed?)"; }
	mv "$t" "$PATCHES/$p1"
	echo "==> Exporting $p2"
	t=$(mktemp)
	git -C "$WS" diff HEAD -- \
		arch/arm64/boot/dts/qcom/Makefile \
		arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts \
		Documentation/devicetree/bindings/arm/qcom.yaml > "$t"
	[ -s "$t" ] || { rm -f "$t"; die "empty patch: $p2 (already applied/committed?)"; }
	mv "$t" "$PATCHES/$p2"
	echo "==> Exporting $p3"
	t=$(mktemp)
	git -C "$WS" diff HEAD -- kernel/printk/printk.c > "$t"
	[ -s "$t" ] || { rm -f "$t"; die "empty patch: $p3 (already applied/committed?)"; }
	mv "$t" "$PATCHES/$p3"
	echo "==> Exporting $p4"
	t=$(mktemp)
	git -C "$WS" diff HEAD -- drivers/base/dd.c > "$t"
	[ -s "$t" ] || { rm -f "$t"; die "empty patch: $p4 (already applied/committed?)"; }
	mv "$t" "$PATCHES/$p4"
	echo "==> Exporting $p5"
	t=$(mktemp)
	git -C "$WS" diff HEAD -- drivers/pinctrl/qcom/pinctrl-msm.c > "$t"
	[ -s "$t" ] || { rm -f "$t"; die "empty patch: $p5 (already applied/committed?)"; }
	mv "$t" "$PATCHES/$p5"
	echo "==> Exporting $p6"
	t=$(mktemp)
	git -C "$WS" diff HEAD -- drivers/gpu/drm/panel/panel-xiaomi-g7a-37-02-0a.c \
		drivers/gpu/drm/panel/Kconfig drivers/gpu/drm/panel/Makefile \
		Documentation/devicetree/bindings/display/panel/xiaomi,g7a-37-02-0a.yaml > "$t"
	[ -s "$t" ] || { rm -f "$t"; die "empty patch: $p6 (already applied/committed?)"; }
	mv "$t" "$PATCHES/$p6"
	local p7="picasso/0007-picasso-bringup-pinmux-audio-gpu.patch"
	echo "==> Exporting $p7"
	git -C "$WS" add -N drivers/soc/qcom/picasso-pinmux.c 2>/dev/null || true
	t=$(mktemp)
	git -C "$WS" diff HEAD -- \
		drivers/soc/qcom/picasso-pinmux.c drivers/soc/qcom/Makefile \
		sound/soc/qcom/qdsp6/q6asm-dai.c sound/core/pcm_native.c \
		sound/soc/soc-pcm.c sound/soc/qcom/Kconfig > "$t"
	if [ -s "$t" ]; then
		mv "$t" "$PATCHES/$p7"
	else
		rm -f "$t"
		echo "    (no bring-up delta to export)"
	fi
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
	loader)
		build_loader
		;;
	bootimg)
		build_bootimg
		;;
	bootimg-luma)
		build_bootimg_luma
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
