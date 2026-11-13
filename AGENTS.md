# Quy tắc bắt buộc — `mainline-linux-picasso`

Áp dụng cho mọi thay đổi trong repository này (kernel mainline Linux cho Redmi K30 5G /
SM7250, boot bằng RAM qua Mu-Silicium UEFI).

## 1. Bất biến

1. **RAM-only.** Không bao giờ ghi UFS: cấm `fastboot flash`/`erase`, `dd` vào `/dev/block/*`
   hay `/dev/sd*`. Chỉ `fastboot boot` và payload trong RAM.
2. **`upstream/` read-only.** `upstream/linux` và `upstream/avicii-mainline` phải luôn sạch và
   đúng commit trong `patches/linux/UPSTREAM`. Không sửa, không tạo file, không hardlink ghi đè.
3. **Patch-first.** Mọi thay đổi kernel chỉ tồn tại dưới dạng patch trong
   `patches/linux/picasso/`; `patches/linux/vendor/` là bản gốc của avicii, **không sửa**.
   Thử nghiệm trong `build/linux/`, xác nhận xong mới `./build.sh export-patches`.
4. **Không hardcode đường dẫn tuyệt đối** trong script/tài liệu; suy dẫn từ gốc repository.
5. **Không tái dùng driver XNU** (`../hakios-picasso/drivers/`) cho Linux; chỉ đọc để tham chiếu
   chuỗi init.

## 2. Nguồn giá trị phần cứng

- DTS: ưu tiên cây downstream đã chạy thật
  (`~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/`), sau đó `prebuilt` DT của
  hakios-picasso. Mỗi node quan trọng ghi lại nguồn trong `docs/dts-crosswalk.md`.
- Không đoán địa chỉ/IRQ/regulator: nếu thiếu, dừng và bổ sung vào crosswalk trước.

## 3. Kiểm chứng

- `./build.sh check` trước khi commit: apply series lên cây sạch + `dtbs_check`.
- Mỗi mốc chạy trên máy phải được ghi vào `STATUS.md` (cmdline, artifact hash, kết quả).
- Không tuyên bố "chạy được" khi chưa có log/ảnh chụp từ máy.

## 4. Cấu trúc

```text
patches/linux/series        thứ tự patch
patches/linux/vendor/       avicii (nguyên bản)
patches/linux/picasso/      delta của picasso
patches/loader/             delta nhỏ lên xnuloader (nhánh boot Image)
configs/linux/              fragment Kconfig
tools/linux/                ramdisk, đóng gói boot image
build/                      cây build cô lập + build/out
```
