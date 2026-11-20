# Residual error audit — mainline 7.3-rc7 + Denial Mobile on picasso

Scope: every defect still visible on the running phone (`<lan-ip>`, kernel
`7.3.0-rc7-00001-ge1927d6cbd57-dirty #199`, boot 2026-11-17 19:36 +07, audit at 19:41–19:50,
uptime 6–14 min). Read-only: no fix was applied, nothing was written on the phone, no repo file
other than this document was touched. Every value below is cited to a source line; anything I could
not prove is marked **[unknown]** and paired with the cheapest experiment that closes it.

Two facts shape the whole audit:

* The system is *functional*: Wi-Fi associated (ch 153/80 MHz), modem+ADSP remoteprocs running,
  40+ QRTR services, touch (NT36672C) probing with firmware, speaker amp (TFA9874) opening DAI
  streams, display scanning out (`dsi_isr` 61296 IRQs), backlight at 2048/4095.
* Almost every remaining error is a **built-in-vs-module** defect (the RAM boot carries no
  `/lib/modules` at all: `ls /lib/modules` is empty) or an **Android-userspace-expectation**
  defect in the Denial image. Device-tree work is only needed for the LED, Type-C and RNG.

## 1. Ranked findings

Severity: S1 = breaks a subsystem that users notice or that protects the hardware; S2 = feature
dead or safety net missing; S3 = cosmetic-but-wrong or degraded; S4 = noise/known.
Class: **A** = kernel config/DTS defect, **B** = missing mainline driver/wiring, **C** =
userland/unit defect in the Denial image, **D** = cosmetic/known.

