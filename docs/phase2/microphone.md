# Capture path bring-up: WCD9385 (SoundWire) + LPI DMIC on SM7250 / picasso

Scope: make **capture** work on the Redmi K30 5G (`picasso`, SM7250/lito) under the
mainline tree in `build/linux/`. Playback already works through `SECONDARY_MI2S_RX` →
TFA9874; today the card exposes only `00-00: MultiMedia1 (*) : : playback 1` and
`arecord -l` lists no capture device.

Verdict up front: **there is no capture path in the mainline port at all** — not a
broken one. Three independent things are missing and all three are needed:

1. **No LPASS codec nodes in the DTS.** `sm7250.dtsi` has `q6afe`/`q6asm`/`q6adm`
   (APR services) and nothing else — no `txmacro`, `rxmacro`, `vamacro`,
   `swr1`/`swr2`, no LPI pinctrl (`build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi`,
   grep for `macro|swr|pinctrl@33c0000` returns nothing; APR block at
   `sm7250.dtsi:2264-2320`).
2. **The macro/LPI drivers are modules and no modules exist on the device.**
   `/proc/config.gz` reports `CONFIG_SND_SOC_LPASS_{RX,TX,VA}_MACRO=m`,
   `CONFIG_SND_SOC_LPASS_MACRO_COMMON=m`, `CONFIG_PINCTRL_SM8250_LPASS_LPI=m`,
   while `ls /lib/modules` is empty and `/proc/modules` has 0 lines. `=m` is
   therefore dead code on this rootfs.
3. **The capture frontend is declared playback-only.** The board file declares
   `dai@0..3 { direction = <Q6ASM_DAI_RX>; }`; in `q6asm-dai.c:1289-1292`
   `Q6ASM_DAI_RX` *deletes the capture stream*, so all four frontends are
   playback-only, and the card has no `MultiMedia2+` link anyway.

Everything else (machine driver, AFE ports, codec driver, SoundWire master, q6routing
mixers) is present in this tree and needs no code change.

---

## 1. Hardware ground truth

All values from the downstream tree proven on this phone
(`~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/`, referred to as `dt/` below)
and the vendor audio pack (`~/droidian/kernel/techpack/audio/`, `ta/`).

| Item | Downstream source | Value |
|---|---|---|
| Codec instance | `dt/picasso-audio-overlay.dtsi:23` (`// wcd9385 TX`), `dt/lito-audio-overlay.dtsi:177-222` | **WCD9385**, split codec (`qcom,split-codec = <1>`), one aux device under Bolero |
| Bolero macro count | `dt/picasso-audio-overlay.dtsi:91-93` | `qcom,num-macros = <3>` → RX + TX + VA (WSA disabled, `:95-98`) |
| Codec reset GPIO | `dt/lito-audio-overlay.dtsi:413-418` (`msm_cdc_pinctrl@57`), `dt/lito-pinctrl.dtsi:640-666` | TLMM **gpio57**, WEST tile; active state = `output-high` (out of reset), sleep = `output-low`; reset is **active-low** |
| Codec supplies | `dt/lito-audio-overlay.dtsi:197-211` | `cdc-vdd-rxtx` = S4A 1.8 V, `cdc-vddio` = S4A 1.8 V, `cdc-vdd-buck` = S4A 1.8 V (650 mA), `cdc-vdd-mic-bias` = **BOB 3.296 V** |
| BOB rail | `dt/lito-regulators.dtsi:775-791` | `pm8150a_bob`, 3.008–3.96 V, `qcom,init-voltage = <3008000>`, mode PASS (RPMH PMIC id `c`) |
| Micbias (picasso) | `dt/picasso-audio-overlay.dtsi:129-135` | `micbias1..4 = 2750 mV` (all four); lito default is 1800 mV (`dt/lito-audio-overlay.dtsi:213-216`) |
| MBHC jack wiring | `dt/picasso-audio-overlay.dtsi:58-59` | `msm-mbhc-hphl-swh = <1>`, `msm-mbhc-gnd-swh = <1>` (both switches present) |
| Analog mic wiring | `dt/picasso-audio-overlay.dtsi:22-29` | AMIC1 ← MIC BIAS1 = "Handset Mic"; AMIC2 ← MIC BIAS2 = "Headset Mic"; AMIC5 ← MIC BIAS3 = "Secondary Mic" |
| TX macro | `dt/lito-audio-overlay.dtsi:24-31` | `tx-macro@3220000`, `qcom,tx-dmic-sample-rate = <2400000>` |
| TX SoundWire master (swr2) | `dt/lito-audio-overlay.dtsi:32-62` | `swrm-io-base = <0x3230000>`, master id 3, **IRQ GIC_SPI 166** (+ PDC 109), 5 ports, TX slave `reg = <0x0D 0x01170223>` |
| TX SWR port map | `dt/lito-audio-overlay.dtsi:49-55` | `<1 PCM_OUT1 0xF>`, `<2 ADC1 0x1> <2 ADC2 0x2>`, `<3 ADC3 0x1> <3 ADC4 0x2>`, `<4 DMIC0..3 0x1,2,4,8>`, `<5 DMIC4..7 0x1,2,4,8>` |
| RX macro | `dt/lito-audio-overlay.dtsi:66-75` | `rx-macro@3200000`, `qcom,rx-bcl-pmic-params = <0x00 0x04 0x3E>` |
| RX SoundWire master (swr1) | `dt/lito-audio-overlay.dtsi:76-101` | `swrm-io-base = <0x3210000>`, master id 2, **IRQ GIC_SPI 155**, 5 ports, RX slave `reg = <0x0D 0x01170224>` |
| VA macro | `dt/lito-audio-overlay.dtsi:166-175` | `va-macro@3370000`, `qcom,va-dmic-sample-rate = <600000>`, `va-clk-mux-select = <1>`, `va-island-mode-muxsel = <0x033A0000>` |
| LPI pin controller | `dt/lito-lpi.dtsi:1-29` | `lpi_pinctrl@33c0000`, slew reg `0x355a000`, 15 pins, clocks `lpass_core_hw_vote`/`lpass_audio_hw_vote` |
| LPI pins — TX SWR | `dt/lito-lpi.dtsi:1351-1461` | clk **gpio0**, data1 **gpio1**, data2 **gpio2**, data3 **gpio14** (all `func1`) |
| LPI pins — RX SWR | `dt/lito-lpi.dtsi:1463-1545` | clk **gpio3**, data **gpio4**, data1 **gpio5** |
| LPI pins — DMIC pads | `dt/lito-lpi.dtsi:1547-1707` | dmic01 = gpio6 (clk)/gpio7 (data); dmic23 = **gpio8/gpio9**; dmic45 = **gpio12/gpio13** |
| DMIC pads enabled on picasso | `dt/picasso-audio-overlay.dtsi:60-62` | dmic01 **commented out**, `cdc-dmic23` + `cdc-dmic45` enabled (note `dt/lito-audio-overlay.dtsi:355-363` leaves the dmic45 pinctrl node `disabled`, picasso still references it) |
| DMIC power rails | `dt/lito-audio-overlay.dtsi:242-253, 281-292` | TX DMIC0/1 ← MIC BIAS3, DMIC2/3 ← MIC BIAS1, DMIC4/5 ← MIC BIAS4 (same for VA DMICn) |
| AFE backend, WCD capture | `ta/asoc/kona.c:183-189, 685, 2725-2732` | `TX_CDC_DMA_TX_3` (48 kHz, S16_LE, 2 ch) for the TX macro; `VA_CDC_DMA_TX_0..2` (8 ch) for the VA macro |
| AFE port ids | mainline `sound/soc/qcom/qdsp6/q6afe.c:343,363` | `AFE_PORT_ID_VA_CODEC_DMA_TX_0 = 0xB021`, `AFE_PORT_ID_TX_CODEC_DMA_TX_3 = 0xB037` |
| Vendor DMIC pad gating | `ta/asoc/kona.c:5477-5556` | `msm_dmic_event()` selects the `cdc-dmic01/23/45` LPI pinctrl state on DAPM PRE_PMU of "Digital Mic0..5" — the vendor drives those pads from the *machine* driver, mainline has no such hook (see §4.6) |

