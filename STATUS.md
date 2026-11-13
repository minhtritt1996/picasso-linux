# Status

## M0 — build from upstream: DONE

Verified locally on 2026-11-13:

| Item | Result |
|---|---|
| `upstream/linux` | v7.3-rc7, commit `24a19b9` (tag object `ec8bf82`), clean |
| `upstream/avicii-mainline` | `7194447`, clean |
| `./build.sh check` | 16 patches apply to a clean tree; DTB builds; 7 dtbs_check diagnostics, all from vendor-series bindings (smmu/wdt/mdss/dpu/videocc/dispcc), none from picasso nodes |
| `./build.sh linux --clean` | `build/out/Image` 53 463 552 B (PE32+ `pei-aarch64-little`, HAS_RELOC), `sm7250-xiaomi-picasso.dtb` 55 524 B |
| kernel config | `EFI_STUB`, `DRM_SIMPLEDRM`, `SYSFB_SIMPLEFB`, `FRAMEBUFFER_CONSOLE`, `SM_GCC_7250`, `PINCTRL_SM7250`, `QCOM_RPMH`, `QCOM_WDT`, `USB_DWC3_QCOM` all `y` |

Notes:

- `Image` is 51 MiB; the XNUP payload window is 64 MiB, so the initramfs must stay below
  ~12 MiB. If that gets tight, `CONFIG_EFI_ZBOOT` (compressed PE) or a smaller config.
- `dtbs_check` reports vendor-series gaps only; `build/out/check.log` has the detail. Fixing
  them is a separate binding patch, not part of M0.

## M1 — boot vehicle (ramdisk + resident fastboot)

Design fixed by the user requirement: rootfs is always a **ramdisk in RAM** (entry `initrd` in
the XNUP payload), and `linux.efi` keeps a **fastboot listener** (stage / oem ramdisk / oem run)
so kernel and initrd can be replaced without re-uploading UEFI, like `xnuloader` for XNU.

| Item | State |
|---|---|
| `uefi/linuxloader/` (linux.efi) | not started |
| `tools/linux/build-bootimg.py` | not started |
| `tools/linux/mkinitramfs.sh` | not started |

## Verified on hardware

Nothing yet. Every entry here must name the artifact hash and the boot command.
