# NFC bring-up — NXP NCI controller over GENI I²C (picasso / SM7250)

Scope: identify the NFC controller of the Redmi K30 5G (`picasso`), map it onto the mainline
`nxp-nci` driver, and define the exact DTS/config/pinmux delta plus the hardware verification chain.

**Bottom line up front.** The controller is an NXP NCI part on QUPv3 **wrapper 0 / SE0**
(`i2c@880000`, address `0x28`) with four EAST-tile TLMM pads (`ven` 12, `clkreq` 31, `irq` 34,
`firm` 35) and the bus on two more EAST-tile pads (42/43). Mainline already has every software
piece (`qcom,geni-i2c` node, `nxp,nxp-nci-i2c` driver). The whole feature is gated by the known
TLMM limitation: the board DTS maps **only the `south` tile** (`sm7250-xiaomi-picasso.dts:392-395`),
so all six NFC pads are unreachable through `pinctrl-msm`, which silently no-ops both pinmux and
GPIO writes for an unmapped tile (`drivers/pinctrl/qcom/pinctrl-msm.c:89-98,111-119`). Consequence:
with a pinmux-only workaround the bus works and `i2cdetect` proves the chip at `0x28`, but the
`enable-gpios`/`interrupts` path cannot be real until the EAST tile is mapped in the TLMM node.

---

## 1. Hardware ground truth

All values below come from the vendor 4.19 tree proven on this exact phone
(`~/droidian/kernel/...`), plus the two mainline files that are part of the port.

### 1.1 Which downstream file is live on picasso

`picasso-sm7250.dts` includes exactly two files, and the NFC node lives in the second one:

| Step | File:line | Include |
|---|---|---|
| board root | `picasso-sm7250.dts:3` | `#include "lito.dtsi"` |
| board root | `picasso-sm7250.dts:4` | `#include "picasso-sm7250.dtsi"` |
| board delta | `picasso-sm7250.dtsi:14` | `#include "xiaomi-sm7250-common.dtsi"` |
| board delta | `picasso-sm7250.dtsi:15` | `#include "picasso-sm7250-pinctrl.dtsi"` |

`picasso-sm7250.dtsi` does **not** include `lito-mtp.dtsi`, `lito-cdp.dtsi` or `lito-atp.dtsi`
(its only other includes are the three battery profiles at `:789-791`). Therefore:

| Candidate node | `compatible` line | Parent bus | Live on picasso? |
|---|---|---|---|
| `nq@28` in `xiaomi-sm7250-common.dtsi` | `:328` (node `:327-342`) | `&qupv3_se0_i2c` (`:322`) | **YES** |
| `nq@28` in `lito-mtp.dtsi` | `:297` (parent `:291`) | `&qupv3_se0_i2c` | no (never included) |
| `nq@28` in `lito-cdp.dtsi` | `:108` (parent `:102`) | `&qupv3_se0_i2c` | no |
| `nq@28` in `lito-atp.dtsi` | `:237` (parent `:231`) | `&qupv3_se0_i2c` | no |

All four nodes are on the **same** bus (SE0) with the **same** wiring (`0x28`, `nq-irq` 34,
`nq-ven` 12, `nq-firm` 35, `nq-clkreq` 31, the same three pinctrl states); they differ only in which
board file the vendor build happens to include. So the "which lito variant" question does not change
a single hardware value for picasso — the live node is the one in `xiaomi-sm7250-common.dtsi`, and the
`lito-mtp`/`cdp`/`atp` copies are dead code for this device.

### 1.2 Boot and enable polarity (used by the plan below)

The live boot cmdline already carries `picasso_pinmux.mode=write`, i.e. the project's out-of-pinctrl
pad driver runs in write mode (see §2.3). NFC_VEN is **active high**: the vendor driver drives the
enable pin high with the comment `HPD : Enable` and low for `ULPM: Disable`
(`drivers/nfc/nq-nci.c:1101-1107`).

### 1.3 Property table