| # | Sev | Finding | Class | Evidence (command → line) | Fix | Proof |
|---|-----|---------|-------|---------------------------|-----|-------|
| 1 | S1 | No swap at all: `zram-swap.service` failed, `CONFIG_ZRAM` not built | A | `systemctl status zram-swap` → `zram-swap[269]: modprobe: FATAL: Module zram not found in directory /lib/modules/7.3.0-rc7-…`; `build/linux/out/.config:2401` `# CONFIG_ZRAM is not set` | `configs/linux/picasso.config`: `CONFIG_ZRAM=y` | `systemctl start zram-swap; systemctl is-active zram-swap` → `active`; `cat /proc/swaps` → `/dev/zram0 partition <N> <used> 100` |
| 2 | S1 | No battery-backed RTC: clock starts from a stale journal stamp and jumps ~4 h 50 m at boot | A | `timedatectl` → `Failed to query server: Failed to read RTC: Input/output error`; `journalctl -k -b` → `rtc-efi rtc-efi.0: hctosys: unable to read the hardware clock`; `journalctl -b` → `System time advanced to timestamp on /var/lib/systemd/timesync/clock: …14:47:45`, `Realtime clock jumped backwards`; `build/linux/out/.config:8457` `CONFIG_RTC_DRV_PM8XXX=m` | `CONFIG_RTC_DRV_PM8XXX=y` (node already present and enabled: `upstream/linux/arch/arm64/boot/dts/qcom/pm8150.dtsi:119-124`) | `cat /sys/class/rtc/rtc1/name` → `pm8xxx-rtc`; `cat /sys/class/rtc/rtc1/since_epoch` returns a plausible epoch; `timedatectl` no longer prints the RTC error; a reboot keeps wall-clock time |
| 3 | S2 | The three PMIC die temperatures are unmonitored: no `pm8150/pm8150l/pm7250b` thermal zone exists, so charging heat never asks the CPU/GPU to back off | A | `ls /sys/class/thermal/` → 27 zones, all `aoss*/cpu*/gpuss*/…` (SoC TSENS only), no `pm81xx`; `build/linux/out/.config:5073` `CONFIG_QCOM_SPMI_TEMP_ALARM=m` | `CONFIG_QCOM_SPMI_TEMP_ALARM=y` (nodes exist: `upstream/linux/arch/arm64/boot/dts/qcom/pm7250b.dtsi:92-99`, `pm8150.dtsi:63`, `pm8150l.dtsi:27`) | `for z in /sys/class/thermal/thermal_zone*; do cat $z/type; done \| grep -E 'pm8150\|pm7250b'` → 3 lines, each with `trip_point_0_temp`/`1`/`2` (stage-1/2/3) |
| 4 | S2 | Notification LED dead: `/sys/class/leds` empty even though `LEDS_CLASS` is built in | A | `ls /sys/class/leds` → empty dir; `build/linux/out/.config:8275` `CONFIG_LEDS_QCOM_LPG=m`, `:9626` `CONFIG_QCOM_PBS=m`; `upstream/linux/arch/arm64/boot/dts/qcom/pm8150l.dtsi:124-132` LPG node `status = "disabled"`; board DTS has no `&pm8150l_lpg` (`grep -n lpg build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts` → nothing) | config `CONFIG_LEDS_QCOM_LPG=y`, `CONFIG_QCOM_PBS=n` (a `=m` PBS caps LPG at `m`: `drivers/leds/rgb/Kconfig:93` `depends on QCOM_PBS \|\| !QCOM_PBS`) + DTS §3a-1 | `ls /sys/class/leds` → `blue:indicator`; `echo 1 > /sys/class/leds/blue:indicator/brightness` lights the white LED (visual) |
| 5 | S2 | Bluetooth TX address service fails 5× per boot then hits the start limit | A | `journalctl -b -u radio-addr-bluetooth` → `radio-addr: /sys/devices/soc0/serial_number: No such file or directory` (×5), `Failed with result 'start-limit-hit'`; `ls /sys/devices/soc0` → *No such file or directory*; `build/linux/out/.config:9620` `CONFIG_QCOM_SOCINFO=m`; driver is created by `upstream/linux/drivers/soc/qcom/smem.c:1254` `platform_device_register_data(… "qcom-socinfo" …)` — impossible when `=m` | `CONFIG_QCOM_SOCINFO=y` (SMEM node already present: `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts:83-86`) | `systemctl start radio-addr-bluetooth; systemctl is-active radio-addr-bluetooth` → `active`; `cat /sys/devices/soc0/serial_number` → decimal serial |
| 6 | S2 | Android cell has `/proc` read-only, so `oom_score_adj`, `timerslack` and `modprobe` writes of every Android service silently fail | C | `dmesg` → `init: Unable to write -1000 to /proc/1/oom_score_adj: open() failed: Read-only file system`; `logd: libprocessgroup: set_timerslack_ns write failed: Read-only file system`; `init: … Unable to write to file '/proc/sys/kernel/modprobe': … Read-only file system` | Denial cell mount: `/proc` `rw` (or drop the write attempts); needs an image change, not a kernel change | After image change: `journalctl -b \| grep -c 'Read-only file system'` drops from 3 distinct sites to 0; `cat /proc/1/oom_score_adj` inside the cell ≠ `-1000`+error |
| 7 | S2 | No capture PCM: microphone unusable (playback-only card) | B | `arecord -l` → header only, no device; `cat /proc/asound/devices` → `2: [0-0]: digital audio playback`, no capture line; `grep -c swr build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi` → 0 nodes | Needs SoC SoundWire + LPASS RX/TX macro + `wcd938x` wiring; see §3c-3 (est. 2–4 days) | `arecord -l` lists `card 0: … device 1: …` capture; `arecord -d 1 /tmp/t.wav` produces non-silent data from the top mic |
| 8 | S2 | Battery temperature missing: `healthd` reports `t=0.0`, no `temp` attribute at all | A | `cat /sys/class/power_supply/pm7250b-battery/temp` → *No such file*; `dmesg` → `healthd: battery l=92 v=4296 t=0.0 …`; QG property list has no `POWER_SUPPLY_PROP_TEMP` (`patches/linux/picasso/0015-power-supply-qcom-pm7250b-add-qg-battery.patch`, list at hunk starting `POWER_SUPPLY_PROP_PRESENT`) | Add `POWER_SUPPLY_PROP_TEMP` fed from the PM7250B ADC5 batt-therm channel; downstream reads it: `picasso-sm7250.dtsi:794-801` `io-channels = <&pm7250b_vadc ADC_BAT_THERM_PU2>` — patch §3c-2 | `cat /sys/class/power_supply/pm7250b-battery/temp` → ~250–350 (tenths of °C, matches room/hand temperature); `healthd` log line shows `t=2xx` |
| 9 | S3 | No Type-C/PD port object at all (`/sys/class/typec` absent); cable state, CC orientation and PD contract invisible | A | `ls /sys/class/typec` → *No such file*; `ls /sys/class/power_supply` → only `pm7250b-battery`, `pm7250b-usb`; `upstream/linux/arch/arm64/boot/dts/qcom/pm7250b.dtsi:53` `typec@1500 { … status = "disabled"; }`; config `:8047 CONFIG_TYPEC=m`, `:8054 CONFIG_TYPEC_QCOM_PMIC=m`, `:5420 CONFIG_REGULATOR_QCOM_USB_VBUS=m` | config §3b-1 + DTS §3a-3 (supply grounded: `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/lito-pmic-overlay.dtsi:451-455` `vdd-pdphy-supply = <&pm8150_l2>`) | `ls /sys/class/typec` → `port0`; `cat /sys/class/typec/port0/data_role` → `[device] host`; `ip -br addr show usb0` still `UP` (gadget survives role handling) |
| 10 | S3 | `slot-success.service` fails permanently and drives `vold: Error getting bootctrl v1.0` | C | `journalctl -b -u slot-success` → `slot-success[554]: No boot_a partition.`, `status=2/INVALIDARGUMENT`; `ls /dev/disk/by-partlabel \| grep boot` → only `boot` (single slot); `cat /etc/slot-success.conf` exists (`units=denial.service sshd.service`, `settle=60`) | Remove `/etc/slot-success.conf` from the Denial image (unit is `ConditionPathExists=`-gated, so it then shows as *skipped*, not *failed*); or add `/etc/systemd/system/slot-success.service.d/10-single-slot.conf` with `[Unit] ConditionPathExists=` | `systemctl --failed` → 0 units; `systemctl status slot-success` → `Condition: … skipped` |
| 11 | S3 | The "45-minute watchdog" is a blind forced reboot; the pet file is never read | C | `cat /run/systemd/system/picasso-watchdog.timer` → `OnBootSec=45min`, `Unit=picasso-watchdog.service`; `cat /run/systemd/system/picasso-watchdog.service` → `ExecStart=/bin/sh -c 'sync; echo b > /proc/sysrq-trigger; reboot -f'`; `ls -la /run/picasso-watchdog-pet` exists but nothing references it (`grep -rn picasso-watchdog-pet /usr /etc /run` → only the file) | If a pet semantics is wanted: add `ExecCondition=/bin/sh -c 'test $(( $(date +%s) - $(stat -c %Y /run/picasso-watchdog-pet) )) -lt 600'`; if it is meant as a session timer, keep it and document it (it is *monotonic*, so finding 2 does not affect it — `systemctl list-timers` next-fire is +45 min from boot) | `systemctl list-timers picasso-watchdog.timer` → `NEXT` correct; with the `ExecCondition`, `systemctl start picasso-watchdog` right after touching the pet file → unit skipped, phone stays up |
| 12 | S3 | No `/dev/hw_random`; Android `prng_seeder` gives up | A | `ls -l /dev/hw_random` → *No such file*; `dmesg` → `prng_seeder: Hanging forever because setup failed: Timed out opening hwrng /dev/hw_random`; `build/linux/out/.config:3907` `CONFIG_HW_RANDOM_QCOM=m`; `grep -in 'rng@\|qcom,prng' build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi` → nothing | `CONFIG_HW_RANDOM_QCOM=y` + an `rng` node (clock already modelled: `build/linux/drivers/clk/qcom/gcc-sm7250.c:2056` `[GCC_PRNG_AHB_CLK]`, binding reference `upstream/linux/arch/arm64/boot/dts/qcom/sm8250.dtsi:997-1002`). Base address is **[unknown]** — see §4 | `ls -l /dev/hw_random` exists; `head -c 16 /dev/hw_random` succeeds; `dmesg \| grep -i hwrng` shows the device |
| 13 | S3 | The Android runtime still expects cgroup **v1** controllers, which this kernel no longer registers | C | `cat /proc/cgroups` → only `cpu cpuacct blkio devices freezer perf_event hugetlb pids`, no `memory`/`cpuset`; `dmesg` → `cgroup: Unknown subsys name 'cpuset'`, `libprocessgroup: Compact: memory controller not found` (×3), `Failed to write 1 to /sys/fs/cgroup/system/uid_1000/pid_288/cgroup.kill`; config `:186 # CONFIG_MEMCG_V1 is not set`, `:201 # CONFIG_CPUSETS_V1 is not set`; `cat /sys/fs/cgroup/cgroup.controllers` → `cpuset cpu memory hugetlb pids` (v2 is fine) | Prefer *not* enabling the legacy controllers (systemd owns v2; `upstream/linux/init/Kconfig:1113,1327` exist only for legacy apps). Fix belongs in Denial's `libprocessgroup` (use `/sys/fs/cgroup/<path>/cgroup.kill` and v2 subtree control) | After userland fix: `journalctl -b \| grep -cE 'Unknown subsys name|memory controller not found'` → 0 |
| 14 | S3 | No regulatory database: Wi-Fi is stuck in world domain, 5 GHz is passive-scan-only | C | `iw reg get` → `country 00: DFS-UNSET`, `(5250 - 5330 @ 80), (0 ms), DFS, …), PASSIVE-SCAN`; `journalctl -k` → `faux_driver regulatory: Direct firmware load for regulatory.db failed with error -2` | Ship `wireless-regdb` (and `crda`-free `regulatory.db` under `/usr/lib/firmware/`) in the Denial image | `iw reg get` shows a real country (e.g. `country VN`), AP active-scan of ch 149+ succeeds |
| 15 | S3 | Android `healthd`/health HAL sysfs paths do not exist on mainline | C | `dmesg` → `healthd: chargingStatePath not found`, `batterySerialPath not found`, `batteryChargeTimeToFullNowPath. not found` (+11 more) | Denial health config: point at the properties that exist (`status`, `health`, `charge_full`, `charge_now`, `voltage_now`) or drop the optional ones | `journalctl -b \| grep -c 'healthd: .*not found'` → 0 |
| 16 | S4 | Two clk-core WARNs at DSI PHY probe taint the kernel (`taint=512`, `W`) | D | `journalctl -k -b` → `WARNING: drivers/clk/clk.c:1257 at clk_core_disable+0xa4/0xac`, `dsi0_phy_pll_out_dsiclk already disabled`, then `clk.c:1116 … already unprepared`; trace `__clk_set_parent_after → clk_core_reparent_orphans_nolock → of_clk_add_hw_provider → dsi_phy_driver_probe` | None cheap: the orphan reparent path (`build/linux/drivers/clk/clk.c:2218-2220`, `3925-3950`) balances prepare/enable of an orphan whose parent only appears when the PHY registers its clocks (`dispcc-sm8250.c:187` `dsi0_phy_pll_out_dsiclk`). Functional impact nil (display scans out); worst case one clock is left enabled. If wanted: set `CLK_IGNORE_UNUSED`/defer DISPCC probe, or upstream the clk-core guard | `cat /proc/sys/kernel/tainted` → `0` after the change; `dmesg \| grep -c 'already disabled'` → 0 |
| 17 | S4 | `venus` + `adsp` firmware loads fail at 8 s because the firmware partition is mounted later by init; the failures are cured by init's rebind | D | `journalctl -k -b` → `qcom-venus aa00000.video-codec: Direct firmware load for …/venus.mdt failed with error -2` → `probe with driver qcom-venus failed with error -22`; later `bound aa00000.video-codec`; `remoteproc remoteproc0: Direct firmware load for …/adsp.mdt failed with error -2` then `Booting fw image …/adsp.mdt` | Nothing to fix (init rebinds); if the rebind were ever removed, `-ENOENT` should return `-EPROBE_DEFER` in both drivers | `ls /dev/video0 /dev/video1` exist; `cat /sys/class/remoteproc/*/state` → `running running` |
| 18 | S4 | Assorted boot noise with no functional impact | D | `dmesg`: `apexd-bootstrap: … LOOP_CONFIGURE: Device or resource busy` (×7, retried), `F2FS-fs (sda34): Can't find valid F2FS filesystem` (×2, `super` is Android-sparse), `EXT4-fs (sde48): VFS: Can't find ext4 filesystem` (modem vfat probed by util-linux), `ufshcd-qcom 1d84000.ufshc: freq-table-hz property not specified`, `ath10k_snoc …: chan info: invalid frequency 0 (idx 41 out of bounds)` (×2), `error: Bind to port 22 … Address already in use` (×3, one listener now), `/vendor/etc/init/droidloom-wifi.rc: 16: Invalid keyword 'oneway'`, `the host has no vibrator`, `UprobeStatsBpfLoad: … Operation not permitted` (Android eBPF on non-LTS kernel), `NetBpfLoad: Unsupported kernel version (7030000)` | Leave, except `droidloom-wifi.rc` syntax (`oneway` → `oneshot`/drop) and the missing vibrator driver (no mainline PM8150B haptics driver wired — class B, cosmetic) | Re-read `journalctl -k -b` and confirm the messages are gone |

