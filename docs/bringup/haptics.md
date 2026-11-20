# Haptics / vibrator on `picasso` (Redmi K30 5G, SM7250) — mainline bring-up

Scope: what physically drives the vibration motor, which mainline driver fits, the DT node,
the Kconfig lines, and the on-device proof. Downstream ground truth is the Xiaomi 4.19 vendor
tree that runs on this exact phone; mainline is `build/linux` (7.3-rc7).

## Verdict

* The actuator is driven **by the PM7250B "VIB LDO"**, not by an external I2C haptics IC:
  SPMI slave **SID 3** (`PM7250B_SID1`), peripheral **0x5300**, vendor setting **3.000 V**, overdrive
  disabled (`~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/pm7250b.dtsi:450-461`).
* **No** `aw8697`/`aw8624`/`drv260x`/`cs40l`/`qcom,haptics` node exists anywhere in the picasso
  include chain, and the picasso vendor defconfig enables exactly one vibrator driver,
  `CONFIG_LEDS_QPNP_VIBRATOR_LDO=y` (`.../arch/arm64/configs/vendor/picasso_user_defconfig:514`).
  The `aw8624`/`aw8697`-style nodes the task mentions belong to **other** SM7250 boards
  (`monet-sm7250.dtsi:43`, `vangogh-sm7250.dtsi:49`, kona) — not to picasso.
* Mainline **already has the matching driver**: `drivers/input/misc/pm8xxx-vibrator.c` with
  `pmi632_regs`, which speaks the *same* register protocol (12-bit mV at 0x40/0x41, enable bit
  0x46.7). The mainline DT binding even lists `qcom,pm7250b-vib`
  (`Documentation/devicetree/bindings/input/qcom,pm8xxx-vib.yaml:21-26`).
* **No kernel code change is needed** — only one DT node plus one Kconfig line, both `=y`
  (never `=m`: this rootfs has no module tree at all). As of this writing the *working tree*
  already carries both (uncommitted): the node at `build/linux/arch/arm64/boot/dts/qcom/
  sm7250-xiaomi-picasso.dts:670-684` and `CONFIG_INPUT_PM8XXX_VIBRATOR=y` appended to
  `configs/linux/picasso.config`. Nothing has been booted with them yet (§5), and no patch in
  `patches/linux/picasso/` contains them (they will be folded into patch 0002 on export).
* The interface mainline must provide is **evdev force feedback (`EV_FF` / `FF_RUMBLE`)**, which
  is exactly what `pm8xxx-vibrator` registers and exactly what Denial Mobile's `hapticd` +
  `droidloom-devices` bridge consume. **No** `/sys/class/leds/*:vibrator` is needed or wanted.

Note on the task title ("LRA"): a plain PMIC LDO is a DC supply and cannot drive a linear
resonant actuator at resonance. Every downstream artifact on this phone is consistent with a
**DC (ERM/coin) motor**; an LRA would require a smart driver IC that simply is not present in
the downstream device tree. See risk R5 for the 30-second check that settles it.

## 1. Hardware ground truth

