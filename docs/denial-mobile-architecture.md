# How Denial Mobile keeps one rootfs image for every phone

Read from the shipped artifacts, not from prose: the official
`denial-mobile-2026.11.15.zip` (`init_boot.img` = the shared installer,
`userdata.img` = the shared system) and the manual
(`manual.denialwm.org/docs/install-on-a-phone/`). Nothing here is guessed; the
installer script is quoted by behaviour below.

## The contract

| Piece | Scope | Carries |
|---|---|---|
| `userdata.img` (ext4, grown by `x-systemd.growfs`) | **shared by every device** | Arch Linux ARM, Denial (compositor + mobile shell), Droidloom, the phone services, and the first-boot integration scripts |
| `boot.img` (per device) | **one per phone** | the kernel **plus an erofs image appended after it** holding that kernel's modules, the firmware that overrides the system's, and the units the kernel needs |
| `init_boot.img` (shared) | every device | the early userspace that finds the system, brings the USB gadget up, and falls back to a rescue fastboot/SSH |

So the answer to "how do different phones share one rootfs": **nothing
device-specific is in the rootfs.** All of it is either (a) discovered at runtime
from the device tree and standard Linux interfaces, or (b) shipped inside that
device's own boot image.

## Device identity at runtime (no per-device config)

From the shared installer's `init`:

```sh
codename=$(tr '\0' '\n' < /proc/device-tree/compatible | head -n 1); codename=${codename#*,}
product=$(tr -d '\0' < /proc/device-tree/model)
mac=$(echo -n "$codename" | md5sum | sed 's/^\(..\)\(..\)\(..\)\(..\).*/02:\1:\2:\3:\4/')
host_addr=$mac:01 dev_addr=$mac:02
```

A new phone needs only a matching device tree: the codename names the device, the
model names it for the host, and the USB MAC pair is derived from the codename, so
each phone gets stable addresses without a config file anywhere.

## The per-device boot image (kernel + an erofs tail)

Verbatim mechanism from the installer:

```sh
size=$(dd if=$boot bs=4 skip=2 count=1 2>/dev/null | od -An -tu4 | tr -d ' ')
mount -t erofs -o ro,fsoffset=$(( (size + 4095) / 4096 * 4096 + 4096 )) $boot /newroot/usr/lib/modules
```

- The boot image is a normal Android boot image whose **kernel size word** says
  where the kernel ends; a page-aligned **erofs filesystem** follows it.
- It is mounted at `/usr/lib/modules` and must contain a directory named after
  `uname -r`; the slot whose directory matches is the slot that booted.
- From it the installer takes: the modules, the firmware override
  (`/sys/module/firmware_class/parameters/path` pointed at
  `<modules>/<release>/firmware`), the units the kernel needs (copied into
  `/run/systemd/system`), and `<modules>/<release>/libexec/stock-firmware`.
- `stock-firmware <modules-dir> <slot>` **binds the phone's own stock vendor
  partitions** in as firmware. If a required file is missing the installer stops
  in rescue and says so, instead of booting a half-working phone.

That is the whole device-porting surface: a kernel, its modules, its firmware
overrides, a few early units, and a script that knows where this phone's vendor
partitions are.

## Finding the system

```sh
# UFS LUNs attach asynchronously and their sdX names change between boots.
part() { for b in /sys/class/block/sd*; do grep -qx "PARTNAME=$1" $b/uevent && echo /dev/${b##*/}; done }
```

- The system is the first partition named **`denial`**, else **`userdata`**, whose
  ext4 root contains the marker **`/usr/lib/phonewave-system/root`**.
- It is mounted read-only, handed to systemd, which remounts it read-write from
  `fstab` (that is where `x-systemd.growfs` grows the filesystem to the partition).
- Same image works on `denial` or on `userdata`: no per-device fstab.

## USB and networking are part of the shared initramfs

- The gadget (**ECM network + ACM serial**, functionfs fastboot when in rescue) is
  created **before** `switch_root`, so the host sees the phone during the whole
  boot; the installed system keeps it and gives `usb0` its address.
- Convention: **`10.77.71.2/30`** and dropbear SSH on the gadget, plus a shell on
  `/dev/ttyGS0`. (Our picasso port used 192.168.7.2 and link-local; aligning on
  10.77.71.2 would match the shared tooling.)
- Rescue reasons are printed on the framebuffer and exposed over fastboot:
  `flash denial|userdata`, `flash package`, `flash ssh-key`, `flash boot`,
  `flash init_boot`, `reboot`. Rescue is entered when there is no system, the
  marker is missing, `/etc/phone-rescue` exists (one visit), a volume key is held,
  or this init_boot is marked `/etc/stay-in-rescue`.

## First boot personalisation (the rootfs side of "device support")

The shared image carries `/usr/lib/phonewave-system/` with the pieces that let one
image become a particular phone at first start:

`first-user`, `setup-session`, `join-groups`, `user-groups`,
`enable-new-services` (enable the units whose hardware exists),
`install-flashed` + `flashed-keys` (the `flash package` / `flash ssh-key` flow),
`pacman-keyring`, `droidloom-owner`, and `root` (the marker).

Everything else adapts through ordinary Linux discovery: device tree, udev, DRM,
ALSA, NetworkManager/ModemManager (or Phonewave's `dmail`/`steinerd`/`hapticd`),
fprintd when a reader is present.

## What picasso must supply to fit this model

1. **A boot image** with our kernel plus an appended erofs containing
   `<release>/` modules, firmware overrides (Adreno/Touch/ADSP as we sort them
   out), the early units, and `libexec/stock-firmware` that binds this phone's
   `modem`, `bluetooth` and `persist` partitions.
2. **A system partition**: the shared `userdata.img` written to our `linux`
   partition, renamed `denial` (the marker check accepts either name, but `denial`
   is the documented one), grown by `x-systemd.growfs`.
3. **The shared early userspace, or an equivalent**: our loader has no
   `init_boot` partition, so our RAM initramfs already implements the same steps
   (find system by PARTNAME + marker, gadget before `switch_root`, rescue path);
   adopting 10.77.71.2 and the marker would make it interchangeable.
4. **Device services**: `steinerd` (rotation), `hapticd`, `dmail` need kernel
   interfaces we do not have yet (sensors, haptics, modem QMI); they disappear or
   go read-only when the service or hardware is missing, which is why the image
   still boots without them.