Hardware table notes that matter for mainline:

* SM7250's LPASS macro **addresses are identical to SM8250** (0x3200000/0x3220000/0x3370000,
  LPI at 0x33c0000), but the **SoundWire IRQs are not** (155/166 upstream vs 297/298 on
  sm8250). Using `sm8250.dtsi`'s IRQ numbers is a silent failure (no interrupts, no
  enumeration).
* The codec's reset pin (gpio57) and the amp's reset pin (gpio58) are both **WEST-tile**
  TLMM pins. In this port the WEST window is *not* mapped into `pinctrl-msm`
  (`patches/linux/picasso/0002-...patch` sets `reg-names = "south"` only, and
  `patches/linux/picasso/0005-pinctrl-msm-skip-unmapped-tiles.patch` turns writes to
  unmapped tiles into silent no-ops). `drivers/soc/qcom/picasso-pinmux.c` works around
  this by ioremapping the whole 16 MiB TLMM and writing the pads directly; its comment
  states the gpiod level requests for those pads are no-ops. See §4.1.

---

## 2. Current mainline state

### 2.1 Drivers: all present, no code gap for the basic path

| Piece | File:line | State |
|---|---|---|
| Machine driver | `sound/soc/qcom/sm8250.c:211` (`qcom_snd_parse_of`), `:220` (`qcom,sm8250-sndcard`), `:58` (`sm8250_be_hw_params_fixup`), `:148` (`qcom_snd_sdw_startup`) | DTS-driven: every PCM/link comes from the `sound` node. BE links are auto-detected (cpu+platform+codec ⇒ `no_pcm`), `common.c:240-320`. No static FE list to extend. |
| FE DAI component | `sound/soc/qcom/qdsp6/q6asm-dai.c:132-163` (templates), `:1285-1292` (direction) | One child node per frontend under `&q6asmdai`. `Q6ASM_DAI_TX` ⇒ capture-only, `Q6ASM_DAI_RX` ⇒ playback-only. |
| AFE DAI component | `sound/soc/qcom/qdsp6/q6dsp-lpass-ports.c:658,668` → `q6afe-dai.c` | `TX_CODEC_DMA_TX_3`, `VA_CODEC_DMA_TX_0..2` all exist; no `&q6afedai` child node needed (only MI2S uses `qcom,sd-lines`). |
| Routing | `sound/soc/qcom/qdsp6/q6routing.c:1112-1160` | Creates `<BE> Audio Mixer` + `MultiMediaN Mixer <BE>` controls for **all** ports unconditionally (live: 1147 controls, incl. `MultiMedia2 Mixer TX_CODEC_DMA_TX_3`, numid 712). |
| WCD938x codec | `sound/soc/codecs/wcd938x.c:3202` (reset), `:3229` (4 supplies), `:184-186` (supply list), `:2643-2690` (AMIC/DMIC/ADC widgets), `:2751-2784` (MIC BIAS), `sound/soc/codecs/wcd-common.c:22-60` (micbias 1.0–2.85 V, default 1.8 V) | Works; **needs all four supplies** (`devm_regulator_bulk_get_enable`) and a `reset-gpios`. Reads the codec variant from CHIPID at runtime, so `qcom,wcd9380-codec` vs `-9385-codec` is cosmetic. |
| WCD938x SoundWire | `sound/soc/codecs/wcd938x-sdw.c:1152` | Binds the two slaves as component master; both slaves must enumerate or the codec component never appears. |
| LPASS macros | `sound/soc/codecs/lpass-{rx,tx,va}-macro.c`; probes `lpass-tx-macro.c:2250-2320`, `lpass-va-macro.c:1543-1600` | Present, but built as modules (below). TX/RX macro require `fsgen` clock from `vamacro`. |
| SoundWire master | `drivers/soundwire/qcom.c:1388-1420` | DAI per port; `SDW Pin0` is playback on an output-only master, capture on an input-only master. |
| LPI pinctrl | `drivers/pinctrl/qcom/pinctrl-sm8250-lpass-lpi.c:40-70` (`sm8250_lpi_pins`), `:72-96` (functions) | Supports gpio0..13 with named functions `swr_tx_clk/swr_tx_data/swr_rx_clk/swr_rx_data/dmic1_clk/dmic1_data/dmic2_clk/dmic2_data/dmic3_clk/dmic3_data`. **Downstream's `gpio14` (tx_swr_data3) does not exist here** and picasso only needs two TX data lanes. |

### 2.2 Config

`configs/linux/picasso.config` already has (lines 130-161): `CONFIG_SND=y`,
`CONFIG_SND_SOC_QDSP6*=y`, `CONFIG_SND_SOC_QCOM=y`, `CONFIG_SND_SOC_QCOM_COMMON=y`,
`CONFIG_SND_SOC_QCOM_SDW=y`, `CONFIG_SND_SOC_SM8250=y`, `CONFIG_SND_SOC_WCD938X=y`,
`CONFIG_SND_SOC_WCD938X_SDW=y`, `CONFIG_SOUNDWIRE=y`, `CONFIG_SOUNDWIRE_QCOM=y`.

Missing (and the reason `/proc/config.gz` shows `=m` — `SND_SOC_SM8250` only *implies*
the macros, `sound/soc/codecs/Kconfig:313-314`, and a module-only rootfs never loads them):

```
CONFIG_SND_SOC_LPASS_MACRO_COMMON=y
CONFIG_SND_SOC_LPASS_RX_MACRO=y
CONFIG_SND_SOC_LPASS_TX_MACRO=y
CONFIG_SND_SOC_LPASS_VA_MACRO=y
CONFIG_PINCTRL_LPASS_LPI=y
CONFIG_PINCTRL_SM8250_LPASS_LPI=y
```

(`COMMON_CLK`/`PM_CLK` are already `y` on arm64; `PINCTRL_SM8250_LPASS_LPI` depends on
`PINCTRL_LPASS_LPI`, `drivers/pinctrl/qcom/Kconfig:132-139`.)

### 2.3 DTS

Board file `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`:

* `&sound` (`:595-625`): `mm1-dai-link` (FE, `<&q6asmdai MSM_FRONTEND_DAI_MULTIMEDIA1>`)
  + `sec-mi2s-dai-link` (BE, `SECONDARY_MI2S_RX` → `&q6routing` → `&speaker_amp`).
  No `audio-routing`, no capture link.
* `&q6afedai` (`:632-642`): only `dai@21` (SECONDARY_MI2S_RX) with `qcom,sd-lines = <0 1>`.
* `&q6asmdai` (`:645-650`): `dai@0..3`, **all `direction = <Q6ASM_DAI_RX>`** ⇒ all
  playback-only.