| Item | Downstream source | Value |
|---|---|---|
| vendor compatible | `drivers/nfc/nq-nci.c:40-41` | `"qcom,nq-nci"` (NXP NQ/PN/SN family, one in-tree driver) |
| client node | `xiaomi-sm7250-common.dtsi:327` | `nq@28` |
| I²C address | `xiaomi-sm7250-common.dtsi:329` | `0x28` (7-bit) |
| bus | `xiaomi-sm7250-common.dtsi:322` | `&qupv3_se0_i2c` |
| bus node | `lito-qupv3.dtsi:83` | `qupv3_se0_i2c: i2c@880000` |
| bus register window | `lito-qupv3.dtsi:85` | `<0x880000 0x4000>` |
| bus GIC interrupt | `lito-qupv3.dtsi:86` | `GIC_SPI 601`, level high |
| bus wrapper | `lito-qupv3.dtsi:13,99` | `qupv3_0: qcom,qupv3_0_geni_se@8c0000` (wrapper 0 = **North** instance) |
| bus clocks | `lito-qupv3.dtsi:89-92` | `GCC_QUPV3_WRAP0_S0_CLK`, `GCC_QUPV3_WRAP_0_M_AHB_CLK`, `GCC_QUPV3_WRAP_0_S_AHB_CLK` |
| bus rate (vendor) | `xiaomi-sm7250-common.dtsi:324` | `qcom,clk-freq-out = <1000000>` → 1 MHz |
| bus pads | `lito-pinctrl.dtsi:1159-1170` | `gpio42`/`gpio43`, function `qup00`, 2 mA, `bias-disable` |
| IRQ pad | `xiaomi-sm7250-common.dtsi:330,334-336` | `qcom,nq-irq = <&tlmm 34 0>`; `interrupt-parent = <&tlmm>`; `interrupts = <34 0>`; name `nfc_irq` |
| IRQ trigger (driver) | `drivers/nfc/nq-nci.c:1606-1607` | `request_irq(..., IRQF_TRIGGER_HIGH, ...)` → **level-high** |
| enable (VEN) | `xiaomi-sm7250-common.dtsi:331` | `qcom,nq-ven = <&tlmm 12 0>`, active high |
| firmware-download select | `xiaomi-sm7250-common.dtsi:332` | `qcom,nq-firm = <&tlmm 35 0>` |
| clock request | `xiaomi-sm7250-common.dtsi:333` | `qcom,nq-clkreq = <&tlmm 31 0>` |
| eSE power | — | **absent** (no `qcom,nq-esepwr`); vendor treats it as optional, `nq-nci.c:1335-1340` |
| regulators | — | **absent** (no `qcom,nq-vdd-1p8-supply`/`-voltage`/`-current`); driver falls back to defaults 1.65–1.95 V, 157 mA (`drivers/nfc/nq-nci.h:58-66`) |
| clock (SoC) | — | **absent**: no `qcom,clk-src`, so `clk_pin_voting = false` (`nq-nci.c:1341-1345`) and no `ref_clk` is looked up (`nq-nci.c:1277`); the vendor does not vote any clock and the CLKREQ pad is only an input, so the reference clock is always-on and not described anywhere ([INFERENCE], see R5) |
| firmware file name | — | **absent from DT**; the vendor driver has no firmware filename — FW download is entered by ioctl (`NFC_SET_PWR`/`NFC_FW_DWL_VEN_TOGGLE`, `nq-nci.c:869-910`) and executed by the vendor HAL |
| pinctrl states | `xiaomi-sm7250-common.dtsi:337-341` | `nfc_active` = `nfc_int_active`, `nfc_enable_active`, `nfc_clk_req_active`; `nfc_suspend` = same `_suspend` |
| pad: IRQ | `lito-pinctrl.dtsi:1187-1200` | `gpio34`, function `gpio`, 2 mA, `bias-pull-up` |
| pad: VEN + FIRM | `lito-pinctrl.dtsi:1217-1230` | `gpio12`, `gpio35`, function `gpio`, 2 mA, `bias-pull-up` |
| pad: CLKREQ | `lito-pinctrl.dtsi:1247-1260` | `gpio31`, function `gpio`, 2 mA, `bias-pull-up` |
| chip type encoding | `drivers/nfc/nq-nci.h:109-113` | `NQ310=0x40`, `NQ330=0x51`, `SN100_A=0xa3`, `SN100_B=0xa4`, `PN66T=0x18` |
| chip type read from | `drivers/nfc/nq-nci.c:1201-1210` | last four bytes of `CORE_RESET_NTF` payload: `[len-3]=chip_type`, `[len-2]=rom`, `[len-1]=fw_major`, `[len]=fw_minor` |
| SN100 vs older FW-download flow | `drivers/nfc/nq-nci.c:869-910`, `:1011-1019` | different VEN/FIRM toggling for `SN100_A/B`, eSE shares VEN with the NFCC |

**The chip is not named anywhere in the downstream tree.** Neither the DTS, the config nor
`drivers/nfc/nq-nci.c` states a part number; the driver supports NQ310 / NQ330 / PN66T / SN100 and
identifies the die at run time from the `CORE_RESET_NTF` byte above. `qcom,nq-firm` being populated
means the board is wired for FW download over the FIRM pad, which all four variants use. So the
identity question is answered by hardware, not by a file — see §4 risk R2 and the verification
chain in §3.4, which prints the same byte through mainline (`net/nfc/nci/rsp.c:191-194`).

### 1.4 TLMM tile of every NFC pad — the blocker

Tile bases (`drivers/pinctrl/qcom/pinctrl-lito.c:21-24`): `SOUTH 0x00100000`, `WEST 0x00500000`,
`EAST 0x00900000`, relative to `0xf000000`; pin *N* ctl register at `tlmm + tile + 0x1000*N`.

