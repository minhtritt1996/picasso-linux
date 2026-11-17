# Camera bring-up — CCI bus, camera PMIC, sensor identification, CAMSS plan

Board: Xiaomi Redmi K30 5G (`picasso`), Qualcomm SM7250/lito, mainline `v7.3-rc7` + Denial Mobile,
RAM-boot through Mu-Silicium UEFI. Everything below is read-only research plus one document; no
kernel build, no phone writes.

Scope of this document: the *first* real camera milestone — get the camera clock controller and the
CCI (camera control interface) I2C buses up, prove a sensor answers on them, and size the CAMSS
pipeline work. It deliberately does not attempt streaming.

Path convention: `DS/` = the proven downstream 4.19 tree at
`~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/`; `ML/` = `build/linux/`;
`REPO/` = this repository root. All phone commands assume the repo root as working directory.

---

## 0. Executive summary (what changes the plan)

1. **CCI, CAMSS and the camera clock controller already exist in mainline for SM7250.** The DTS
   nodes are in `ML/arch/arm64/boot/dts/qcom/sm7250.dtsi`, the drivers are in the tree
   (`i2c-qcom-cci` matches `qcom,sm7250-cci`, `camss` matches `qcom,sm7250-camss`). What is missing
   is: the config symbols, the board-level `status = "okay"`, and the rail votes.
2. **The camera sensors on picasso are: IMX686 (64 MP main), OV8856 (8 MP ultrawide), S5K3T2
   (20 MP front), S5K5E9YX04 (5 MP macro), GC02M1 (2 MP rear depth), GC02M1 (2 MP front depth).**
   There is **no OV02B10** anywhere in the shipped vendor camera stack. Of these, mainline already
   has an OV8856 driver; the other five are absent.
3. **PM8008 is *not* in picasso's camera power path.** The chip is instantiated on the charger I2C
   bus (`qupv3_se9` = `i2c@98c000`, address 0x08), but **no** `L1P..L7P` rail has a consumer
   anywhere in the lito/picasso downstream DTS. picasso's camera rails are GPIO-gated fixed rails
   fed from PM8150 `S5A`, PM8150 `L16A`, PM8150L `S8C` and PM8150L `BOB`. PM8008 is therefore a
   *presence check*, not a dependency. (Details and the one contradiction in §4.)
4. **The real blocker is the TLMM wall, and it is now precisely quantifiable:** every camera pad
   except `gpio10`-adjacent south-tile pins lives in the EAST or WEST tile, and the board DTS maps
   only the SOUTH tile (`ML/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts:392-395`). The
   pinctrl driver's register accessors turn all writes to unmapped tiles into silent no-ops
   (`ML/drivers/pinctrl/qcom/pinctrl-msm.c:88-99`, patch `0005-pinctrl-msm-skip-unmapped-tiles`),
   so nothing crashes — the pads simply never get muxed and never toggle. Muxing must go through
   `ML/drivers/soc/qcom/picasso-pinmux.c`, which already owns this problem for BT/touch/amp.

Consequence for sequencing: stage 1 = config + CCI nodes + rails + camera pads in `picasso-pinmux`;
stage 2 = `i2cdetect`/`i2ctransfer` sensor proof on the CCI adapters; stage 3 = CAMSS graph +
one new sensor driver (IMX686) or reuse of `ov8856`.

---

## 1. Hardware ground truth

### 1.1 Sensors, buses and pads

The downstream DTS **does not name the sensors** — `qcom,cam-sensor` nodes only carry
`cell-index`, `csiphy-sd-index`, `cci-device`, `cci-master` and rail/gpio lists
(`DS/picasso-sm7250-camera-sensor-mtp.dtsi`). Sensor identity comes from the shipped vendor camera
stack inside the retail image
(`~/droidian/downloads/picasso_images_V12.5.7.0.RGICNXM_20211025.0000.00_11.0_cn/images/super.img`,
byte offset 5574738203 — `com.qti.sensor.picasso_*.so` and `com.qti.sensormodule.picasso_*.bin`):

| role | vendor module | cell-index | CSIPHY | CSI lanes | CCI dev / master | MCLK pad | reset pad |
|---|---|---|---|---|---|---|---|
| 64 MP wide (main) | `picasso_imx686.so` (+ `_sunny_v2_wide`, `_ofilm_mp_wide` .bin) | 0 | 1 | **unknown** (see §4-U1) | 1 / 1 | `gpio14` = MCLK1 | `gpio29` |
| 8 MP ultrawide | `picasso_ov8856.so` | 1 | 0 | **unknown** | 0 / 0 | `gpio15` = MCLK2 | `gpio30` |
| 20 MP front | `picasso_s5k3t2.so` | 2 | 2 | **unknown** | 1 / 0 | `gpio13` = MCLK0 | `gpio32` |
| 5 MP macro | `picasso_s5k5e9yx04.so` | 3 | 3 | **unknown** | 1 / 0 | `gpio25` = MCLK4 | `gpio23` |
| 2 MP rear depth | `picasso_rear_depth_gc02m1.so` | 4 | 0 | **unknown** | 1 / 0 | `gpio16` = MCLK3 | `gpio21` |
| 2 MP front depth | `picasso_front_gc02m1.so` | 5 | 3 | **unknown** | 1 / 1 | `gpio15` = MCLK2 | `gpio33` |

Sensor-to-megapixel mapping is corroborated by the downstream comments on each camera node:
"for 64M Wide" (`DS/picasso-sm7250-camera-sensor-mtp.dtsi:49`), "for 8M UltraWide" (`:103`),
"for 8M Front" (`:133`), "for 5M Macro" (`:161`), "for 2M rear depth" (`:335`),
"for 2M Front depth" (`:416`), "for 20M Front" (`:274`).

Per-sensor citations in `DS/picasso-sm7250-camera-sensor-mtp.dtsi`:

| item | downstream `file:line` | value |
|---|---|---|
| main: `csiphy-sd-index` | `picasso-sm7250-camera-sensor-mtp.dtsi:230` | `1` |
| main: `cci-device` / `cci-master` | `…-sensor-mtp.dtsi:265-266` | `1` / `1` |
| main: MCLK1 pad + reset pad | `…-sensor-mtp.dtsi:257-258` | `gpio14` (mux `cam_mclk`, `:253`), `gpio29` (`gpio-reset = <1>`, `:259`) |
| main: ata MCLK rate | `…-sensor-mtp.dtsi:268-271` | `CAM_CC_MCLK1_CLK`, 19200000 Hz, level `turbo` |
| ultrawide: csiphy / cci | `…-sensor-mtp.dtsi:522,553-554` | `0` / dev `0`, master `0` |
| ultrawide: pads | `…-sensor-mtp.dtsi:545-546` | `gpio15` MCLK2, `gpio30` reset |
| front: csiphy / cci | `…-sensor-mtp.dtsi:295,326-327` | `2` / dev `1`, master `0` |
| front: pads | `…-sensor-mtp.dtsi:318-319` | `gpio13` MCLK0, `gpio32` reset |
| macro: csiphy / cci | `…-sensor-mtp.dtsi:598,630-631` | `3` / dev `1`, master `0` |
| macro: pads | `…-sensor-mtp.dtsi:622-623` | `gpio25` MCLK4, `gpio23` reset |
| rear depth: csiphy / cci | `…-sensor-mtp.dtsi:377,407-408` | `0` / dev `1`, master `0` |
| rear depth: pads | `…-sensor-mtp.dtsi:399-400` | `gpio16` MCLK3, `gpio21` reset |
| front depth: csiphy / cci | `…-sensor-mtp.dtsi:459,489-490` | `3` / dev `1`, master `1` |
| front depth: pads | `…-sensor-mtp.dtsi:481-482` | `gpio15` MCLK2, `gpio33` reset |

**No `i2c_freq_mode` property exists** in the downstream sensor nodes (grep over the whole
`DS/` tree returns nothing); the downstream CCI driver picks 400 kHz fast mode by default and the
timings live in the CCI controller nodes as `i2c_freq_100Khz_cci*` / `400Khz` / `custom` / `1Mhz`
children (`DS/camera/lito-camera.dtsi:164-223` for CCI0, `:255-314` for CCI1). In mainline the
equivalent knob is the standard `clock-frequency` on each `i2c-bus@N` child, already set to 400000
in `ML/arch/arm64/boot/dts/qcom/sm7250.dtsi:923-936`.

**Sensor I2C slave addresses are not in any DTS.** Downstream `reg = <0x00..0x05>` on
`qcom,cam-sensor` is the cell index, not the address. The addresses live inside the vendor
`picasso_*.so` sensor modules. This is why stage 2 is an empirical bus scan (§3d), not a paste.

### 1.2 CCI controllers

| item | downstream `file:line` | mainline `file:line` | value |
|---|---|---|---|
| CCI0 base | `DS/camera/lito-camera.dtsi:137` | `ML/.../sm7250.dtsi:895` | `0x0ac4a000`, 0x1000 |
| CCI0 IRQ | `DS/camera/lito-camera.dtsi:139` | `ML/.../sm7250.dtsi:896` | GIC SPI 460, edge rising |
| CCI0 masters | `DS/camera/lito-camera.dtsi:151-157` | `ML/.../sm7250.dtsi:923-936` | master0 = `gpio17/18`, master1 = `gpio19/20` |
| CCI1 base | `DS/camera/lito-camera.dtsi:231` | `ML/.../sm7250.dtsi:940` | `0x0ac4b000`, 0x1000 |
| CCI1 IRQ | `DS/camera/lito-camera.dtsi:234` | `ML/.../sm7250.dtsi:941` | GIC SPI 271, edge rising |
| CCI1 masters | `DS/camera/lito-camera.dtsi:246-252` | `ML/.../sm7250.dtsi:968-973` | master0 = `gpio27/28`; mainline declares only `i2c-bus@0` |
| CCI clock | `DS/camera/lito-camera.dtsi:147-149` | `ML/.../sm7250.dtsi:899-912` | CCI clk src 37.5 MHz; mainline also votes CAMNOC 150 MHz |
| CCI GDSC | `DS/camera/lito-camera.dtsi:143` | `ML/.../sm7250.dtsi:897` | `camcc TITAN_TOP_GDSC` |
| CCI pin mux | `DS/lito-pinctrl.dtsi:668-695` (`cci0_active`, `cci1_active`), `:724` (`cci2_active`) | `ML/.../sm7250.dtsi:1880-1905` | function `cci_i2c`, 2 mA, pull-up |

Note the vendor numbering trap: `cci-device`/`cci-master` in the sensor nodes map to the vendor's
`qcom,cci0`/`qcom,cci1` nodes, but `cci1` (`0xac4b000`) is wired to only one pad pair
(`gpio27/28`, `DS/camera/lito-camera.dtsi:246-252`) while the main sensor claims
`cci-device = 1, cci-master = 1`. The two interpretations — "main sensor is on CCI1's second
(missing) master" vs. "the vendor's master index is CCI0 master 1 (`gpio19/20`)" — are not
resolvable from DT alone. Both controllers are enabled and all three buses are scanned (§3d).

### 1.3 Camera rails and regulators

All downstream camera rails are `regulator-fixed` "GPIO LDO" nodes in
`DS/picasso-sm7250-camera-sensor-mtp.dtsi:49-186`; the enable pad and the *input* rail are the
hardware truth, the 2.8/1.8/1.2 V values are what the vendor camera stack requests:

| rail node | enable pad | pad line | `vin-supply` | declared V |
|---|---|---|---|---|
| `camera_rear_wide_vana_ldo` | `tlmm gpio127` | `:58` | `BOB` | 2800000 |
| `camera_rear_wide_vana2_ldo` | `tlmm gpio127` | `:73` | `S5A` | 1800000 |
| `camera_rear_wide_dvdd_ldo` | `tlmm gpio45` | `:86` | `S8C` | 1200000 |
| `camera_rear_ultrawide_vio_ldo` (all sensors' VIO) | `tlmm gpio68` | `:99` | `S5A` | 1800000 |
| `camera_rear_ultrawide_vana_ldo` | `tlmm gpio122` | `:112` | `BOB` | 2800000 |
| `camera_rear_ultrawide_vdig_ldo` | `tlmm gpio126` | `:127` | `S8C` | 1200000 |
| `camera_rear_front_vana_ldo` | `tlmm gpio125` | `:142` | `BOB` | 2800000 |
| `camera_rear_front_vdig_ldo` | `tlmm gpio125` | `:157` | `S8C` | 1200000 |
| `camera_rear_macro_vana_ldo` | `tlmm gpio56` | `:170` | `BOB` | 2800000 |
| `camera_rear_macro_vdig_ldo` | **`pm8150l_gpios 12`** | `:183` | `S8C` | 1200000 |
| wide actuator VAF | `L16A` (PM8150 LDO16) | `:196` | — | 2800000 |
| macro actuator VAF | `L16A` | `:567` | — | 2800000 |

RPMH parent rails, all defined in `DS/lito-regulators.dtsi`:

| label | downstream `file:line` | RPMH resource | hardware window / downstream init |
|---|---|---|---|
| `S5A` | `:55-62` | `smpa5` | 1664000–2040000 V, init 1664000 |
| `L16A` | `:414-423` | `ldoa16` | 2600000–3304000, init 2600000, init mode LPM |
| `S8C` | `:554-561` | `smpc8` | 696000–1416000, init 696000 |
| `BOB` | `:775-790` | `bobc1` | 3008000–3960000, init 3008000, mode PASS |

The board DTS currently votes **none** of these (`S5A`/`L16A`/`S8C`/`BOB` absent from
`ML/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts:237-380`), and the comment at `:231-236`
says Linux is the only RPMH voter on this boot — so without consumers these rails have no
guaranteed state. They must be declared `regulator-boot-on`/`always-on` in stage 1.

### 1.4 PM8008 (present in DT, absent from the camera power path)

| item | downstream `file:line` | value |
|---|---|---|
| node 0x08 | `DS/pm8008.dtsi:3-5` | `compatible = "qcom,i2c-pmic"`, `reg = <0x8>` |
| second address 0x09 | `DS/pm8008.dtsi:50-55` | vendor splits one chip into two I2C clients |
| periph map | `DS/pm8008.dtsi:12` | `<0x09 0x24 0xc0 0xc1>` |
| chip-en / OCP IRQ | `DS/pm8008.dtsi:14-23` | `PM8008_EN`, IRQ `0x09 4` |
| LDO regs | `DS/pm8008.dtsi:64-125` | L1/L2 `0x4000/0x4100` 528k–1504k; L3–L7 `0x4200..0x4600` 1.5–3.4 V |
| GPIO block | `DS/pm8008.dtsi:30-47` | `pinctrl@c000`, gpio1..2, `pm8008_gpio1_active` func1 output |
| bus | `DS/lito.dtsi:3857-3860` → `DS/lito-qupv3.dtsi:411` | `qupv3_se9_i2c` = `i2c@98c000`, WRAP1 SE3, IRQ GIC SPI 356 |
| bus pads | `DS/lito-pinctrl.dtsi:1483-1495` | `gpio46/47`, function `qup13`, 2 mA, no bias |
| bus neighbours | `DS/picasso-sm7250.dtsi:652-660` | `bq25970-standalone@66` (charger) on the same bus |
| LDO supplies | `DS/lito.dtsi:3879-3885` | `vdd_l1_l2 = S8C`, `vdd_l3_l4 = BOB`, `vdd_l5 = BOB`, `vdd_l6/l7 = S5A` |
| LDO windows narrowed | `DS/lito.dtsi:3887-3922` | L1P ≤1056000, L2P ≤1104000, L3P 2800000, L4P 2800000–2904000, L5P 1800000, L6P/L7P ≤1800000 |
| enable pad | `DS/lito-pinctrl.dtsi:2086-2099` | `pm8008_active` = `gpio44`, pull-up, output-high, 2 mA |
| enable applied on picasso | `DS/picasso-sm7250.dtsi:977-979` | `pinctrl-0 = <&pm8008_active>` only |
| IRQ (lito default) | `DS/lito.dtsi:3870-3877`, pad `DS/lito-pinctrl.dtsi:2073-2084` | `tlmm gpio45`, edge rising — **picasso reuses gpio45 as the wide DVDD enable** (`…-sensor-mtp.dtsi:86`) |

**PM8008 consumers:** a tree-wide grep for `&L[1-7]P` outside `pm8008.dtsi` matches only
`DS/bengal.dtsi` and `DS/camera/bengal-camera-sensor-*.dtsi`. On lito/picasso, `L1P..L7P` have
**zero** consumers. Also note the lito voltage overrides (`L3P = 2.8 V`, `L5P = 1.8 V`,
`L1P/L2P ≤ 1.056 V`) are exactly lito-camera-shaped, so the chip may still be populated for other
variants; that is a presence question, not a model question (§3d stage 1a).

### 1.5 TLMM tile assignment of every camera pad

Tile bases: `DS/../drivers/pinctrl/qcom/pinctrl-lito.c:21-25` →
`SOUTH 0x00100000`, `WEST 0x00500000`, `EAST 0x00900000`, `REG_SIZE 0x1000`, relative to
`0x0f000000` (`ML/drivers/soc/qcom/picasso-pinmux.c:26-31`). Pin→tile table
(`pinctrl-lito.c:1328-1580`, mirrored by `ML/drivers/pinctrl/qcom/pinctrl-sm7250.c:1497-1660`):

| pad | tile | downstream pin entry | function needed |
|---|---|---|---|
| 13, 14, 15, 16 | EAST | `pinctrl-lito.c:1353,1355,1357,1359` | `cam_mclk` = f1 |
| 17, 18, 19, 20 | EAST | `:1361,1363,1365,1367` | `cci_i2c` = f1 |
| 21 (rear-depth reset) | EAST | `:1369` | gpio |
| 23 (macro reset) | EAST | `:1373` | gpio |
| 25 (MCLK4) | EAST | `:1377` | `cam_mclk` = **f3** (`cci_timer4`=f1, `cci_async`=f2) |
| 27, 28 (CCI1) | EAST | `:1381,1383` | `cci_i2c` = f1 |
| 29 (main reset) | EAST | `:1385` | gpio |
| 30 (ultrawide reset) | EAST | `:1387` | gpio |
| 32 (front reset) | EAST | `:1391` | gpio |
| 33 (front-depth reset) | **WEST** | `:1393` | gpio |
| 44 (PM8008 enable) | EAST | `:1414` | gpio |
| 45 (wide DVDD enable) | EAST | `:1416` | gpio |
| 46, 47 (PM8008 bus) | **WEST** | `:1418,1420` | `qup13` = f1 |
| 56 (macro VANA enable) | **WEST** | `:1438` | gpio |
| 68 (shared VIO enable) | EAST | `:1461` | gpio |
| 122, 125, 126, 127 (VANA/VDIG enables) | **WEST** | `:1566,1571,1572,1574` | gpio |

Today `ML/drivers/soc/qcom/picasso-pinmux.c:92-124` muxes only BT (38-41, 73), i2c8 (49, 50),
sec-MI2S (70-72, 74), touch SPI (6-9) + reset/IRQ (4, 5) and the amp reset (58). **No camera pad
is muxed today**, and driver-side `&tlmm N`/pinctrl-state writes to those pads are silent no-ops.

---

## 2. Current mainline state

| component | driver (`ML/…`) | config symbol / current value | DTS node | live probe |
|---|---|---|---|---|
| camera clock ctrl | `drivers/clk/qcom/camcc-sm7250.c`, Kconfig:1425, Makefile:163 | `CONFIG_SM_CAMCC_7250` — **absent from `picasso.config` and from `arch/arm64/configs/defconfig`** | `sm7250.dtsi:1079-1098`, no `status` ⇒ enabled | **not bound**: `ad00000.clock-controller` has no driver symlink; `cam_cc_*` clocks absent from `clk_summary` |
| CCI | `drivers/i2c/busses/i2c-qcom-cci.c`; `qcom,sm7250-cci` → `cci_v2_data` (`:816`, `:714`), adapter name `"Qualcomm-CCI"` (`:551`), `I2C_FUNC_SMBUS_EMUL` (`:454-457`) | `CONFIG_I2C_QCOM_CCI` = `m` in defconfig, absent in `picasso.config` | `sm7250.dtsi:893` (disabled `:921`), `:938` (disabled `:966`) | not bound |
| CAMSS | `drivers/media/platform/qcom/camss/camss.c`: `qcom,sm7250-camss` (`:5961`), resources `:5849-5860`, CSIPHY ops `csiphy_ops_3ph_1_0` (`camss-csiphy-3ph-1-0.c:1371`) | `CONFIG_VIDEO_QCOM_CAMSS` = `m` in defconfig; absent in `picasso.config`; `MEDIA_SUPPORT=y`/`VIDEO_DEV=y` already at `configs/linux/picasso.config:243-244` | `sm7250.dtsi:976-1077` (disabled `:1076`), 10 reg windows, IRQs 448/464/465/466/467/468/469/477/478/479, `iommus = <&apps_smmu 0x800 0x5e0>`, power domains `ife0/ife1/top` | not bound |
| CAMSS interconnect | `camss_icc_get()` (`camss.c:5502-5517`) tolerates a missing `interconnects` property (`of_icc_get()` returns `NULL`, `drivers/interconnect/core.c:577-599`) | — | no `interconnects` in the camss node; comment at `sm7250.dtsi:1065` | probe will not fail |
| CAMSS supplies | each CSIPHY wants `vdd-csiphy-0p9` (`camss.c:1957`), board must supply it | — | missing in board DTS | — |
| PM8008 MFD | `drivers/mfd/qcom-pm8008.c` (claims addr+1 as dummy client, `:204`), Makefile:284 | `CONFIG_MFD_QCOM_PM8008` = `m` in defconfig only | absent | not present |
| PM8008 regulators | `drivers/regulator/qcom-pm8008-regulator.c`, Makefile:124; binding `Documentation/devicetree/bindings/mfd/qcom,pm8008.yaml` | `CONFIG_REGULATOR_QCOM_PM8008` = `m` in defconfig only | absent | not present |
| ov8856 | `drivers/media/i2c/ov8856.c` (`ovti,ov8856` at `:2475`) | `CONFIG_VIDEO_OV8856` not set | — | — |
| imx686 | **absent** | — | — | — |
| s5k3t2 | **absent** (`s5k3m5` is a different sensor) | — | — | — |
| s5k5e9 | **absent** (`s5k5baf` is different) | — | — | — |
| gc02m1 | **absent** (`gc05a2`, `gc08a3`, `gc0308`, `gc0310`, `gc2145` only) | — | — | — |
| imx355 | present (`drivers/media/i2c/imx355.c:1340`) but **not used by picasso** | — | — | — |

Working reference inside this very tree: `ML/arch/arm64/boot/dts/qcom/sm7250-oneplus-avicii.dts`
is a sibling SM7250 board that already does `&cci0 { status = "okay"; }` (`:1252`), an I2C sensor
on `&cci0_i2c1` (`:1257`), a full PM8008 node on `&i2c9` (`:1179-1249`) and a CAMSS graph with
`vdd-csiphy-0p9-supply` + `port@3` (`:1294-1312`). Stage 1–3 DTS below follows that proven shape.

---

## 3. Change plan

### (a) DTS — paste-ready into `ML/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`

Stage 1 + stage 2 (CCI enumeration). Append at end of file; only existing labels are referenced,
plus seven new labels defined here (four RPMH parents, three GPIO-gated rails).

```dts
/*
 * ---- Camera stage 1/2 -------------------------------------------------
 * camcc (clock-controller@ad00000) is already enabled in sm7250.dtsi:1079;
 * the CCI controllers are not (sm7250.dtsi:921,966).  Each i2c-bus@N child
 * becomes one i2c adapter named "Qualcomm-CCI" (i2c-qcom-cci.c:551), so
 * cci0 contributes two and cci1 one.
 *
 * Pad muxing for gpio13-33/44-47/56/68/122-127 is done in
 * drivers/soc/qcom/picasso-pinmux.c (EAST/WEST tiles are not mapped by the
 * board's &tlmm node); the pinctrl states inherited from sm7250.dtsi apply
 * as silent no-ops.
 */

/*
 * Camera rail parents.  Downstream: S5A = VIO, S8C = VDIG, BOB = VANA,
 * L16A = VAF (lito-regulators.dtsi:55,414,554,775).  Linux is the only RPMH
 * voter on this boot, so they must be voted and kept on.
 */
&apps_rsc {
	regulators-0 {
		vreg_l16a_2p8: ldo16 {
			regulator-name = "vreg_l16a_2p8";
			regulator-min-microvolt = <2800000>;
			regulator-max-microvolt = <2800000>;
			regulator-initial-mode = <RPMH_REGULATOR_MODE_HPM>;
			regulator-boot-on;
			regulator-always-on;
		};

		vreg_s5a_1p8: smps5 {
			regulator-name = "vreg_s5a_1p8";
			regulator-min-microvolt = <1800000>;
			regulator-max-microvolt = <1800000>;
			regulator-initial-mode = <RPMH_REGULATOR_MODE_HPM>;
			regulator-boot-on;
			regulator-always-on;
		};
	};

	regulators-1 {
		/* downstream floor is 3.008 V; the "2.8 V" in the vendor
		 * camera dtsi is the sensor's request, not a BOB setting */
		vreg_bob_3p0: bob {
			regulator-name = "vreg_bob_3p0";
			regulator-min-microvolt = <3008000>;
			regulator-max-microvolt = <3960000>;
			regulator-initial-mode = <RPMH_REGULATOR_MODE_AUTO>;
			regulator-boot-on;
		};

		vreg_s8c_1p2: smps8 {
			regulator-name = "vreg_s8c_1p2";
			regulator-min-microvolt = <1200000>;
			regulator-max-microvolt = <1200000>;
			regulator-initial-mode = <RPMH_REGULATOR_MODE_HPM>;
			regulator-boot-on;
			regulator-always-on;
		};
	};
};

/*
 * GPIO-gated camera rails (downstream ...-sensor-mtp.dtsi:49-186).  The
 * enable pads are driven by picasso-pinmux, so these are modelled as
 * always-on until the TLMM tiles are mapped properly.
 */
/ {
	camera_vio_1p8: regulator-camera-vio {
		compatible = "regulator-fixed";
		regulator-name = "camera_vio_1p8";
		regulator-min-microvolt = <1800000>;
		regulator-max-microvolt = <1800000>;
		vin-supply = <&vreg_s5a_1p8>;
		regulator-always-on;
	};

	camera_vana_2p8: regulator-camera-vana {
		compatible = "regulator-fixed";
		regulator-name = "camera_vana_2p8";
		regulator-min-microvolt = <2800000>;
		regulator-max-microvolt = <2800000>;
		vin-supply = <&vreg_bob_3p0>;
		regulator-always-on;
	};

	camera_vdig_1p2: regulator-camera-vdig {
		compatible = "regulator-fixed";
		regulator-name = "camera_vdig_1p2";
		regulator-min-microvolt = <1200000>;
		regulator-max-microvolt = <1200000>;
		vin-supply = <&vreg_s8c_1p2>;
		regulator-always-on;
	};
};

&cci0 {
	status = "okay";
};

&cci1 {
	status = "okay";
};
```

Optional stage 1a (PM8008 / charger-bus presence probe; keep separate from the block above so the
camera proof does not depend on a chip that may not be populated):

```dts
/* qupv3_se9 = i2c@98c000 (lito-qupv3.dtsi:411); pads gpio46/47, qup13 */
&i2c9 {
	status = "okay";
};
```

Stage 3 (CAMSS graph) is *not* paste-ready yet — it needs the discovered sensor address and lane
count. The shape to add later, mirroring `sm7250-oneplus-avicii.dts:1294-1312`, is:

```dts
&camss {
	vdd-csiphy-0p9-supply = <&vreg_l5a_0p88>;	/* downstream L5A, 0.88 V */

	status = "okay";

	ports {
		#address-cells = <1>;
		#size-cells = <0>;

		port@1 {			/* main sensor: csiphy-sd-index 1 */
			reg = <1>;

			camss_csiphy1_ep: endpoint {
				data-lanes = <0 1 2 3>;	/* lane count TBD, see U1 */
			};
		};
	};
};
```

### (b) `configs/linux/picasso.config` — exact lines

Stage 1/2 (append near the existing GENI block, `configs/linux/picasso.config:163-165`):

```kconfig
# Camera stage 1/2: camcc provides TITAN_TOP_GDSC which the CCI controllers
# hang off, and the CCI clocks (SM_CAMCC_7250 is not in arm64 defconfig).
CONFIG_SM_CAMCC_7250=y
# One i2c adapter per CCI bus child; the sensors are only reachable here.
CONFIG_I2C_QCOM_CCI=y
# i2cdetect/i2ctransfer on those adapters; already satisfied by
# arch/arm64/configs/defconfig:614, listed here so a future base change keeps it
CONFIG_I2C_CHARDEV=y
```

Stage 3 (CAMSS + the one sensor driver mainline already has):

```kconfig
CONFIG_VIDEO_QCOM_CAMSS=y
CONFIG_VIDEO_OV8856=y
```

Only if the PM8008 presence probe is positive:

```kconfig
CONFIG_MFD_QCOM_PM8008=y
CONFIG_REGULATOR_QCOM_PM8008=y
```

Tristate trap: the RAM boot has no modules (`STATUS.md` "Wi-Fi" section). Every symbol above is
`m` (or absent) in `arch/arm64/configs/defconfig` (`:631, :863, :889, :933`) — all must be `=y`.

### (c) Patches

1. **`patches/linux/picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch`** — regenerate:
   it owns the board DTS. Adds the two `&apps_rsc` regulator children, the three
   `regulator-fixed` rail nodes, and `&cci0`/`&cci1` enable (plus `&i2c9` for stage 1a).
2. **`patches/linux/picasso/0007-picasso-bringup-pinmux-audio-gpu.patch`** — regenerate: it
   creates `drivers/soc/qcom/picasso-pinmux.c`. Add a camera block to
   `picasso_pinmux_init()` (after the touch block, before the amp reset):

   ```c
   /*
    * Drive field is the raw 3-bit TLMM value: (mA / 2) - 1, i.e. 0 = 2 mA,
    * 1 = 4 mA, 2 = 6 mA (drivers/pinctrl/qcom/pinctrl-msm.c:337-340).  The
    * existing calls in this file pass one step high with optimistic comments;
    * the values below follow the vendor dtsi literally.
    */
   /* Camera: CCI I2C pads (cci_i2c = f1, EAST), 2 mA, pull-up */
   picasso_mux(17, TLMM_TILE_EAST, 1, 0, 3, false, 0);	/* CCI0 master0 SDA/SCL */
   picasso_mux(18, TLMM_TILE_EAST, 1, 0, 3, false, 0);
   picasso_mux(19, TLMM_TILE_EAST, 1, 0, 3, false, 0);	/* CCI0 master1 */
   picasso_mux(20, TLMM_TILE_EAST, 1, 0, 3, false, 0);
   picasso_mux(27, TLMM_TILE_EAST, 1, 0, 3, false, 0);	/* CCI1 master0 */
   picasso_mux(28, TLMM_TILE_EAST, 1, 0, 3, false, 0);

   /* Camera MCLK pads: cam_mclk is f1 on 13/14/15/16, f3 on 25 */
   picasso_mux(13, TLMM_TILE_EAST, 1, 1, 0, false, 0);	/* MCLK0 front, 4 mA */
   picasso_mux(14, TLMM_TILE_EAST, 1, 1, 0, false, 0);	/* MCLK1 main, 4 mA */
   picasso_mux(15, TLMM_TILE_EAST, 1, 2, 0, false, 0);	/* MCLK2, 6 mA */
   picasso_mux(16, TLMM_TILE_EAST, 1, 1, 0, false, 0);	/* MCLK3 rear depth, 4 mA */
   picasso_mux(25, TLMM_TILE_EAST, 3, 1, 0, false, 0);	/* MCLK4 macro, f3, 4 mA */

   /* Sensor resets, idle high (active-low in the vendor dtsi), 2 mA */
   picasso_mux(29, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* main */
   picasso_mux(30, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* ultrawide */
   picasso_mux(32, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* front */
   picasso_mux(21, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* rear depth */
   picasso_mux(23, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* macro */
   picasso_mux(33, TLMM_TILE_WEST, 0, 0, 0, true, 1);	/* front depth */

   /* Rail enables (vendor ...-sensor-mtp.dtsi:58..183), 2 mA */
   picasso_mux(68, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* shared VIO 1.8 V */
   picasso_mux(127, TLMM_TILE_WEST, 0, 0, 0, true, 1);	/* main VANA 2.8 V */
   picasso_mux(45, TLMM_TILE_EAST, 0, 0, 0, true, 1);	/* main VDIG 1.2 V */
   picasso_mux(122, TLMM_TILE_WEST, 0, 0, 0, true, 1);	/* ultra/depth VANA */
   picasso_mux(126, TLMM_TILE_WEST, 0, 0, 0, true, 1);	/* ultra VDIG */
   picasso_mux(125, TLMM_TILE_WEST, 0, 0, 0, true, 1);	/* front VANA/VDIG */
   picasso_mux(56, TLMM_TILE_WEST, 0, 0, 0, true, 1);	/* macro VANA */

   /* stage 1a only: QUP1 SE3 (i2c9) pads, qup13 = f1, 2 mA */
   picasso_mux(46, TLMM_TILE_WEST, 1, 0, 0, false, 0);
   picasso_mux(47, TLMM_TILE_WEST, 1, 0, 0, false, 0);
   ```
   (`pm8150l_gpios 12`, the macro VDIG enable at `…-sensor-mtp.dtsi:183`, is an SPMI PMIC GPIO and
   needs `CONFIG_PINCTRL_QCOM_SPMI_PMIC` (already `y` at `configs/linux/picasso.config:12`; the
   board already drives `pm8150l_gpios 3/5` at `sm7250-xiaomi-picasso.dts:431,738,750`) plus a
   `regulator-fixed` consumer with
   `gpio = <&pm8150l_gpios 12 GPIO_ACTIVE_HIGH>`; not needed for the main sensor.)
3. **No new patch is foreseen for CCI or CAMSS themselves** — the mainline drivers and the
   `qcom,sm7250-*` compatibles already exist. If §4-U4 shows camcc probing too late for CCI, the
   fix is a `qcom,camcc` probe-order (`-EPROBE_DEFER` already handled by `power-domains`), not a
   driver patch.
4. Stage 3 only: a new driver file, e.g.
   `patches/linux/picasso/0016-media-i2c-add-imx686-driver.patch`. One paragraph for the commit
   message: *"Add a V4L2 I2C subdevice driver for the Sony IMX686 (64 MP, 4-lane MIPI CSI-2) used as
   the main camera on the Xiaomi Redmi K30 5G. The driver follows the ov8856 driver structure:
   runtime-PM power sequence on avdd/dvdd/dovdd plus an active-low reset GPIO, a register-table
   mode list for the full-resolution and binned readout modes, a link-frequency control, and a
   fwnode-graph endpoint to the SM7250 CSIPHY. Mode register tables are derived from the vendor
   `com.qti.sensor.picasso_imx686.so` init sequences in the shipped image; no vendor code is
   copied."*

### (d) Runtime verification and exact expected output

Stage 1 — camcc + CCI bound (no hardware state change, no sensor needed):

```sh
# from the repo root, after ./build.sh linux && ./build.sh pack-boot && ./tools/ramboot-cycle.sh
ssh -i ~/.ssh/picasso_key root@<ip> '
  dmesg | grep -iE "camcc|ad00000|cci|i2c-qcom-cci";
  ls -l /sys/bus/platform/devices/ac4a000.cci/driver \
        /sys/bus/platform/devices/ac4b000.cci/driver \
        /sys/bus/platform/devices/ad00000.clock-controller/driver;
  i2cdetect -l | grep -i qualcomm-cci'
```

Expected, exactly:

* `ls -l` prints three symlinks resolving to `…/drivers/i2c-qcom-cci` and `…/drivers/camcc-sm7250`
  (today all three print `No such file or directory` — see §5).
* `i2cdetect -l` prints **three** lines, e.g.
  `i2c-2  i2c  Qualcomm-CCI  I2C adapter` (cci0 master0), `i2c-3 … Qualcomm-CCI` (cci0 master1),
  `i2c-4 … Qualcomm-CCI` (cci1 master0). Bus numbers are probe-order dependent; the count `3` and
  the name `Qualcomm-CCI` are the assertions.
* `dmesg | grep -i camcc` shows the camcc driver binding `ad00000.clock-controller`; no
  `failed to get` / `-517` lines for CCI.

Stage 2 — first hardware proof that a camera sensor answers (read-only bus scan):

```sh
ssh -i ~/.ssh/picasso_key root@<ip> 'for b in $(i2cdetect -l |
  grep Qualcomm-CCI | cut -f1 | sed s/i2c-//); do
    echo "== bus $b"; i2cdetect -y "$b"; done'
```

Expected: each adapter prints a clean 0x00–0x77 grid with **no** `Error: …` / `Remote I/O error`
lines (that alone proves CCI register access, clocks and GDSC), and at least one non-`--` cell on
the bus that carries a powered sensor — the address that appears *is* the sensor's slave address
and is recorded from this run (no DTS in any tree states it).

Sensor identity, positive identification of the ultrawide (mainline driver's own ID check,
`ML/drivers/media/i2c/ov8856.c:27-28`: register `0x300a`, 3-byte read, expected `0x00885a`):

```sh
# <bus> and <addr> from the scan above; 0x300a is a 16-bit register address
ssh -i ~/.ssh/picasso_key root@<ip> "i2ctransfer -y -f <bus> w2@<addr> 0x30 0x0a r3"
```

Expected exactly `0x00 0x88 0x5a`. Any other value means that address is not an OV8856; a timeout
means the sensor is not powered or not out of reset.

Stage 2 alternative (driver-level proof, needs the CAMSS node and a sensor node with the discovered
address — stage 3 preparatory): bind `ovti,ov8856` at that address with
`link-frequencies = /bits/ 64 <360000000 180000000>` (4-lane menu,
`ML/drivers/media/i2c/ov8856.c:1511-1518`) and `data-lanes = <1 2 3 4>`; success is
`/sys/bus/i2c/devices/<bus>-<addr>/name` → `ov8856` and **no** `chip id mismatch: 885a!=…` in
dmesg (`ov8856.c:1705`).

Stage 1a — PM8008 presence (only after `&i2c9` is enabled; read-only probe):

```sh
ssh -i ~/.ssh/picasso_key root@<ip> 'for d in /sys/class/i2c-dev/i2c-*; do
    readlink -f "$d/device" | grep -q 98c000 &&
      { b=${d##*-}; echo "== bus $b"; i2cdetect -y "$b"; }; done'
```

Expected: `66` present (bq25970 charger, `DS/picasso-sm7250.dtsi:652-660`). `08` present ⇒ PM8008
is populated; `08` absent ⇒ PM8008 is not on this board and the camera rails are purely the
GPIO-gated ones in §1.3.

Stage 3 — pipeline (after the sensor driver exists):

```sh
ssh -i ~/.ssh/picasso_key root@<ip> '
  media-ctl -p -d /dev/media0 | grep -E "entity|pad0"; v4l2-ctl --list-devices'
```

Expected: entities `qcom-camss`/`csiphy1`/`csid1`/`vfe1` plus the sensor subdev, with an enabled
link `ov8856 … -> csiphy1`; `v4l2-ctl --list-devices` lists a video node whose formats include
`SBGGR10_1X10/3280x2464`.

---

## 4. Unknowns and risks

| # | unknown / risk | why it matters | cheapest experiment |
|---|---|---|---|
| U1 | **CSI lane count per sensor is not in any DT.** Vendor `picasso_*.so` sensor modules carry the lane/link configuration; no text source in this tree. | `data-lanes`/`link-frequencies` are mandatory for CAMSS and for `ov8856` probe (`ov8856.c:2322-2360`). | Bind the sensor and try the 4-lane menu first (all six modules are ≥2-lane; mainline `ov8856` 4-lane menu is `360/180 MHz`). If probe rejects `data-lanes`, retry 2-lane (`720/360 MHz`). One boot each. |
| U2 | **Pads are in unmapped TLMM tiles.** pinctrl writes to EAST/WEST are silent no-ops (`pinctrl-msm.c:88-99`). | Without muxing, CCI has no pins, no MCLK reaches a sensor, no reset/rail ever toggles. | Two candidates, both one boot: (i) map EAST in the board `&tlmm` node (`reg` south+east) and read `dmesg | grep -i pinctrl` + `/sys/kernel/debug/gpio`; (ii) if that still breaks gpiochip registration, keep the `picasso-pinmux` route (§3c-2). |
| U3 | **BOB floor 3.008 V vs "2.8 V" VANA.** The vendor models VANA as a fixed 2.8 V rail off BOB, but BOB's hardware minimum is 3.008 V (`lito-regulators.dtsi:775-790`). Either a discrete LDO sits between them or AVDD really is ~3.0 V. | Over/under-volting AVDD is the fastest way to kill a sensor module. | Declare BOB at 3.008 V, boot, then read `/sys/kernel/debug/regulator/regulator_summary` for the actual set point and the consumer list; if VANA must be 2.8 V, the gate node needs `gpio`/`vin` from the true LDO (measure once with a meter). Do this **before** powering sensors unattended. |
| U4 | **`cci-device`/`cci-master` numbering is inconsistent with the pin groups** (§1.2). | Guessing wrong costs a boot cycle per guess. | Enable both controllers, scan all three adapters (stage 2). The bus that answers tells you the truth; no DT change needed to learn it. |
| U5 | **PM8008 absent or its IRQ pad conflicts.** Binding requires `interrupts`; the vendor's IRQ pad `gpio45` is reused as the main sensor's DVDD enable on picasso, and picasso drops `pm8008_interrupt` from `pinctrl-0` (`DS/picasso-sm7250.dtsi:977-979`). | A node that cannot satisfy the binding (no usable IRQ) will not probe; forcing it onto gpio45 fights the rail. | Stage 1a `i2cdetect` at 0x08. If absent, delete the idea. If present, use `interrupts-extended` on a *different* spare pad only after confirming the IRQ line with a meter. |
| U6 | **Rail enables are set once at `subsys_initcall`** by `picasso-pinmux`. No runtime power cycling. | A sensor that fails its first ID read cannot be recovered without a reboot; power sequencing per camera is impossible. | Accept for stage 2; the real fix is U2 (map EAST/WEST so GPIOs work). Test recovery by rebooting once with a deliberately wrong `data-lanes`. |
| U7 | **`regulator-fixed` nodes are always-on placeholders** — no `gpio` property. | A future sensor driver calling `regulator_enable()` gets a no-op; power sequencing would silently not happen. | Once U2 is fixed, switch each rail node to `gpio = <&tlmm N GPIO_ACTIVE_HIGH>` and drop `regulator-always-on`. |
| U8 | **IMX686 has no mainline driver and no register-table source in this tree.** | The main camera cannot produce frames until a driver exists. | Copy the init sequence out of `com.qti.sensor.picasso_imx686.so` (shipped image, offset ~5574738203 in `super.img`) as the first driver scaffold; verify the chip-ID read against the module before writing mode tables. |
| U9 | Camcc probe ordering / GDSC. CCI declares `power-domains = <&camcc TITAN_TOP_GDSC>`; camcc itself needs `RPMHPD_CX` and `GCC_CAMERA_AHB_CLK`. | If camcc defers, CCI never binds — looks like "driver missing". | `dmesg | grep -i deferred` after boot; `cat /sys/kernel/debug/pm_genpd/*/status` to confirm `TITAN_TOP_GDSC` is on. |
| U10 | **Camera clock MCLK is not enabled anywhere yet.** No mainline consumer of `CAM_CC_MCLK1_CLK` exists. | Without MCLK the sensor cannot answer I2C (most sensors need the clock for their internal state machine). | Stage 3 sensor node must declare `clocks = <&camcc CAM_CC_MCLK1_CLK>`; for stage 2, if `i2cdetect` sees nothing, add a temporary consumer or use the `ov8856` node with `assigned-clock-rates`. Note that `&camcc` labels come from `dt-bindings/clock/qcom,camcc-sm7250.h` (already included by `sm7250.dtsi:6`). |

---

## 5. Live observations (read-only, phone at <lan-ip>)

```sh
$ ssh -i ~/.ssh/picasso_key root@<lan-ip> 'uname -r; ls /dev/i2c-*; ls /dev/media*'
7.3.0-rc7-00001-ge1927d6cbd57-dirty
/dev/i2c-0
# no /dev/media* match
```

```sh
$ … 'ls /sys/bus/i2c/devices; cat /sys/class/i2c-dev/i2c-0/name'
0-0034 -> ../../../devices/platform/soc@0/9c0000.geniqup/988000.i2c/i2c-0/0-0034
i2c-0  -> ../../../devices/platform/soc@0/9c0000.geniqup/988000.i2c/i2c-0
Geni-I2C
```
One I2C bus only (the TFA9874 amp at 0x34). No CCI adapter exists yet.

```sh
$ … 'dmesg | grep -iE "camss|cci|csiphy|pm8008|imx|ov02|s5k|gc02"'
(no output)
```

```sh
$ … 'ls /proc/device-tree/soc@0 | grep -iE "cci|camss|camcc|isp"'
cci@ac4a000
cci@ac4b000
display-subsystem@ae00000
isp@acb3000'
$ … 'cat /proc/device-tree/soc@0/cci@ac4a000/status; cat /proc/device-tree/soc@0/isp@acb3000/status'
disabled
disabled
$ … 'cat /proc/device-tree/soc@0/clock-controller@ad00000/status'
(empty — node has no status property, i.e. enabled)
```
The DT already carries CCI/CAMSS/camcc; both camera nodes are `disabled`.

```sh
$ … 'for d in ad00000.clock-controller ac4a000.cci ac4b000.cci acb3000.isp; do \
         printf "%s: " $d; readlink /sys/bus/platform/devices/$d/driver || echo "(none)"; done'
ad00000.clock-controller: (none)
ac4a000.cci: (none)
ac4b000.cci: (none)
acb3000.isp: (none)
```
No camera driver is bound — camcc has no driver because `CONFIG_SM_CAMCC_7250` is unset, which is
the first config fix.

```sh
$ … 'cat /proc/device-tree/soc@0/pinctrl@f100000/reg | od -An -tx1 | head -2'
 00 00 00 00 0f 10 00 00 00 00 00 00 00 30 00 00
```
One 0x300000 window at 0xf100000 — SOUTH only, confirming the TLMM wall in §1.5.

```sh
$ … 'for t in i2cdetect i2cget i2ctransfer media-ctl v4l2-ctl; do printf "%s: " $t; command -v $t || echo missing; done'
i2cdetect: /usr/bin/i2cdetect
i2cget: /usr/bin/i2cget
i2ctransfer: /usr/bin/i2ctransfer
media-ctl: /usr/bin/media-ctl
v4l2-ctl: /usr/bin/v4l2-ctl
```
All verification tooling is already on the phone; no additional userland packages needed for
stages 1–3 evidence.

```sh
$ … 'ls /sys/class/video4linux/*/name | xargs -r cat'
qcom-venus-encoder
qcom-venus-decoder
```
The only V4L2 nodes today are the Venus codec pair (`CONFIG_VIDEO_QCOM_VENUS`); no CAMSS video
node.

---

## 6. Staged plan recap and effort

| stage | work | effort | proof |
|---|---|---|---|
| 1 | `CONFIG_SM_CAMCC_7250`, `CONFIG_I2C_QCOM_CCI`, `CONFIG_I2C_CHARDEV`; 4 RPMH rails + 3 gate rails; `&cci0`/`&cci1`; camera pads in `picasso-pinmux.c` | ~1 day: 2 config lines, ~40 DTS lines, ~25 pinmux lines, 1 boot cycle per iteration | 3 `Qualcomm-CCI` adapters, camcc bound, `dmesg` clean |
| 1a | `&i2c9` + `gpio46/47` mux; `i2cdetect` for `66`/`08` | ~2 h, same boot as stage 1 | 0x66 always; 0x08 decides PM8008 |
| 2 | sensor scan on the 3 CCI buses; `i2ctransfer` OV8856 ID read | ~2 h after stage 1 | `0x00 0x88 0x5a` from `0x300a`; address table for all modules |
| 3 | CAMSS graph (`port@N`, `vdd-csiphy-0p9`), `CONFIG_VIDEO_QCOM_CAMSS=y`, `CONFIG_VIDEO_OV8856=y`, ov8856 node | ~1–2 days (driver exists; graph + lane/link discovery is the work) | `media-ctl -p` link, `v4l2-ctl --list-devices`, first RAW10 frame |
| 4 | new IMX686 driver (main camera) | ~1–2 weeks: `ov8856.c` is 2496 lines; a minimal single-mode driver is ~700–900 lines plus a register table lifted from the vendor module, then CAMSS tuning | `chip id` match, one 64 MP RAW frame |

Why IMX686 last: it is the only sensor with no mainline driver, it is the most complex mode set,
and the OV8856 path proves the whole CCI → CSIPHY → CSID → VFE chain using a driver that already
exists — decoupling "board/pipeline is wrong" from "sensor driver is wrong".