* No `wcd938*`, `swr*`, `txmacro`/`rxmacro`/`vamacro`, `lpass_tlmm`, no BOB regulator
  (`&apps_rsc` regulators-1 at `:308-354` has LDOs only).

Line numbers are as of the tree used for this document; the board file is shared and is
being edited concurrently (it grew by 27 lines in the audio-unrelated `i2c`/modem area
during this session), so anchor on the block names, not the numbers.
* `sm7250.dtsi` has `q6afe`/`q6afecc`/`q6asm`/`q6adm` under the ADSP APR
  (`:2264-2320`) and `sound: sound {}` at `:2496`; nothing else audio-related.

### 2.4 Live state (<lan-ip>, kernel `7.3.0-rc7-00001-ge1927d6cbd57-dirty #199`)

```
$ cat /proc/asound/cards
 0 [XiaomiRedmiK305]: sm8250 - Xiaomi-Redmi-K30-5G
                      Xiaomi-RedmiK305G-picasso-G7AE
$ cat /proc/asound/pcm
00-00: MultiMedia1 (*) :  : playback 1
$ arecord -l
**** List of CAPTURE Hardware Devices ****
$ cat /proc/asound/devices
  2: [ 0- 0]: digital audio playback
  3: [ 0]   : control
 33:        : timer
```

So: card up, one playback PCM, **zero capture PCMs**, and by construction no codec
component (`wcd938x` needs `swr1`/`swr2` + the macros, which are not in the DTS and
whose drivers are not built in).

---

## 3. Change plan

Order matters. ASoC defers card registration when a link's codec component is missing
(`sound/soc/soc-core.c:1026-1036`), so adding the capture link before the codec can
enumerate **removes the working playback card**. Land (a)+(b) first, prove codec
enumeration, then the card links — or land everything at once and verify the card still
registers (§3d step 1).

### 3.1 (a) DTS — paste-ready additions

All of this goes into `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`
(new patch, §3c). Only existing labels are referenced (`&soc`, `&q6afecc`, `&tlmm`,
`&vreg_s4a_1p8`, `&sound`, `&q6asmdai`, `&q6afedai`, `&q6routing`, `&apps_rsc`);
new labels are declared here.

Each block below was checked by composing it onto the current board DTS and running it
through the kernel's own device-tree toolchain (no build, no files written):

```sh
{ cat <board>.dts; } | cpp -P -nostdinc -I include -I arch/arm64/boot/dts \
    -I arch/arm64/boot/dts/qcom -undef -D__DTS__ -x assembler-with-cpp - \
  | dtc -I dts -O dtb -o /dev/null -
# from build/linux/; no Error/FATAL may appear, only the board's pre-existing
# warnings (13 of them today: 1 unique_unit_address, 10 interrupt_provider,
# 2 graph_child_address)
```

Two DTC rules bite on paste: node **properties must precede subnodes** (so
`audio-routing` goes above the `*-dai-link` nodes, and the BOB node goes after
`qcom,pmic-id`), and a node reference cannot reopen a child node by name — `&sound`
and `&q6asmdai` are therefore *replaced*, not extended.

**(a1) LPASS macros, SoundWire masters, LPI pinmux.** Addresses from
`dt/lito-audio-overlay.dtsi:24,66,166`, LPI from `dt/lito-lpi.dtsi:1-5`, IRQs from
`dt/lito-audio-overlay.dtsi:44,87`. Node structure, clock lists and SWR port parameters
follow `build/linux/arch/arm64/boot/dts/qcom/sm8250.dtsi`: `vamacro` `:2695-2707`,
`rxmacro` `:2709-2727`, `swr1` `:2729-2753`, `txmacro` `:2755-2774`, `swr2` `:2776-2801`,
`lpass_tlmm` `:2803-2938` — the same WCD938x port map as
`dt/lito-audio-overlay.dtsi:49-55, 91-95`, which is why the port tables are reused
verbatim.

