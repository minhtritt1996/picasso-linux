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

## Haptics / vibrator (M4)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| bus / slave | `pm7250b.dtsi:450-452`, board `PM7250B_SID1=3` | SPMI SID 3 |
| node | `pm7250b.dtsi:457-460` | `qcom,vibrator@5300`, `qcom,qpnp-vibrator-ldo`, `reg = <0x5300 0x100>` |
| voltage / overdrive | same | `qcom,vib-ldo-volt-uv = <3000000>`, `qcom,disable-overdrive` |
| registers | `leds-qpnp-vibrator-ldo.c:18-29` | STATUS1 0x08 bit7 VREG_READY; VSET_LB 0x40 (12-bit mV, 8 mV step, 1504–3544 mV); EN_CTL 0x46 bit7 |
| mainline driver | `drivers/input/misc/pm8xxx-vibrator.c` (`pmi632_regs`) | same registers; DT `compatible = "qcom,pm7250b-vib", "qcom,pmi632-vib"` |
| userland | `70-hapticd-builtin.rules` | input device with FF caps, bustype ≠ 0003/0005/0006 → `HAPTICD_DEVICE=1` |

## NFC (NXP NCI, M4)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| bus | `xiaomi-sm7250-common.dtsi:322-324` | `qupv3_se0_i2c` = `lito-qupv3.dtsi:83` `i2c@880000`, vendor 1 MHz (mainline 400 kHz) |
| client | `xiaomi-sm7250-common.dtsi:327-329` | `nq@28`, address `0x28`, `qcom,nq-nci` |
| IRQ | `:330,334-336`; `nq-nci.c:1606` | tlmm 34, vendor requests level-high |
| control pads | `:331-333` | `qcom,nq-ven` tlmm 12 (active high), `qcom,nq-firm` tlmm 35, `qcom,nq-clkreq` tlmm 31 |
| pads tile | `lito-pinctrl.dtsi:1159-1260`, `pinctrl-lito.c` | SDA/SCL gpio42/43 (qup00, EAST); VEN/CLKREQ/IRQ/FIRM EAST |
| regulators / clock / firmware | — | absent in downstream (always-on, ROM NCI firmware) |
| mainline driver | `drivers/nfc/nxp-nci/i2c.c` | `nxp,nxp-nci-i2c`, needs `enable-gpios` + `interrupts` |
| pinmux workaround | `drivers/soc/qcom/picasso-pinmux.c` | mux 42/43 (qup00), VEN high, FIRM low, CLKREQ/IRQ pulled-up inputs |

## Audio capture (WCD9385 + Bolero TX/DMIC, M4)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| codec | `lito-audio-overlay.dtsi:177-222`, `picasso-audio-overlay.dtsi:23` | WCD9385 split codec, `qcom,split-codec = <1>` |
| codec reset | `lito-pinctrl.dtsi:640-666`, `lito-audio-overlay.dtsi:413-418` | tlmm 57 WEST, active low |
| supplies | `lito-audio-overlay.dtsi:197-211`, `lito-regulators.dtsi:775-791` | rxtx/io/buck = S4A 1.8 V, mic-bias = BOB (3.008–3.96 V) |
| micbias | `picasso-audio-overlay.dtsi:129-135` | 2.75 V on all four |
| analog mics | `picasso-audio-overlay.dtsi:22-29` | AMIC1←BIAS1 handset, AMIC2←BIAS2 headset, AMIC5←BIAS3 secondary |
| macros | `picasso-audio-overlay.dtsi:91-93` | 3 macros (RX/TX/VA), WSA disabled |
| rx macro / swr1 | `lito-audio-overlay.dtsi:66-101` | `rx-macro@3200000`, `swrm-io-base 0x3210000`, master 2, IRQ GIC_SPI 155, slave `0x01170224` |
| tx macro / swr2 | `lito-audio-overlay.dtsi:24-62` | `tx-macro@3220000`, `swrm-io-base 0x3230000`, master 3, IRQ GIC_SPI 166, slave `0x01170223`, dmic rate 2.4 MHz |
| VA macro | `lito-audio-overlay.dtsi:166-175` | `va-macro@3370000`, dmic rate 600 kHz |
| SWR port maps | `lito-audio-overlay.dtsi:49-55` | PWM_OUT1/ADC1-4/DMIC0-7 → integers (reused verbatim) |
| LPI pinctrl | `lito-lpi.dtsi:1-29`, `:1351-1545` | `lpi_pinctrl@33c0000`, slew `0x355a000`; TX clk gpio0 data1/2 gpio1/2; RX clk gpio3 data gpio4/5 |
| DMIC pads | vendor `cdc-dmic23/45` | gpio8/9 (dmic2), gpio12/13 (dmic3), drive 8 mA |
| card | mainline `sm8250.c` + `q6asm-dai` | MM2 = capture FE (`Q6ASM_DAI_TX`), BE `TX_CODEC_DMA_TX_3` |

## Camera (M4, stage 1/2)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| CCI | mainline `sm7250.dtsi` `cci@ac4a000`/`ac4b000` (disabled by default) | enable both; adapters appear as `Qualcomm-CCI` |
| sensors | `picasso-sm7250-camera-sensor-mtp.dtsi` | IMX686 64 MP main, OV8856 8 MP UW, S5K3T2 20 MP front, S5K5E9YX04 5 MP macro, GC02M1 ×2 depth |
| mainline sensor drivers | `build/linux/drivers/media/i2c/` | only OV8856 present; IMX686/S5K3T2/S5K5E9/GC02M1 need drivers |
| PM8008 | `picasso-sm7250.dtsi:977` | instantiated on charger bus `0x98c000` addr 0x08; **not** in the camera rails |
| rails | `lito-regulators.dtsi:55,414,554,775` | VIO = S5A, VDIG = S8C, VANA = BOB, VAF = L16A; enable pads on EAST/WEST tiles |
| camcc | mainline `sm7250.dtsi:1079` | already enabled; provides TITAN_TOP_GDSC |

## GNSS / hotspot

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| GNSS | none in vendor DTS; `image/modemr.jsn` `{"provider":"gps","service":"gps_service"}` | engine is a PD of the modem image; reached only via QMI LOC (service 16) |
| WLAN AP | `lito.dtsi:3635-3667` | WCN3990 nodes already complete in mainline; AP mode + same-channel concurrency advertised by the firmware |