## 2. Hardware ground truth

Downstream tree proven on this phone: `~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/`.

| Item | Downstream `file:line` | Value |
|------|------------------------|-------|
| Notification LED controller | `pm8150l.dtsi:376-382` | `qcom,pwm-lpg` @ `0xbc00` (SPMI, PM8150L), `qcom,num-lpg-channels = <2>`; 5 LPG channels in the mainline table (`build/linux/drivers/leds/rgb/leds-qcom-lpg.c:1783-1796`) |
| Notification LED (tri-led) | `pm8150l.dtsi:384-410` | `qcom,tri-led` @ `0xd000`: `red` = `<&pm8150l_lpg 0>`, `green` = `<&pm8150l_lpg 1>`, `blue` = `<&pm8150l_lpg 2>` |
| Notification LED on picasso | `picasso-sm7250.dtsi:628-640` | `red`/`green` `status = "disabled"`, `blue` relabelled **`white`** → the only populated emitter is LPG channel 2 |
| LPG ramp tuning (downstream-only) | `pm8150l.dtsi:337-374` | `qcom,lpg-chan-id` 1/2/3, `ramp-step-ms = <100>`, `ramp-low-index = <0>`, `ramp-high-index = <20>`, `ramp-from-low-to-high`, `ramp-pattern-repeat` |
| PMIC RTC | `pm8150.dtsi:105-109` | `qcom,pm8941-rtc` @ `0x6000` (alarm `0x6100`), IRQ `<0x0 0x61 0x1>` |
| Charger / QG battery | `picasso-sm7250.dtsi:794-801` | `&pm7250b_qg` reads `ADC_BAT_THERM_PU2` + `ADC_BAT_ID_PU2` ("batt-therm", "batt-id") |
| Battery data | `picasso-sm7250.dtsi:785-789` | `batterydata-picasso-G7A{,SIB,NVT}M4P-4500mah.dtsi`, `batt-id-range-pct = <15>` |
| Type-C PD PHY | `pm7250b.dtsi:186-205` | `qcom,qpnp-pdphy` @ `0x1700`, 8 IRQs (sig-tx…fr-swap); board override `picasso-sm7250.dtsi:783-785` |
| PD PHY supplies | `lito-pmic-overlay.dtsi:451-455` | `vdd-pdphy-supply = <&pm8150_l2>`, `vbus-supply = <&smb5_vbus>`, `vconn-supply = <&smb5_vconn>`; vendor driver requires 2.8–3.3 V (`~/droidian/kernel/drivers/usb/pd/qpnp-pdphy.c:69-71` `VDD_PDPHY_VOL_MIN/MAX`) → board rail is `vreg_l2a_3p1` (3.072 V, `sm7250-xiaomi-picasso.dts:249-256`) |
| PD PHY usage | `picasso-sm7250.dtsi:736` | `qcom,usbpd-phandle = <&pm7250b_pdphy>` (charger ↔ PD link) |
| PMIC die temp alarms | `pm7250b.dtsi:212-216`, `pm8150.dtsi:63`, `pm8150l.dtsi:27` | `qcom,spmi-temp-alarm` @ `0x2400`, IRQ `<SID 0x24 0x0 EDGE_BOTH>`, ADC channel `ADC5_DIE_TEMP` |
| Microphone codec | `picasso-audio-overlay.dtsi:129-134` | `qcom,wcd938x-codec`, micbias1-4 all `2750 mV`; used as `qcom,codec-aux-devs` (`:73`), TX path commented `// wcd9385 TX` (`:23`) |
| Speaker amp (already working) | `picasso-sm7250.dtsi` (TFA9874 @ i2c8 `0x34`), mainline board DTS `:583-597` | Secondary MI2S RX, `qcom,sd-lines = <0 1>` |
| BT controller address source | `/usr/bin/radio-addr` (Denial) reads `/sys/devices/soc0/serial_number` | SOCINFO from SMEM (`SMEM_HW_SW_BUILD_ID`, `serial_num`), node `sm7250-xiaomi-picasso.dts:83-86` `smem@80900000` (2048 KiB) |
| Hardware RNG | **absent from the vendor DT**; clock exists | `GCC_PRNG_AHB_CLK` modelled at `build/linux/drivers/clk/qcom/gcc-sm7250.c:1167,2056`; base address **[unknown]** (SM8250 reference `0x793000`, `sm8250.dtsi:997`) |