| Pad | Vendor group line | Tile | Function index / name |
|---|---|---|---|
| `gpio42` (SDA) | `pinctrl-lito.c:1410` | **EAST** | 1 = `qup00` |
| `gpio43` (SCL) | `pinctrl-lito.c:1412` | **EAST** | 1 = `qup00` |
| `gpio12` (VEN) | `pinctrl-lito.c:1351` | **EAST** | 0 = `gpio` |
| `gpio31` (CLKREQ) | `pinctrl-lito.c:1389` | **EAST** | 0 = `gpio` |
| `gpio34` (IRQ) | `pinctrl-lito.c:1395` | **EAST** | 0 = `gpio` |
| `gpio35` (FIRM) | `pinctrl-lito.c:1397` | **EAST** | 0 = `gpio` |

Mainline agrees on the tile assignment (`drivers/pinctrl/qcom/pinctrl-sm7250.c:1510,1529,1532,1533,1540,1541`),
and the tile names are `east`/`south`/`west` (`pinctrl-sm7250.c:12-16`).

Consequence, read off the current code:

* `msm_pinctrl_probe()` maps only the named resources that exist and leaves the rest NULL
  (`pinctrl-msm.c:1617-1631`); the board DTS names only `south`
  (`sm7250-xiaomi-picasso.dts:392-395`, confirmed live: `reg-names = "south"`).
* Every accessor is guarded by `if (!pctrl->regs[g->tile])` and returns 0 without touching hardware:
  `msm_readl`/`msm_writel` (`pinctrl-msm.c:89-98`), `msm_readl_io`/`msm_writel_io` (`:111-119`).
  So `pinctrl-0 = <&qup_i2c0_default>` is a **silent no-op**, `gpiod_set_value()` on pads 12/35 is a
  **silent no-op**, and `msm_gpio_irq_handler()` (`:1384-1390`) reads an interrupt-status value of 0
  for pad 34, so the NFC interrupt is **never delivered**.
* This is exactly why the port already ships an out-of-pinctrl pad writer,
  `drivers/soc/qcom/picasso-pinmux.c` (`subsys_initcall` at `:136`), used for BT (EAST pads 38-41,73),
  amp (WEST 49/50), MI2S (WEST 70-74) and touch (EAST 4/5, WEST 6-9).
* Independent live corroboration: the touch controller's INT is on the **EAST** pad 5 and its kernel
  IRQ count is 0 while the driver "polls every 20 ms" — the port already works around an
  undeliverable EAST interrupt by polling.

Hence: **the I²C pads and all four control pads sit on a tile that is not muxable by `pinctrl-msm`
today.** The I²C pads can be muxed by `picasso-pinmux` (proven for BT/amp on the same tiles); VEN can
be driven statically high the same way; the interrupt cannot be faked — it needs the real tile.

---

## 2. Current mainline state

### 2.1 Driver — present, complete, and the right one

| Piece | mainline `file:line` | Note |
|---|---|---|
| I²C glue | `drivers/nfc/nxp-nci/i2c.c:275` `nxp_nci_i2c_probe()` | |
| compatible | `drivers/nfc/nxp-nci/i2c.c:359-362` | `"nxp,nxp-nci-i2c"` |
| enable GPIO | `drivers/nfc/nxp-nci/i2c.c:299` | `devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW)` — **required** |
| firmware GPIO | `drivers/nfc/nxp-nci/i2c.c:305` | `devm_gpiod_get_optional(dev, "firmware", GPIOD_OUT_LOW)` |
| interrupt | `drivers/nfc/nxp-nci/i2c.c:325-339` | threaded IRQ; DT trigger used as-is, rising edge if the DT gives none |
| core / NCI registration | `drivers/nfc/nxp-nci/core.c:128-171` | no clock, no regulator, no firmware needed to probe |
| binding | `Documentation/devicetree/bindings/net/nfc/nxp,nci.yaml:14-47` (in this tree the file lives under `bindings/net/nfc/`, there is no `bindings/nfc/` directory) | compatible `nxp,nxp-nci-i2c`, or `nxp,{nq310,pn547,pn553,pn557}` + that; requires `compatible`, `enable-gpios`, `interrupts`, `reg`; `firmware-gpios` optional; example uses `IRQ_TYPE_LEVEL_HIGH` |

So no new driver is needed for the NCI handshake. Two real gaps remain, both outside this driver:

1. **TLMM tile** (§1.4) — blocks `enable-gpios`, `interrupts`, and the bus pads.
2. **Firmware download protocol** — mainline's `drivers/nfc/nxp-nci/firmware.c:21-43` implements the
   PN544-class protocol (`0xF0` reset, `0xF1` get-version, `0xC0` write, `0xE0` check-integrity,
   `0xF4` get-die-id), while the vendor flow for NQ330/SN100 is NCI-based
   (`nq-nci.c:869-910`, `:1011-1019`). Irrelevant for bring-up: the part answers NCI from ROM, so
   `CORE_RESET`/`CORE_INIT` (and therefore the version handshake) work **without any firmware blob**.
   Do not add a firmware filename to the DT node, or the NCI core will try the wrong downloader.

