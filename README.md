# picasso-linux

Mainline Linux (`v7.3.5`) + [Denial Mobile](https://github.com/denysvitali/denial) cho **Xiaomi Redmi K30 5G (`picasso`, Qualcomm SM7250 / Snapdragon 765G)**, khởi động hoàn toàn từ RAM qua **Mu-Silicium UEFI** (không ghi UFS, không flash phân vùng hệ thống).

[Phần cứng](#trạng-thái-phần-cứng) · [Chuỗi boot](#chuỗi-boot-ram-only) · [Build](#build-từ-mã-nguồn) · [Boot & Debug](#boot--debug) · [Danh sách patch](#danh-sách-patch-39-patches) · [Cấu trúc](#cấu-trúc-repository) · [Donate](#ủng-hộ-dự-án-donate)

---

## Trạng thái phần cứng

Cập nhật đến **2026-11-20** (kiểm chứng gần nhất tại **Boot #220** ngày 2026-11-18: 39 patches apply sạch, `dtbs_check` pass, `deniald` render 120 Hz, `pw-play` phát loa ngoài, cảm ứng 1080×2400 hoạt động ổn định):

| Subsystem | Trạng thái | Chi tiết kiểm chứng trên máy thật |
| :--- | :---: | :--- |
| **Boot (RAM-only, UEFI)** | ✅ | `fastboot boot` -> XBL -> BootShim -> Mu-Silicium -> XnuLoader -> `linux.efi` -> kernel |
| **CPU & Bộ nhớ** | ✅ | Đủ 8 nhân CPU / 5305 MiB RAM |
| **USB Console (CDC-ACM)** | ✅ | `/dev/ttyACM0` sẵn sàng tại `t ≈ 2s`, điều khiển shell qua `console_cmd.py` |
| **Log Persistence (RAM)** | ✅ | Vùng nhớ `0xF3F00000` (1 MiB), trích xuất sau reset bằng `fastboot get_staged` |
| **Lưu trữ UFS** | ✅ | Nhận đủ 6 LUN, GPT, mount read-only `userdata` (`sda34`, `f2fs`) |
| **Màn hình (DPU / DSI)** | ✅ | DPU 6.1 / DSI / 7nm PHY / panel `xiaomi-g7a` `1080×2400 @ 120 Hz`, DSC native |
| **Đèn nền (WLED)** | ✅ | `pm8150l_wled`, bật/tắt DPMS mượt, không phát sinh OVP IRQ |
| **Cảm ứng (SPI)** | ✅ | Novatek NT36672C (`NVTCapacitiveTouchScreen`), `1080×2400`, `event2` |
| **GPU (Adreno 620)** | ✅ | Mesa / freedreno, 0 lỗi GPU fault/hangcheck, `deniald` render `XR30` |
| **Wi-Fi (WCN3990)** | ✅ | `ath10k_snoc`, `wlan0` tự khởi tạo, quét 2.4 & 5 GHz WPA2, nhận DHCP |
| **Bluetooth (WCN3991)** | ✅ | Tải firmware `crbtfw32.tlv` + `crnv32.bin`, `hci0` ở trạng thái powered |
| **Mạng USB ECM** | ✅ | Giao diện `enx020000000001`, DHCP `10.77.71.1/30`, độ trễ ping `0.29 ms` |
| **SSH** | ✅ | `sshd` lắng nghe trên `0.0.0.0:22`, đăng nhập bằng SSH key qua USB (`10.77.71.2`) và Wi-Fi |
| **Âm thanh (PipeWire/UCM2)** | ✅ | Ampli `TFA9874` (`i2c8/0x34`), `WCD938x`, ADSP PAS, phát trực tiếp qua loa ngoài (`pw-play`) |
| **Modem & QRTR** | ✅ | `tqftpserv` / `pd-mapper` / `rmtfs`, `remoteproc1` chạy ổn định >86s, QRTR hoạt động |
| **ADSP & Cảm biến** | ✅ | `hexagonrpcd` + `steinerd` (ALS / accel / gyro / rotation), hết lỗi reset watchdog ADSP |
| **Pin (Fuel Gauge)** | ✅ | `pm7250b` QG đọc `VOLTAGE_NOW`, `CURRENT_NOW`, `CAPACITY`, báo trạng thái sạc qua `upower` |
| **Phím cứng** | ✅ | `KEY_POWER`, `KEY_VOLUMEDOWN`, `KEY_VOLUMEUP` |
| **Giải mã Video (Venus)** | ✅ | `h264_v4l2m2m`, giải mã `1280×720` đạt `~476 fps` |
| **Camera (V4L2)** | ✅ | Định hướng cảm biến `ov8856` (90°), `imx686` (90°), `s5k3t2` (270°), `braun` nhận đúng rotation |
| **Droidloom (Android Cell)** | ✅ | Khởi động được Android container, cài đặt và chạy ứng dụng (TikTok) |
| **Denial Mobile trên UFS** | ✅ | Rootfs trên `sda36` (20 GiB `ext4`), `systemd` PID 1, SSH, cập nhật `pacman -Syu` |
| **NFC (NXP NCI)** | ✅ | Giữ IRQ tắt khi cold boot (`0019-nfc-nxp-nci-keep-irq-disabled-when-cold.patch`) |
| **Nâng cấp gói hệ thống** | ✅ | `pacman` nâng cấp `droidloom-image` & `droidloom-runtime` lên `0.1.0-42` |

---

## Chuỗi boot (RAM-only)

```text
XBL -> fastboot boot boot-linux.img
    -> BootShim (copy FD -> 0x9F100000, XNUP -> 0xF0000000)
    -> Mu-Silicium UEFI
    -> XnuLoader (entry "loader.efi")
    -> linux.efi (EFI stub, cài EFI config table DTB)
    -> Linux Image + initramfs
    -> Denial Mobile (systemd, trên UFS hoặc thuần RAM)
```

Toàn bộ quá trình boot diễn ra trên RAM, không ghi đè hay chỉnh sửa phân vùng khởi động trên UFS.

---

## Build từ mã nguồn

### Yêu cầu công cụ & blob độc quyền
- **Công cụ build:** `aarch64-linux-gnu-gcc`, `make`, `bison`, `flex`, `bc`, `libssl-dev`, `libelf-dev`, `python3`, `git`
- **Blob tách riêng (gitignored, cần đặt đúng vị trí trước khi đóng gói `initrd` / `bootimg`):**
  - Firmware GPU / Touch / Wi-Fi (`prebuilt/fw-*/`): xem [`prebuilt/README.md`](prebuilt/README.md)
  - UEFI (`uefi/SILICIUM_UEFI.fd`, `uefi/picasso.dtb`): xem [`uefi/README.md`](uefi/README.md)
  - Modem / QRTR helpers (`initramfs/bin/`): xem [`initramfs/bin/README.md`](initramfs/bin/README.md)

### Các bước thực hiện

```sh
# 1. Cài dtschema cục bộ (tùy chọn, chỉ làm 1 lần; thiếu vẫn chạy check nhưng bỏ qua schema validation)
./scripts/setup-dtschema.sh

# 2. Biên dịch kernel, DTB và EFI loader
./build.sh linux                # Image + sm7250-xiaomi-picasso.dtb -> build/out/
./build.sh loader               # linux.efi (patched XnuLoader) -> build/out/

# 3. Đóng gói initramfs và ảnh boot
./scripts/mkinitramfs.sh        # build/out/initrd.cpio.gz (~0.6 MB, xz)
./build.sh bootimg              # build/out/boot-linux.img + payload-linux.bin (USB fastboot listener)
./build.sh bootimg-auto         # build/out/boot-linux-auto.img (one-shot auto-boot, không cần máy tính)

# 4. Kiểm chứng toàn trình & xuất patch
./build.sh check                # apply sạch 39 patches + dtbs_check
./build.sh export-patches       # cập nhật patches/linux/picasso/*.patch từ build/linux/
```

---

## Boot & Debug

```sh
# Boot tự động hoàn toàn từ UEFI (không cần kết nối máy tính sau khi gửi ảnh)
fastboot boot build/out/boot-linux-auto.img

# Chế độ dev với USB fastboot listener trong XnuLoader
fastboot boot build/out/boot-linux.img

# Các lần nạp lại kernel/initramfs tiếp theo trong chế độ dev (thuần RAM)
fastboot stage build/out/payload-linux.bin && fastboot oem run
```

```sh
# Tương tác qua console USB (/dev/ttyACM0)
python3 scripts/console_cmd.py uname
python3 scripts/console_cmd.py mem
python3 scripts/console_cmd.py reboot

# Lấy log RAM console (0xF3F00000) sau khi thiết bị reset
fastboot get_staged
```

### Quy trình kiểm soát chất lượng (Gates)

| Gate | Lệnh | Mục đích |
| :--- | :--- | :--- |
| **1. Preflight** | `scripts/preflight.sh` | Kiểm tra docs, policy scan, thứ tự patch series, cú pháp và khoảng trắng |
| **2. Change** | PR checklist | Chỉ cho phép thay đổi dạng patch; giữ nguyên thư mục `vendor/` |
| **3. CI** | `./build.sh check` | Áp dụng sạch 39 patches và biên dịch toàn bộ artifact |
| **4. Hardware** | `scripts/ramboot-cycle.sh` | Chu trình RAM boot khép kín: arm -> stage -> run -> capture log (không flash) |

Chi tiết 4 gate xem tại [`.github/workflows/README.md`](.github/workflows/README.md).

---

## Danh sách patch (39 patches)

### `vendor/` — Nền SM7250 từ `avicii-mainline` (14 patches, giữ nguyên bản)

| Patch | Nội dung |
| :--- | :--- |
| `01-bindings` | Qualcomm IDs SM7250, socinfo, SCM, PDC, cpufreq |
| `02-clk` | GCC, RPMh CC, dispcc, gpucc, videocc, camcc |
| `03-pinctrl` | TLMM SM7250 + UART pinctrl |
| `04-interconnect` | RPMh NoC SM7250 |
| `05-soc-pm` | AOSS, RPMH, PDC, power domains |
| `06-ufs` | UFS host SM7250 |
| `07-display` | MDSS / DPU / DSI / DSC / PHY SM7250 |
| `08-iommu` | SMMU context banks |
| `09-power` | PMIC / regulators PM8150, PM8150B, PM8150L |
| `10-touch` | Novatek NT36xxx I2C (reference) |
| `11-remoteproc` | Q6V5 PAS (modem / ADSP) |
| `12-audio` | APR, q6afe / q6asm / q6adm, WCD938x, WSA881x |
| `13-media` | Venus codec, CAMSS |
| `14-dts` | DTS gốc OnePlus Nord (`avicii`) |

### `picasso/` — Delta riêng cho Redmi K30 5G (25 patches)

| Patch | Nội dung |
| :--- | :--- |
| `0001` | `dts`: di chuyển vùng `reserved-memory` của board |
| `0002` | `dts`: `sm7250-xiaomi-picasso.dts` hoàn chỉnh cho thiết bị (bringup & camera rotation) |
| `0003` | `printk`: RAM console tại `0xF3F00000` |
| `0004` | `base`: dd probe trace (debug initcall) |
| `0005` | `pinctrl-msm`: bỏ qua các tile chưa map (cửa sổ SOUTH-only) |
| `0006` | `drm`: panel `xiaomi-g7a` (1080×2400 DSC 120Hz) |
| `0007` | `picasso`: pinmux driver (`uart5`/`i2c8`/MI2S/TLMM east+west), audio, GPU |
| `0008` | `bluetooth`: điều khiển luồng `hci_qca` cho WCN3991 |
| `0009` | `drm/msm/a6xx`: sửa lỗi use-after-free ở OPP |
| `0010` | `drm/msm/a6xx`: sửa vote phụ thuộc RPMh |
| `0011` | `touch`: driver SPI Novatek NT36672C (SRAM firmware, page cache, IRQ) |
| `0012` | `qrtr-ns`: thử lại announce khi gặp `EAGAIN` (bringup Wi-Fi) |
| `0013` | `backlight`: cân bằng OVP IRQ trên `qcom-wled` |
| `0014` | `drm/msm`: bỏ log `pr_info` theo từng khung hình (giảm spam console) |
| `0015` | `media/venus`: bỏ qua IRIS1 power collapse khi idle timeout |
| `0016` | `power-supply`: driver pin `pm7250b` QG |
| `0017` | `ASoC/wcd938x`: bảo vệ các cổng capture SoundWire |
| `0018` | `init`: chẩn đoán trạng thái async (init monitor) |
| `0019` | `nfc`: giữ IRQ NXP NCI tắt khi cold boot |
| `0020` | `ASoC/q6afe`: bảo toàn mask kênh đang hoạt động |
| `0021` | `soundwire/qcom`: đồng bộ lại trạng thái slave sau alert |
| `0022` | `pinctrl SM7250`: quyền sở hữu eGPIO |
| `0023` | `clk/qcom/camcc`: SM7250 PLL2 v2 |
| `0024` | `media/camss`: bổ sung SM7250 CPHY |
| `0025` | `media/i2c`: bổ sung driver cảm biến camera `imx686` và `s5k3t2` |

---

## Cấu trúc repository

```text
upstream/linux              linux-stable v7.3.5, pinned (read-only, gitignored)
upstream/avicii-mainline    port tham chiếu SM7250 — OnePlus Nord (read-only, gitignored)
upstream/hakios-xnu-kernel  mã nguồn XnuLoader cho EFI launcher (read-only, gitignored)

patches/linux/UPSTREAM      pin commit upstream (single source of truth)
patches/linux/series        thứ tự áp dụng patch (vendor/ trước, picasso/ sau)
patches/linux/vendor/       14 patch từ avicii-mainline (không sửa)
patches/linux/picasso/      25 patch delta của picasso (xuất từ build/linux/)

configs/linux/              fragment Kconfig (picasso.config)
prebuilt/                   identity.h (fw-gpu/, fw-touch/, fw-wcn3990/ lấy từ máy, gitignored)
uefi/                       BootShim.bin; picasso.dtb + SILICIUM_UEFI.fd (gitignored)
scripts/                    build-bootimg.py, mkinitramfs.sh, gen-boot-logo.py, console_cmd.py,
                            ramboot-cycle.sh, policy-scan.sh, preflight.sh, setup-dtschema.sh
initramfs/                  init.c (PID 1), boot_logo.h, tone/, gnss/, bin/ (gitignored)
bringup/                    công cụ debug trên máy: audiocheck, touch-probe, pmtest-ladder, suspend-*
docs/                       tài liệu phần cứng, kiến trúc Denial Mobile, nghiên cứu bringup & boot history
.github/workflows/          CI build/apply series & tài liệu mô tả 4 gates
build/                      thư mục build cô lập và đầu ra build/out/ (gitignored)
```

---

## Tài liệu & Tham chiếu

### Tài liệu nội bộ
- [`docs/hardware-status.md`](docs/hardware-status.md) — trạng thái phần cứng hiện tại và giới hạn đã biết
- [`docs/changelog/boot-history.md`](docs/changelog/boot-history.md) — nhật ký chi tiết từng lần boot (cmdline, hash, log)
- [`docs/bringup/`](docs/bringup/) — ghi chép nghiên cứu từng phân hệ (camera, NFC, GNSS, audio, ...)
- [`docs/mainline-linux-plan.md`](docs/mainline-linux-plan.md) & [`docs/dts-crosswalk.md`](docs/dts-crosswalk.md) — kế hoạch kỹ thuật và đối chiếu Device Tree
- [`docs/denial-mobile-architecture.md`](docs/denial-mobile-architecture.md) — kiến trúc tích hợp Denial Mobile

### Dự án tham chiếu
- [lazybrar/avicii-mainline](https://github.com/lazybrar/avicii-mainline) — mainline Linux cho OnePlus Nord (`avicii`, SM7250, base `v7.3-rc7`)
- [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) — XnuLoader (EFI launcher) và Mu-Silicium UEFI cho HakiOS
- [YinCQL/UniXNU-picasso](https://github.com/YinCQL/UniXNU-picasso) — port XNU/UniXNU cho `picasso`, tham chiếu chuỗi khởi tạo và driver hint

---

## Ủng hộ dự án (Donate)

Nếu dự án giúp ích cho việc nghiên cứu kernel mainline hoặc chạy Denial Mobile trên Xiaomi Redmi K30 5G, bạn có thể ủng hộ quá trình phát triển qua địa chỉ ví EVM duy nhất bên dưới:

```text
0xc604a10270d5e37fa36676f2dec54383399b7e51
```

| Mạng lưới hỗ trợ (EVM) | Token gợi ý |
| :--- | :--- |
| **Ethereum / Arbitrum / Optimism / Base** | ETH, USDT, USDC |
| **BNB Smart Chain (BSC)** | BNB, USDT, USDC |
| **Polygon** | POL (MATIC), USDT, USDC |

*(Vui lòng kiểm tra đúng mạng lưới tương thích EVM trước khi gửi.)*
