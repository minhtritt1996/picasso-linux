# prebuilt/ — Hướng dẫn chuẩn bị firmware trên máy build

Các blob firmware độc quyền **không được lưu trong git** (đã bị `.gitignore` chặn). Để build đầy đủ `initrd.cpio.gz` và `boot-linux-auto.img`, bạn cần trích xuất từ ROM stock Xiaomi Redmi K30 5G (`picasso`, MIUI/HyperOS) hoặc gói `linux-firmware` và đặt đúng cấu trúc thư mục sau.

---

## 1. Firmware cần đặt trong `prebuilt/` (đóng gói vào initramfs)

`scripts/mkinitramfs.sh` sẽ tự động đóng gói các thư mục dưới đây vào `initrd.cpio.gz`:

### `prebuilt/fw-gpu/` (GPU Adreno 620)
| File | Nguồn lấy |
|---|---|
| `a620_zap.mbn`, `a620_zap.mdt`, `a620_zap.b00`, `a620_zap.b01`, `a620_zap.b02` | ROM stock `picasso`: `/vendor/firmware/a620_zap.*` |
| `a650_sqe.fw` (`31,804` B, Gen3) | `linux-firmware`: `qcom/a650_sqe.fw` |
| `a650_gmu.bin` (`41,548` B) | `linux-firmware`: `qcom/a650_gmu.bin` |
| `a630_sqe.fw` (`32,304` B) | `linux-firmware`: `qcom/a630_sqe.fw` |

### `prebuilt/fw-touch/` (Cảm ứng Novatek NT36672C SPI)
| File | Nguồn lấy |
|---|---|
| `novatek_nt36672c_g7a_fw03.bin` (`139,264` B) | ROM stock `picasso`: `/vendor/firmware/novatek_nt36672c_g7a_fw03.bin` |

### `prebuilt/fw-wcn3990/` (Wi-Fi WCN3990 metadata)
| File | Nguồn lấy |
|---|---|
| `firmware-5.bin` (`60` B) | `linux-firmware`: `/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin` |

> Nếu không đặt trong `prebuilt/fw-wcn3990/`, `scripts/mkinitramfs.sh` sẽ tự fallback lấy từ `/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin` trên máy build (nếu gói `linux-firmware` đã cài sẵn).

### Gợi ý trích xuất nhanh từ ROM stock `vendor.img`
```sh
# Giải nén super.img / vendor.img (dùng simg2img + lpunpack hoặc mount trực tiếp trên điện thoại)
mkdir -p prebuilt/fw-gpu prebuilt/fw-touch prebuilt/fw-wcn3990

cp /mnt/vendor/firmware/a620_zap.*                    prebuilt/fw-gpu/
cp /mnt/vendor/firmware/novatek_nt36672c_g7a_fw03.bin prebuilt/fw-touch/

# Từ gói linux-firmware trên máy Linux build (hoặc git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
cp /lib/firmware/qcom/{a650_sqe.fw,a650_gmu.bin,a630_sqe.fw} prebuilt/fw-gpu/
cp /lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin         prebuilt/fw-wcn3990/
```

---

## 2. Firmware tự đọc trực tiếp từ UFS lúc boot (không cần chép vào repo)

`initramfs/init.c` (`mount_firmware()`) sẽ tự mount trực tiếp các phân vùng sẵn có trên máy ở chế độ chỉ đọc:

- **`modem` (`PARTNAME=modem`) & `dsp` (`PARTNAME=dsp`)** → symlink vào `/lib/firmware/qcom/sm7250/xiaomi/picasso/image/`: cung cấp `adsp.mdt/.b*`, `modem.mdt/.b*`, `wlanmdsp.mbn`, `bdwlan.bin`, `venus.*`, `*.jsn`.
- **`bluetooth` (`PARTNAME=bluetooth`)** → mount vào `/lib/firmware/qca/`: cung cấp `crbtfw32.tlv`, `crnv32.bin`.

---

## 3. Các file phụ trợ khác (gitignored)

- **UEFI (`uefi/SILICIUM_UEFI.fd`, `uefi/picasso.dtb`)**: xem [`uefi/README.md`](../uefi/README.md).
- **QRTR/Modem helpers (`initramfs/bin/{rmtfs,pd-mapper,tqftpserv}`)**: xem [`initramfs/bin/README.md`](../initramfs/bin/README.md).