### 2.2 Device tree — the bus node exists, the client node does not

| Piece | mainline `file:line` | State |
|---|---|---|
| QUP wrapper 0 | `arch/arm64/boot/dts/qcom/sm7250.dtsi:570-604` | `geniqup@8c0000`, comment names downstream SE0 |
| SE0 I²C | `sm7250.dtsi:583-604` | `i2c0: i2c@880000`, `qcom,geni-i2c`, SPI 601, `GCC_QUPV3_WRAP0_S0_CLK`, `dmas`, interconnects, `status = "disabled"` |
| SE0 pad state | `sm7250.dtsi:1852-1857` | `qup_i2c0_default`: `gpio42`,`gpio43`, `qup00`, 2 mA, `bias-disable` — the exact vendor state |
| TLMM | `sm7250.dtsi:1803-1820` | three reg windows `south`/`west`/`east`, `gpio-ranges = <&tlmm 0 0 146>`, `wakeup-parent = <&pdc>` |
| board override | `sm7250-xiaomi-picasso.dts:392-395` | narrows `&tlmm` to `reg = <0xf100000 0x300000>`, `reg-names = "south"` |
| wrapper enabled | `sm7250-xiaomi-picasso.dts:478-480` | `&qupv3_id_0 { status = "okay"; }` |
| client node | — | **absent**; nothing in the board DTS references `&i2c0` |

Live confirmation that this is the exact state on the phone: the parent is `okay`, the SE0 child is
`disabled`, and the pinctrl node advertises only the `south` resource (§5).

### 2.3 Pinmux escape hatch — present, already covers the same tiles

`drivers/soc/qcom/picasso-pinmux.c` writes vendor tile registers directly (`:83-136`), and is the
established pattern for exactly this class of pad:

* EAST: `gpio38-41` (uart5 / `qup05`, func 1), `gpio73` (SW_CTRL), `gpio4` (touch reset, output),
  `gpio5` (touch INT input + pull-up) — `:94-121`.
* WEST: `gpio49/50` (i2c8 / `qup12`, func 2), `gpio70-74` (secondary MI²S), `gpio58` (amp reset).
* Bias encoding: `0` none, `1` pull-down, `2` keeper, `3` pull-up; the mux value is the function's
  index in the vendor `PINGROUP()` list (`picasso-pinmux.c:43`).

NFC needs six more entries in that table (§3.3).

### 2.4 Kernel config

| Symbol | Where | State |
|---|---|---|
| `CONFIG_NFC` | `arch/arm64/configs/defconfig:205` | `=m` |
| `CONFIG_NFC_NCI` | `defconfig:206` | `=m` |
| `CONFIG_NFC_NXP_NCI` | `defconfig:207` | `=m` |
| `CONFIG_NFC_NXP_NCI_I2C` | `defconfig:208` | `=m` |
| `CONFIG_I2C_QCOM_GENI` | `configs/linux/picasso.config:164` | `=y` |
| `CONFIG_NFC*` in `configs/linux/picasso.config` | — | **no lines** |
| `CONFIG_NFC*` in `configs/linux/base.config` | — | **no lines** |

The running kernel already has all four NFC symbols as modules (live `/proc/config.gz`), because
`./build.sh configure` starts from `defconfig` (`build.sh:94-101`). So the symbols exist; the only
decision left is their mode (`=m` today vs `=y` in §3.2).

### 2.5 Live probe result today

`/dev/i2c-0` is the **amp** bus (`geniqup@9c0000/i2c@988000`, device `0-0034`), not NFC. There is no
GENI instance at `0x880000`, no `nxp_nci` module loaded, no `/dev/nfc0`, and the only `nfc` strings in
`dmesg` come from the Android userspace apex. Details in §5.

---

## 3. Change plan

### 3.1 (a) DTS — paste-ready

Insert after the existing `&qupv3_id_0` block (`sm7250-xiaomi-picasso.dts:478-480`). `&i2c0` is the
label of `sm7250.dtsi:583`; the `&tlmm` GPIO macros are already available (file includes
`dt-bindings/gpio/gpio.h` at `:13`, and `IRQ_TYPE_*`/`GIC_*` via `sm7250.dtsi`).