## 3. Change plan

### 3a. Device tree — ready to paste into `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`

**3a-1 Notification LED** (uses only existing labels; `pm8150l_lpg` comes from
`pm8150l.dtsi:124`). Add one include at the top, next to the existing `dt-bindings` block
(`:12-19`):

```dts
#include <dt-bindings/leds/common.h>
```

and append the node (e.g. after the `&pm8150l_wled` block at `:643-649`):

```dts
/*
 * Notification LED: PM8150L TRILED, channel 2 is the only populated emitter.
 * Downstream (vendor/qcom/picasso-sm7250.dtsi:628-640) disables red and green
 * and relabels blue as "white", so expose exactly that one channel.
 */
&pm8150l_lpg {
	status = "okay";

	led@2 {
		reg = <2>;
		color = <LED_COLOR_ID_BLUE>;
		function = LED_FUNCTION_INDICATOR;
		default-state = "off";
	};
};
```

If the emitter stays dark at `brightness=255`, set `qcom,power-source = <1>;` on the LPG node
(valid values 0/1/3 per
`upstream/linux/Documentation/devicetree/bindings/leds/leds-qcom-lpg.yaml:70-78`); this is the one
property where the vendor tree is silent — §4, experiment E2.

**3a-2 Hardware RNG** — *do not paste yet*: the register base is unknown (§4, E3). Once confirmed:

```dts
&{/soc@0} {
	rng: rng@793000 {
		compatible = "qcom,prng-ee";
		reg = <0 0x00793000 0 0x1000>;
		clocks = <&gcc GCC_PRNG_AHB_CLK>;
		clock-names = "core";
	};
};
```

**3a-3 Type-C / PD** (grounded: supply is PM8150 LDO2, as on the vendor lito overlay):

```dts
/* Type-C: PM7250B port + PD PHY. PD PHY VDD is PM8150 LDO2 (3.072 V), exactly
 * as the vendor lito overlay wires it (lito-pmic-overlay.dtsi:451-455). PD is
 * left disabled so the current peripheral-mode USB gadget keeps working. */
&pm7250b_vbus {
	status = "okay";
};

&pm7250b_typec {
	vdd-pdphy-supply = <&vreg_l2a_3p1>;
	status = "okay";

	connector {
		compatible = "usb-c-connector";

		power-role = "dual";
		data-role = "dual";
		self-powered;
		typec-power-opmode = "default";
		pd-disable;

		vbus-supply = <&pm7250b_vbus>;
	};
};
```

(`pd-disable`/`typec-power-opmode` follow the only in-tree PM7250B user,
`upstream/linux/arch/arm64/boot/dts/qcom/sm7225-fairphone-fp4.dts:940-955`, which documents the same
role-switch/charger conflict we have.)