```dts
&soc {
	/* Bolero VA macro: always-on capture macro, also the "fsgen" clock source
	 * for RX/TX macros. Address and DMIC rate: vendor lito-audio-overlay.dtsi:166-175. */
	vamacro: codec@3370000 {
		compatible = "qcom,sm8250-lpass-va-macro";
		reg = <0 0x03370000 0 0x1000>;
		clocks = <&q6afecc LPASS_CLK_ID_TX_CORE_MCLK LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_MACRO_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_DCODEC_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>;
		clock-names = "mclk", "macro", "dcodec";
		clock-output-names = "fsgen";
		#clock-cells = <0>;
		#sound-dai-cells = <1>;
		qcom,dmic-sample-rate = <600000>;
		status = "okay";
	};

	/* Bolero RX macro (headphone/earpiece side). Needed even for capture-only:
	 * the WCD9385 component master binds rx+tx slaves (wcd938x.c:3452-3499). */
	rxmacro: rxmacro@3200000 {
		compatible = "qcom,sm8250-lpass-rx-macro";
		reg = <0 0x03200000 0 0x1000>;
		clocks = <&q6afecc LPASS_CLK_ID_TX_CORE_MCLK LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_CLK_ID_TX_CORE_NPL_MCLK LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_MACRO_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_DCODEC_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&vamacro>;
		clock-names = "mclk", "npl", "macro", "dcodec", "fsgen";
		clock-output-names = "mclk";
		#clock-cells = <0>;
		#sound-dai-cells = <1>;
		pinctrl-names = "default";
		pinctrl-0 = <&rx_swr_active>;
		status = "okay";
	};

	/* Bolero TX macro: WCD9385 ADC/DMIC capture over SoundWire plus the LPI
	 * DMIC pads (gpio8/9, gpio12/13). */
	txmacro: txmacro@3220000 {
		compatible = "qcom,sm8250-lpass-tx-macro";
		reg = <0 0x03220000 0 0x1000>;
		clocks = <&q6afecc LPASS_CLK_ID_TX_CORE_MCLK LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_CLK_ID_TX_CORE_NPL_MCLK LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_MACRO_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_DCODEC_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&vamacro>;
		clock-names = "mclk", "npl", "macro", "dcodec", "fsgen";
		clock-output-names = "mclk";
		#clock-cells = <0>;
		#sound-dai-cells = <1>;
		pinctrl-names = "default";
		pinctrl-0 = <&tx_swr_active &dmic2_active &dmic3_active>;
		status = "okay";
	};

	/* RX SoundWire master (swr1). IRQ 155 is the *SM7250* one
	 * (vendor lito-audio-overlay.dtsi:87) - sm8250 uses 298. */
	swr1: soundwire@3210000 {
		compatible = "qcom,soundwire-v1.5.1";
		reg = <0 0x03210000 0 0x2000>;
		interrupts = <GIC_SPI 155 IRQ_TYPE_LEVEL_HIGH>;
		clocks = <&rxmacro>;
		clock-names = "iface";
		label = "RX";
		qcom,din-ports = <0>;
		qcom,dout-ports = <5>;
		qcom,ports-sinterval-low =	/bits/ 8 <0x03 0x1f 0x1f 0x07 0x00>;
		qcom,ports-offset1 =		/bits/ 8 <0x00 0x00 0x0b 0x01 0x00>;
		qcom,ports-offset2 =		/bits/ 8 <0x00 0x00 0x0b 0x00 0x00>;
		qcom,ports-hstart =		/bits/ 8 <0xff 0x03 0xff 0xff 0xff>;
		qcom,ports-hstop =		/bits/ 8 <0xff 0x06 0xff 0xff 0xff>;
		qcom,ports-word-length =	/bits/ 8 <0x01 0x07 0x04 0xff 0xff>;
		qcom,ports-block-pack-mode =	/bits/ 8 <0xff 0x00 0x01 0xff 0xff>;
		qcom,ports-lane-control =	/bits/ 8 <0x01 0x00 0x00 0x00 0x00>;
		qcom,ports-block-group-count =	/bits/ 8 <0xff 0xff 0xff 0xff 0x00>;
		#sound-dai-cells = <1>;
		#address-cells = <2>;
		#size-cells = <0>;
		status = "okay";

		wcd_rx: codec@0,4 {
			compatible = "sdw20217010d00";
			reg = <0 4>;
			qcom,rx-port-mapping = <1 2 3 4 5>;
		};
	};

	/* TX SoundWire master (swr2). Vendor slave address 0x0D/0x01170223
	 * (lito-audio-overlay.dtsi:59-62); mainline enumerates and uses reg<0,N>
	 * as the instance id, so <0 3> is the right form (sm8250-mtp.dts:794). */
	swr2: soundwire@3230000 {
		compatible = "qcom,soundwire-v1.5.1";
		reg = <0 0x03230000 0 0x2000>;
		interrupts = <GIC_SPI 166 IRQ_TYPE_LEVEL_HIGH>;
		clocks = <&txmacro>;
		clock-names = "iface";
		label = "TX";
		qcom,din-ports = <5>;
		qcom,dout-ports = <0>;
		qcom,ports-sinterval-low =	/bits/ 8 <0xff 0x01 0x01 0x03 0x03>;
		qcom,ports-offset1 =		/bits/ 8 <0xff 0x01 0x00 0x02 0x00>;
		qcom,ports-offset2 =		/bits/ 8 <0xff 0x00 0x00 0x00 0x00>;
		qcom,ports-block-pack-mode =	/bits/ 8 <0xff 0xff 0xff 0xff 0xff>;
		qcom,ports-hstart =		/bits/ 8 <0xff 0xff 0xff 0xff 0xff>;
		qcom,ports-hstop =		/bits/ 8 <0xff 0xff 0xff 0xff 0xff>;
		qcom,ports-word-length =	/bits/ 8 <0xff 0xff 0xff 0xff 0xff>;
		qcom,ports-block-group-count =	/bits/ 8 <0xff 0xff 0xff 0xff 0xff>;
		qcom,ports-lane-control =	/bits/ 8 <0xff 0x00 0x01 0x00 0x01>;
		#sound-dai-cells = <1>;
		#address-cells = <2>;
		#size-cells = <0>;
		status = "okay";

		wcd_tx: codec@0,3 {
			compatible = "sdw20217010d00";
			reg = <0 3>;
			qcom,tx-port-mapping = <2 3 4 5>;
		};
	};

	/* LPI (LPASS) pin controller: SWR pads + DMIC pads.
	 * Pinmap/functions: pinctrl-sm8250-lpass-lpi.c (pins gpio0..13).  This
	 * board uses gpio0-5 (SWR), 8/9 (dmic2), 12/13 (dmic3); the vendor's
	 * gpio14 (tx_swr_data3) has no mainline pin and is not needed. */
	lpass_tlmm: pinctrl@33c0000 {
		compatible = "qcom,sm8250-lpass-lpi-pinctrl";
		reg = <0 0x033c0000 0 0x20000>,
		      <0 0x03550000 0 0x10000>;
		gpio-controller;
		#gpio-cells = <2>;
		gpio-ranges = <&lpass_tlmm 0 0 14>;
		clocks = <&q6afecc LPASS_HW_MACRO_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>,
			 <&q6afecc LPASS_HW_DCODEC_VOTE LPASS_CLK_ATTRIBUTE_COUPLE_NO>;
		clock-names = "core", "audio";

		tx_swr_active: tx-swr-active-state {
			clk-pins {
				pins = "gpio0";
				function = "swr_tx_clk";
				drive-strength = <2>;
				slew-rate = <1>;
				bias-disable;
			};
			data-pins {
				pins = "gpio1", "gpio2";
				function = "swr_tx_data";
				drive-strength = <2>;
				slew-rate = <1>;
				bias-bus-hold;
			};
		};

		rx_swr_active: rx-swr-active-state {
			clk-pins {
				pins = "gpio3";
				function = "swr_rx_clk";
				drive-strength = <2>;
				slew-rate = <1>;
				bias-disable;
			};
			data-pins {
				pins = "gpio4", "gpio5";
				function = "swr_rx_data";
				drive-strength = <2>;
				slew-rate = <1>;
				bias-bus-hold;
			};
		};

		/* vendor cdc-dmic23 (gpio8 clk, gpio9 data) */
		dmic2_active: dmic2-active-state {
			clk-pins {
				pins = "gpio8";
				function = "dmic2_clk";
				drive-strength = <8>;
				output-high;
			};
			data-pins {
				pins = "gpio9";
				function = "dmic2_data";
				drive-strength = <8>;
				bias-pull-down;
			};
		};

		/* vendor cdc-dmic45 (gpio12 clk, gpio13 data) */
		dmic3_active: dmic3-active-state {
			clk-pins {
				pins = "gpio12";
				function = "dmic3_clk";
				drive-strength = <8>;
				output-high;
			};
			data-pins {
				pins = "gpio13";
				function = "dmic3_data";
				drive-strength = <8>;
				bias-pull-down;
			};
		};
	};
};
```

**(a2) WCD9385 node + BOB rail.** Codec values from
`dt/lito-audio-overlay.dtsi:177-222` (structure), `dt/picasso-audio-overlay.dtsi:129-135`
(micbias) and `:58-59` (jack switches); BOB from `dt/lito-regulators.dtsi:775-791`.
MBHC thresholds mirror upstream `sm8250-mtp.dts:41-43` and the driver defaults
(`wcd938x.c:3246-3250`: `hs_thr = 1700`, `hph_thr = 50`, 8 buttons).

```dts
/* add next to the existing / { ... } children (e.g. below speaker_amp's node) */
	wcd9385: audio-codec {
		compatible = "qcom,wcd9385-codec";

		/* vendor: msm_cdc_pinctrl@57, active state output-high.
		 * Reset is active-low (wcd938x.c:3251-3262 sets 1 then 0 with
		 * ACTIVE_LOW semantics). WEST tile: see risk 4.1. */
		reset-gpios = <&tlmm 57 GPIO_ACTIVE_LOW>;

		vdd-buck-supply = <&vreg_s4a_1p8>;
		vdd-rxtx-supply = <&vreg_s4a_1p8>;
		vdd-io-supply = <&vreg_s4a_1p8>;
		vdd-mic-bias-supply = <&vreg_bob>;

		qcom,micbias1-microvolt = <2750000>;
		qcom,micbias2-microvolt = <2750000>;
		qcom,micbias3-microvolt = <2750000>;
		qcom,micbias4-microvolt = <2750000>;

		qcom,mbhc-headset-vthreshold-microvolt = <1700000>;
		qcom,mbhc-headphone-vthreshold-microvolt = <50000>;
		qcom,mbhc-buttons-vthreshold-microvolt = <75000 150000 237000 500000 500000 500000 500000 500000>;
		qcom,hphl-jack-type-normally-closed;
		qcom,ground-jack-type-normally-closed;

		qcom,rx-device = <&wcd_rx>;
		qcom,tx-device = <&wcd_tx>;

		#sound-dai-cells = <1>;
	};
```