```dts
/*
 * NFC: NXP NCI controller on QUPv3 wrapper 0 / SE0 (downstream qupv3_se0_i2c,
 * xiaomi-sm7250-common.dtsi:322-343).  Address 0x28; VEN on tlmm 12 (active
 * high, nq-nci.c:1101-1107), firmware-download select on tlmm 35, IRQ on
 * tlmm 34 which the vendor requests level-high (nq-nci.c:1606).  The vendor
 * bus rate is 1 MHz (xiaomi-sm7250-common.dtsi:324); start at 400 kHz.
 * All six pads are EAST-tile TLMM pins: until &tlmm maps that tile they are
 * muxed by drivers/soc/qcom/picasso-pinmux.c instead.
 */
&i2c0 {
	status = "okay";
	clock-frequency = <400000>;

	nfc@28 {
		compatible = "nxp,nxp-nci-i2c";
		reg = <0x28>;

		interrupt-parent = <&tlmm>;
		interrupts = <34 IRQ_TYPE_LEVEL_HIGH>;

		enable-gpios = <&tlmm 12 GPIO_ACTIVE_HIGH>;
		firmware-gpios = <&tlmm 35 GPIO_ACTIVE_HIGH>;
	};
};
```

Notes:

* `enable-gpios` and `interrupts` are the two binding-required properties
  (`bindings/net/nfc/nxp,nci.yaml:36-40`); `firmware-gpios` is optional but the pad exists and the
  vendor drives it, so describe it.
* No `pinctrl-*` in the client node: the pads are muxed either by `qup_i2c0_default` (once the EAST
  tile is mapped) or by `picasso-pinmux`.
* No regulator, no clock, no `nxp,firmware-name`: see §1.3 and §2.1 — adding a firmware name would
  engage mainline's PN544-era downloader, which is wrong for this die.
* Keep `compatible = "nxp,nxp-nci-i2c"` generic. The binding only allows the specific fallbacks
  `nxp,{nq310,pn547,pn553,pn557}`; `nxp,nq330`/`nxp,sn100` do not exist upstream, so a specific
  string would fail `dtbs_check`. Refine only after §3.4 prints the die byte.

Syntax check performed while writing this document: the snippet was inserted into a throwaway copy
of `sm7250-xiaomi-picasso.dts` and compiled with the tree's own include paths. It compiles with no
new warnings and the resulting DTB contains exactly

```
i2c@880000 { status = "okay"; clock-frequency = <0x61a80>;          /* 400 kHz */
             nfc@28 { compatible = "nxp,nxp-nci-i2c"; reg = <0x28>;
                      interrupts = <0x22 0x04>;                   /* 34, LEVEL_HIGH */
                      enable-gpios   = <&tlmm 0x0c 0x00>;         /* 12, ACTIVE_HIGH */
                      firmware-gpios = <&tlmm 0x23 0x00>; }; };   /* 35, ACTIVE_HIGH */
```

`interrupt-parent` inside an `additionalProperties: false` NFC binding is the pattern used by every
sibling schema in `Documentation/devicetree/bindings/net/nfc/` (e.g. `nxp,nci.yaml:58`), so
`./build.sh check`'s `dtbs_check` should stay clean.

### 3.2 (b) Config lines for `configs/linux/picasso.config`

Append (the fragment overrides the `defconfig` `=m` values; `=y` because the RAM initramfs does not
load modules and the kernel version string is `-dirty`, which makes out-of-tree module matching
needlessly fragile during bring-up):

```
# NFC: NXP NCI controller on GENI SE0 (i2c@880000, addr 0x28)
CONFIG_NFC=y
CONFIG_NFC_NCI=y
CONFIG_NFC_NXP_NCI=y
CONFIG_NFC_NXP_NCI_I2C=y
```

`CONFIG_I2C_QCOM_GENI=y` is already present (`configs/linux/picasso.config:164`) and must stay `=y`.
`CONFIG_NFC_DIGITAL`, `CONFIG_NFC_HCI` and the LLCP stack are not needed for the NCI version
handshake (`net/nfc/nci` needs only `CONFIG_NFC_NCI`).

### 3.3 (c) Patches

1. **Board DTS — folded into `patches/linux/picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch`.**
   Edit `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts` and re-export with
   `./build.sh export-patches`; the device's DTS is owned by that patch, do not open a new one for it.

