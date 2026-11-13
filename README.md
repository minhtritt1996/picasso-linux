# mainline-linux-picasso

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
upstream/linux              torvalds/linux, pinned tag (read-only)
upstream/avicii-mainline    reference port, pinned commit (read-only)
patches/linux/UPSTREAM      pins
patches/linux/series        ordered patch list
patches/linux/vendor/       avicii patches, unmodified
patches/linux/picasso/      this device's deltas
configs/linux/              kernel config fragments
tools/linux/                boot image + initramfs tooling
uefi/linuxloader/           linux.efi (EDK2 app) source
build/                      isolated build tree and build/out artifacts
```

## Build

```sh
./build.sh linux                # Image + sm7250-xiaomi-picasso.dtb -> build/out/
./build.sh export-patches       # regenerate patches/linux/picasso/*.patch
./build.sh check                # apply series to a clean tree, run dtbs_check
```

`check` runs `dtbs_check` through a repo-local dtschema; set it up once with
`./scripts/setup-dtschema.sh` (no root, gitignored venv). Without it, `check` applies and
builds the series but skips the schema validation with a warning.

Requires: `aarch64-linux-gnu-gcc`, `make`, `bison`, `flex`, `bc`, `libssl-dev`, `libelf-dev`,
`python3` (dtschema), `git`. Kernel builds are out-of-tree in `build/linux/out/`.

## Boot

```sh
# once, from the stock bootloader
fastboot boot build/out/boot-linux.img

# every kernel/initramfs change after that: no UEFI re-upload
fastboot stage build/out/Image
fastboot oem ramdisk && fastboot stage build/out/initrd.cpio.gz
fastboot oem run
```

The root filesystem is always a ramdisk in RAM (XNUP entry `initrd`, handed to the kernel via
`/chosen/linux,initrd-*`); `linux.efi` stays resident with a fastboot listener, so a rebuild is
staged and run without touching UFS — the same model as `xnuloader`/XNU userland. A `reboot`
inside the initramfs returns to the loader.

RAM only. If the phone has to be recovered, reboot to the stock bootloader; nothing on the
device was modified.