| Item | Downstream source (`file:line`) | Value |
|---|---|---|
| Bus / controller | `pm7250b.dtsi:450-452` | SPMI, PM7250B second slave: node `qcom,pm7250b@3`, `reg = <0x3 SPMI_USID>`, i.e. **SID 3**. Board defines `PM7250B_SID1 = 3` (`sm7250-xiaomi-picasso.dts:22-23`). |
| Peripheral address | `pm7250b.dtsi:458` | `reg = <0x5300 0x100>` → base **0x5300** (`chip->base = (uint16_t)base`, vendor driver `:450,:472`). |
| Compatible (vendor) | `pm7250b.dtsi:457` | `"qcom,qpnp-vibrator-ldo"` (LED-class driver, `drivers/leds/leds-qpnp-vibrator-ldo.c:536,543`). |
| Output voltage setting | `pm7250b.dtsi:459`; `leds-qpnp-vibrator-ldo.c:381-388` | `qcom,vib-ldo-volt-uv = <3000000>` → **3.000 V** (register value is mV; `vlevel = roundup(uV, 8000)/1000`, `:89`). |
| Overdrive | `pm7250b.dtsi:460`; `:389-390` | `qcom,disable-overdrive` present → **no 2×/6 V boost pulse**; steady 3.0 V play. |
| Voltage set registers | `leds-qpnp-vibrator-ldo.c:21,89-93` | `VSET_LB = 0x40`: 2-byte little-endian write, low byte at 0x40, bits[3:0] at 0x41; unit **mV**, 12-bit. |
| Enable register / polarity | `:23-24,118-124` | `EN_CTL = 0x46`, `BIT(7)`; **active high**, no GPIO, no pinmux, no reset line. |
| Ready status | `:18-19,62-70` | `STATUS1 = 0x08`, `BIT(7) = VREG_READY`; vendor polls (100 µs step, 1 ms timeout) after every enable and after a voltage change while enabled. |
| Voltage range / step | `:27-29` | VMIN **1504 mV**, VMAX **3544 mV**, step **8 mV**. |
| IRQ | `pm7250b.dtsi:456-461` (no `interrupts`) | **None**; the driver is poll/timer driven, no IRQ line. |
| GPIO (reset/enable/int) | node has no GPIO property; `:456-461` | **None.** |
| Regulators / boost | driver includes no `regulator/consumer.h` (vendor `:1-15`), no `regulator_get` call; no `haptics_boost`/`hap_boost` node in `lito*`/`picasso*` | **None** — the LDO is internal to PM7250B. |
| Clocks | `:456-461` | **None.** |
| Firmware | `:456-461` | **None.** |
| Downstream userland surface | `leds-qpnp-vibrator-ldo.c:370-374,484` | LED class device named `vibrator` with extra attrs `state`, `duration`, `activate`, `vmax_mv` — i.e. Android/HAL writes `/sys/class/leds/vibrator/{duration,activate}`. |

Cross-check on the SPMI side tree (no conflict at 0x5300 on the primary slave): SID 2 children in
`pm7250b.dtsi` include `clock-controller@5b00` only in the 0x5xxx range (`:258`); 0x5300 exists
only on SID 3.

## 2. Current mainline state

| Item | State | Citation / live evidence |
|---|---|---|
| `qcom-spmi-haptics.c` | **does not exist** upstream | `ls build/linux/drivers/input/misc/` → no such file |
| PMIC vibrator driver | present, exactly the needed register model | `build/linux/drivers/input/misc/pm8xxx-vibrator.c:54-65` (`pmi632_regs`: enable 0x46/BIT(7), drv 0x40 mask 0xFF, drv2 0x41 bits[3:0]<<8, mV units), `:14-18` (1504 / 3544 mV, 8 mV step), `:129-131` enable write, `:114-124` voltage write order (VSET then EN) |
| Register-model match | **identical** to the vendor LDO block | vendor `:21,23-24,27-29,89-93,118-124` vs mainline `:54-65` |
| Compatible match | `qcom,pmi632-vib` (driver), `qcom,pm7250b-vib` new-style compatible documented | driver `:284-288`; binding `:18-26` |
| Kconfig symbol | `CONFIG_INPUT_PM8XXX_VIBRATOR` (tristate, `select INPUT_FF_MEMLESS`, `depends on MFD_SPMI_PMIC`) | `build/linux/drivers/input/misc/Kconfig:216-224` |
| Config in this repo | **present in the working tree, uncommitted**: `CONFIG_INPUT_PM8XXX_VIBRATOR=y` appended to `configs/linux/picasso.config` | `git diff configs/linux/picasso.config` |
| Runtime kernel config | `CONFIG_INPUT_PM8XXX_VIBRATOR=m`, `CONFIG_INPUT_FF_MEMLESS=m`, `CONFIG_INPUT=y`, `CONFIG_MFD_SPMI_PMIC=y` — i.e. the running image predates that config edit | phone: `zcat /proc/config.gz` |
| Module loadable? | **no** — the RAM-boot rootfs has no module tree | phone: `/lib/modules` and `/usr/lib/modules` empty, `find / -xdev -name '*.ko*'` → 0 real modules |
| DTS node | **present in the working tree, uncommitted** (`sm7250-xiaomi-picasso.dts:670-684`); upstream `pm7250b.dtsi:212-217` is still empty, the node is a board-level merge into `pmic@3` | `sed -n 670,684p build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts` |
| Board DTS | includes `pm7250b.dtsi`, defines `PM7250B_SID1 3` | `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts:22-28` |
| SPMI controller | label `spmi_bus`, `#address-cells = <2>`, `#size-cells = <0>` | `build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi:1491,1507-1508` |
| SPMI child naming | platform child will be `0-03:vibrator@5300` (SPMI dev name = `"%d-%02x"` of ctrl,usid) | `build/linux/drivers/spmi/spmi.c:67,494-521`; live: `/sys/bus/spmi/devices/0-03` |
| Live probe result | **nothing** — the booted image predates the uncommitted DT/config edits: SID 3 has no child node, no platform device, no input device, `hapticd` sees 0 actuators | §5 |
| Other haptics drivers present but irrelevant | `INPUT_AW8697_HAPTICS` (`drivers/input/misc/aw8697.c:315`, Kconfig `:944`), `qti-haptics` (vendor only, `qcom,haptics` = PM8150B/PM660 block; `pm8150b.dtsi` is never included on lito/picasso) | `grep -l pm8150b.dtsi lito* picasso*` → none |