```dts
/* Insert as the FIRST CHILD of the existing &apps_rsc { regulators-1 { ... } }
 * block, i.e. immediately after `qcom,pmic-id = "c";`
 * (build/linux/.../sm7250-xiaomi-picasso.dts:308-311).  DTC requires node
 * properties to precede subnodes, so pasting it right after the
 * `regulators-1 {` line fails with "Properties must precede subnodes".
 * No vdd-bob-supply: this tree has no vph_pwr label and the other rails here
 * omit their parents (qcom-rpmh-regulator treats the parent as optional). */
		vreg_bob: bob {
			regulator-name = "vreg_bob";
			regulator-min-microvolt = <3008000>;
			regulator-max-microvolt = <3960000>;
			regulator-initial-mode = <RPMH_REGULATOR_MODE_AUTO>;
		};
```

**(a3) Card: capture frontend, capture backend, routes.**
Replace the existing `&sound` block (`sm7250-xiaomi-picasso.dts:568-598`) with this;
`mm1-dai-link`, `sec-mi2s-dai-link` and `pinctrl-0` are unchanged, the rest is new.
Routing strings are the intersection of mainline DAPM names
(`wcd938x.c:2643-2691, 2751-2787`; `lpass-tx-macro.c:1441-1457, 1700-1711`) and the
vendor's actual wiring (`dt/picasso-audio-overlay.dtsi:22-57`).

```dts
&sound {
	compatible = "qcom,sm8250-sndcard";
	model = "Xiaomi-Redmi-K30-5G";
	pinctrl-names = "default";
	pinctrl-0 = <&sec_mi2s_default>;

	audio-routing =
		/* analog mics (vendor picasso-audio-overlay.dtsi:22-29) */
		"AMIC1", "MIC BIAS1",
		"MIC BIAS1", "Handset Mic",
		"AMIC2", "MIC BIAS2",
		"MIC BIAS2", "Headset Mic",
		"AMIC5", "MIC BIAS3",
		"MIC BIAS3", "Secondary Mic",
		/* WCD9385 -> Bolero TX macro SoundWire ADC lanes */
		"TX SWR_ADC0", "ADC1_OUTPUT",
		"TX SWR_ADC1", "ADC2_OUTPUT",
		"TX SWR_ADC2", "ADC3_OUTPUT",
		"TX SWR_ADC3", "ADC4_OUTPUT",
		/* LPI DMIC pads (vendor: TX DMICn <- MIC BIASn) */
		"TX DMIC2", "MIC BIAS1",
		"TX DMIC3", "MIC BIAS1",
		"TX DMIC4", "MIC BIAS4",
		"TX DMIC5", "MIC BIAS4";

	mm1-dai-link {
		link-name = "MultiMedia1";

		cpu {
			sound-dai = <&q6asmdai MSM_FRONTEND_DAI_MULTIMEDIA1>;
		};
	};

	/* capture frontend: PCM device 1 (links are numbered in order) */
	mm2-dai-link {
		link-name = "MultiMedia2";

		cpu {
			sound-dai = <&q6asmdai MSM_FRONTEND_DAI_MULTIMEDIA2>;
		};
	};

	sec-mi2s-dai-link {
		link-name = "Secondary MI2S Playback";

		cpu {
			sound-dai = <&q6afedai SECONDARY_MI2S_RX>;
		};

		platform {
			sound-dai = <&q6routing>;
		};

		codec {
			sound-dai = <&speaker_amp>;
		};
	};

	/* capture backend: AFE TX_CODEC_DMA_TX_3 = TX macro (kona.c:183,685) */
	wcd-capture-dai-link {
		link-name = "WCD Capture";

		cpu {
			sound-dai = <&q6afedai TX_CODEC_DMA_TX_3>;
		};

		codec {
			sound-dai = <&wcd9385 1>, <&swr2 0>, <&txmacro 0>;
		};

		platform {
			sound-dai = <&q6routing>;
		};
	};
};
```

**(a4) Make the frontend a capture DAI.** Replace the board's `&q6asmdai` block
(`sm7250-xiaomi-picasso.dts:615-620`). `dai@1` (= `MultiMedia2`) becomes TX
(capture-only); the unused `dai@2`, `dai@3` stay playback so a later playback
frontend does not need them flipped.

```dts
/* q6asm-dai needs one child per frontend; Q6ASM_DAI_RX removes the capture
 * stream, Q6ASM_DAI_TX removes playback (q6asm-dai.c:1289-1292). */
&q6asmdai {
	dai@0 { reg = <0x0>; direction = <Q6ASM_DAI_RX>; };
	dai@1 { reg = <0x1>; direction = <Q6ASM_DAI_TX>; };
	dai@2 { reg = <0x2>; direction = <Q6ASM_DAI_RX>; };
	dai@3 { reg = <0x3>; direction = <Q6ASM_DAI_RX>; };
};
```

### 3.2 (b) `configs/linux/picasso.config`

```
CONFIG_SND_SOC_LPASS_MACRO_COMMON=y
CONFIG_SND_SOC_LPASS_RX_MACRO=y
CONFIG_SND_SOC_LPASS_TX_MACRO=y
CONFIG_SND_SOC_LPASS_VA_MACRO=y
CONFIG_PINCTRL_LPASS_LPI=y
CONFIG_PINCTRL_SM8250_LPASS_LPI=y
```

No other config change: `SND_SOC_WCD938X{,_SDW}=y`, `SOUNDWIRE_QCOM=y`,
`SND_SOC_QCOM_SDW=y` are already there (`picasso.config:140,156-157,161`).
`SND_SOC_LPASS_WSA_MACRO` stays off (WSA881x unused on this board,
`dt/picasso-audio-overlay.dtsi:95-98`).

### 3.3 (c) Patches

1. **`patches/linux/picasso/0016-arm64-dts-qcom-sm7250-picasso-audio-capture.patch`**
   — the board-DTS change of §3.1: BOB rail, `vamacro`/`rxmacro`/`txmacro`,
   `swr1`/`swr2` (SM7250 IRQs), `lpass_tlmm` + SWR/DMIC pin states, `wcd9385` codec
   node, `audio-routing`, `MultiMedia2` FE link, `WCD Capture` BE link, and
   `q6asmdai` `dai@1` switched to `Q6ASM_DAI_TX`. No driver code.
2. **`patches/linux/picasso/0017-soc-qcom-picasso-pinmux-hold-wcd9385-reset.patch`**
   — required for the codec reset to be real. `drivers/soc/qcom/picasso-pinmux.c`
   already writes WEST/EAST pads directly because `pinctrl-msm` cannot (its own header
   comment says the gpiod requests are no-ops; `patches/linux/picasso/0005-...` makes the
   writes silently skipped). Add to `picasso_pinmux_init()` right after the TFA9874
   block:

   ```c
	/* WCD9385 reset: GPIO57, active low. The codec needs a low pulse after
	 * power-up; gpiod writes never reach the WEST tile through pinctrl-msm,
	 * so the pulse is produced here (the codec's own toggle in
	 * wcd938x_reset() is a no-op on this board). */
	picasso_mux(57, TLMM_TILE_WEST, 0, 2, 0, true, 0);
	if (do_write) {
		mdelay(1);
		picasso_mux(57, TLMM_TILE_WEST, 0, 2, 0, true, 1);
	}
   ```

   Effect: the codec is out of reset with a genuine 1 ms low pulse before
   `wcd938x` probes, so the SoundWire slave answers enumeration. Without it the reset
   line keeps whatever the boot chain left (unknown) and the probe is a coin flip.
3. **`patches/linux/picasso/0018-sound-soc-wcd938x-optional-reset.patch`** —
   *only if* (4.1) shows the pin is unusable: `devm_gpiod_get()` →
   `devm_gpiod_get_optional()` in `wcd938x.c:3202` and skip `wcd938x_reset()` when
   NULL. Keep this as a fallback; it loses the post-init reset the driver wants.

No change to `sound/soc/qcom/sm8250.c`: links are fully DTS-driven (`:211`), DPCM
streams are inferred from the FE DAI caps, and `sm8250_be_hw_params_fixup` (`:58-96`)
already forces 48 kHz/S16_LE/2ch for capture BEs — which is what the mics use.

### 3.4 (d) Runtime verification chain

Prefix for everything: `ssh -i ~/.ssh/picasso_key root@$IP`. `$IP` = <lan-ip>
(<lan-ip>, <lan-ip>, 10.77.71.2 as fallbacks).

1. **Nodes probed / codec enumerated** (no state change):

   ```sh
   dmesg | grep -iE 'wcd938x|soundwire|swr|tx.?macro|rx.?macro|va.?macro|lpass'
   ls /sys/bus/soundwire/devices
   ls /sys/bus/platform/drivers/wcd938x-codec/ 2>/dev/null
   ```

   Expected: the macros and both masters probe without `-EPROBE_DEFER`/`-EINVAL`
   lines; `/sys/bus/soundwire/devices` contains **two** entries of the form
   `sdw:<link>:0217:010d:00:3` and `...:4` (name format from
   `drivers/soundwire/` device naming: `sdw:<link>:<mfg>:<part>:<class>:<unique>`).
   If instead `dmesg` shows
   `ASoC: Component ... not found for link WCD Capture`, the codec did not bind and
   the card is deferred (see §4.1).

2. **Card still registers, now with a capture PCM**:

   ```sh
   cat /proc/asound/cards
   cat /proc/asound/pcm
   arecord -l
   ls -l /dev/snd/
   ```

   Expected `/proc/asound/pcm`:

   ```
   00-00: MultiMedia1 (*) :  : playback 1
   01-00: MultiMedia2 (*) :  : capture 1
   ```

   Expected `arecord -l`:

   ```
   **** List of CAPTURE Hardware Devices ****
   card 0: XiaomiRedmiK305 [Xiaomi-Redmi-K30-5G], device 1: MultiMedia2 (*)
     Subdevices: 1/1
     Subdevice #0: subdevice #0
   ```

   `/dev/snd/pcmC0D1c` must exist (see §3.5 — Denial's init creates ALSA nodes by
   hand).

3. **Handset mic (AMIC1 → ADC1 → TX SWR_ADC0 → DEC0)**:

   ```sh
   amixer -c 0 cset name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 1
   amixer -c 0 cset name='TX DEC0 MUX' SWR_MIC
   amixer -c 0 cset name='TX SMIC MUX0' ADC0
   amixer -c 0 cset name='TX_AIF1_CAP Mixer DEC0' 1
   amixer -c 0 cset name='TX_DEC0 Volume' 84
   arecord -D hw:0,1 -f S16_LE -r 48000 -c 2 -d 5 /tmp/mic.wav
   sox /tmp/mic.wav -n stat 2>&1 | grep -E 'Maximum amplitude|RMS +amplitude'
   ```

   Success: `arecord` exits 0, the file is 5 s × 48 kHz × 2 ch × 2 B ≈ 960 kB, and
   `RMS amplitude` is **> 0.005** while someone talks near the bottom mic
   (silence/DC gives < 0.001). Control names are live-confirmed on this kernel:
   `MultiMedia2 Mixer TX_CODEC_DMA_TX_3` = numid 712 (`amixer -c 0 controls`).

4. **Headset mic (AMIC2 → ADC2 → TX SWR_ADC1 → DEC0)**, 4-pole headset plugged in:

   ```sh
   amixer -c 0 cset name='TX SMIC MUX0' ADC1
   arecord -D hw:0,1 -f S16_LE -r 48000 -c 2 -d 5 /tmp/hs.wav
   sox /tmp/hs.wav -n stat 2>&1 | grep -E 'Maximum amplitude|RMS +amplitude'
   ```

   Success: same criteria; additionally `cat /proc/asound/card0/input*`/`dmesg` shows
   the jack event (`Headset Jack`) and the MBHC path powers MIC BIAS2. If the mic is
   silent but the jack is detected, check `amixer -c 0 cget name='MIC BIAS2'`.

5. **DMIC pads (pads on gpio8/9 and gpio12/13)**:

   ```sh
   amixer -c 0 cset name='TX DEC2 MUX' MSM_DMIC
   amixer -c 0 cset name='TX DMIC MUX2' DMIC2
   amixer -c 0 cset name='TX_AIF1_CAP Mixer DEC2' 1
   amixer -c 0 cset name='TX_DEC2 Volume' 84
   arecord -D hw:0,1 -f S16_LE -r 48000 -c 2 -d 5 /tmp/dmic.wav
   sox /tmp/dmic.wav -n stat 2>&1 | grep -E 'Maximum amplitude|RMS +amplitude'
   ```

   Success: RMS > 0.005 with sound near the top/bottom DMIC. Repeat with
   `TX DEC4 MUX`/`TX DMIC MUX4` = `MSM_DMIC`/`DMIC4` for the second pad. If both are
   silent, the pads belong to the VA macro → §4.2.

6. **Regression check** (the card must not lose playback): `speaker-test -D hw:0,0 -c 2
   -t sine -l 1` or the repo's `tools/tone`, and `cat /proc/asound/pcm` still shows
   `00-00 ... playback 1`.

### 3.5 Denial userspace: what `tools/init.c` must change (report only, not edited)

`tools/init.c` hand-creates the ALSA device nodes because devtmpfs does not populate
`/dev/snd` without udev:

* `audio_selftest()` (`tools/init.c:239-278`) has
  `static const char *const names[] = { "controlC0", "pcmC0D0p", "timer" };`
  → add **`"pcmC0D1c"`** (and `"pcmC0D1p"` if a second playback PCM ever appears).
  Without it `/dev/snd/pcmC0D1c` never appears even though `/sys/class/sound/` has it,
  and `arecord`/PipeWire fail with `No such file or directory` (exactly today's
  `arecord -D hw:0,1` error).
* The UCM2 drop-in written at `tools/init.c:1272-1306` defines only
  `SectionDevice."Speaker"` with `PlaybackPCM "hw:${CardId},0"`. Add a capture device
  and the capture enable/disable csets in `ucm_hifi`:

  ```
  	SectionDevice."Mic" {
  		Comment "Internal + headset microphone"
  		EnableSequence [
  			cset "name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 1"
  			cset "name='TX DEC0 MUX' SWR_MIC"
  			cset "name='TX SMIC MUX0' ADC0"
  			cset "name='TX_AIF1_CAP Mixer DEC0' 1"
  			cset "name='TX_DEC0 Volume' 84"
  		]
  		DisableSequence [
  			cset "name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 0"
  		]
  		Value {
  			CapturePriority 100
  			CapturePCM "hw:${CardId},1"
  			CaptureChannelMap 0 1
  		}
  	}
  ```

  A headset variant is a second `SectionDevice` that only differs in
  `cset "name='TX SMIC MUX0' ADC1"` (ALSA UCM2 selects between them with
  `JackControl`, e.g. `"Headset Jack"`). Files worth copying verbatim as includes
  exist on the device already: `/usr/share/alsa/ucm2/codecs/qcom-lpass/tx-macro/
  {DMIC0,HeadphoneMic,SoundwireMic0}*Seq.conf` (read live, contents quoted in §5).
* WirePlumber rule at `tools/init.c:1257-1271` matches only
  `{ node.name = "~alsa_output.*" }`. Add the capture twin:

  ```
       {
         matches = [
           { node.name = "~alsa_input.*" }
         ]
         actions = {
           update-props = {
             audio.format = "S16LE"
             audio.rate = 48000
             audio.channels = 2
           }
         }
       }
  ```

  (The ADSP only accepts 48 kHz on these ports, so leaving capture nodes at a
  PipeWire-chosen 44.1 kHz makes every open fail.)

---

## 4. Unknowns / risks, each with the cheapest resolving experiment

### 4.1 WEST-tile pins: does the codec reset actually toggle? (highest risk)

`gpio57` is WEST (`pinctrl-sm7250.c:1556`), and this port only maps the `south` window
(`0002-patch`, `0005-patch` makes writes to unmapped tiles no-ops). `picasso-pinmux.c`
proves the hardware registers *are* reachable when the whole 16 MiB TLMM is ioremapped,
and explicitly notes that gpiod level writes for such pads do nothing.

Consequence: with §3.1(a2) alone the codec may never leave reset, the SoundWire slaves
never enumerate, `wcd9385` never binds, and — because of `soc-core.c:1036` —
**the working playback card disappears**.

Cheapest experiment: land §3.1 + §3.3-1/2 together and read
`dmesg | grep -iE 'wcd|sdw|swr'` plus `ls /sys/bus/soundwire/devices`. If the slaves
don't appear: (i) boot with `picasso_pinmux.mode=read` and add a temporary
`picasso_mux(57, ...)` print to learn the pre-boot level of pin 57 (the function always
prints `ctl/io` before deciding to write); (ii) if the pulse still does not take,
fall back to §3.3-3 (optional reset). Do not land the card link without this check —
keep the `wcd-capture-dai-link` in a separate commit from the codec/macro nodes.

### 4.2 DMIC pads: TX macro or VA macro?

Vendor enables `cdc-dmic23`/`cdc-dmic45` (gpio8/9, gpio12/13) and its machine driver
flips those pad states for *both* `TX DMICn` and `VA DMICn` paths (`kona.c:5477-5556`);
the vendor's DMIC pads are documented only as LPI pins, and which macro drives them is
decided in LPASS. The plan assumes TX macro (vendor recording path, `TX_CDC_DMA_TX_3`).

Cheapest experiment: step 5 of §3.4 (two cset runs on the TX macro). If both pads are
silent, add the VA link instead — same pin states, different backend:

```dts
	va-capture-dai-link {
		link-name = "VA Capture";
		cpu { sound-dai = <&q6afedai VA_CODEC_DMA_TX_0>; };
		codec { sound-dai = <&vamacro 0>; };
		platform { sound-dai = <&q6routing>; };
	};
```

with `audio-routing` `"VA DMIC2", "MIC BIAS1"` … and csets
`'VM DEC0 MUX' VA_DMIC` / `'VA DMIC MUX0' DMIC0` (see
`/usr/share/alsa/ucm2/codecs/qcom-lpass/va-macro/DMIC0EnableSeq.conf`), plus
`vdd-micb-supply = <&vreg_bob>` on `vamacro` (binding requires nothing but the driver's
`vdd-micb` DAPM widget exists, `lpass-va-macro.c:1142`). In that variant move the two
DMIC pin states from `txmacro`'s `pinctrl-0` to `vamacro`'s, so the pads are muxed even
when the TX macro is not enabled.

### 4.3 SM7250 macro register layout vs the `sm8250` compatible

Upstream has no `qcom,sm7250-lpass-*-macro`; we bind the sm8250 data
(`lpass-tx-macro.c:2494`, `lpass-va-macro.c:1774`, `lpass_ver_9`). lito and sm8250 are
the same LPASS generation and the vendor uses one Bolero driver for both
(`ta/asoc/codecs/bolero`), so this is a strong inference, not proof.

Cheapest experiment: after the macros probe, `dmesg | grep -i macro` must be free of
regmap/`-EINVAL` errors, and step 3 of §3.4 must produce non-zero RMS. A register-level
sanity check without a build: `devmem`-free — read
`/sys/kernel/debug/regmap/*tx*/registers` and confirm the reset defaults match
`tx_defaults` (`lpass-tx-macro.c:296-320`).

### 4.4 SoundWire port parameters copied from sm8250

`qcom,ports-*` tables in §3.1(a1) come from `sm8250.dtsi`, which was tuned for
WCD9380 on SM8250. The vendor's SM7250 tables are expressed differently
(`qcom,swr-port-mapping`, `dt/lito-audio-overlay.dtsi:49-55, 91-95`) but describe the
same codec port map, so the values should hold; a mismatch shows up as
`soundwire ... port ... not found`/no capture data.

Cheapest experiment: capture with §3.4 step 3; if the DAI runs but the buffer is all
zeros, dump `cat /sys/kernel/debug/soundwire/*/...` (master/slave port status) before
touching the tables.

### 4.5 BOB rail voltage/enable

`wcd938x.c` only *enables* its four supplies; it never sets a voltage. BOB keeps the
level the boot chain left (vendor init 3.008 V, `dt/lito-regulators.dtsi:782`). Micbias
2750 mV < 3.008 V so headroom is fine, but if BOB is *off* at handover the micbiases
never come up and analog mics are silent with a clean-looking capture stream.

Cheapest experiment: `cat /sys/kernel/debug/regulator/regulator_summary | grep -A3 bob`
after boot — expect `vreg_bob` enabled, `use_count ≥ 1`, 3.0–3.3 V. Cross-check
`amixer -c 0 cget name='MIC BIAS1'` toggles when the capture path is up.

### 4.6 Nobody re-muxes the DMIC pads per stream (mainline gap)

Upstream vamacro applies its DMIC pin state once at probe
(`qrb5165-rb5.dts:1458-1463`: `pinctrl-0 = <&dmic01_active>`), while the vendor machine
driver flips pads on DAPM events (`kona.c:5529-5551`). We follow upstream (pin state on
`txmacro`), so the pads are muxed permanently and only the macro's DMIC clock is gated.
Cost: a few hundred µA when idle; no functional risk.

Cheapest experiment: none needed; if idle current matters later, move the two states to
a `pinctrl-1 = "sleep"` and add a DAPM hook in the machine driver.

### 4.7 Functional gap: nobody knows which physical mic is on which pin

Downstream's routing names (`Handset Mic` = AMIC1, `Headset Mic` = AMIC2,
`Secondary Mic` = AMIC5) are the only mapping we have; there is no `mixer_paths*.xml`
on the host and `/vendor` on the phone is an empty mount, so the Android
`handset-mic`/`dmic` paths could not be read. Which DMIC pad (2 vs 3) carries a
populated mic, and whether AMIC5 is fitted, is unknown.