2. **New: `patches/linux/picasso/0016-picasso-pinmux-nfc.patch`** — extends
   `drivers/soc/qcom/picasso-pinmux.c` with the six EAST-tile NFC pads, because `pinctrl-msm` cannot
   reach that tile on this port. The change is six `picasso_mux()` calls in
   `picasso_pinmux_init()` (after the Bluetooth block at `:94-98`), using the mux indices from the
   vendor `PINGROUP()` list: pads 42/43 to function 1 (`qup00`, 2 mA, no bias), pad 12 to function 0
   as an output driven **high** (VEN, chip powered), pad 35 to function 0 as an output driven low
   (FIRM, NCI mode), pads 31 and 34 to function 0 as inputs with pull-up (CLKREQ, IRQ). Code:

   ```c
   /* NFC: QUPv3_0 SE0 pads (qup00 = function 1, 2 mA) */
   picasso_mux(42, TLMM_TILE_EAST, 1, 2, 0, false, 0);	/* SDA */
   picasso_mux(43, TLMM_TILE_EAST, 1, 2, 0, false, 0);	/* SCL */
   /* NFC control pads: VEN high (HPD), FIRM low, CLKREQ/IRQ inputs, pull-up */
   picasso_mux(12, TLMM_TILE_EAST, 0, 2, 3, true, 1);	/* VEN   */
   picasso_mux(35, TLMM_TILE_EAST, 0, 2, 3, true, 0);	/* FIRM  */
   picasso_mux(31, TLMM_TILE_EAST, 0, 2, 3, false, 0);	/* CLKREQ*/
   picasso_mux(34, TLMM_TILE_EAST, 0, 2, 3, false, 0);	/* IRQ   */
   ```

   Effect: bus + chip power become real, which is enough to reach the `0x28` ACK milestone. It does
   **not** make the interrupt work (that needs the tile mapped), so it cannot by itself produce a
   completed NCI handshake.

3. **New: `patches/linux/picasso/0017-pinctrl-msm-map-east-tile.patch`** (the real unblock; shared
   with BT/touch, see `STATUS.md` "TLMM (pinmux) — root cause of the silent BT/touch"). This patch is
   driver-side only — the `&tlmm` widening itself is a DTS edit and therefore belongs to patch 0002.
   The driver part is whatever kills `devm_gpiochip_add_data()` once a second tile window is mapped:
   with `reg = <0xf100000 0x300000>, <0xf500000 0x300000>, <0xf900000 0x300000>` and
   `reg-names = "south", "west", "east"` in the DTS, the boot currently dies inside the gpiochip
   registration. `pinctrl-msm.c:1617-1631` already skips absent tiles and kernel-side `ioremap` reads
   of both extra windows are proven to work, so the fault is in the multi-tile gpiochip/irqchip
   setup, not the address map. Minimal-change rule: touch nothing else in the driver, and keep the
   absent-tile skip (`if (!res) pctrl->regs[i] = NULL;`) so boards that describe one tile still boot.

4. **Diagnostic-only (no patch preferred):** the die byte is observable with dynamic debug on
   `net/nfc/nci/rsp.c:191-194` + `ntf.c:42` (§3.4). Only if those four bytes do not decode to a
   plausible chip id, add a temporary `print_hex_dump()` of the `CORE_RESET_NTF` payload in
   `nci_core_reset_ntf_packet()`.

### 3.4 (d) Verification chain and the exact expected output

Run from the repository root; `$PHONE` is one of the known addresses
(`<lan-ip>`, `<lan-ip>`, `<lan-ip>`, `10.77.71.2`).

**Step 0 — resolve the bus index (do not assume it).** Adapter numbers are registration order.

```sh
ssh -i "$HOME/.ssh/picasso_key" root@$PHONE \
  'for d in /sys/bus/i2c/devices/i2c-*; do printf "%s %s %s\n" "$d" "$(cat $d/name)" "$(readlink -f $d/of_node)"; done'
```

Expected shape (the numbers themselves are registration order, [INFERENCE]: `qupv3_id_0` is declared
before `qupv3_id_1` in `sm7250.dtsi:570` vs `:630`, so SE0 should land first and push the amp bus
from `i2c-0` to `i2c-1`):

```
/sys/bus/i2c/devices/i2c-0 Geni-I2C /sys/.../soc@0/geniqup@8c0000/i2c@880000
/sys/bus/i2c/devices/i2c-1 Geni-I2C /sys/.../soc@0/geniqup@9c0000/i2c@988000
```

Take the number whose path ends in `geniqup@8c0000/i2c@880000` and use it as `$BUS` below; never
assume the number, the amp bus already owns `i2c-0` today (§5).

**Step 1 — the chip answers on the bus.** This alone proves pads + power + address:

```sh
ssh -i "$HOME/.ssh/picasso_key" root@$PHONE "i2cdetect -y -r $BUS"
```

Expected (only `0x28`, the bus is otherwise empty):

```
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
20: -- -- -- -- -- -- -- -- 28 -- -- -- -- -- -- --
```

If `0x28` is missing while `i2c@880000` is `okay` and the controller probed
(`dmesg | grep 880000.i2c` shows `geni_i2c 880000.i2c: Bus frequency ...`), then the pads or the
rail are wrong — go to risk R3.

**Step 2 — driver bound, NCI device registered.**

```sh
ssh -i "$HOME/.ssh/picasso_key" root@$PHONE \
  'ls -l /sys/bus/i2c/devices/'$BUS'-0028/driver; ls -l /dev/nfc0; dmesg | grep -iE "nxp_nci|nci|nfc0"'
```

Expected: `/sys/bus/i2c/devices/$BUS-0028/driver -> .../drivers/nxp_nci_i2c` (directory exists),
`/dev/nfc0` present. Note: `/dev/nfc0` appears at probe time even when the interrupt path is dead, so
it is **not** proof of a handshake.