**No DT change** is needed for RTC, PMIC temp alarms, zram or socinfo — the nodes already exist and
are enabled; those four are config-only.

### 3b. Config lines — `configs/linux/picasso.config`

**3b-1 Mandatory (fixes S1–S3 findings):**

```kconfig
# Compressed RAM swap. Built in, never =m: the RAM initramfs has no module
# loader, so zram-swap.service dies with "Module zram not found".
CONFIG_ZRAM=y

# Battery-backed clock on the PM8150. Without it the only RTC is rtc-efi,
# whose hctosys read fails and boot time comes from a stale journal stamp.
CONFIG_RTC_DRV_PM8XXX=y

# PMIC die-temperature alarms (PM8150/PM8150L/PM7250B). =m leaves the three
# PMICs out of the thermal framework entirely.
CONFIG_QCOM_SPMI_TEMP_ALARM=y

# /sys/devices/soc0 (serial number, machine, soc_id). socinfo is instantiated
# by qcom_smem, so =m means the device is never created and radio-addr cannot
# derive a Bluetooth address.
CONFIG_QCOM_SOCINFO=y

# Notification LED (PM8150L TRILED). =m is invisible: no module loader.
# QCOM_PBS=m would cap LPG at =m (drivers/leds/rgb/Kconfig:93), so drop it.
CONFIG_LEDS_QCOM_LPG=y
CONFIG_QCOM_PBS=n

# Hardware RNG for Android's prng_seeder (=m gives no /dev/hw_random).
CONFIG_HW_RANDOM_QCOM=y
```

**3b-2 Optional (Type-C/PD, needs DTS §3a-3 as well):**

```kconfig
CONFIG_TYPEC=y
CONFIG_TYPEC_TCPM=y
CONFIG_TYPEC_QCOM_PMIC=y
CONFIG_REGULATOR_QCOM_USB_VBUS=y
```

### 3c. Patches under `patches/linux/picasso/`

**3c-1 `0016-arm64-dts-qcom-picasso-add-pm8150l-lpg-and-pm7250b-typec.patch`** — the DTS hunks of
§3a-1 and §3a-3 plus the `dt-bindings/leds/common.h` include. No C change. Description: enable the
PM8150L LPG controller for the single populated notification-LED channel, and the PM7250B Type-C
port with its PD PHY powered from PM8150 LDO2 while PD stays disabled, so the ECM/USB gadget is
unaffected.

**3c-2 `0017-power-supply-qcom-pm7250b-expose-battery-temperature.patch`** — extend the QG block
added by `0015-power-supply-qcom-pm7250b-add-qg-battery.patch`: add `struct iio_channel *batt_therm`
(optional, `devm_iio_channel_get(dev, "batt-therm")`), a `POWER_SUPPLY_PROP_TEMP` getter that calls
`iio_read_channel_processed()` and returns the value in deci-degrees, and add the property to
`pm7250b_bat_props[]`. Device tree: declare the ADC channel on `&pm7250b_charger`
(`io-channels = <&pm7250b_adc ADC5_BAT_THERM_100K_PU>; io-channel-names = "batt-therm";`) — the
channel *id* is the open question in §4/E1. Effect: `healthd`/upower get a real temperature, and the
charging stack can implement JEITA.