Cheapest experiment: §3.4 steps 3–5, one cset set per source, RMS per file.

---

## 5. Live observations

Host/phone, all read-only. Phone: `<lan-ip>`, kernel
`7.3.0-rc7-00001-ge1927d6cbd57-dirty #199 SMP PREEMPT Tue Nov 17 18:45:54 +07 2026
aarch64`; reachable at the start, unreachable by the end of the session (the known ADSP
user-PD DOG reset). Command outputs below are the record.

```
$ cat /proc/asound/cards
 0 [XiaomiRedmiK305]: sm8250 - Xiaomi-Redmi-K30-5G
                      Xiaomi-RedmiK305G-picasso-G7AE

$ cat /proc/asound/pcm
00-00: MultiMedia1 (*) :  : playback 1

$ arecord -l
**** List of CAPTURE Hardware Devices ****

$ cat /proc/asound/devices
  2: [ 0- 0]: digital audio playback
  3: [ 0]   : control
 33:        : timer

$ ls /proc/asound/card0/
id  pcm0p

$ amixer -c 0 controls | wc -l
1147

$ amixer -c 0 controls | grep -E "MultiMedia2 Mixer (TX_CODEC_DMA_TX_3|VA_CODEC_DMA_TX_0)"
numid=712,iface=MIXER,name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3'
numid=706,iface=MIXER,name='MultiMedia2 Mixer VA_CODEC_DMA_TX_0'

$ amixer -c 0 controls | grep -ciE "CODEC_DMA"
176

$ arecord -D hw:0,1 -f S16_LE -r 48000 -c 2 -d 1 /dev/null
arecord: main:850: audio open error: No such file or directory      # no pcmC0D1c at all

$ ls -l /dev/snd
crw-rw----+ 1 root audio 116,  3 controlC0
crw-rw----+ 1 root audio 116,  2 pcmC0D0p
crw-rw----+ 1 root audio 116, 33 timer
# by hand-mknod'ed by tools/init.c; the class has no udev to populate it

$ ls /sys/class/sound/
card0  controlC0  pcmC0D0p  timer

$ zcat /proc/config.gz | grep -E "SND_SOC_LPASS|WCD938|SOUNDWIRE|LPASS_LPI"
CONFIG_REGMAP_SOUNDWIRE=y
CONFIG_PINCTRL_LPASS_LPI=m
CONFIG_PINCTRL_SM8250_LPASS_LPI=m
CONFIG_SND_SOC_QCOM=y
CONFIG_SND_SOC_QCOM_COMMON=y
CONFIG_SND_SOC_QCOM_SDW=y
CONFIG_SND_SOC_SM8250=y
CONFIG_SND_SOC_WCD938X=y
CONFIG_SND_SOC_WCD938X_SDW=y
CONFIG_SND_SOC_LPASS_MACRO_COMMON=m
CONFIG_SND_SOC_LPASS_WSA_MACRO=m
CONFIG_SND_SOC_LPASS_VA_MACRO=m
CONFIG_SND_SOC_LPASS_RX_MACRO=m
CONFIG_SND_SOC_LPASS_TX_MACRO=m
CONFIG_SOUNDWIRE=y
CONFIG_SOUNDWIRE_QCOM=y

$ ls /lib/modules ; cat /proc/modules | wc -l
0                          # no modules installed anywhere: every '=m' above is dead

$ dmesg | grep -iE "wcd|soundwire|swr|asoc|va-macro|tx-macro|lpass"
[   10.496823] q6asm-dai open: comp 3000000.remoteproc:glink-edge:apr:service@7:dais pdata ... stream 0
   (only q6asm open lines: no wcd/sdw/swr/macro probe lines at all)

$ dmesg | grep -i "asound"
[   16.240832] asound:  0 [XiaomiRedmiK305]: sm8250 - Xiaomi-Redmi-K30-5G
                                     Xiaomi-RedmiK305G-picasso-G7AE

$ dmesg | grep -i adsp | head -2
[   16.239011] state: adsp = running
[   36.253601] state: adsp = running

$ cat /proc/cmdline
console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0
systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10
```

