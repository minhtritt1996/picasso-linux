# Multi-OS on picasso, and an upgrade-proof Denial

Three questions answered from the tree and the docs: can the phone boot with
**no host computer**, how do we run **several OSes**, and how does Denial's rootfs
stay upgradable (`pacman -Syu`) **without re-applying our fixes** every time.

## 1. Booting without a computer: what works today

The loader already implements the UEFI-native path: when no payload is in memory
it reads `\EFI\hakios\payload.bin` **from the volume it was started from**
(`build/loader/xnuloader/loader.c`, `load_payload_from_volume()`), then boots it
exactly as if fastboot had staged it. `build/loader/docs/uefi-boot.md` records
the status:

| Mechanism | Verified |
|---|---|
| `fastboot boot` (payload staged by the shim into `0xF0000000`) | on the phone |
| QEMU loader device | in QEMU |
| **payload file on the ESP** (`\EFI\hakios\payload.bin`) | **QEMU only** |

On the phone this path is blocked by one thing: *this port's* Project Silicium
Boot Manager was changed to start the XNU loader **from the in-memory container
instead of a menu** and does not look at a volume (`uefi/README.md`,
`docs/uefi-boot.md` "On a phone"). So today:

- **no computer ⇒ no boot**, unless one of these is done once:
  1. **install to a boot slot** (write the UEFI/loader boot image to `boot_a`),
     i.e. the classic install path: the phone then starts from UFS by itself; or
  2. **patch the Boot Manager** to scan volumes / show a menu and autoboot
     `\EFI\BOOT\BOOTAA64.EFI` from an ESP on UFS — after that the loader's volume
     path (already written and tested in QEMU) starts working on the phone; or
  3. use **HakiOS's U-Boot** as the firmware instead (started from RAM by ABL),
     which loads the loader and payload itself.

Option 2 is the one that also gives multi-OS for free (next section). It needs:
an ESP (FAT32) on UFS, the Boot Manager patch, and `0xF0000000` left free by the
firmware (unknown on the phone, per the same doc).

## 2. Multi-OS layout

Everything the firmware can start lives on **one FAT32 EFI system partition**; each
OS owns a payload plus its data partition:

```
ESP (FAT32, 256-512 MiB, new GPT partition)
  EFI/BOOT/BOOTAA64.EFI          the loader (used for the default entry)
  EFI/hakios/payload.bin         mainline Linux / HakiOS payload (kernel + initrd + dtb)
  EFI/hakios/kernel.ver
  EFI/denial/payload.bin         Denial Mobile payload (same kernel, Denial cmdline/initrd)
  EFI/denial/kernel.ver
  EFI/droidian/payload.bin       (optional third)
  EFI/hakios/boot.conf           NEW: the entry the loader boots (see below)
```

Two small changes make this real:

1. **Loader reads a config file** instead of the hardcoded path: `boot.conf` with
   `payload=<path>`, `cmdline=<extra args>`, so one loader binary serves every
   OS and the Boot Manager menu can pick a *different* `boot.conf` per entry
   (e.g. by `\EFI\hakios\boot.conf` for the default and a second loader copy whose
   config path differs, or by a Boot Manager variable). This is a patch under
   `patches/loader/`, small and testable in QEMU first.