**Step 3 — CORE_RESET/CORE_INIT complete (the real handshake) and the die byte.**

```sh
ssh -i "$HOME/.ssh/picasso_key" root@$PHONE \
  'echo "file ntf.c +p" > /sys/kernel/debug/dynamic_debug/control; \
   echo "file rsp.c +p" > /sys/kernel/debug/dynamic_debug/control; \
   dmesg -C; nfctool --list; timeout 8 nfctool --device nfc0 --poll; dmesg | grep -iE "nci_ver|manufact"'
```

Expected lines (values depend on the die):

```
nci: nci_core_reset_ntf_packet: nci_ver 0x1x, config_status 0x0
nci: nci_core_init_rsp_packet: manufact_id 0x4
nci: nci_core_init_rsp_packet: manufact_specific_info 0x........
```

(the `nci: <function>: ` prefix comes from `pr_fmt` in `net/nfc/nci/ntf.c:16` and `rsp.c:15`, so
`dmesg | grep nci_ver` matches only these lines)

`nci_ver` and `config_status` are printed by `net/nfc/nci/ntf.c:38-45`; `manufact_id` 0x4 is NXP, and
`manufact_specific_info` is printed by `net/nfc/nci/rsp.c:191-194` (both `pr_debug`, hence the
dynamic-debug step). The **low byte** of `manufact_specific_info` is the chip type that the vendor
driver parses from the NTF tail (`nq-nci.c:1201-1210` vs `struct nci_core_reset_ntf` in
`include/net/nfc/nci.h:400-407`): `0x51` → NQ330/PN553 class, `0xa3`/`0xa4` → SN100. This mapping is
[INFERENCE] and is the one thing Step 3 must confirm before the DTS `compatible` is specialised.

