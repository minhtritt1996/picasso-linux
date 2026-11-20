# picasso-linux

Mainline Linux trên Xiaomi Redmi K30 5G (`picasso`, Qualcomm SM7250 / Snapdragon 765G),
boot hoàn toàn từ RAM qua Mu-Silicium UEFI — không ghi UFS, không flash.

Đây không phải ROM Android. Mục tiêu là một port kernel mainline hoàn chỉnh chạy
[Denial Mobile](https://github.com/denysvitali/denial) trên phần cứng thật.

---

## Trạng thái bringup (2026-11-20)

| Subsystem | Trạng thái |
|---|---|
| Boot (RAM-only, UEFI chain) | ✅ **DONE** — `fastboot boot` → XBL → BootShim → Mu-Silicium → XnuLoader → `linux.efi` → kernel |
| 8 CPU / 5305 MiB RAM | ✅ **DONE** |
| USB console (CDC-ACM) | ✅ **DONE** — `/dev/ttyACM0` tại t≈2 s, shell qua `console_cmd.py` |
| Log persistence (RAM console) | ✅ **DONE** — `0xF3F00000` 1 MiB, đọc bằng `fastboot get_staged` sau reset |
| UFS enumerate + mount | ✅ **DONE** — 6 LUN, GPT, `userdata` (sda34, f2fs) mount read-only |
| Display 1080×2400 @ 120 Hz | ✅ **DONE** — DPU 6.1 / DSI / 7nm PHY / panel `xiaomi-g7a`, DSC native |
| Backlight (WLED) | ✅ **DONE** — `pm8150l_wled`, DPMS ON/OFF, zero OVP IRQ |
| Touch (Novatek NT36672C, SPI) | ✅ **DONE** — `NVTCapacitiveTouchScreen`, 1080×2400, `event2` |
| GPU (Adreno 620) | ✅ **DONE** — Mesa / freedreno, zero GPU fault/hangcheck, `deniald` render `XR30` |
| Wi-Fi (WCN3990 / ath10k_snoc) | ✅ **DONE** — `wlan0` tự lên, scan 2.4 & 5 GHz WPA2, kết nối DHCP |
| Bluetooth (WCN3991) | ✅ **DONE** — patch download `crbtfw32.tlv` + `crnv32.bin`, `hci0` powered |
| USB ECM networking | ✅ **DONE** — `enx020000000001`, DHCP `10.77.71.1/30`, ping 0.29 ms |
| SSH | ✅ **DONE** — sshd trên `0.0.0.0:22`, key login qua USB (`10.77.71.2`) và Wi-Fi |
| Audio (PipeWire/UCM2) | ✅ **DONE** — TFA9874 amp (i2c8/0x34), WCD938x, ADSP PAS, `pw-play` → speaker |
| Modem (tqftpserv / pd-mapper / rmtfs) | ✅ **DONE** — `remoteproc1` running >86 s, QRTR hoạt động |
| ADSP + sensors (steinerd) | ✅ **DONE** — `hexagonrpcd`, ALS/accel/gyro/rotation, không còn ADSP watchdog reset |
| Battery (pm7250b / QG) | ✅ **DONE** — `VOLTAGE_NOW`, `CURRENT_NOW`, `CAPACITY`, `upower` charging |
| Buttons (power / vol+/ vol-) | ✅ **DONE** — `KEY_POWER`, `KEY_VOLUMEDOWN`, `KEY_VOLUMEUP` |
| Video decode (Venus H.264) | ✅ **DONE** — `h264_v4l2m2m`, 1280×720 @ ~476 fps |
| Camera orientation (V4L2) | ✅ **DONE** — ov8856 90°, imx686 90°, s5k3t2 270°, `braun` nhận rotation |
| Droidloom (Android container) | ✅ **DONE** — Android cell boot, TikTok cài được |
| Denial Mobile rootfs on UFS | ✅ **DONE** — `sda36` (20 GiB ext4), systemd PID 1, SSH, `pacman -Syu` |
| NFC (NXP NCI) | ✅ **DONE** — `0018-nfc-nxp-nci-keep-irq-disabled-when-cold.patch` |
| pacman / system upgrades | ✅ **DONE** — `droidloom-image` & `droidloom-runtime` lên 0.1.0-42 |

> **Boot #220** (2026-11-18) là lần verify gần nhất — 39 patches apply sạch, `dtbs_check` pass,
> `deniald` render 120 Hz, `pw-play` → speaker, touch 1080×2400 active.

---

## Chuỗi boot

```text
XBL → fastboot boot boot-linux.img
      → BootShim (copy FD → 0x9F100000, XNUP → 0xF0000000)
      → Mu-Silicium UEFI
      → XnuLoader (entry "loader.efi")
      → linux.efi (EFI stub, cài EFI config table DTB)
      → Linux Image + initramfs
      → Denial Mobile (systemd, trên UFS hoặc thuần RAM)
```

Không ghi UFS. Không flash. Mọi thứ sống trong RAM.

---

## Layout

```text
upstream/linux              torvalds/linux v7.3-rc7, pinned (read-only, gitignored)
upstream/avicii-mainline    reference SM7250 port — OnePlus Nord (read-only, gitignored)
upstream/hakios-xnu-kernel  XnuLoader source cho EFI launcher (read-only, gitignored)

patches/linux/UPSTREAM      pin commit của upstream, source of truth duy nhất
patches/linux/series        thứ tự patch (vendor/ trước, picasso/ sau)
patches/linux/vendor/       14 patch avicii-mainline, nguyên bản — không sửa
patches/linux/picasso/      24 patch delta của picasso (export từ build/linux/)

configs/linux/              Kconfig fragment (picasso.config)
prebuilt/                   identity.h, fw-gpu/, fw-touch/, device-tree.dtb
tools/
  build-bootimg.py          XNUP payload + boot-linux.img (UEFI chain)
  build-luma-bootimg.py     Android boot v2 cho path ABL stock
  mkinitramfs.sh            RAM initramfs nhỏ (~0.3 MB, static init)
  pack-rootfs.sh            RAM root filesystem đầy đủ
  console_cmd.py            USB-ACM console: một lệnh, capture, reboot
  ramboot-cycle.sh          một hardware gate cycle (arm → stage → run → log)
  policy-scan.sh            gate 1 rules
  preflight.sh              gate 1 locally / gate 3 static half trong CI
docs/
  mainline-linux-plan.md    kế hoạch kỹ thuật đầy đủ
  dts-crosswalk.md          crosswalk nguồn gốc từng DT node quan trọng
  luma-port-brief.md        tóm tắt port Luma (ABL path)
.github/
  workflows/ci.yml          CI: apply series + clean builds
  workflows/README.md       mô tả 4 gates
build/                      cây build cô lập + build/out/ (gitignored)
```

---

## Patch series (39 patches)

### vendor/ — avicii-mainline cho SM7250 (14 patches, không sửa)

| Patch | Nội dung |
|---|---|
| 01-bindings | qcom IDs SM7250, socinfo, SCM, PDC, cpufreq |
| 02-clk | GCC, RPMh CC, dispcc, gpucc, videocc, camcc |
| 03-pinctrl | TLMM SM7250 + UART pinctrl |
| 04-interconnect | RPMh NoC SM7250 |
| 05-soc-pm | AOSS, RPMH, PDC, power domains |
| 06-ufs | UFS host SM7250 |
| 07-display | MDSS/DPU/DSI/DSC/PHY SM7250 |
| 08-iommu | SMMU context banks |
| 09-power | PMIC/regulators PM8150/PM8150B/PM8150L |
| 10-touch | Novatek NT36xxx i2c (reference) |
| 11-remoteproc | Q6V5 PAS (modem/adsp) |
| 12-audio | APR, q6afe/q6asm/q6adm, WCD938x, WSA881x |
| 13-media | Venus codec, CAMSS |
| 14-dts | DTS OnePlus Nord (avicii) |

### picasso/ — delta của thiết bị (24 patches)

| Patch | Nội dung |
|---|---|
| 0001 | dts: move board reserved-memory |
| 0002 | dts: `sm7250-xiaomi-picasso.dts` — DTS đầy đủ của thiết bị (toàn bộ bringup, camera rotation) |
| 0003 | printk: RAM console `0xF3F00000` |
| 0004 | base: dd probe trace (debug initcall) |
| 0005 | pinctrl-msm: skip unmapped tiles (SOUTH-only window) |
| 0006 | drm: panel `xiaomi-g7a` (1080×2400 DSC 120Hz) |
| 0007 | picasso bringup: pinmux driver (uart5/i2c8/MI2S/TLMM east+west), audio, GPU |
| 0008 | bluetooth: hci_qca WCN3991 flow control |
| 0009 | drm/msm/a6xx: fix OPP use-after-free |
| 0010 | drm/msm/a6xx: fix RPMh dependency votes |
| 0011 (input) | touch: Novatek NT36672C SPI driver (SRAM firmware, page cache, IRQ) |
| 0011 (net) | qrtr-ns: retry announce on EAGAIN (Wi-Fi bringup) |
| 0012 | backlight: qcom-wled OVP IRQ balance |
| 0013 | drm/msm: drop per-frame pr_info (console spam) |
| 0014 | media/venus: skip IRIS1 power collapse on idle timeout |
| 0015 | power-supply: pm7250b QG battery |
| 0016 | ASoC/wcd938x: guard SoundWire capture ports |
| 0017 | async status diagnostics (init monitor) |
| 0018 | nfc: NXP NCI keep IRQ disabled when cold |
| 0019 | ASoC/q6afe: preserve active channels mask |
| 0020 | soundwire/qcom: resync slave status after alert |
| 0021 | pinctrl SM7250: eGPIO ownership |
| 0022 | clk/qcom/camcc: SM7250 PLL2 v2 |
| 0023 | media/camss: add SM7250 CPHY |
| 0024 | media/i2c: add imx686 + s5k3t2 camera drivers |

---

## Build

```sh
./build.sh linux                # Image + sm7250-xiaomi-picasso.dtb → build/out/
./build.sh loader               # linux.efi (patched XnuLoader) → build/out/
./build.sh export-patches       # regenerate patches/linux/picasso/*.patch
./build.sh check                # apply series sạch, dtbs_check
```

```sh
./tools/mkinitramfs.sh          # build/out/initrd.cpio.gz (~0.3 MB)

UEFI_FD=<SILICIUM_UEFI.fd> BOOTSHIM=<BootShim.bin> UEFI_DTB=<picasso.dtb> \
  ./build.sh bootimg            # build/out/boot-linux.img + payload-linux.bin
```

**Yêu cầu:** `aarch64-linux-gnu-gcc`, `make`, `bison`, `flex`, `bc`, `libssl-dev`,
`libelf-dev`, `python3`, `git`.

**dtschema** (một lần duy nhất):
```sh
./tools/setup-dtschema.sh       # virtualenv local, không cần root
```
Nếu thiếu, `./build.sh check` vẫn chạy nhưng bỏ qua schema validation.

---

## Boot

```sh
# lần đầu, từ stock bootloader
fastboot boot build/out/boot-linux.img

# mọi lần sau (kernel/initramfs mới): không cần UFS
fastboot stage build/out/payload-linux.bin && fastboot oem run
```

```sh
# console USB
python3 tools/console_cmd.py uname
python3 tools/console_cmd.py mem
python3 tools/console_cmd.py reboot

# đọc log sau reset
fastboot get_staged
```

---

## Gates

| Gate | Lệnh | Chứng minh |
|---|---|---|
| 1 preflight | `tools/preflight.sh` | docs, policy scan, patch series, syntax, whitespace |
| 2 change | PR checklist | chỉ patch; `vendor/` không được sửa |
| 3 CI | `./build.sh check` | 39 patches apply sạch; mọi artifact build được |
| 4 hardware | `tools/ramboot-cycle.sh` | một RAM boot: arm → stage → run → capture; không flash |

Mô tả đầy đủ: [`.github/workflows/README.md`](.github/workflows/README.md).

---

## Nguồn tham chiếu

- [lazybrar/avicii-mainline](https://github.com/lazybrar/avicii-mainline) — mainline Linux cho OnePlus Nord (`avicii`), cùng SoC SM7250; base `v7.3-rc7`, 14 patch theo subsystem
- [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) — XnuLoader (EFI launcher) và Mu-Silicium UEFI cho HakiOS
- [YinCQL/UniXNU-picasso](https://github.com/YinCQL/UniXNU-picasso) — port XNU/UniXNU cho picasso, nguồn tham chiếu init sequence và driver hint
