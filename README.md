# picasso-linux

Mainline Linux on the Xiaomi Redmi K30 5G (`picasso`, Qualcomm SM7250 / Snapdragon 765G),
booted entirely from RAM through the HakiOS Mu-Silicium UEFI that already runs on this phone.

Not a phone OS. The goal is a working mainline kernel port: USB console, UFS (read-only at
first), display, touch, Wi-Fi (see `docs/mainline-linux-plan.md` for the full roadmap).

- Plan and decisions: [`docs/mainline-linux-plan.md`](docs/mainline-linux-plan.md)
- Reference port on the same SoC (OnePlus Nord): `lazybrar/avicii-mainline`
- Hardware oracle (downstream 4.19 that runs everything on this phone): `~/droidian`

## Boot chain

```text
XBL -> fastboot boot boot-linux.img -> BootShim -> Mu-Silicium UEFI
     -> payload entry "loader.efi" (linux.efi launcher) -> Linux Image
```

Nothing is written to UFS. `upstream/` is read-only; every change lives in `patches/linux/`.

## Layout

```text
upstream/linux              torvalds/linux v7.3-rc7, pinned (read-only, gitignored)
upstream/avicii-mainline    reference SM7250 port, pinned (read-only, gitignored)
upstream/hakios-xnu-kernel  XnuLoader source for the EFI launcher (read-only, gitignored)

patches/linux/UPSTREAM      the pins above, single source of truth
patches/linux/series        ordered patch list (vendor/ first, then picasso/)
patches/linux/vendor/       avicii patches, unmodified
patches/linux/picasso/      this device's deltas, exported from build/linux

configs/linux/              kernel config fragments
tools/                      boot images, initramfs, console, gates
  build-bootimg.py          XNUP payload + boot-linux.img (UEFI chain)
  build-luma-bootimg.py     Android boot v2 image for the stock ABL path
  mkinitramfs.sh            the small bring-up initramfs
  pack-rootfs.sh            the big RAM root filesystem
  console_cmd.py            USB-ACM console: one command, capture, reboot
  ramboot-cycle.sh          one hardware gate cycle (arm, stage, run, log)
  policy-scan.sh            gate 1 rules
  preflight.sh              gate 1 locally / gate 3 static half in CI
prebuilt/                   identity.h and small device data (no vendor firmware)
userland/fedora/            systemd units and configs baked into the RAM root
docs/                       plan, Luma port brief, DTS crosswalk
.github/                    workflows, gate documentation, PR template
build/                      isolated build tree and build/out artifacts (gitignored)
```

## Gates

Four gates keep the working tree honest; they are described in
[`.github/workflows/README.md`](.github/workflows/README.md) and enforced by
`tools/preflight.sh` + `.github/workflows/ci.yml`:

| Gate | Command | What it proves |
|---|---|---|
| 1 preflight | `tools/preflight.sh` | documents, policy scan, patch series, syntax, whitespace |
| 2 change | PR checklist | kernel changes only as exported patches; `vendor/` untouched |
| 3 CI | `./build.sh check` + clean builds | 22 patches apply to a clean upstream; every artifact builds |
| 4 hardware | `tools/ramboot-cycle.sh` | one RAM boot: arm, stage, run, capture. Never flashes |

## Build

```sh
./build.sh linux                # Image + sm7250-xiaomi-picasso.dtb -> build/out/
./build.sh loader               # linux.efi (patched XnuLoader) -> build/out/
./build.sh export-patches       # regenerate patches/linux/picasso/*.patch
./build.sh check                # apply series to a clean tree, run dtbs_check
```

The ramdisk and the boot image are built by the tools, because they need HakiOS artifacts that
are not vendored here:

```sh
./tools/mkinitramfs.sh    # build/out/initrd.cpio.gz (static init, ~0.3 MB)

UEFI_FD=<SILICIUM_UEFI.fd> BOOTSHIM=<BootShim.bin> UEFI_DTB=<picasso.dtb> \
  ./build.sh bootimg            # build/out/boot-linux.img + payload-linux.bin
```

`payload-linux.bin` is the XNUP container for `fastboot stage`; the UEFI artifacts come from the
existing HakiOS build tree (they are not part of this repository). `CMDLINE="..."` overrides
`/chosen/bootargs`.

`check` runs `dtbs_check` through a repo-local dtschema; set it up once with
`./tools/setup-dtschema.sh` (no root, gitignored venv). Without it, `check` applies and
builds the series but skips the schema validation with a warning.

Requires: `aarch64-linux-gnu-gcc`, `make`, `bison`, `flex`, `bc`, `libssl-dev`, `libelf-dev`,
`python3` (dtschema), `git`. Kernel builds are out-of-tree in `build/linux/out/`.

## Boot

```sh
# once, from the stock bootloader
fastboot boot build/out/boot-linux.img

# every kernel/initramfs change after that: no UEFI re-upload
fastboot stage build/out/payload-linux.bin
fastboot oem run
```

Once Linux is up it serves a USB CDC-ACM console (`ttyGS0`), independent of the
loader:

```sh
stty -F /dev/ttyACM0 raw -echo
printf 'help\r' > /dev/ttyACM0 & cat /dev/ttyACM0
# commands: help, uname, mem, cat FILE, reboot
```

`reboot` returns to the loader (the boot image is installed on the `boot` partition).

### A root filesystem bigger than the payload

The 63 MiB payload holds `linux.efi` + `Image` + `dtb`; a real userspace does not fit there.
XnuLoader's *resident ramdisk* does: `oem ramdisk` makes the next download land at
`win_base + 0x08000000` and the loader patches `/chosen/linux,initrd-start|end` in place, so
the kernel gets it as initrd.

```sh
PAYLOAD_NO_INITRD=1 ./build.sh bootimg        # payload without an initrd entry
fastboot oem ramdisk
fastboot stage rootfs.cpio.gz                 # up to ~1.1 GiB with the 2 GiB window
fastboot stage build/out/payload-linux.bin
fastboot oem run
```

The loader caps the window it claims for Linux payloads at 2 GiB; without that cap it claims
the largest free block (~5 GiB), the firmware runs out of memory to load the kernel and the
phone resets before printing anything. Diagnostics: `fastboot getvar result` (`linux-fail N`),
`fastboot oem log` + `fastboot get_staged log.txt`, `fastboot oem ramoops` (the kernel's RAM
console) and `fastboot oem screenshot` (raw framebuffer, `getvar screen` gives the geometry).
The kernel also mirrors its console into RAM (`0xF3F00000`); the loader copies that
into its log at start-up, so `fastboot get_staged` after any reset contains the last
kernel boot (and `logdump` keeps it on UFS).

The staged blob is a complete XNUP payload (`linux.efi` + `Image` + `initrd` + `dtb`), so a
rebuild is one `stage` and one `oem run`. The root filesystem is always the ramdisk in RAM
(entry `initrd`, handed to the kernel via `/chosen/linux,initrd-*`); `linux.efi` is the patched
XnuLoader, so the payload region and the USB command channel are the same ones XNU uses.

To get back to the loader after Linux is up, `reboot` (or the init's `auto_reboot=<seconds>`
kernel argument) warm-resets the phone; whether that lands in the loader depends on the boot
path installed on the device — otherwise `fastboot boot` again.

RAM only. If the phone has to be recovered, reboot to the stock bootloader; nothing on the
device was modified.