Dependency sanity: children of an SPMI pmic node become platform devices with the spmi device as
parent (`drivers/mfd/qcom-spmi-pmic.c:284` `devm_of_platform_populate()`), and the driver takes the
parent regmap (`pm8xxx-vibrator.c:214`). SID 3 is already probed successfully by `pmic-spmi` on the
live phone, so that regmap exists.

## 3. Change plan

### (a) DTS node — in `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`

This is **already in the working tree** (`:670-684`); the text below is the exact, verified block
(it compiles to the correct parent — see the end of this subsection). It uses only labels that
already exist (`&spmi_bus`, `sm7250.dtsi:1491`) or are plain literals; the `pmic@3` node from
`pm7250b.dtsi` is *merged* by unit address, so no mainline file is touched.

```dts
/* Haptics: PM7250B hosts the QPNP vibrator LDO at SPMI peripheral 0x5300
 * (vendor `qcom,vibrator@5300` under `pm7250b@3`).  In-tree pm8xxx-vibrator
 * drives it through its pmi632 register set (enable 0x46.7, 12-bit mV level
 * at 0x40/0x41, 8 mV step): the vendor LDO and the pmi632 block are the same
 * registers.  The vendor requests 3.0 V with overdrive disabled; the mainline
 * driver has no overdrive and starts at its default level, so register the
 * node and let ff-memless scale the rumble. */
&spmi_bus {
	pmic@3 {
		vibrator@5300 {
			compatible = "qcom,pm7250b-vib", "qcom,pmi632-vib";
			reg = <0x5300>;
		};
	};
};
```

`pmic@3` (literal) and `pmic@PM7250B_SID1` (macro, `:23`) are the same node name; the literal is
what is in the tree today. No `interrupts`, `*-gpios`, `*-supply`, `clocks`, `firmware-name` — the
hardware has none, and the vendor properties (`qcom,vib-ldo-volt-uv`, `qcom,disable-overdrive`) must
**not** be copied in: mainline's binding has `additionalProperties: false` and its driver ignores
both.

Verified without touching the tree (cpp + dtc piped through stdin, nothing written):

```sh
cd build/linux/arch/arm64/boot/dts/qcom
cpp -nostdinc -undef -P -x assembler-with-cpp -I . -I ../../../../../include - \
	< sm7250-xiaomi-picasso.dts \
  | dtc -I dts -O dts -o - - | grep -B5 -A4 'vibrator@5300'
#   reg = <0x03 0x00>;                 <-- parent is the SID-3 pmic node
#   ...
#   vibrator@5300 {
#           compatible = "qcom,pm7250b-vib", "qcom,pmi632-vib";
#           reg = <0x5300>;
#   };
# no duplicate-node / duplicate-property / unit-address warning for this node.
```

### (b) `configs/linux/picasso.config`

The needed line is `CONFIG_INPUT_PM8XXX_VIBRATOR=y`. It is **already appended** to the fragment in
the working tree (uncommitted), next to the NFC block, with:

```
# Haptics: PM7250B QPNP vibrator LDO (in-tree pm8xxx-vibrator, pmi632 regs)
# -> input FF_RUMBLE, consumed by hapticd (70-hapticd-builtin.rules)
CONFIG_INPUT_PM8XXX_VIBRATOR=y
```

