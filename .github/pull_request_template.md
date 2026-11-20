## What changed

<!-- one paragraph; which subsystem(s) and which hardware test proves it -->

## Gate 2 checklist

- [ ] Kernel source changed in `build/linux/` first, then
      `./build.sh export-patches`; no hand-edited files under `patches/`.
- [ ] `patches/linux/vendor/` untouched; the new picasso patch is listed in
      `patches/linux/series`.
- [ ] `./build.sh check` is green (series applies to a clean upstream, DTB builds).
- [ ] `docs/changelog/boot-history.md` records the hardware run (cmdline, artifacts, what was seen);
      `docs/hardware-status.md` updated if a done-criterion moved.
- [ ] The hardware run was RAM-boot only: `fastboot boot`, `stage`,
      `oem ramdisk`, `oem run`. No `flash`, no `erase`, no `dd` to a block device.
- [ ] `scripts/preflight.sh` is green.

## Verification

<!-- commands and their observed output; "not run, because …" is acceptable -->
