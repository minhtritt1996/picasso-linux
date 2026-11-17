# Kế hoạch Bringup Giai đoạn 2 (`picasso-linux` — Redmi K30 5G / SM7250)

Tài liệu này định nghĩa kế hoạch kỹ thuật và phân chia công việc (Task DAG) cho **Agent điều phối `omp`** (cùng đội ngũ worker `omp` do nó quản lý qua Orca Orchestration) để nghiên cứu, bringup và kiểm chứng các tính năng còn lại trên **Redmi K30 5G (`picasso`, SM7250)** chạy mainline Linux (`v7.3-rc7`) + Denial Mobile (Arch Linux ARM).

---

## 0. Quy tắc bất biến & Môi trường hiện tại (BẮT BUỘC TUÂN THỦ)

1. **Đọc kỹ `AGENTS.md` và `STATUS.md` trước khi làm bất cứ việc gì:**
   - **RAM-only tuyệt đối**: CẤM `fastboot flash`, `fastboot erase`, hoặc `dd` vào `/dev/block/*` hay `/dev/sd*`. Chỉ dùng `./tools/ramboot-cycle.sh` (`fastboot stage` + `fastboot oem run`).
   - **`upstream/` read-only**: Không sửa `upstream/linux` hay `upstream/avicii-mainline`.
   - **Patch-first**: Sửa code kernel trong `build/linux/`, sau khi kiểm chứng trên máy thì chạy `./build.sh export-patches` để xuất ra `patches/linux/picasso/*.patch`. Không sửa `patches/linux/vendor/*`.
   - **Kiểm chứng bắt buộc**: Chạy `./tools/policy-scan.sh` và `./build.sh check` trước khi hoàn tất mỗi mốc; ghi đầy đủ kết quả + SHA-256 của `build/out/{Image,sm7250-xiaomi-picasso.dtb,payload-linux.bin}` vào `STATUS.md`.
2. **Truy cập thiết bị đang chạy (`Denial Mobile` trên `/dev/sda36`)**:
   - **SSH qua USB ECM**: `ssh -i ~/.ssh/picasso_key root@10.77.71.2`
   - **SSH qua Wi-Fi (`wlan0`)**: `ssh -i ~/.ssh/picasso_key root@<lan-ip>` (hoặc kiểm tra IP hiện tại qua USB).
   - **User thường**: `picasso` / mật khẩu `picassopass`.
   - **Watchdog 45 phút (`2700s`)**: Máy có cơ chế tự reset về fastboot loader sau 45 phút (`tools/init.c` + `picasso-watchdog.timer`). Khi đang làm việc trên máy, định kỳ chạy `ssh -i ~/.ssh/picasso_key root@10.77.71.2 'touch /run/picasso-watchdog-pet'` để gia hạn bộ đếm.
   - **Âm lượng**: Giữ âm lượng ở `0%` (`MUTED`) khi thử nghiệm trừ khi đang test thu/phát âm thanh cần thiết.
3. **Nguồn tham chiếu phần cứng (Không đoán mò địa chỉ/IRQ/GPIO/Regulator)**:
   - Cây DTS downstream chuẩn của máy: `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/` (`picasso-*.dtsi`, `lito-*.dtsi`, `lagoon-*.dtsi`) và driver downstream trong `~/droidian/kernel/drivers/` + `~/droidian/kernel/techpack/`.
   - Ghi nhận mọi ánh xạ phần cứng mới vào `docs/dts-crosswalk.md`.

---

## 1. Cách tổ chức Đội ngũ Agent `omp` (Orca Orchestration)

Bạn (Agent `omp` nhận bàn giao này) đóng vai trò **Coordinator**. Hãy dùng Orca Orchestration (`/home/tritm/.config/orca/linux-orca-cli-shim/orca skills get orchestration`) để khởi chạy và điều phối một đội ngũ worker `omp`:
```bash
/home/tritm/.config/orca/linux-orca-cli-shim/orca orchestration worker-start --spec "<task spec>" --worktree current --agent omp
```
- **Quy tắc phân chia**:
  - Các worker **nghiên cứu / đối chiếu DTS & driver** (`research/recon`) có thể chạy **song song**.
  - Các bước **sửa `build/linux/`, build kernel (`./build.sh linux && ./build.sh pack-boot`) và test phần cứng (`./tools/ramboot-cycle.sh`)** phải được gom nhóm hoặc chạy tuần tự qua **1 worker phụ trách build/hardware loop** để tránh xung đột cây `build/linux/` và tránh 2 worker cùng reboot điện thoại một lúc.

---

## 2. Danh sách Hạng mục & Task Nghiên cứu / Bringup