**3c-3 (not a patch — scoping note) microphone capture** is class B: `sm7250.dtsi` has **no**
SoundWire controller nodes and no LPASS RX/TX macro nodes, so `CONFIG_SND_SOC_WCD938X_SDW=y` has
nothing to bind to, and `&sound` has no capture DAI link (`sm7250-xiaomi-picasso.dts:568-598`, four
playback links only). The work is: SM7250 `swr0/swr1` nodes with port tables, `lpass_rx_macro` +
`lpass_tx_macro` (or `lpass_va_macro`) with the SM7250 clock bindings, a `wcd938x` node with
`qcom,micbias1..4` = 2750 mV and reset GPIOs from the vendor overlay
(`~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/picasso-audio-overlay.dtsi:129-134`), plus
`MultiMedia2/3 Capture` DAI links. Estimate 2–4 days of hardware iteration; cheapest first step is
E4.

### 3d. Verification (run from the repo root, read-only on the phone unless stated)

```sh
# after the kernel is rebuilt and RAM-booted again:
ssh … root@<lan-ip> 'systemctl --failed --no-pager'   # expect 0 units
ssh … root@<lan-ip> 'zramctl; cat /proc/swaps'          # /dev/zram0 present
ssh … root@<lan-ip> 'cat /sys/class/rtc/rtc*/name; timedatectl | head -6'
ssh … root@<lan-ip> 'for z in /sys/class/thermal/thermal_zone*; do cat $z/type; done | grep -cE "pm8150|pm7250b"'   # 3
ssh … root@<lan-ip> 'ls /sys/class/leds'                # blue:indicator
ssh … root@<lan-ip> 'cat /sys/devices/soc0/serial_number; systemctl is-active radio-addr-bluetooth'
ssh … root@<lan-ip> 'ls /sys/class/typec; cat /sys/class/typec/port0/data_role; ip -br addr show usb0'
ssh … root@<lan-ip> 'cat /sys/class/power_supply/pm7250b-battery/temp; ls -l /dev/hw_random'
ssh … root@<lan-ip> 'cat /proc/sys/kernel/tainted; journalctl -k -b -p err --no-pager | tail -20'
```

The `--failed` list from the audit baseline (3 units) must become empty except for whatever
class-C items the image fix has not landed yet; `tainted` may legitimately stay `512` until
finding 16 is addressed.

## 4. Unknowns and risks

| # | Unknown / risk | Cheapest experiment |
|---|----------------|---------------------|
| E1 | Which ADC5 channel is the battery thermistor: vendor uses `ADC_BAT_THERM_PU2`; mainline ids offer `ADC5_BAT_THERM_100K_PU` (`0x4a`), `…_30K_PU` (`0x2a`), `…_400K_PU` (`0x6a`) — `upstream/linux/include/dt-bindings/iio/qcom,spmi-vadc.h:144,162,180` | Add all three as `pm7250b_adc` channel nodes, then `cat /sys/bus/iio/devices/iio:device*/in_voltage*_raw` while the phone is at room temperature and pick the one whose NTC curve maps to ~25 °C (one boot, no code) |
| E2 | PM8150L TRILED `qcom,power-source` (0/1/3) is not described by the vendor tree; the in-tree PM8150L user (`qrb5165-rb5.dts:895-917`) omits it | Apply §3a-1 without it; if `blue:indicator` accepts `brightness=255` but stays dark, retry with `qcom,power-source = <1>` (one boot, one DTS line) |
| E3 | PRNG base address on SM7250 (clock is known, address is not) | Add `rng@793000` (SM8250 layout) + `CONFIG_HW_RANDOM_QCOM=y`; success is `dmesg` line from `msm-rng` and a working `/dev/hw_random`; a `-ENODEV`/no-probe result means the base is wrong and the address must come from the SM7250 register map |
| E4 | Whether the SM7250 SoundWire controllers are register-compatible with `qcom,soundwire-v1.5.1` (`sm8250.dtsi:2729-2776`) | Copy `swr1` only, with SM7250 clock bindings, and check `dmesg` for `soundwire` probe + `/sys/bus/soundwire/devices` before touching the codec |
| E5 | Enabling Type-C/TCPM can fight the gadget for the data role | After §3a-3/§3b-2, `ip -br addr show usb0` must stay `UP` and the ssh session must survive; if the link drops, add `pd-disable` (already in the snippet) and keep `data-role = "device"` |
| E6 | `MEMCG_V1`/`CPUSETS_V1=y` would make `/proc/cgroups` match Android's expectation but collide with systemd's cgroup2 mount | Do **not** enable; if Denial insists, test on a scratch boot only: `systemctl set-property user.slice MemoryMax=…` must keep working afterwards |
| E7 | Whether the 45-minute timer is intended (session watchdog) or a bug (it reboots a healthy phone) | Ask the design intent; mechanically it is provable with `systemctl list-timers`, no boot needed |

## 5. Live observations (command → excerpt)

