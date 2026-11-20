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
| node | `picasso-sm7250.dtsi:23,133-137` | `novatek,NVT-ts-spi`, `9.6 MHz` (`spi-max-frequency = <9600000>`, per `:137` `//4800000,9600000,15000000,19200000` & `touch_spi@0` `10 MHz` at `:23`) |
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
| TLMM 3-tile | `sm7250-oneplus-avicii.dts:488` (`sm7250.dtsi` `&tlmm`) | `&tlmm` maps all 3 tiles (`south`/`west`/`east`) with `gpio-reserved-ranges = <0 4>, <59 4>` (TZ-owned SE1 `gpio0-3` + SE6/QUP10 `gpio59-62`), enabling `tlmm 34` EAST IRQ (PDC 53) |
| regulators / clock / firmware | — | absent in downstream (always-on, ROM NCI firmware) |
| mainline driver | `drivers/nfc/nxp-nci/i2c.c` | `nxp,nxp-nci-i2c`, needs `enable-gpios` + `interrupts`; `0018*` keeps IRQ disabled in `NXP_NCI_MODE_COLD` (`IRQ_NOAUTOEN`) so `gpio 34` pull-up does not storm at `VEN=0` |
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
| card | mainline `sm8250.c` + `q6asm-dai` | MM2 = capture FE (`Q6ASM_DAI_TX`), BE `TX_CODEC_DMA_TX_3`; `0019*` preserves `active_channels_mask` in `q6afe_cdc_dma_port_prepare`; `0020*` resyncs `sdw:3:0` `Alert`->`Attached`; `initramfs/init.c` UCM2 `Picasso-HiFi.conf` exposes `SectionDevice."Mic"` (`AMIC1`/`ADC1` on `DEC0`) |

## Camera (M4, stage 1–3)

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| CCI | mainline `sm7250.dtsi` `cci@ac4a000`/`ac4b000` (disabled by default) | enable both; `i2c-3` (`cci0_i2c0`), `i2c-4` (`cci0_i2c1`), `i2c-5` (`cci1_i2c0`) |
| sensors (live CCI IDs) | `picasso-sm7250-camera-sensor-mtp.dtsi` + `com.qti.sensormodule.picasso_*.bin` | `s5k3t2` (`i2c-3` `0x56`, `0x3142`, MCLK0 `gpio13`, rst `gpio32`, CSIPHY2 4-lane D-PHY), rear `gc02m1` (`i2c-3` `0x37`, `0x02e0`, MCLK3 `gpio16`, rst `gpio21`, CSIPHY0 1-lane), `imx686` (`i2c-4` `0x1a`, `0x0686`, MCLK1 `gpio14`, rst `gpio29`, CSIPHY1 3-trio C-PHY), front `gc02m1` (`i2c-4` `0x10`, `0x02e0`, MCLK2 `gpio15`, rst `gpio33`, CSIPHY3 1-lane), `ov8856` (`i2c-5` `0x10`, `0x00885a`, MCLK2 `gpio15` 6 mA @ 19.2 MHz, rst `gpio30`, CSIPHY0 2-lane D-PHY), `s5k5e9yx04` (`i2c-5` `0x2d`, `0x559b`, MCLK4 `gpio25`, rst `gpio23`, CSIPHY3 2-lane D-PHY) |
| EEPROMs / VCMs | live `i2cdetect` on CCI | `i2c-3`: `0x50` (front EEPROM); `i2c-4`: `0x0c` (`dw9800`), `0x51` (wide EEPROM); `i2c-5`: `0x0c` (`dw9714v`), `0x51` (macro EEPROM), `0x52` (Ofilm UW EEPROM) |
| mainline sensor drivers | `build/linux/drivers/media/i2c/` | `ov8856` (`5-0010`, `/dev/v4l-subdev17`, `SGRBG10_1X10/3280x2464`), `s5k3t2` (`3-0056`, `/dev/v4l-subdev18`, `SGBRG10_1X10/2592x1940` + `5184x3880`, `0024*`), `imx686` (`4-001a`, `/dev/v4l-subdev19`, `SBGGR10_1X10/4624x3472`, `0024*`); `s5k5e9`/`gc02m1` verified over CCI `i2ctransfer` |
| PM8008 | `picasso-sm7250.dtsi:977` | absent on hardware (`i2cdetect -y -r 1` sees only `0x66` bq25970); not in camera power path |
| rails & eGPIO | `lito-regulators.dtsi:55,414,554,775`, `pinctrl-lito.c:52` | VIO=`S5A` (`gpio68`), VDIG=`S8C` (`gpio45` wide, `gpio126` UW, `gpio125` front, `pm8150l_gpios 12` macro), VANA=`BOB` (`gpio127` wide, `gpio122` UW, `gpio125` front, `gpio56` macro), VAF=`L16A`. `gpio115..145` carry `egpio_present=BIT(11)` and need `egpio_enable=BIT(12)` (`0021*` + `0007*`). Paired `regulator-fixed` nodes sharing `gpio125`/`gpio127` require `CONFIG_GPIO_SHARED_PROXY=y` |
| camcc | `camcc-lito.c:2366`, `sm7250.dtsi:1090` | `TITAN_TOP_GDSC` required before gating `cam_cc_mclk0..4_clk`; `cam_cc_pll2_config` updated to SM7250 v2.0 (`soc_id=400`, `revision=2.0`) Zonda values (`0022*`) |
| camss | `lito-camera.dtsi:552-608`, `sm7250.dtsi:987`, `cam_csiphy_1_2_hwreg.h` | `vfe0` (`0xacaf000`) & `vfe1` (`0xacb6000`) reg sizes trimmed `0x5200` → `0x4000` (`0001*`); `MEDIA_BUS_TYPE_CSI2_CPHY` (`bus-type = <1>`, `lane_regs_sm7250_3ph` + `CSI2_RX_CFG0_PHY_TYPE_SEL`) added in `0023*`; `&camss` enabled with `ov8856` (`port@0`), `imx686` (`port@1`, C-PHY), and `s5k3t2` (`port@2`, D-PHY) (`0002*`) |
| sensor rotation | `picasso-sm7250-camera-sensor-mtp.dtsi:231,296,378` | `imx686` (`rotation = <90>`, `sensor-position-roll = <90>`), `ov8856` (`rotation = <90>`, `sensor-position-roll = <90>`), `s5k3t2` (`rotation = <270>`, `sensor-position-roll = <270>`); exposes `V4L2_CID_CAMERA_SENSOR_ROTATION` for `braun` portrait correction (`0002*`) |

## GNSS / hotspot

| Hạng mục | Downstream | Giá trị |
|---|---|---|
| GNSS | none in vendor DTS; `image/modemr.jsn` `{"provider":"gps","service":"gps_service"}` | engine is a PD of the modem image; reached only via QMI LOC (service 16) from a single persistent `AF_QIPCRTR` client (`initramfs/gnss/picasso-gnss.c` -> `picasso-gnss.service`) |
| WLAN AP | `lito.dtsi:3635-3667` | WCN3990 nodes already complete in mainline; AP mode + same-channel concurrency advertised by the firmware |