2. **Boot Manager menu**: one entry per OS, default with a short timeout, volume
   key to stop it. (Patch to Silicium's BdsDxe / XNU wiring in `uefi/`.)

Payload size budget: the region is 63 MiB and the current mainline payload is
~62 MiB (Image 58 MiB + dtb + initrd). Denial needs the same kernel with a
smaller initrd, so it fits, but **the payload cannot carry a rootfs** — each OS's
system stays in its own UFS partition (`denial` 20 GiB, `darwin`, `userdata`),
which is exactly how Denial expects it (`docs/denial-mobile-architecture.md`).

## 3. Denial's rootfs must survive `pacman -Syu`

Rule: **nothing we fix may live inside the Arch rootfs**. Everything we control
must be either in the payload (outside the filesystem pacman manages) or applied
**idempotently on every boot** by the initramfs.

| What we fix | Where it lives | Survives `pacman -Syu`? |
|---|---|---|
| Kernel, drivers (panel, touch, GPU, pinmux) | payload `Image` + dtb on the ESP | yes — outside the rootfs |
| Kernel config/features | the same payload | yes |
| Firmware (touch fw03, Adreno, stock partitions) | initramfs `/lib/firmware` + a tree on the partition, **copied into the system on every boot** by the initramfs | yes — re-applied each boot |
| Root password / SSH key | initramfs writes `flashed/authorized_keys` + `/etc/shadow` on every boot | yes — re-applied each boot |
| Masked/broken units, gadget, 10.77.71.2, console | initramfs units and config written per boot | yes |
| Anything that must live in the rootfs (an upstream bug) | **pacman hook + file shipped from the ESP**, re-installed each boot | yes, if expressed that way |

The gap this closes: hand-editing the rootfs (what we did for Luma) is lost on the
first upgrade and impossible to review. The per-boot initramfs is also how the
`flashed-keys` / `install-flashed` machinery of the image already works, so we are
following the image's own convention instead of fighting it.

Proof cycle to run once the system is installed: note the versions
(`pacman -Q denial droidloom`), run `pacman -Syu`, reboot, then check that the
panel/touch fixes are still active (kernel side), that SSH still works, that
`/usr/lib/firmware/novatek/*` exists, and that `denial.service` is running.

## 4. Order of work

1. Loader `boot.conf` support (+ QEMU test) — enables multi-OS and per-OS cmdlines.
2. Boot Manager menu/volume scan patch (Silicium) — enables no-computer boot.
3. ESP partition on UFS (GPT edit, written once) with both payloads.
4. Denial system in `denial` (image write) + initramfs that follows the contract
   (marker, gadget before switch_root, 10.77.71.2, per-boot fixes).
5. First boot over SSH (`first-user ... && first-user --start`), then the upgrade
   proof cycle above.

## 5. Vendor evidence: the "media payload" boot image (UniXNU-picasso)

`github.com/YinCQL/UniXNU-picasso` (branch `main-ios16`) does exactly the
no-host / multi-entry layout this plan wants. From its `board.toml`:

```toml
[bootimg.image]
kernel_offset = 0x8000
ramdisk_offset = 0x200000     # "The 60 MiB ramdisk fits the committed firmware mapping at this offset"
header_version = 1
[firmware]
fd = "Build/picassoPkg/{target}_CLANGPDB/FV/SILICIUM_UEFI.fd"
loader = "firmware"
[payload]
volume = "PICASSOESP"         # FAT payload: config, kernel, trust caches, ADT
size_mib = 60                 # "95% content limit"
title = "Boot"
```

- **kernel slot = the UEFI firmware FD**, **ramdisk slot = a FAT volume named
  `PICASSOESP`** holding the payload as files. The firmware (their Mu-Silicium
  branch `UniXNU`) mounts that volume and boots the entry titled "Boot".
- The payload therefore does **not** go through `fastboot stage` into
  `0xF0000000`, so the 63 MiB RAM-staging limit stops being the constraint; the
  media region (~60 MiB, 95% usable) is.
- Because the media is a filesystem, several payloads and a config file fit in
  one boot image: that is the multi-OS and no-host path.

Measured on our build (`build/out/Image`): raw **60,303,872 B (57.5 MiB)**,
`gzip -1` **20,117,871 B (19.2 MiB)**, `xz -1` **14,620,584 B (13.9 MiB)**, dtb
61 KB, Luma initramfs ~1.8 MB. Two OS payloads at ~23 MiB each (Image.gz + initrd
+ dtb + boot.conf) fit in one 60 MiB media with room to spare.

To adopt it, four steps:

1. firmware with payload-volume support (FD from `YinCQL/Mu-Silicium` branch
   `UniXNU`, or port that feature into our Mu-Silicium);
2. `boot.conf` support in the loader (`payload=`, `cmdline=`) so one loader
   serves several entries — the volume read itself already exists
   (`load_payload_from_volume()` in `build/loader/xnuloader/loader.c`);
3. packaging: `mkfs.vfat -n PICASSOESP` + `mcopy` into the boot image's ramdisk
   slot, FD in the kernel slot (`tools/`, following `tools/build-bootimg.py`);
4. gzip decode in the loader (simplest) so the kernel fits compressed; the
   uncompressed 57.5 MiB image alone would fill the media.