### Task 1: Rung / Haptics (`Vibrator`)
- **Mục tiêu**: Bringup bộ điều khiển rung tuyến tính (LRA Haptics) trên `picasso` và tích hợp với userland (`input` force-feedback / `/sys/class/leds/*vibrator*` / Denial `haptics`).
- **Hướng nghiên cứu & thực hiện**:
  1. Kiểm tra `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/` (`picasso*.dtsi`, `pm8150b.dtsi` / `pm7250b.dtsi` hoặc chip haptics rời AW8697/DRV26xx trên I2C nếu có) xem `picasso` dùng **PMIC `qcom,pm8941-misc` / `qcom,spmi-haptics` (`pm7250b_haptics` tại `0xc000` trên SPMI SID 3)** hay **Awinic AW8624/AW8697 trên bus I2C**.
  2. Đối chiếu với `upstream/avicii-mainline` (`sm7250-oneplus-avicii.dts`) và `build/linux/drivers/input/misc/` (`qcom-spmi-haptics.c`, `aw8697`...).
  3. Bổ sung node DTS + Kconfig trong `configs/linux/picasso.config`, cập nhật `docs/dts-crosswalk.md`, và kiểm chứng bằng `evtest` / `fftest` hoặc sysfs trigger trên máy thật.

### Task 2: Microphone (Thu âm — DMIC / Headset Mic / Q6AFE / WCD938x / Soundwire)
- **Mục tiêu**: Hiện tại loa ngoài (`Speaker` qua `SEC_MI2S_RX` / TFA98xx) đã hoạt động; cần nghiên cứu và bật đường thu âm (**Microphone** chính/phụ) qua PipeWire/ALSA UCM2.
- **Hướng nghiên cứu & thực hiện**:
  1. Kiểm tra topology âm thanh của `picasso` trong `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/` (`lito-audio*.dtsi` / `picasso-audio*.dtsi`) và mixer path Android (`/vendor/etc/mixer_paths*.xml` trên phân vùng `/dev/disk/by-partlabel/vendor` của máy hoặc trong `prebuilt/`):
     - `picasso` dùng codec WCD937x/WCD938x qua SWR (SoundWire) / Bolero (`LPASS` VA/TX macro) hay Q6AFE `TX_CDC_DMA_TX_3` / `TERT_MI2S_TX`?
  2. Kiểm tra các card/PCM capture hiện có trên máy (`arecord -l`, `amixer -c 0 contents`) và cấu hình ALSA UCM2 trong `tools/init.c` (`/run/alsa-ucm2/conf.d/sm7250/Picasso-HiFi.conf` — `CapturePCM`, mixer `MultiMedia1 Mixer ...`).
  3. Cấu hình route thu âm, kiểm chứng bằng `pw-record` / `arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 /tmp/mic.wav` và kiểm tra biên độ tín hiệu thu được (`sox` / `ffprobe` / RMS non-zero).

### Task 3: Mạng di động (Cellular Modem / QMI / ModemManager / Calls / SMS / Mobile Data) & Wi-Fi Hotspot (AP Mode)
- **Mục tiêu**:
  - **3A — Mạng di động (`qcom-q6v5-mss`, `ipa`, `rmnet`, `qrtr`, `ModemManager` / `ofono` / `telephony`)**:
    1. Kiểm tra trạng thái hiện tại trên máy: `remoteproc` của `mpss` (`4080000.remoteproc`), `qrtr-lookup` (các dịch vụ QMI UIM/NAS/DMS/WDS/Voice đã xuất hiện chưa), giao diện mạng `ipa` / `wwan0` / `rmnet_data0` (`CONFIG_QCOM_IPA`, `CONFIG_QRTR_MHI` / `CONFIG_RPMSG_QCOM_SMD`).
    2. Kiểm tra `ModemManager` / `qmicli` / daemon telephony của Denial Mobile (`denial-telephony` / `modemmanager.service`) xem đã nhận modem, đọc trạng thái SIM (`qmicli -d qrtr://0 --uim-get-card-status`), đăng ký mạng và thiết lập kết nối dữ liệu (PDP context) được chưa.
  - **3B — Wi-Fi Hotspot (AP Mode / Tethering trên `wlan0` `ath10k_snoc` WCN3991)**:
    1. Kiểm tra `iw list` trên máy xem `ath10k_snoc` đã quảng bá `AP` mode và khả năng tạo virtual interface (`AP/VLAN` hoặc đồng thời `managed` + `AP`) chưa.
    2. Kiểm tra NetworkManager / `hostapd` / `dnsmasq` / `nftables` (`CONFIG_NF_NAT_MASQUERADE`, `CONFIG_BRIDGE`, `ip_forward`) khi bật Wi-Fi Hotspot từ Denial Settings hoặc `nmcli dev wifi hotspot`.