Failure signature for the unmapped-tile case (today's expected behaviour with only patch 0016):
`nfctool --device nfc0 --poll` returns an error and `dmesg` shows an NCI request timeout (`-110`) after
`NCI_RESET_TIMEOUT`, because the CORE_RESET_NTF arrives on I²C but no interrupt ever fires
(`/proc/interrupts` shows `msmgpio 34` at 0).

**Step 4 — interrupt arrives** (only meaningful once the EAST tile is mapped):

```sh
ssh -i "$HOME/.ssh/picasso_key" root@$PHONE 'grep msmgpio /proc/interrupts'
```

Expected: a line `... msmgpio 34 Level nxp_nci_i2c` whose count increases while the chip is up. If
the count stays 0 while Step 1 passes, the trigger type is wrong: try
`interrupts = <34 IRQ_TYPE_EDGE_RISING>` (the vendor's line is level-high, but the pad is
open-drain and the mainline handler is one-shot; edge is the fallback the driver itself assumes when
no trigger is given, `nxp-nci/i2c.c:330-333`).

---

## 4. Unknowns and risks

| # | Risk / unknown | Why it matters | Cheapest experiment that resolves it |
|---|---|---|---|
| R1 | **EAST tile cannot be mapped** — two/three tile windows kill the boot inside `devm_gpiochip_add_data` (`STATUS.md` TLMM section) | Blocks `enable-gpios`, `interrupts` and the bus pads; caps NFC at "chip answers on I²C" | Boot a DTS with `reg-names = "south", "east"` only (2 tiles). If it hangs, rebuild with `CONFIG_GPIO_CDEV=n`: if it then boots, the fault is the gpio chardev path in the multi-tile `gpiochip_setup_dev`; otherwise add step prints in `gpiochip_add_irqchip`. One boot each, no hardware risk |
| R2 | **Die identity unknown** (NQ330/PN553 vs SN100) | Decides whether FW download ever needs the NCI flow and whether `compatible` can be specialised | Step 3's `manufact_specific_info` low byte. No extra hardware action — one `nfctool --poll` |
| R3 | **NFC supply rail unknown** — the vendor node declares no `qcom,nq-vdd-1p8-supply`, so VDDH is either always-on or gated by a PMIC rail the DT does not name | If it is gated, `0x28` never ACKs no matter how good the pinmux is | After patch 0016, run `i2cdetect`; if silent, dump `regulator_summary` and look for an always-on/nfc-named rail, then cross-check `lito-regulators.dtsi` for the rail the vendor CFA/LDO list assigns to NFC. One read-only sweep |
| R4 | **IRQ electrical mode** — vendor requests `IRQF_TRIGGER_HIGH`, the pad is open-drain with pull-up | Level-high on a one-shot threaded handler can storm or miss | `grep msmgpio /proc/interrupts` before/after one `nfctool --poll`; if 0, switch the DTS to `IRQ_TYPE_EDGE_RISING` and re-test (one DTS rebuild) |
| R5 | **Clock source** — no `qcom,clk-src`/`ref_clk` in the vendor node; the chip is clocked from an always-on source | If the source is gated, the NFCC never answers at all | Already mitigated: cmdline carries `clk_ignore_unused`. If Step 1 fails while power looks right, check the PMIC RF-clock output enable in `lito-pmic-overlay.dtsi` |
| R6 | **VEN permanently high** after patch 0016 | The chip stays powered (and draws current) even with no driver; the probe-time `set_mode(COLD)` cannot power-cycle it because the GPIO write is a no-op | Accept for bring-up, then delete the VEN line from patch 0016 once R1 is fixed. Cost measurement: battery current with `picasso_pinmux.mode=read` vs `write` |
| R7 | **Firmware download** would use the wrong protocol in mainline (`firmware.c:21-43`) | Would waste hours on `0xF0/0xC0` errors | Do not add a firmware name. ROM NCI is enough for reset/init/poll; only revisit if a specific RF feature needs the patched FW |
| R8 | `nfctool`/`neard` are not installed on the current image (§5) | Step 3 cannot run | `pacman -S neard` (provides `nfctool`) on the Denial rootfs, or drive NCI through `neard`'s systemd unit; no kernel change |

---

## 5. Live observations (read-only, 2026-11-17, phone `<lan-ip>`, kernel `7.3.0-rc7-00001-ge1927d6cbd57-dirty`)

Every command below is read-only; nothing was flashed, written, loaded or power-cycled.

**Command**

```sh
ssh -i "$HOME/.ssh/picasso_key" root@<lan-ip> \
  'ls -l /dev/i2c-*; ls /sys/bus/i2c/devices/; cat /proc/device-tree/soc@0/geniqup@8c0000/i2c@880000/status; \
   cat /proc/device-tree/soc@0/pinctrl@f100000/reg-names; echo; \
   cat /proc/device-tree/soc@0/pinctrl@f100000/qup-i2c0-default-state/{pins,function}; echo'
```

**Output excerpt**

```
crw-rw---- 1 root i2c 89, 0 Nov 17 14:47 /dev/i2c-0
0-0034
i2c-0
disabled
south
gpio42gpio43
qup00
```

Interpretation: exactly one GENI I²C bus exists and it is the amp bus; the SE0 node is present but
`disabled`; the TLMM advertises only the `south` resource while the pre-defined SE0 pad state
(`gpio42`/`gpio43` → `qup00`) is already in the DTB — i.e. the DTS side is one `status = "okay"`
away, and the tile is the wall.

**Command**

```sh
ssh -i "$HOME/.ssh/picasso_key" root@<lan-ip> \
  'grep -nE "gpio12|gpio31|gpio34|gpio35|gpio42|gpio43" /sys/kernel/debug/gpio; grep msmgpio /proc/interrupts; \
   dmesg | grep -iE "i2c_geni|880000|nfc|nq-nci" | head'
```

**Output excerpt**

```
 gpio12  : in  low  func0 2mA no pull
 gpio31  : in  low  func0 2mA no pull
 gpio34  : in  low  func0 2mA no pull
 gpio35  : in  low  func0 2mA no pull
 gpio42  : in  low  func0 2mA no pull
 gpio43  : in  low  func0 2mA no pull
 137: ... msmgpio      5 Edge      picasso-nvt-vendor
[ 12.612074] apexd-bootstrap: Found pre-installed APEX /system/apex/com.android.nfcservices.apex
[ 24.713947] [com.android.nfc] Success
```

Two extra facts worth recording:

1. Those `in low func0` values for EAST pads are the **NULL-tile guard's zero**
   (`pinctrl-msm.c:89-98,111-119`), not real pad state — the DEBUFS dump read 0 because the tile is
   not mapped. Do not use them as electrical evidence.
2. `msmgpio 5` is the touch INT on the **same EAST tile**, registered (`Edge`) with a **count of 0**
   while the touch driver logs `ready: event buffer 0x22d00, polling every 20 ms`. That is live,
   independent proof that an EAST-tile GPIO interrupt does not reach the GIC on this port — the exact
   failure NFC's pad 34 will hit.

**Command**

```sh
ssh -i "$HOME/.ssh/picasso_key" root@<lan-ip> \
  'cat /proc/cmdline; zcat /proc/config.gz | grep -E "CONFIG_NFC|CONFIG_NXP_NCI"; \
   command -v i2cdetect i2cget nfctool neard'
```

**Output excerpt**

```
console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0 ... clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10
CONFIG_NFC=m
CONFIG_NFC_NCI=m
CONFIG_NFC_NXP_NCI=m
CONFIG_NFC_NXP_NCI_I2C=m
/usr/bin/i2cdetect
/usr/bin/i2cget
nfctool: MISSING
neard: MISSING
```

So: the driver stack is already built (as modules) in the running kernel, the pad writer is active in
write mode, `i2cdetect` is available for Step 1, and only the userspace NFC tooling is missing.
