# DTS crosswalk: downstream (proven) → mainline picasso

Nguồn downstream: `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/` (kernel 4.19 đã chạy
toàn bộ phần cứng trên máy này). Mỗi giá trị đưa vào DTS mainline phải có dòng trong bảng này.

## Memory / reserved

| Hạng mục | Downstream | Giá trị | Ghi chú mainline |
|---|---|---|---|
| RAM banks | vendor DT `/memory` (`prebuilt/device-tree.dtb`) | `0x80000000+0x3cc00000`, `0xc0000000+0x40000000`, `0x100000000+0x100000000` (6 GB) | MemTotal thực tế 4.5 GiB; giữ nguyên 3 bank |
| `hyp` | vendor DT `hyp_region@80000000` | `0x80000000+0x600000` | |
| `xbl_aop` | `xbl_aop_region@80700000` | `0x80600000+0x260000` | |
| `cmd-db` | `reserved-memory@80860000` | `0x80860000+0x20000` | khớp `soc.args` (hakios) |
| `sec_apps` | `sec_apps_region@808ff000` | `0x808ff000+0x1000` | |
| `smem` | `smem_region@80900000` | `0x80900000+0x200000` | |
| `removed` | `removed_region@80b00000` | `0x80b00000+0x1300000` | |
| `qtee_apps` | `qtee_apps_region@81e00000` | `0x81e00000+0x4200000` | **khác avicii** (0x6800000) |
| `camera` | `pil_camera_region@86000000` | `0x86000000+0x500000` | |
| `npu` | `pil_npu_region@86500000` | `0x86500000+0x500000` | |
| `video` | `pil_video_region@86a00000` | `0x86a00000+0x500000` | |
| `cdsp` | `pil_cdsp_region@87400000` | `0x87400000+0x1e00000` | **khác avicii** (0x9cc00000) |
| `adsp` | `pil_adsp_region@89200000` | `0x89200000+0x2800000` | |
| `wlan_fw` | `pil_wlan_fw_region@8ba00000` | `0x8ba00000+0x200000` | |
| `ipa_fw` / `ipa_gsi` | `pil_ipa_fw_region@8bc00000` / `...@8bc10000` | `0x8bc00000+0x10000` / `0x8bc10000+0x5400` | |
| `gpu` | `pil_gpu_region@8bc15400` | `0x8bc15400+0x2000` | avicii tách 2 node vì TZ cần 4 KiB; kiểm tra lại khi làm GPU |
| `modem_wlan` | `modem_wlan_region@8c000000` | `0x8c000000+0xf800000` | khớp `soc.args` |
| `cdsp_sec` | `cdsp_sec_regions@9f400000` | `0x9f400000+0xc00000` | |
| `cont_splash` | `cont_splash_region` | `0xa0000000+0x2300000` | cần cho display M4 |
| `dfps_data` | `dfps_data_region@a2300000` | `0xa2300000+0x100000` | |

## Display / panel (`g7a_37_02_0a`)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| timing | `dsi-panel-g7a-37-02-0a-dsc-video.dtsi:66-68` | 1080x2400, 120 Hz |
| DSC | same file `:280-287` | v1.1, slice 540x20, per-pkt 2, 8bpc/8bpp, block prediction |
| video mode | same file (panel-type) | `dsi_video_mode`, ctrl 0, phy 0 |
| TE | `xiaomi-sm7250-common.dtsi:163` | `tlmm 10` |
| reset | `xiaomi-sm7250-common.dtsi:164` | `pm8150l_gpios 3` |
| supplies | `lito-sde-display.dtsi:204-212` | `vddio = L1C`, `lab/ibb = ab_vreg/ibb_vreg` |
| backlight | PM8150L WLED (`lito-sde-display.dtsi`) | không phải PWM |

## Touch (Novatek NT36672C)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| bus | `lito-qupv3.dtsi:495` | `spi@984000` (SE7) |
| node | `picasso-sm7250.dtsi:133+` | `novatek,NVT-ts-spi`, 5 MHz |
| reset / IRQ | `picasso-sm7250.dtsi:138-140` | `tlmm 4` / `tlmm 5` (flag `0x2001`) / `pm8150l_gpios 3` |
| firmware | `picasso-sm7250.dtsi:152-165` | `novatek_nt36672c_g7a_fw0{1,2,3}.bin` theo vendor/glass |

## USB

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| controller | `lito-usb.dtsi:6-11` | `ssusb@a600000`, `iommus = <&apps_smmu 0xE0 0x0>` |
| dwc3 | `lito-usb.dtsi:81+` | `snps,dwc3` tại `dwc3@a600000` |
| QMP PHY | `lito-usb.dtsi:125` | `qmp_phy_base`, combo PHY |

## Charger / fuel gauge

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| smb5 | `picasso-sm7250.dtsi:700+` | `pm7250b_charger`, battery-data, PD PHY |
| gauge | `xiaomi-sm7250-common.dtsi:57+` | QGauge battery-data (không chip rời) |

## UFS (M3)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| PHY | `lito-mtp.dtsi:54-76` | `qcom,ufs-phy-qmp-v4-lito`; `vcc = pm8150a_l7`, `vddp-ref-clk = pm8150_l9` |
| controller | `lito.dtsi` (ufshc@1d84000) | cần bổ sung dòng cụ thể khi làm M3 |

### Novatek SRAM host-download registers

Source: downstream `drivers/input/touchscreen/nt36672c/nt36xxx_mem_map.h`,
`NT36675_memory_map`, selected by trim ID `0c 00 00 72 66 03` (`hw_crc=2`).
BOOT_RDY=0x3f10d; ILM/DLM destination=0x3f128/0x3f12c;
length=0x3f118/0x3f130 (three bytes); golden CRC=0x3f100/0x3f104;
CRC enable=0x3f30e (bit 7). Event reset/host-command offsets=0x60/0x50.
These are controller SRAM/register accesses, with no persistent flash program.