### Task 4: Định vị GPS / GNSS (`PDS` / `LOC` QMI service qua QRTR)
- **Mục tiêu**: Bringup định vị vệ tinh GNSS/GPS qua modem MPSS (dịch vụ QMI `LOC` service ID `16 = 0x10` trên QRTR) với `geoclue` / `gnss-share` / `ModemManager` hoặc công cụ test QMI LOC.
- **Hướng nghiên cứu & thực hiện**:
  1. Kiểm tra `qrtr-lookup` trên máy xem service `16` (`LOC` — Location service trên node MPSS) đã đăng ký chưa.
  2. Kiểm tra cách Denial Mobile (`geoclue2`, `ModemManager` `--location-status` / `--location-enable-gps-raw` / `--location-enable-gps-nmea`, hoặc `qmicli -d qrtr://0 --loc-*`) lấy tọa độ GNSS và cấu hình để hệ thống nhận được NMEA/fix.

### Task 5: NFC (`NXP SN100x` / `PN553` / `NQxxx` trên I2C)
- **Mục tiêu**: Xác định chip NFC của Redmi K30 5G (`picasso`), thêm node DTS + driver kernel (`nxp-nci` / `st21nfca` / `pn5xx`), và kiểm chứng giao tiếp I2C/NCI.
- **Hướng nghiên cứu & thực hiện**:
  1. Tìm node `nfc` (`qcom,nq-nci` / `nxp,nxp-nci-i2c`) trong `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/picasso*.dtsi`: ghi rõ bus I2C (`qupv3_se*`), địa chỉ I2C (`0x28`), chân GPIO `irq-gpios`, `enable-gpios` (`ven`), `firmware-gpios`, clock `cxo` / `rf_clk`, và regulator vào `docs/dts-crosswalk.md`.
  2. Kiểm tra driver mainline (`drivers/nfc/nxp-nci/i2c.c`, `CONFIG_NFC=y`, `CONFIG_NFC_NCI=y`, `CONFIG_NFC_NXP_NCI=y`, `CONFIG_NFC_NXP_NCI_I2C=y`) và công cụ userland (`neard` / `nfctool`).

### Task 6: Camera (`CAMSS` / `CCI` I2C / Cảm biến Sony IMX686 64MP + IMX355 + OV02B10 + S5K3T2 + `libcamera` / `pipewire`)
- **Mục tiêu**: Nghiên cứu kiến trúc Camera Subsystem (`qcom,sm7250-camss` / `cci` / `csiphy` / `csid` / `vfe`) trên SM7250 và các cảm biến camera của `picasso`.
- **Hướng nghiên cứu & thực hiện**:
  1. Khảo sát `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/picasso-camera-sensor-mtp.dtsi` (hoặc `lito-camera*.dtsi`) và `upstream/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi` + `upstream/avicii-mainline`:
     - Kiểm tra xem `sm7250.dtsi` hoặc `patches/linux/vendor/` đã có `camss` và `cci` (`ac4a000.cci`, `ac4b000.cci`) chưa.
     - Liệt kê đầy đủ bảng nguồn (LDO/PM8008 PMIC phụ cho camera!), xung nhịp (`camcc`), chân reset/pwdn GPIO, địa chỉ CCI I2C, và MIPI CSI-2 lanes của từng cảm biến vào `docs/dts-crosswalk.md`.
  2. Bringup trước bus `CCI` + `PM8008` (PMIC cấp nguồn camera) + probe I2C ID của cảm biến trước/sau (hoặc `camss` pipeline nếu đã có driver cảm biến tương ứng trong mainline).

### Task 7: Rà soát & Xử lý các lỗi còn tồn đọng (`dmesg`, `journalctl`, Services, Power/Thermal, Droidloom)
- **Mục tiêu**: Quét toàn diện log hệ thống trên thiết bị để phát hiện và xử lý triệt để các lỗi/cảnh báo còn sót:
  1. Kiểm tra `dmesg --level=err,warn` và `journalctl -p err..warning -b` trên máy:
     - Các unit systemd bị `failed` (`systemctl --failed`).
     - Các lỗi probe deferred hoặc thiếu firmware/regulator.
     - Trạng thái quản lý nhiệt (`thermal_zone*`), tần số CPU (`cpufreq` schedutil), trạng thái sạc USB Type-C / PD (`pm7250b`), cảm biến vân tay cạnh bên (`goodix` / `fpc`), đèn LED thông báo.
     - Kiểm tra tính ổn định khi suspend/idle, âm thanh sau khi mở khóa, và kết nối lại Wi-Fi/SSH.

---

## 3. Tiêu chuẩn bàn giao cuối cùng của Đội ngũ `omp`

Khi hoàn tất từng hạng mục hoặc toàn bộ kế hoạch:
1. Mọi thay đổi kernel được export sạch sẽ vào `patches/linux/picasso/*.patch` qua `./build.sh export-patches` và config vào `configs/linux/picasso.config`.
2. `./tools/policy-scan.sh` và `./build.sh check` đều báo `OK`.
3. Mọi thông số phần cứng mới có mặt trong `docs/dts-crosswalk.md`.
4. `STATUS.md` được cập nhật đầy đủ với trạng thái từng tính năng, lệnh kiểm chứng, log thực tế từ máy và mã băm SHA-256 của `build/out/*`.
