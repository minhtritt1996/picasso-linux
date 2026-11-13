# Status

## M0 — build from upstream: DONE

| Item | Result |
|---|---|
| `upstream/linux` | v7.3-rc7, commit `24a19b9` (tag object `ec8bf82`), clean |
| `upstream/avicii-mainline` | `7194447`, clean |
| `./build.sh check` | 16 patches apply to a clean tree; DTB builds; 7 dtbs_check diagnostics, all from vendor-series bindings (smmu/wdt/mdss/dpu/videocc/dispcc), none from picasso nodes |
| `./build.sh linux --clean` | `build/out/Image` 53 463 552 B (PE32+ `pei-aarch64-little`), `sm7250-xiaomi-picasso.dtb` ~55 KB |
| kernel config | `EFI_STUB`, `DRM_SIMPLEDRM`, `SYSFB_SIMPLEFB`, `FRAMEBUFFER_CONSOLE`, `SM_GCC_7250`, `PINCTRL_SM7250`, `QCOM_RPMH`, `QCOM_WDT`, `USB_DWC3_QCOM` all `y` |

## M1 — boot vehicle: code complete, not yet booted on hardware

The root filesystem is always a ramdisk in RAM. `linux.efi` is the existing XnuLoader plus a
72-line patch, so the payload window and the fastboot command channel are the same ones XNU
uses; no new USB stack and no firmware change.

| Piece | Result |
|---|---|
| `patches/loader/0001-xnuloader-boot-mainline-linux-image.patch` | adds `Image`/`initrd`/`dtb` payload entries, installs the DTB as an EFI configuration table, `LoadImage`+`StartImage` |
| `./build.sh loader` | `build/out/linux.efi` 71 680 B, PE32+ AArch64 (clang-15 + lld-link-15) |
| `tools/linux/mkinitramfs.sh` | `build/out/initrd.cpio.gz` 266 739 B (static init; prints model/cores/memory, optional `auto_reboot=`) |
| `tools/linux/build-bootimg.py` | XNUP + outer boot image; verifies PE of loader/Image, patches `/chosen/linux,initrd-*`, writes `payload-linux.bin` for `fastboot stage` |
| packed payload | 53 857 776 B of 66 060 288 available; `Image` @ `0xf0011838`, `initrd` @ `0xf330e250` (matches `/chosen`) |
| packed boot image | `build/out/boot-linux.img` 17 973 248 B; gzip stream 55 955 104 B = BootShim 176 + FD 0x200000 + XNUP |

Offline checks run on the packed image: gzip decompresses to exactly shim+FD+XNUP, XNUP magic
and entry sizes match, `loader.efi` and `Image` are PE32+ machine `0xaa64`, and the DTB inside
the payload carries `linux-payload@f0000000`, `/chosen/linux,initrd-start` and the bootargs.

## Not verified

No fastboot device was attached, so nothing has been booted on the phone yet. First hardware
run is `fastboot boot build/out/boot-linux.img`.