The TLMM/pinmux side, from the same phone (relevant to risk 4.1):

```
$ cat /sys/kernel/debug/pinctrl/f100000.pinctrl/pinmux-pins | grep -E "pin (49|50|57|58|70|71|72|74)"
pin 49 (GPIO_49): device 988000.i2c function qup12 group gpio49
pin 50 (GPIO_50): device 988000.i2c function qup12 group gpio50
pin 57 (GPIO_57): UNCLAIMED
pin 58 (GPIO_58): GPIO f100000.pinctrl:604          # TFA9874 reset, claimed
pin 70 (GPIO_70): device sound function sec_mi2s group gpio70
pin 71 (GPIO_71): device sound function sec_mi2s group gpio71
pin 72 (GPIO_72): device sound function sec_mi2s group gpio72
pin 74 (GPIO_74): device sound function sec_mi2s group gpio74
```

Note the software view says "GPIO"/"sec_mi2s" for WEST pins whose hardware writes are
dropped by `0005-pinctrl-msm-skip-unmapped-tiles.patch`; the pads keep the boot chain's
mux (`drivers/soc/qcom/picasso-pinmux.c` comment and `STATUS.md` "TLMM — the wall").

UCM2 material already on the device (read live, to be reused by Denial):

```
$ ls /usr/share/alsa/ucm2/Qualcomm/sm8250
HDMI.conf  HiFi.conf  Qualcomm-RB5-WSA8815-Speakers-DMIC0.conf

$ cat /usr/share/alsa/ucm2/Qualcomm/sm8250/HiFi.conf        # excerpt
SectionDevice."Mic" {
	Comment "Mic"
	Include.lpassvae.File "/codecs/qcom-lpass/va-macro/DMIC0EnableSeq.conf"
	Include.lpassvad.File "/codecs/qcom-lpass/va-macro/DMIC0DisableSeq.conf"
	Value { CapturePriority 100  CapturePCM "hw:${CardId},2" }
}

$ cat /usr/share/alsa/ucm2/codecs/qcom-lpass/tx-macro/DMIC0EnableSeq.conf
	cset "name='TX DEC0 MUX' MSM_DMIC"
	cset "name='TX DMIC MUX0' DMIC0"
	cset "name='TX DMIC MUX1' DMIC1"
	cset "name='TX_AIF1_CAP Mixer DEC0' 1"
	cset "name='TX_AIF1_CAP Mixer DEC1' 1"
	cset "name='TX_DEC0 Volume' 100"
	cset "name='TX_DEC1 Volume' 100"

$ cat /usr/share/alsa/ucm2/codecs/qcom-lpass/tx-macro/HeadphoneMicEnableSeq.conf
	cset "name='TX DEC0 MUX' SWR_MIC"
	cset "name='TX SMIC MUX0' ADC1"
	cset "name='TX_AIF1_CAP Mixer DEC0' 1"
	cset "name='TX_DEC0 Volume' 110"
```

`/vendor` on the phone is an empty mount (`ls -la /vendor` → only `.`/`..`), so no
Android `mixer_paths*.xml` was readable; `mount` shows `/dev/sda36` as `/`,
`/dev/sde47` `/mnt/dsp_fw`, `/dev/sde48` `/mnt/modem_fw`, `/dev/sde33`
`/usr/lib/firmware/qca`.
