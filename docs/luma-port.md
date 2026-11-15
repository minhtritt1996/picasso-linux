# Luma Mobile port: Xiaomi Redmi K30 5G (picasso, SM7250)

Working brief for porting [Luma Mobile](https://simplyluma.com) to picasso, prepared by
following https://simplyluma.com/agents/port-a-phone.md (fetched 2026-11-15), together
with `docs/devices.md`, `docs/kernel.md`, `scripts/mobile/build-phone-userdata.sh`,
`scripts/mobile/common/initramfs/luma-ostree-boot.sh` and
`scripts/devices/build-device-kit.py` from https://github.com/SimplyLuma/luma.

Luma does not support this phone: `devices/` on `main` holds `fairphone/fp6`,
`raspberrypi/rpi5`, `microsoft/sp12in`. This is a new port, so tier `experimental`
until it is on the Luma kernel base.

## 1. Target

| Item | Value | Source |
|---|---|---|
| Vendor / codename | Xiaomi / `picasso` | vendor + upstream kernel name |
| Marketing name | Redmi K30 5G | |
| SoC | Qualcomm SM7250 (Snapdragon 765G) | `soc/soc.conf`, vendor DTS |
| Form factor / year | phone / 2019 | |
| Storage | UFS, 6 LUNs, GPT `sda1..sda35` + `sde1..sde62` | `STATUS.md` hardware table |
| Sectors | 4096 B (UFS) → the published 4096-byte userdata table is readable | Luma image gate `layout` |
| Slots | A-only (single `boot`, no `boot_a/b`) | partition scan |
| `userdata` today | `sda34`, f2fs, Android's | `STATUS.md` |
| Bootloader | unlocked (RAM boots through `fastboot boot`) | used all session |
| Factory image | MIUI fastboot ROM (not in the tree); the phone's own partitions are the firmware oracle | |
| Current kernel | v7.3-rc7 + `lazybrar/avicii-mainline` patches (not the Luma base) | `docs/mainline-linux-plan.md` |

Partition names already known from the vendor GPT: `boot`, `dtbo`, `userdata`,
`cache` (ext4, 384 MB, used for logs), `modem` (`sde48`, vfat, holds `image/`),
`bluetooth` (`sde33`, vfat), `dsp`, `modemst1`, `modemst2`, `fsg`, `fsc`,
`persist`. No `vendor` partition (Android dynamic partitions).

## 2. Done criteria — the test that proves each line

Written before any work, per section 1 of the brief. "State today" is what this
workspace has actually observed; everything else is `not yet`.

| Criterion | Proof | State today |
|---|---|---|
| Boots standalone from flash to the Luma lock screen, 3× in a row | power cycle, no USB, no `fastboot boot` | not yet — RAM boot only |
| Display 1080×2400 @ 120 Hz | moving test pattern, `modetest`/compositor report | partial: first real output, then a pre-init hang with DSC (`STATUS.md` display section) |
| Touch accurate to the edges | scripted taps at 4 corners + centre land | not yet (NT36672C driver present) |
| USB networking + charging current | gadget link up 1 h, `/sys/class/power_supply` current | ACM console only; no CDC-ECM gadget yet |
| Wi-Fi associates, traffic 1 h | ping gateway + large transfer | works over `wlan0` + NetworkManager (ssh verified); 1 h soak not run |
| Bluetooth pairs a headset, A2DP plays | pair, connect, play, reconnect after reboot | controller registers with mgmt: not yet (patch and NVM download complete) |
| Speaker, earpiece, mic, jack | tone out, record in | not yet |
| Modem: SIM, registration, data, calls both ways, SMS/MMS | real traffic; a call needs a human | not yet — modem trips `DOG detects stalled initialization` |
| Camera: still + video per sensor, correct colours | per-channel means/variance, colour on a known scene | not yet |
| Suspend/resume, 20 cycles, wake by button and by call | `pm_test` ladder, then 20 cycles | not yet |
| Sensors, GPS, vibration, flashlight, battery % | values change when moved; fix outdoors | not yet |
| Android apps: video with sound, typing in a field | a real app, not the launcher | not yet |
| Updates: origin `luma/1/aarch64/nightly`, update installs, rollback starts | one OS update + one rollback | not yet |
| All of the above on a fresh install from the published image | wipe, follow `INSTALL.md`, first boot without network | not yet |

## 3. What a Luma install consists of on this phone

1. **`userdata`** — the generic phone image, nothing device-specific:
   `luma-nightly-20261114.2-aarch64-disk.sparse`, 9 370 839 372 B, SHA-256
   `d3dd2eebc1cdc2cb52b20b71b26349f3fde84411baad3f133edf328da79fe00b`
   (`https://media.simplyluma.com/media/aarch64/nightly/latest.json`). GPT with
   4096-byte sectors: `LumaBoot` (FAT32, 480 MiB at 1 MiB, volume ID
   `032D-57AD`) and `LumaRoot` (ext4, UUID `03797c05-cf87-4eda-adc4-6ce111baf2d3`).
2. **`boot`** — our image: kernel + picasso DTB + an initramfs carrying the
   shared OSTree resolver (`scripts/mobile/common/initramfs`), command line
   naming `pmos_root_uuid=03797c05-…` and `pmos_boot_uuid=032D-57AD` (the kit
   builder refuses an image without them). The resolver picks the highest
   `version` entry and rewrites the `ostree=` argument; no fixed deployment
   path is ever baked in. A-only: one `boot` partition, no slot switch.
   `dtbo` policy: the stock bootloader concatenates the `dtbo` partition onto
   the boot image, so a complete DTB in `boot` plus stock overlays in `dtbo`
   is wrong; either flash `dtbo` blank (as fp6 does) or ship the DTB as the
   boot image's dtb section and leave `dtbo` untouched — decide on hardware.
3. **Device kit** — with no `[device_repo]` (only Luma signs repositories, and
   that starts at adoption), modules and firmware live in the boot image, as
   the Pixel 9 port does. Firmware comes from the phone's own `modem`,
   `bluetooth` and `dsp` partitions: `firmware.manifest` carries names and
   SHA-256 only, `INSTALL.md` says where the person gets them.
4. **Kernel** — one Luma kernel. Verified with `git ls-remote` and the GitHub
   API on 2026-11-15:

   | ref | commit | content |
   |---|---|---|
   | `luma-7.2` (base) | `497791a0a96b` | upstream `v7.2.15` |
   | `sm7250` (family) | `497791a0a96b` | **identical to the base: empty** |
   | `milos-7.2` | `f20e69edcd1c` | 135 commits ahead — the worked family branch to copy |

   `arch/arm64/boot/dts/qcom/` in the base has no `sm7250*` file (606 DT files,
   `sm6350`/`sm8250`/`sm8450` and friends only), so the family `.dtsi` and the
   board DTS are net-new, exactly as this workspace has them.
   `luma/config/` exists only on family branches and holds just
   `<family>.fragment` (`milos.fragment`: 425 lines). Its non-negotiables for a
   phone: `CONFIG_FUSE_FS=y`, `CONFIG_ANDROID_BINDER_IPC=y`,
   `CONFIG_SECURITY_SELINUX=y` with `CONFIG_LSM="…,selinux,…"`,
   `CONFIG_PSI=y`, `CONFIG_ZRAM=y`.

   Rebase work, **measured** on 2026-11-15: shallow clone of `luma-7.2`
   (`git clone --depth 1 --single-branch --branch luma-7.2`, 2.1 GB, HEAD
   `497791a0a Linux 7.2.15`), then `git apply --check` for each patch in
   `patches/linux/picasso/` (they are plain `git diff` files, so `git apply`,
   not `git am`; the PR itself will be commits).

   | Patch | Files | Result on v7.2.15 |
   |---|---|---|
   | 0001 sm7250 reserved memory | `sm7250.dtsi`, `sm7250-oneplus-avicii.dts` | **conflict**: the avicii DTS files do not exist in the base — DTS is not a patch there but a new file |
   | 0002 picasso board | `sm7250-xiaomi-picasso.dts`, `Makefile`, `qcom.yaml` | **conflict** (`Makefile` line 353 drift) — add the board DTS as a new file plus its `Makefile`/`qcom.yaml` hunk |
   | 0003 printk RAM console | `kernel/printk/printk.c` | applies cleanly |
   | 0004 dd probe trace | `drivers/base/dd.c` | applies, but it is debug scaffolding: **drop before the PR** |
   | 0005 pinctrl-msm unmapped tiles | `drivers/pinctrl/qcom/pinctrl-msm.c` | applies cleanly |
   | 0006 panel g7a-37-02-0a | binding, `Kconfig`, new driver, `Makefile` | binding + `Kconfig` + driver apply cleanly; the `Makefile` one-liner anchors on a 7.3 line (`panel-samsung-ams644vk04`) — re-add it in alphabetical position |
   | 0007 pinmux + audio | `picasso-pinmux.c`, q6asm-dai, soc-pcm, pcm_native | applies cleanly |

   So the family branch is: new `sm7250.dtsi` + `sm7250-xiaomi-picasso.dts`
   (credit `lazybrar/avicii-mainline` and the mainline `sm8250` dtsi in the
   commit messages), four patches as they are, the panel Makefile line, and
   `luma/config/sm7250.fragment` derived from the working config in
   `build/linux/out/.config` plus what `milos.fragment` proves a phone needs.

5. **`socs/qcom/sm7250/soc.toml`** does not exist yet (HTTP 404 on `main`) — the
   family branch does. It must be added with `repository = SimplyLuma/linux`,
   `branch = sm7250`, page size, load offsets and the firmware directory.
6. **Boot image format** (proven on this phone by the Droidian work,
   `droidian/docs/02-boot-image-layout.md`): `mkbootimg` header v2, base 0,
   pagesize 4096, kernel at `0x8000` (`Image.gz`), ramdisk at `0x1200000`
   (**not** `0x1000000` — a ~48 MB decompressed kernel lands there), tags at
   `0x100`, DTB at `0x1728000`; the DTB field is required or fastboot drops the
   image. The page size matches the Luma initramfs' 4096-byte GPT. An unlocked
   bootloader boots unsigned images (custom ROMs do), so no AVB footer is
   needed; `boot.toml [avb] footer` can stay `none` unless a boot fails without it.
7. **The boot image target exists**: `./build.sh bootimg-luma` builds
   `build/out/boot-luma.img` from `Image`, `build/out/initrd.cpio.gz` and the
   picasso DTB with `tools/linux/build-luma-bootimg.py`. It refuses a command
   line without `pmos_root_uuid`/`pmos_boot_uuid`, refuses overlapping
   kernel/ramdisk/DTB load addresses and an image over the partition limit, and
   re-reads the result with `unpack_bootimg`. Current image: 20 299 776 B,
   kernel @0x8000, ramdisk @0x1200000, DTB @0x1728000. This is exactly the
   vehicle the stock bootloader uses, so `fastboot boot build/out/boot-luma.img`
   (RAM-only, allowed) proves the Luma boot path without writing anything.

8. **Kit constraints** (`scripts/devices/build-device-kit.py`): the boot image's
   command line must contain `pmos_root_uuid=03797c05-…` and
   `pmos_boot_uuid=032D-57AD` or the kit build refuses it; `[kit] flash` accepts
   only `flash <part> {userdata|kit|boot|dtbo}`, `boot {boot}`,
   `--set-active=[ab]`, `reboot`, and refuses preserved partitions. With no
   `[device_repo]` (no Luma signing key) there is no `…-kit.sparse`, so the boot
   image carries modules and firmware.

## 4. Working rules in this workspace

- **RAM-only by default.** Everything in this port so far is `fastboot boot`,
  `oem ramdisk` + `stage` + `oem run`, or `reboot` back into the loader. The
  one exception in the log is the `boot` partition, written once on the owner's
  explicit instruction (`STATUS.md`, "Workspace invariant note").
- **Nothing that writes to the phone happens without the owner's standing
  approval** (section 2 of the brief): unlocking, flashing, erasing, `dd`,
  `set_active`. What each write changes and what can go wrong is stated in the
  approval request, not discovered afterwards.
- **Never write**: `modemst1`, `modemst2`, `fsg`, `fsc`, `persist` (radio
  calibration and the phone's identity), `devinfo`, `misc`, `frp`, `keystore`.
  Never relock the bootloader.
- **One path back exists at all times**: the MIUI fastboot ROM for picasso, and
  the loader chain images kept in `build/out/` (`boot-linux.img`,
  `payload-linux.bin`).
- **Self-healing**: the payload command line now carries `panic=10`, so an
  early kernel panic resets the SoC back to the bootloader instead of leaving
  the phone dark (learned the hard way, 2026-11-14).

## 5. Fast loop (no flashing)

Proven in this workspace, and exactly section 3's "change it live" loop:

- **Root filesystem in RAM** through the loader's resident ramdisk
  (`fastboot oem ramdisk` + `stage`), up to ~4 GiB; a Fedora 44 aarch64 tree
  boots to a shell and a desktop today.
- **ssh over Wi-Fi** (`wlan0`, key-only) as the primary path, USB-ACM console as
  the fallback (it wedges when a daemon starts; Wi-Fi stays up).
- **Logs**: `pstore`/RAM console window `0xF3F00000` folded into the loader log
  (`fastboot get_staged`), plus the UFS `cache` partition (ext4, rw) for
  journals across boots.
- **Rebuild loop**: `./build.sh linux` → `./build.sh bootimg` → `fastboot boot`
  or stage; a kernel rebuild is ~30 s once the tree is warm.

## 6. Agreed with the owner (2026-11-15, in writing)

Standing actions, no per-run question:

| Allowed | Notes |
|---|---|
| Reboot the phone (`ssh reboot`, `fastboot reboot`, `oem run`) | |
| RAM-boot images (`fastboot boot`, `oem ramdisk` + `stage` + `oem run`) | never writes flash |
| Restart services, deploy OS updates on a running system | |

Always a fresh OK: real calls and texts to the owner's number, anything that
touches `modemst1`/`modemst2`/`fsg`/`fsc`/`persist`, anything that could brick
(repartition, relock, `vbmeta`).

Not allowed yet:

- Writing `boot`, `dtbo` or `userdata`. The owner's words: **RAM-boot only
  until the port is finished; flashing `userdata` is a separate decision
  later.** This is stricter than the brief's default and holds until the owner
  changes it.
- The owner has the picasso MIUI fastboot ROM, so the way back to stock exists;
  it is not on this machine yet.
- Pulling/replugging the cable at the office is "later" — the phone is dead
  until then.

## 7. Open items

1. The phone is hung (panic with USB dead since 2026-11-14 22:30): it needs a
   power cycle or a cable pull before any of the above can run.
2. The way back to stock is on the dev machine already, in the Droidian
   workspace's `downloads/`: `miui_PICASSO_V13_bn.zip`,
   `miui_PICASSO_V13.0.5.0.SGICNXM_*.zip` and
   `OrangeFox-R11.3_3-picasso.zip`. A read-only partition dump is still worth
   taking before the first write.
3. `socs/qcom/sm7250/soc.toml` to write, kernel rebase onto `luma-7.2`, and the
   boot image format decision (stock bootloader header v2 with the DTB section,
   versus the current UEFI-shim chain) — all research, no phone writes.
4. Whether the phone may be carried for a day once suspend and modem work
   (section 4, step 13).
5. Autonomous recovery from a *hang* (panic=10 only covers panics): the RAM
   rootfs should run a hardware watchdog (`RuntimeWatchdogSec=60` in systemd,
   `qcom_wdt` in the kernel), so a stalled kernel resets the SoC by itself
   instead of waiting for hands (brief section 2).