`=y` is mandatory, not stylistic: the driver `select`s `INPUT_FF_MEMLESS`, so FF_MEMLESS also ends
up `=y`, and a `=m` value (what the currently booted image has) can never be loaded because the
rootfs ships no `/usr/lib/modules` content. Do not add an explicit `CONFIG_INPUT_FF_MEMLESS=y`
unless you want it pinned: it is redundant with the `select`, and `depends on MFD_SPMI_PMIC` is
already satisfied by `configs/linux/picasso.config:13`.

### (c) Patch file

**No new patch file.** `./build.sh export-patches` regenerates
`patches/linux/picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch` from
`git diff HEAD -- arch/arm64/boot/dts/qcom/Makefile arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts
Documentation/devicetree/bindings/arm/qcom.yaml` (see `build.sh`, `export_patches()`), so the
vibrator hunk belongs to **0002** and will be folded in automatically. Creating a separate
`0016-…` patch would duplicate the node (and fail to apply after 0002).

One-paragraph description for the 0002 changelog: *Adds an `&spmi_bus` override to the picasso
board DTS that merges a `vibrator@5300` child into the existing PM7250B second-slave pmic node
(`pmic@3`), with `compatible = "qcom,pm7250b-vib", "qcom,pmi632-vib"` and `reg = <0x5300>`. This
mirrors the downstream MTP device tree (`pm7250b.dtsi:450-461`), which places the vibrator LDO on
SPMI SID 3 at peripheral 0x5300 driven at 3.0 V with overdrive disabled, and binds it to the
existing mainline `pm8xxx-vibrator` driver, whose `pmi632_regs` register layout (12-bit mV at
0x40/0x41, enable bit 0x46.7) is the vendor `qcom,qpnp-vibrator-ldo` protocol. No driver code is
touched; the accompanying `configs/linux/picasso.config` line `CONFIG_INPUT_PM8XXX_VIBRATOR=y`
builds the driver in (modules cannot be loaded on this image).*

If a later rebase makes `pm7250b.dtsi` carry its own `vibrator@5300` node, drop this hunk and use the
resulting label (`&pm7250b_vib { status = "okay"; };`) instead — see risk R8.

### (d) Runtime verification and expected output