```
$ ssh root@<lan-ip> 'uptime; uname -r'
 19:41:59 up 5 min,  1 user,  load average: 4.57, 4.48, 2.10
 7.3.0-rc7-00001-ge1927d6cbd57-dirty

$ systemctl --failed --no-pager
 ● radio-addr-bluetooth.service loaded failed failed Give Bluetooth controllers without an address the device's own
 ● slot-success.service         loaded failed failed Mark the Linux boot slot successful once the boot has proved itself
 ● zram-swap.service            loaded failed failed Compressed swap in RAM (zram)

$ journalctl -b -u zram-swap --no-pager -o cat
 zram-swap[269]: modprobe: FATAL: Module zram not found in directory /lib/modules/7.3.0-rc7-00001-ge1927d6cbd57-dirty

$ journalctl -b -u slot-success --no-pager -o cat
 slot-success[554]: No boot_a partition.
 slot-success.service: Main process exited, code=exited, status=2/INVALIDARGUMENT

$ journalctl -b -u radio-addr-bluetooth --no-pager -o cat | head -3
 radio-addr: /sys/devices/soc0/serial_number: No such file or directory
 radio-addr-bluetooth.service: Main process exited, code=exited, status=1/FAILURE
 radio-addr-bluetooth.service: Failed with result 'exit-code'.

$ date; timedatectl | head -3
 Tue Nov 17 19:42:24 +07 2026
 Failed to query server: Failed to read RTC: Input/output error

$ journalctl -k -b -o cat | grep -E 'hctosys|System time advanced|jumped'
 rtc-efi rtc-efi.0: hctosys: unable to read the hardware clock
 systemd[1]: System time advanced to timestamp on /var/lib/systemd/timesync/clock: Tue 2026-11-17 14:47:45 +07
 systemd-journald[241]: .../user-1000.journal: Realtime clock jumped backwards relative to last journal entry, rotating.

$ journalctl -k -b -o cat | grep -E 'venus|adsp.mdt|crbtfw'
 qcom-venus aa00000.video-codec: Direct firmware load for qcom/sm7250/xiaomi/picasso/image/venus.mdt failed with error -2
 qcom-venus aa00000.video-codec: probe with driver qcom-venus failed with error -22
 remoteproc remoteproc0: Direct firmware load for .../adsp.mdt failed with error -2
 Bluetooth: hci0: QCA Failed to request file: qca/crbtfw32.tlv (-2)
 Bluetooth: hci0: QCA setup on UART is completed          # later, after the firmware mount
 bound aa00000.video-codec ; remoteproc0: Booting fw image .../adsp.mdt, size 9340

$ ls /sys/class/leds | wc -l ; ls /sys/class/thermal | grep -c thermal_zone
 0
 27

$ for z in /sys/class/thermal/thermal_zone{0,1,2}; do echo "$(basename $z) $(cat $z/type) $(cat $z/temp)"; done
 thermal_zone0 aoss0-thermal 48900
 thermal_zone1 cpu0-thermal 47600
 thermal_zone2 cpu1-thermal 47900

$ cat /sys/class/power_supply/pm7250b-battery/{status,voltage_now,current_now,capacity}
 Charging / 4347606 / 46234 / 96
$ cat /sys/class/power_supply/pm7250b-battery/temp
 cat: .../temp: No such file or directory

$ ls /sys/class/typec
 ls: cannot access '/sys/class/typec/': No such file or directory

$ cat /proc/sys/kernel/tainted
 512
$ journalctl -k -b -o cat | grep -A1 'WARNING: drivers/clk/clk.c:1257'
 WARNING: drivers/clk/clk.c:1257 at clk_core_disable+0xa4/0xac, CPU#3: kworker/u32:1/59
  dsi0_phy_pll_out_dsiclk already disabled

$ cat /proc/cgroups            # no memory, no cpuset
 cpu 0 203 1 / cpuacct 0 203 1 / blkio 1 3 1 / devices 0 203 1 / freezer 0 203 1 …

$ cat /sys/power/mem_sleep   # s2idle only; no suspend attempted this boot
 [s2idle] freeze mem disk

$ systemctl list-timers --all --no-pager | grep picasso
 Tue 2026-11-17 20:21:16 +07  37min -  -  picasso-watchdog.timer  picasso-watchdog.service

$ iw reg get | head -3
 global
 country 00: DFS-UNSET

$ aplay -l | head -3 ; arecord -l | head -3
 card 0: XiaomiRedmiK305 [Xiaomi-Redmi-K30-5G], device 0: MultiMedia1 (*) []
 **** List of CAPTURE Hardware Devices ****            # (no device follows)
```

Notes for reproducing: this boot's `dmesg` ring has already wrapped (it holds only ~20 min of
kmsg), so `journalctl -k -b` is the authoritative source for the early messages quoted above;
`/proc/config.gz` exists on the phone and matches `build/linux/out/.config`, which makes every
config claim in this document verifiable at runtime.