Run from the repo root (build with the project's own entry point; do not hand-roll a config):

```sh
./build.sh                       # coordinator owns build+flash; this doc does not run it
```

On the phone, after the new Image is RAM-booted:

```sh
# 1. built-in, not a module
zcat /proc/config.gz | grep -E 'INPUT_PM8XXX_VIBRATOR|INPUT_FF_MEMLESS'
#   CONFIG_INPUT_FF_MEMLESS=y
#   CONFIG_INPUT_PM8XXX_VIBRATOR=y

# 2. platform device created under the SID-3 SPMI device (name proves the SID!)
find /sys/devices -name 'vibrator@5300'
#   /sys/devices/platform/soc@0/c440000.spmi/spmi-0/0-03/vibrator@5300
#   (must be 0-03, not 0-02 — see R2)

# 3. driver bound
grep DRIVER /sys/bus/platform/devices/0-03:vibrator@5300/uevent
#   DRIVER=pm8xxx-vib

# 4. input node + its FF capabilities
grep -l pm8xxx_vib_ffmemless /sys/class/input/input*/name
#   /sys/class/input/input5/name            (N is whatever is free)
cat /sys/class/input/input5/capabilities/ff
#   1001f0000 0
#   = word1 bits 16..20 (FF_RUMBLE 0x50, FF_PERIODIC 0x51, FF_SINE 0x52,
#     FF_TRIANGLE 0x53, FF_SQUARE 0x54) + bit 32 (FF_GAIN 0x60); word0 empty
#   (64-bit ulong; on a 32-bit kernel it would print 4 words instead)

# 5. udev tag that makes hapticd claim it as the built-in actuator
udevadm info -q property -n /dev/input/event5 | grep HAPTICD
#   HAPTICD_DEVICE=1
#   HAPTICD_ID=builtin

# 6. daemon side, no unit restart needed
hapticctl rescan && hapticctl list
#   builtin
journalctl -b -u hapticd | tail -3
#   hapticd: 1 actuator(s) available
#   hapticd: ready on system bus

# 7. the actual proof: motor buzzes ~300 ms
hapticctl vibrate 300 100
#   returns after ~0.3 s, no error; hand feels a 300 ms buzz
```

Vendor-parity intensity (3.0 V instead of the 3.55 V a 100 % rumble maps to):

```sh
hapticctl vibrate 500 73      # 1504 + 2048*73/100 = 2999 mV == qcom,vib-ldo-volt-uv
```

Android-in-container path (this is how the desktop/UI triggers haptics today):

```sh
dmesg | grep -A1 'droidloom haptics'
#   before: "droidloom haptics:" / "the host has no vibrator"
#   after:  no such error line; droidloom-devices logs "droidloom-devices: hapticd did not play: …"
#           only on a real failure
```

Raw FF check without hapticd (optional; `evtest`/`fftest` are **not** installed on this image):
install `evtest` and run `evtest /dev/input/event5`; expected menu entry
`Testing ... FF_RUMBLE / FF_PERIODIC / FF_GAIN`, then after `Uploading effect #0` the motor spins.

## 4. Unknowns / risks

| # | Risk | Why it matters | Cheapest experiment that resolves it |
|---|---|---|---|
| R1 | Driver built as `=m` | This rootfs has **no** `/usr/lib/modules` and zero `.ko` files, so `=m` means "never probes" — silently, with no dmesg line. The currently booted kernel is exactly in that state; the uncommitted fragment fixes it with `=y` | Already answered: `find / -xdev -name '*.ko*'` → nothing. Keep `=y` and confirm with step 1 of §3(d). |
| R2 | Node placed under the wrong pmic (SID 2 instead of 3) | Writes at SID2/0x5340 hit unimplemented SPMI addresses → probe may succeed but nothing vibrates (harmless: SID2 only implements `clock-controller@5b00` in that range) | `find /sys/devices -name 'vibrator@5300'` must show `.../0-03/...`; if it shows `0-02`, the node is under `pmic@PM7250B_SID` |
| R3 | Max FF magnitude maps to **3552 mV**, above the chip's documented 3544 mV max (upstream off-by-one-step: `MAX-MIN+STEP`) | Undefined behaviour at full intensity; could clamp, cut out, or overdrive | Compare `hapticctl vibrate 500 100` vs `500 90` (`≈3352 mV`). If 100 misbehaves, cap intensity in `hapticd` config or add a one-line clamp patch |
| R4 | Mainline never polls `STATUS1.VREG_READY` (`0x08` bit 7) before/after enabling; the vendor does (`:62-70`) | First pulse or back-to-back short pulses could be weak/late while the LDO ramps | Three pulses with gaps: `hapticctl vibrate 50 100` ×3 with ~100 ms between; if the first is weak, add a `regmap_read_poll_timeout` on `base+0x08` in `pm8xxx_vib_set()` (patch, ~10 lines, cite vendor `:62-70`) |
| R5 | Motor type: ERM vs LRA is **not documented** in any downstream file; a DC LDO cannot drive an LRA | If it really were an LRA, this whole plan yields weak/buzzy output and needs a smart driver that no downstream source references | Feel/serial-response test: `hapticctl vibrate 1000 100` (~3.5 V) vs `1000 20` (~1.9 V). ERM amplitude scales smoothly with voltage; an LRA on DC barely moves at any voltage |
| R6 | Vendor tuning dropped: 3.0 V fixed + overdrive off; mainline is 1.5–3.55 V scaled by rumble magnitude | Long-term drive above Xiaomi's design point (loudness vs. motor/lens longevity) | Burn-in at `hapticctl vibrate 500 73` (3.0 V) first; only go to 100 % deliberately |
| R7 | udev rule ignores USB/BT/virtual bustypes and zero FF caps (`/usr/lib/udev/rules.d/70-hapticd-builtin.rules`) | `pm8xxx` input device has bustype 0 and FF_RUMBLE set, so it passes — but a future change to the driver's `id.bustype` would silently drop the tag | `udevadm info -q property -n /dev/input/eventN \| grep HAPTICD_ID` → `HAPTICD_ID=builtin` |
| R8 | If a future rebase adds `vibrator@5300` to `pm7250b.dtsi` (with properties), our board block re-declares `compatible`/`reg` in a second block | dtc duplicate-property warning/failure, or a lost `status = "disabled"` | `./build.sh check` (applies the series to a clean tree + `dtbs_check`); then switch to the `&pm7250b_vib` label form |
| R9 | Stale build artifact: `build/linux/out/.config` still contains `CONFIG_INPUT_QCOM_VIBRATOR_LDO=y`, a symbol from an abandoned custom `drivers/input/misc/qcom-vib-ldo.c` that no longer exists in the tree | `./build.sh` regenerates the config from defconfig + fragments, so it is ignored; but a manual `make O=out` reuse would complain about an unknown symbol and confuse a bisect | `grep -n QCOM_VIBRATOR_LDO build/linux/out/.config` → drop `build/linux/out` (or run `./build.sh clean`) before the next build. Do not resurrect that driver: the in-tree `pm8xxx-vibrator` is register-identical. |

Non-risks worth stating: no rail to enable, no GPIO to mux, no IRQ to route, no firmware to load,
and the SPMI SID-3 revid read already succeeds on this unit (otherwise `pmic-spmi` would not be
bound and no child would ever be created).

## 5. Live observations (read-only, phone <lan-ip>)

```
$ uname -a
Linux picasso 7.3.0-rc7-00001-ge1927d6cbd57-dirty #199 SMP PREEMPT Tue Nov 17 18:45:54 +07 2026 aarch64

$ ls /sys/class/leds
(nothing — no LED-class vibrator, unlike downstream)

$ for d in /sys/class/input/input*; do printf "%s %s ff=" "$(basename $d)" "$(cat $d/name)"; cat $d/capabilities/ff; done
input0 pm8941_pwrkey ff=0
input1 pm8941_resin ff=0
input2 NVTCapacitiveTouchScreen ff=0
input3 gpio-keys ff=0
input4 Xiaomi-Redmi-K30-5G Headset Jack ff=0

$ systemctl status hapticd --no-pager
   Active: active (running) ...
   hapticd[401]: hapticd: 0 actuator(s) available

$ cat /etc/hapticd/config.json
{"default_device":"builtin"}

$ hapticctl list
(nothing)

$ pacman -Qi hapticd | head -4
Description     : D-Bus service for evdev force-feedback and sysfs vibrators
Required By     : calls-manager  phonewave-image

$ cat /usr/lib/udev/rules.d/70-hapticd-builtin.rules
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{capabilities/ff}!="0", \
  ATTRS{id/bustype}!="0003|0005|0006", ENV{HAPTICD_DEVICE}="1", ENV{HAPTICD_ID}="builtin"

$ dmesg | grep -A1 'droidloom haptics'
[   16.148696] droidloom haptics:
[   16.148726] the host has no vibrator

$ strings /usr/bin/droidloom-devices | grep -i haptic
droidloom-devices: hapticd did not play:
droidloom-devices: no vibrator from hapticd:
org.hapticd  org.hapticd.Haptics  PlayPattern  Cancel  ListDevices

$ zcat /proc/config.gz | grep -E 'INPUT_PM8XXX_VIB|INPUT_FF_MEMLESS|MFD_SPMI_PMIC'
CONFIG_INPUT_FF_MEMLESS=m
CONFIG_INPUT_PM8XXX_VIBRATOR=m
CONFIG_MFD_SPMI_PMIC=y

$ ls -l /sys/bus/spmi/devices/
0-00  0-01  0-02  0-03  0-04  0-05  spmi-0
$ readlink /sys/bus/spmi/devices/0-03/driver
.../drivers/pmic-spmi
$ ls /sys/bus/spmi/devices/0-03/of_node
#address-cells  #size-cells  compatible  name  reg      # no vibrator child yet
$ cat /sys/bus/spmi/devices/0-03/of_node/compatible
qcom,pm7250bqcom,spmi-pmic
$ find /sys/devices -name '*vibrator*'
(nothing — the booted DT has no vibrator@5300 child, so no platform device exists)
$ zcat /proc/config.gz | grep -i qcom_vib
(nothing; the abandoned CONFIG_INPUT_QCOM_VIBRATOR_LDO never reached this kernel)

$ find / -xdev -name '*.ko*' 2>/dev/null | wc -l
0                       # => CONFIG_...=m can never be loaded on this image
```

The booted image (`#199`, built 18:45) predates the working-tree DTS edit (file mtime 19:51) and the
`picasso.config` edit, so this boot is the *before* state; the whole chain
"DT child → platform device → driver → evdev → hapticd" is still unproven on hardware.

Reading of the evidence: the hardware block is reachable (SID 3 bound, revid read OK) but has no
child node, no driver, no input device, so `hapticd` advertises zero actuators and the Android-in-
container bridge logs `the host has no vibrator`. The downstream interface was an LED class device;
the mainline interface that the installed userland actually consumes is evdev `FF_RUMBLE`.
