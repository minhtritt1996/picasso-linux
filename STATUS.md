# Status

## M0 — build from upstream: DONE

`./build.sh linux` / `loader` / `bootimg` build from `upstream/` + `patches/`:
`Image` 53 MB (PE32+), `sm7250-xiaomi-picasso.dtb`, `linux.efi` (patched XnuLoader),
`initrd.cpio.gz`, `payload-linux.bin` (XNUP), `boot-linux.img` (Android boot v1).

## M1 — boot vehicle: DONE (verified on hardware)

- `fastboot boot build/out/boot-linux.img` → Mu-Silicium → XnuLoader → Linux EFI stub.
- Kernel boots to userspace (init banner), 8 CPUs, 5305 MiB RAM, simpledrm console.
- Loader stays resident with its fastboot server (`stage` + `oem run`), `reboot`
  from Linux returns to it (boot image installed on `boot`).
- Kernel console is mirrored into RAM (`0xF3F00000`, 1 MiB reserved) and the loader
  folds it into its log at start-up: readable with `fastboot get_staged` after a
  reset, and persisted in the `logdump` partition.

## M2 — USB console: DONE (verified on hardware)

- dwc3 + qcom glue + HS PHY probe (`CONFIG_INTERCONNECT_QCOM_SM7250=y`,
  PM8150 rails L2A/L5A/L12A, `dr_mode = "peripheral"`).
- Init builds a CDC-ACM gadget and serves a console on `/dev/ttyGS0`:

```sh
stty -F /dev/ttyACM0 raw -echo
printf 'help\r' > /dev/ttyACM0 & cat /dev/ttyACM0
```

  Commands: `help`, `uname`, `mem`, `cat FILE`, `reboot`.
- `reboot` (also `auto_reboot=<s>` on the cmdline) resets to the loader in ~10 s.

## Known blockers

- **TLMM**: only the **south** tile answers; reads of the west (`0xf500000`) and
  east (`0xf900000`) windows never return and hang the worker. The driver now
  skips tiles the board leaves out of the mapping, and the picasso DTS describes
  only `south` (touch reset/IRQ gpio4/5, panel TE gpio10, UFS reset). The probe
  completes (`probe of f100000.pinctrl ... returned 0`). West/east pins (camera,
  some QUP) are unavailable until the correct bases/clocks are found.
- `arm-smmu 3da0000` (Adreno SMMU) fails with -110 at ~10.7 s; harmless so far,
  GPU needs it later.
- **Display: no kernel frame reaches the panel.** MDSS/DPU/DSI/PHY/panel all
  probe, `msmdrmfb` gets fbcon (135x150), the DSI video engine runs
  (`STATUS0 0x201b`, `VIDEO_MODE_ENGINE_BUSY=1`) with DSC enabled
  (`VIDEO_COMPRESSION_MODE_CTRL 0x4380b01`, slices 2 x 540 chunk, 1080 B/pkt,
  pkt/line 1), the panel takes init + PPS + 0x11(70 ms)+0x29. Screen stays on the
  loader splash: the vendor panel node sets `qcom,bl-update-flag =
  "delay_until_first_frame"`, so the logo means **no frame ever arrived** (not
  "stale LCD"). Signals that mislead: DSI writes report success (fire-and-forget),
  DCS read fails with -110 because the cmd engine is blocked by the video engine,
  `DSI_IRQ_VIDEO_DONE` is never unmasked in video mode.
  - Fixed `drm_dsc_config.pic_width/pic_height` (were 0 -> PPS said 0x0). Still black.
  - Experiment switch in the panel driver: `G7A_NO_DSC 1` (uncompressed link,
    120 Hz, 2.08 Gbps/lane; 60 Hz = 1.04 Gbps/lane also tried).
  - `tools/linux/init.c` now paints `/dev/fb0` with `/dev/urandom` (2 s after
    start): snow => scanout reaches the panel, logo => data path dead. Result
    pending on hardware.
  - Enabling the PM8150L WLED driver with guessed properties turned the backlight
    off - leave it to the bootloader until the correct values are known.
### Wi-Fi (WCN3990) — in progress

- Board DTS: WLAN rails `l1a` 0.752 V, `l14a` 1.8 V, `l2c` 1.304 V, `l10c`/`l11c`
  3.312 V; `&wifi` (ath10k_snoc) and `&mpss` enabled.
- `CONFIG_REMOTEPROC`, `QCOM_RPROC_COMMON`, `QCOM_Q6V5_PAS`, `ATH10K_SNOC`,
  `CFG80211`/`MAC80211`/`RFKILL` are **built in** (the RAM boot has no modules).
  Tristate traps: `CFG80211` is capped at `m` while `RFKILL=m` (`depends on
  RFKILL || !RFKILL`); `QCOM_Q6V5_MSS` needs `QCOM_SYSMON=n`/`RPMSG_QCOM_SMD=n`;
  `qcom,sm7250-mpss-pas` is driven by `qcom_q6v5_pas`, not `qcom-q6v5-mss`.
- There is **no `vendor` partition** (dynamic partitions), so init mounts the
  **modem** partition (`/dev/sde48`, vfat) at
  `/lib/firmware/qcom/sm7250/xiaomi/picasso`; it carries `image/` with
  `modem.mdt`, `adsp.b*`, `wlanmdsp.mbn`, `bdwlan.bin` (verified present) but no
  `crbtfw32.tlv`. Init rebinds `4080000.remoteproc` after the mount and writes
  `start` to `remoteproc0/state`.
- Result: `remoteproc0: Booting fw image .../modem.mdt`, `remote processor modem
  is now up`, state `running`. After ~40 s: `dog_hal_common.c:180:DOG detects
  stalled initialization` — the modem needs **rmtfs** (EFS) and **pd-mapper**
  (PD map, the `modemr.jsn`/`adspr.jsn` files are in the image dir). Next:
  cross-build `linux-msm/rmtfs` + `linux-msm/pd-mapper` (libqrtr, AF_QIPCRTR),
  add `rmtfs_mem` reserved region (client-id 1, vmid 15), start them before
  `remoteproc0/state`.
- Done: static aarch64 `rmtfs` (udev replaced with the `/sys/class/rmtfs/...`
  sysfs path), `pd-mapper` (lzma stubbed, plain `.jsn` only) and `tqftpserv` in
  `tools/linux/bin/`, packed into the initramfs by `mkinitramfs.sh`; EFS images
  copied from `modemst1`/`modemst2`/`fsc`/`fsg` into `/run/rmtfs` (flash is never
  written). rmtfs answers the modem's EFS requests, pd-mapper loads
  `adspr/adsps/adspua/cdspr/modemr/modemuw.jsn`, QRTR works.
- Still blocked: the modem trips its own watchdog ~40 s after start
  (`dog_hal_common.c:180:DOG detects stalled initialization`); it never asks
  pd-mapper for a domain list before that, and tqftpserv serves nothing, so the
  stall is inside the modem firmware init, not the host services. Next: capture
  the modem's dump (`CONFIG_QCOM_Q6V5_PAS` dump collection) or compare the
  sequence against Android's `rmtfs` (raw-partition mode).

### Display — first real output, then a pre-init hang

- With `G7A_NO_DSC=1` (uncompressed 120 Hz) the panel showed a **full white
  screen** for the first time: the DPU/DSI path does move pixels. White is
  consistent with the panel decoding a raw stream while the bootloader left its
  DSC decoder on.
- Booting with DSC re-enabled (fixed PPS) never reached init: no USB gadget, no
  `auto_reboot`, so the machine needed a manual power reset. Same for a
  display-disabled build with all three pinctrl tiles mapped, so the hang is not
  the display alone.
- The display pipeline is now disabled in the board DTS (`&mdss { status =
  "disabled"; }`) until the pre-init hang is understood; the panel driver stays
  compiled.
- Reminder: `patches/linux/picasso/0005-pinctrl-msm-skip-unmapped-tiles.patch`
  must stay applied - restoring `pinctrl-msm.c` from the vendor baseline made
  the pinctrl probe fail with `-EINVAL: invalid resource (null)`, which defers
  UART/DSI/UFS (all pinmux consumers) and looks like a dead board.

### ADSP + modem both running (audio prerequisite)

- `&adsp { firmware-name = "qcom/sm7250/xiaomi/picasso/image/adsp.mdt"; status = "okay"; }`;
  `CONFIG_QCOM_Q6V5_PAS=y` already covers the `qcom,sm7250-adsp-pas` node.
- init starts remoteprocs **by name** (adsp then modem) and the monitor logs both:
  `state: adsp = running`, `state: modem = running` at 6/9/16 s.
- With ADSP enabled it probes first, so `remoteproc0` is the adsp and the modem is
  `remoteproc1` - hard-coded `remoteproc0` paths start the wrong rproc.
- Next for audio: q6apm/q6prm + `sound` card + WCD938x (SoundWire) + WSA881x +
  tfa98xx (i2c 0x34); for Wi-Fi: ath10k still does not create wlan0, needs the
  modem's wlan pd (pd-mapper/QMI) - the modem still DOGs at ~44 s.

### Audio — stack in, card blocked by the amp's pins (TLMM)

- Config (built in, no modules): SOUND/SND/SND_SOC, QCOM_APR, SND_SOC_QCOM(+COMMON,
  SDW), all SND_SOC_QDSP6_* (AFE/ADM/ASM/APM/PRM), SND_SOC_SM8250, WCD938X(+SDW),
  WSA881X, TFA9874, SOUNDWIRE(+QCOM), SND_USB_AUDIO + SND_SOC_USB (SM8250 machine
  driver requires the USB-offload helpers at link time).
- DTS: `&sound { compatible = "qcom,sm8250-sndcard"; ... }` with mm1 -> q6asmdai
  MM1 and a Secondary-MI2S link (pins gpio70/71/72/74, `sec_mi2s`), TFA9874 amp on
  a new `i2c8` (SE8, 0x988000, gpio49/50 `qup12`, reset tlmm58) and
  `&q6asmdai { dai@0..7 { direction = <Q6ASM_DAI_RX>; } }` (the driver needs one
  child per frontend dai: without them it fails "No dais found in DT").
- Verified: APR/q6core/q6afe/q6adm/q6asm all probe; the card now defers only on
  `Secondary MI2S Playback: codec dai not found` -> the TFA9874 never answers,
  because its i2c pins 49/50 are WEST-tile pins that the TLMM override drops.
- ADSP (remoteproc0) also trips `USER-PD DOG detects stalled initialization` at
  ~46 s, the audio twin of the modem DOG.

### TLMM — the wall that blocks BT, touch and the amp

- Only the window `0xf100000 + 0x300000` is safely accessible; mapping
  `0xf000000`, 16 MiB, or vendor-style offsets (WEST 0x400000 / EAST 0x800000)
  inside a larger window all abort before init.
- Because every pin fits in that 3 MiB window at `0x1000*id`, the flat layout is
  the only viable one, but any attempt to give the EAST/WEST groups their offsets
  in the same window still kills the boot at ~0.33 s (before pinctrl probes).
- Until this is cracked, BT (uart5 38-41, swctrl 73), touch (4/5/9) and the amp
  (49/50) cannot be muxed; Wi-Fi/ADSP/modem/display paths do not depend on it.

### Vendor ground truth (Droidian dmesg + picasso-lito-combined.dtb)

- TLMM: one node `pinctrl@f000000`, `reg = <0xf000000 0x1000000>`, flat
  `qcom,lito-pinctrl`; the vendor driver bakes tile offsets
  (SOUTH 0x100000 / WEST 0x500000 / EAST 0x900000) into the per-pin registers.
- The TFA98XX amp is on **i2c 988000 (SE8), addr 0x34** at 400 kHz
  (`tfa98xx 1-0034`, `i2c_geni 988000.i2c: Bus frequency ... 400000Hz`) with pins
  gpio49/50 (`qup12`, WEST tile) and reset tlmm 58 - matches our DTS.
- Touch is `NVT-ts-spi` (Novatek SPI) in the vendor kernel; BT uses the 4-wire
  QUART (`894000.qcom,qup_uart`).
- So the vendor kernel muxes WEST/EAST pins on the same hardware: the register
  addresses are right, and our failures are in how mainline reaches them, not in
  the platform map.
- Loader now has `oem peek <hex>` (reads one word, replies the value, exception
  hook prints the fault). It must be exercised from a **probe payload** (XNUP with
  loader.efi + usb and no kernel), because the loader only serves fastboot while
  it waits before `oem run`.

### TLMM probe result: the EAST/WEST windows ARE readable

- Kernel-side `ioremap + readl` initcalls (temporary, in the printk patch):
  - `0xf526000` (WEST, gpio38 ctl area) -> `0x00000000`, mapped;
  - `0xf926000` (EAST, gpio38 under the vendor offset) -> `0x00000001`, mapped.
  Both boots reached init with the USB gadget up, so there is **no XPU or
  hypervisor block** on those windows.
- Therefore every earlier "multi-tile" crash is a **software bug** in the
  pinctrl/gpiochip path, not the address map, and the flat vendor layout
  (one window from 0xf100000 with SOUTH 0 / WEST 0x400000 / EAST 0x800000) is
  the correct target - it still dies before init today, so the next step is to
  find what the driver does differently when all groups share one tile
  (watch `msm_gpio_get_direction` in the gpiochip-add loop and the SDC/UFS
  groups whose ctl offsets are absolute).
- Quick unblock path if that takes too long: a small late_initcall that writes
  the handful of mux registers directly (BT gpio38-41 qup05 + gpio73, amp
  gpio49/50 qup12), bypassing pinctrl entirely, to prove BT/amp on hardware.

### Audio: sound card + amp are up (playback click pending)

- Missing built-in driver was the blocker: `CONFIG_I2C_QCOM_GENI=m` meant **no i2c
  buses at all** in the RAM kernel, so the TFA9874 never probed and the card stayed
  deferred. With `I2C_QCOM_GENI=y` (+`SPI_QCOM_GENI=y`) the amp binds
  (`/sys/bus/i2c/devices/0-0034`) and the card registers:
  `0 [XiaomiRedmiK305]: sm8250 - Xiaomi-Redmi-K30-5G`,
  `/proc/asound/pcm: 00-00: MultiMedia1 (*) : : playback 1`,
  `input: ... Headset Jack`.
- `&adsp` runs and PDR reports `msm/adsp/audio_pd state 0x1fffffff` at ~4.4 s
  (again at ~44 s after the ADSP user-PD DOG restarts it).
- `tools/linux/tone` + `tone` console command exist; `open("/dev/snd/pcmC0D0p",
  O_RDWR)` returns EINVAL today (node is controlC0/pcmC0D0p/timer, dev 116:2) -
  next: try O_WRONLY, then instrument which `snd_pcm_open` path rejects it.
- `drivers/soc/qcom/picasso-pinmux.c` muxes the pads pinctrl cannot (uart5
  38-41 qup05, i2c8 49/50 qup12) with `picasso_pinmux.mode=read|write`; reads of
  `0xf526000`/`0xf926000` work, writes execute without faults.

### Bluetooth: chip talks, patch download stalls

- With `bootcpu=1` the loader watches the kernel from core 0, and a hung/killed
  child returns to fastboot without a human (measured ~48 s), while the kernel
  keeps running on core 1 - which makes `oem ramoops` a *live* kernel-log read.
- `&uart5` stays stable now (the missing `wakeup-parent = <&pdc>` on the TLMM was
  one of the causes) and the pads are muxed by `picasso-pinmux`.
- The chip answers: `compatible = "qcom,wcn3991-bt"` (not wcn3990) makes the
  version read succeed - Product 0x0a, SOC 0x40010320, ROM 0x0302, Patch 0x0de9.
- Firmware must live under `/lib/firmware/qca/` (the driver prefixes `qca/`), so
  init mounts the bluetooth partition at `/lib/firmware/qca` and the DT uses
  `firmware-name = "image/crnv32.bin", "image/crbtfw32.tlv"` (property order is
  **nvm, rampatch**; the driver's [0] is the nvm).
- Remaining: the rampatch TLV transfer (`command 0xfc00 tx timeout`,
  `Failed to send TLV segment -110`) after the version read, at 115200 and 3 M,
  with and without `uart-has-rtscts`. Each setup also logs one
  `Frame reassembly failed (-84)` (the 0xFD wake byte). Next: teach `qca_recv()`
  to drop the 0xFD wakeup byte, and/or check CTS/RTS levels during the TLV send.

### TLMM (pinmux) — root cause of the silent BT/touch

- `sm7250-xiaomi-picasso.dts` overrides `&tlmm` to a single `south` window at
  `0xf100000`; `pinctrl-msm` then returns 0 for every pin whose group sits on an
  unmapped tile (EAST: BT uart5 gpio38-41 + swctrl 73, touch reset 4/IRQ 5;
  WEST: touch CS 9, panel TE 10). No error anywhere - this is why the WCN3990
  never saw a muxed UART.
- Hardware bisect (initcall_debug, step prints in `msm_pinctrl_probe`):
  - single-tile window: probe reaches `enabled`, `gpio init ... chip added
    ret 0`, and pinmux writes for gpio38-41/73 execute;
  - two or three tiles mapped (south/west/east at 0xf100000/0xf500000/
    0xf900000): the kernel dies **inside `devm_gpiochip_add_data`**, right after
    "adding chip", before any pinmux write;
  - a multi-tile boot with gpiochip init skipped survives 30 s and muxes the
    EAST pins fine, so the east/west windows themselves are accessible;
  - flat `0xf000000`, `wakeup-parent` removed, shifted offsets: all still die.
- Next: instrument `of_gpiochip_add` / `gpiochip_add_irqchip` /
  `gpiochip_setup_dev` to find the multi-tile gpiochip crash, then BT/touch
  pinmux works.

### Bluetooth (WCN3990) — not started

Vendor node: `qca,wcn3990`, `qca,bt-sw-ctrl-gpio = <&tlmm 73>`, supplies
`pm8150_s4` (io), `l2c` (core), `l10c` (pa), `l14a` (xtal). The firmware lives
on the dedicated **`bluetooth` partition** (`/dev/sde33`, vfat): init mounts it
at `/lib/firmware/bt` and `bt/image/{crbtfw32.tlv,crnv32.bin,crnv32u.bin}` are
present. Next: serdev `hci_qca` on the BT UART (find the vendor UART node),
`qca,bt-sw-ctrl-gpio` = tlmm 73, then `hciattach`/`btmgmt`.
- Done: `&uart5` (SE5, `serial@894000`) enabled with the `qup_uart5_default`
  pins, `aliases { serial1 = &uart5; }` (geni-serial needs it, otherwise
  `Invalid line -19`), BLUETOOTH child `qcom,wcn3990-bt` with
  `vddio=s4a`, `vddxo=l14a`, `vddrf=l2c`, `vddch0=l10c`,
  `swctrl-gpios = <&tlmm 73>` and `firmware-name = "bt/image/crbtfw32.tlv",
  "bt/image/crnv32.bin"`. Kernel: `BT`, `BT_HCIUART`, `BT_HCIUART_SERDEV`,
  `BT_HCIUART_QCA`, `SERIAL_QCOM_GENI` built in.
- Result: `894000.serial: ttyHS1 ... is a MSM`, `Bluetooth: hci0: setting up
  wcn399x`, then `command 0xfc00 tx timeout` /
  `Reading QCA version information failed (-110)` in a retry loop - the chip
  does not answer on the UART. Next: verify the four rails are actually enabled
  (regulator state), the `swctrl-gpios` direction the driver requests, and
  whether the chip needs an explicit enable line/`qca,bt-sw-ctrl` edge.

### Touch (Novatek NT36672C, SPI) — not started

Vendor: `spi@984000` = GENI SE wrapper1/SE1 (currently `i2c1` in our dtsi),
5 MHz, reset `tlmm 4`, IRQ `tlmm 5` (flag 0x2001), `reset-tddi =
pm8150l_gpios 3`, CS on `gpio 9`. Mainline `novatek-nvt-ts` is **i2c-only**
(NT11205/NT36672A) and needs the vendor firmware
`novatek_nt36672c_g7a_fw0{1,2,3}.bin`, so a SPI driver has to be written
(reference: downstream `NVT-ts-spi` + `drivers/input/hakios.novatek.c`).

### Audio — not started

Vendor overlay: `wsa881x_0211..0214` (SoundWire speakers), `nxp,tfa98xx` on i2c
(0x34), WCD938x codec. Mainline has `wcd938x`, `wsa881x`, `tfa9874` and
`q6apm/q6prm`; the ADSP (`sm7250-adsp-pas`, firmware `adsp.b*` in the modem
image dir) must come up first, which depends on the same rmtfs/pd-mapper stack
as the modem.

- Touch, Bluetooth and audio are not started.
- `userdata` is mounted read-only (f2fs). Writing a log file there needs a
  read-write mount; the console/RAM log already survives resets, so that is a
  separate decision.

## Verified on hardware

| What | Evidence |
|---|---|
| kernel to init | panel banner, `Run /init as init process` |
| USB gadget | host sees `1d6b:0104`, `/dev/ttyACM0` at t≈2-4 s |
| console | `uname` → `Linux 7.3.0-rc7 ... aarch64`, `mem` → 5432852 kB |
| console reboot | `reboot` → loader (`product: xnuloader`) in 10 s |
| log persistence | `get_staged` shows `== previous kernel console ==` after reset |
| UFS | `ufshcd-qcom 1d84000.ufshc` probe 0; 6 LUNs, GPT partitions scanned (`sda1..sda35`, `sde1..sde62`) |
| userdata | mounted **read-only**: `sda34` found by PARTNAME, `mount (f2fs, ro)`; klog: `userdata /dev/sda34 mounted read-only` |
| display | MDSS/DPU 6.1.0 (`dpu hardware revision:0x60010000`), DSI + 7nm PHY, panel `panel-xiaomi-g7a` bound; `msmdrmfb` fb0 registered on minor 1 |

## Bring-up matrix (2026-11-14, measured on hardware)

| Subsystem | Evidence today | Remaining |
|---|---|---|
| GPU | `arm-smmu 3da0000` probes, `adreno 3d00000.gpu` binds, GMU present, `a650_sqe.fw`/`a650_gmu.bin`/`a620_zap.mbn` load, **`/dev/dri/renderD128` exists** | userspace acceleration (Mesa/freedreno) + a render smoke test |
| Audio | card `XiaomiRedmiK305` (snd-sm8250), TFA9874 binds (`0-0034` on i2c0), ADSP PAS running, APR q6core/q6afe/q6asm/q6adm probe, PDR audio_pd up | PCM open returns EINVAL before `soc_pcm_open`/`q6asm_dai_open` (both instrumented, never called) - DPCM FE path; ADSP user-PD DOG every ~44 s |
| Bluetooth | chip answers Product 0x0a / SOC 0x40010320 / ROM 0x0302 / patch 0x0de9 with `qcom,wcn3991-bt`; firmware from `/lib/firmware/qca` with `firmware-name = nvm, rampatch` | rampatch TLV send times out (`0xfc00`); one 0xFD frame-reassembly error per setup |
| Wi-Fi | modem PAS boots, rmtfs serves EFS, pd-mapper answers domain lists (node 1 = modem), ADSP up | modem watchdog DOG at ~45 s then `can't start rproc modem: -110`; no wlan0 |
| Display | DSI timing/DSC correct, DPU busy, PLL set_rate 600 MHz + 801.88 MHz, colour-quadrant test painted into fb0 | one human look: four coloured quadrants / white / logo |

Iteration aids: `bootcpu=1` makes the loader watch the kernel from core 0 and
return to fastboot on its own (~48 s) while the kernel keeps running, so
`oem ramoops` becomes a live kernel-log read; `tone` console command; colour
framebuffer pattern; `0001..0007` patch series re-verified from a clean tree
(`[OK] series applies to a clean tree and the DTB builds`).

### Display — FIXED (2026-11-14): panel reset polarity + init-before-video

Two real bugs, both proven on hardware:

1. **The panel was held in reset.** `reset-gpios = <&pm8150l_gpios 3 GPIO_ACTIVE_LOW>`
   but the driver drove the logical sequence `1, 0, 1` and left it at 1 — with
   ACTIVE_LOW that is physical **low** = reset asserted. Every DCS command timed
   out (`dsi_cmds2buf_tx ... ret=-110`, `power mode pm=0xff`) and the panel
   stayed dark while the DPU happily scanned out frames
   (`INTF_FRAME_COUNT` 1414 → 1842, `INTF_STATUS` en=1, fb0 painted,
   `fbsum` 65536/65536 nonzero words). Fix: drive `0, 1, 0` (end deasserted) in
   `prepare`, assert in `unprepare`. After that the panel answers:
   `prepare: power mode pm=0x9c err=0`.
2. **The DSI video engine started before the panel was initialised.** The msm
   bridge chain ran `msm_dsi_host_enable()` in `atomic_pre_enable`, i.e. before
   the panel bridge's `drm_panel_prepare()/enable()`, so the panel's exit-sleep
   and display-on commands were issued on an already-running video link. Moved
   the host enable into a new `.atomic_enable` callback, so the order is now
   panel init → display on → video stream.

Result on hardware: console text on screen, framebuffer noise, then the
red/green/blue timeline. Display bring-up is done for the current (DSC native,
120 Hz) configuration; DSC-off 60 Hz also works as a fallback fork.

### Audio — amp path works end to end, no sound yet, ADSP watchdog is the wall

Verified on hardware (2026-11-14):

- **TLMM access model.** Kernel `ioremap` writes to the vendor tile windows work
  (0xf100000 + SOUTH 0x100000 / WEST 0x500000 / EAST 0x900000); the *same*
  writes through `/dev/mem` (Normal-NC, merging) stall the bus and lose a CPU.
  Never debug TLMM with `/dev/mem` writes — use a kernel driver.
- **Pin mux** (`drivers/soc/qcom/picasso-pinmux.c`, runs before GENI probes):
  uart5 38-41 (east), i2c8 49/50 (west), secondary MI2S 70/71/72/74 (west,
  function 1, 8 mA) and the amp reset gpio58 (west, GPIO, output).
- **Amp reset polarity**: the TFA9874 reset is **active high** and idles low
  (vendor: `GPIOF_OUT_INIT_LOW`, pulses 1 → 0). Driving it high holds the amp
  in reset and every i2c read NAKs (`-ENXIO: failed to read revision`).
  The DT now uses `GPIO_ACTIVE_HIGH`.
- With reset released the amp answers on i2c8 (0x34), the card binds, the route
  `SEC_MI2S_RX Audio Mixer MultiMedia1` is set, `hw_params` and PCM `write`
  succeed, and the amp reports `status 0x0017 clk 0xe2c2 sys 0x0018`
  (CLKS stable, AMPE+DCA on) with 16-bit/32-BCK TDM config. No audible output
  yet: either the DSP is not putting samples on the wire or the amp is fed from
  the other data line.
- **ADSP watchdog**: `qcom_q6v5_pas ... fatal error: err_qdi.c:1045:
  EF:sensor_process:0x1 ... USER-PD DOG detects stalled initialization`
  kills and recovers the ADSP every ~20 s on its own, which also tears down any
  audio stream (`ASoC error (-5)`, `fail to close AFE port`). The sensor PD
  inside the Xiaomi ADSP image never finishes init — the usual cause is the
  missing rmtfs/pd-mapper services that the DSP expects. Playing the tone
  within the first seconds after boot succeeds without the crash.

### Buttons — all three work (2026-11-14)

`dmesg input` after the fix:

| device | input node | key code | button |
| --- | --- | --- | --- |
| `pm8941_pwrkey` | `.../pon@800/pwrkey` | 116 `KEY_POWER` | power (KPDPWR_N of the PM8150 PON) |
| `pm8941_resin` | `.../pon@800/resin` | 114 `KEY_VOLUMEDOWN` | volume down (RESIN_N) |
| `gpio-keys` | `gpio-keys` | 115 `KEY_VOLUMEUP` | volume up (PM8150L GPIO5, pull-up) |

Verified by pressing each key twice and reading every `/dev/input/event*`
(`keys 40` in the initramfs console): each produced a DOWN/up pair on the right
device with the right code.

The one non-obvious dependency: the pwrkey/resin children get their regmap from
the `pon@800` node, so **`CONFIG_POWER_RESET_QCOM_PON` must be built in** (it
was `=m`, and the RAM initramfs has no module loader — with it missing both
children stayed unbound and no power/volume-down key ever appeared). Enabling
`CONFIG_RTC_DRV_PM8XXX=y` instead hangs the boot before the console; that
driver is not needed for the keys.

### Touch (NT36672C over GENI SPI SE7) — communication works, reports do not

What is proven on hardware (2026-11-14):

- SE7 (0x984000) is the touch's SPI controller; its four pads are gpio6/7/8/9
  (function `qup11`, 6 mA) and they are muxed by `picasso-pinmux.c`, because
  the TLMM tiles those pins live in are not mapped by mainline pinctrl here.
  Muxing only 6/7/8 is not enough: without gpio9 (CS) every read returned
  `ff ff ff ff ff ff`.
- With CS muxed the controller answers: id `0c 00 00 72 66 03`, which matches
  the vendor trim-id table entry for the NT36675 memory map, i.e. the event
  buffer at 0x22d00 (the 0x11a00 of the NT36672A map reads all zeros here).
- Reset (tlmm 4) and interrupt (tlmm 5) are on TLMM pins the pinctrl driver does
  not map; the early mux drives reset low then high and the driver polls instead
  of using the interrupt line.
- `nvt_bootloader_reset` (0x5a to 0x7fff80) and disabling the "read fast" path
  (0 to 0x3f310) are implemented as the vendor does.

Not working yet: no report with a valid finger slot is ever seen — the reader
scans the four candidate event buffers and never scores one, so the controller
is not producing touch reports. Next things to check, in order: the vendor's
`nvt_read_fw_info`/fw-status handshake (the controller may still be in its
bootloader after our reset, not in its flash firmware), the interrupt pad's
pull configuration (vendor uses bias-pull-down), and whether this panel needs
its firmware re-flashed (the vendor driver carries nt36672c_g7a_fw0X.bin).

Driver: `drivers/input/touchscreen/picasso-nvt-spi.c`, enabled with
`CONFIG_TOUCHSCREEN_PICASSO_NVT=y`.

### Resident ramdisk for Linux — loader support in place, boot still fails

The loader (`build/loader/xnuloader/loader.c`, flashed into `boot` on 2026-11-14)
now does what the XNU path always did, for Linux:

- `boot_linux()` no longer requires an `initrd` entry. When the payload has none
  and the host armed `oem ramdisk`, it takes the resident image at
  `win_base + RESIDENT_RD_OFFSET` (`0x08000000`), patches
  `/chosen/linux,initrd-start|end` **in place** (`patch_linux_initrd()`, an FDT
  walker; the packer writes 8-byte zero placeholders so no resize is needed) and
  boots. Capacity is `win_size - 0x08000000 - 0x30000000` ≈ 4.3 GiB.
- Every early return sets a numbered reason, visible with
  `fastboot getvar result` → `linux-fail N` (1 missing image/initrd/dtb,
  2 /chosen patch, 3 install_configuration_table, 4 LoadImage).
- `tools/linux/build-bootimg.py --initrd` is optional; `PAYLOAD_NO_INITRD=1
  ./build.sh bootimg` builds such a payload (60 042 232 B).

Verification so far: the flashed loader boots the normal payload (initrd inside
the container) and the kernel comes up with its USB-ACM console. The resident
flow (`oem ramdisk` + stage initrd + stage payload + `oem run`) makes the loader
start the kernel, but the phone ends up back at the loader with no console and
`result: idle`, i.e. a reset, not one of the numbered bails — so the /chosen
patch is accepted and the kernel dies afterwards. Next: read the screen during
that run (loader early-exception dump vs kernel text), and check whether the
kernel sees the range as reserved (`dmesg | grep -i initrd` on a boot that
reaches userspace).

**Workspace invariant note:** the `boot` partition was written (user's explicit
instruction) so loader changes take effect on `reboot`; the running system is
still RAM-only apart from that one boot image.

#### Resident path: breadcrumb result

A reset-surviving breadcrumb word (`0xF3FF0000`, printed at loader start,
`0x1000`…`0x1005` through `boot_linux`) shows the loader gets to **0x1005**,
i.e. `/chosen` was patched and `LoadImage` succeeded; the phone then resets
while the kernel is entering (no console text, no `linux-fail` code). So the
DTB side is fine and the fault is in how the kernel meets the resident region:
the loader *claims* the window (`choose_window`, `allocate_pages`) for the XNU
path, and for Linux that big `EfiLoaderData` block — with the initrd inside it —
is handed to the kernel as usable memory. Next things to try, in order:
(1) do not claim the window in the Linux path (only choose an address, pass it
via /chosen, let the kernel treat that range as reserved through the initrd
properties); (2) build with `CONFIG_EFI_DEBUG` so the stub's own messages appear;
(3) compare with the XNU path, which uses the same resident image but ignores
the EFI map entirely.

### Resident ramdisk for Linux — WORKS (2026-11-14)

```
fastboot oem ramdisk
fastboot stage build/out/initrd.cpio.gz      # resident image, up to ~1.1 GiB
fastboot stage build/out/payload-linux.bin   # built with PAYLOAD_NO_INITRD=1
fastboot oem run
```
→ `dmesg`: `Freeing initrd memory: 1592K`, console up, `uname` answers. The payload
carries no initrd at all; the loader rewrote `/chosen/linux,initrd-start|end`
with `win_base + 0x08000000` and the staged size.

The bug that took three flashes: `choose_window()` claimed the **largest free
block (~5 GiB, capped at MAX_MEM_SIZE)** for every payload, which is right for
XNU (whose kernel gets copied into that window) but starves UEFI on the Linux
path — `LoadImage` still succeeded, yet the kernel reset the phone in under a
second, before printing anything (breadcrumb stopped at `0x1005`, empty
ramoops). For Linux payloads the window is now capped at **2 GiB**
(`RESIDENT_RD_OFFSET` 128 MiB + `RESIDENT_RD_RESERVE` 768 MiB + ~1.1 GiB of
resident image), leaving ~3 GiB to the firmware and the kernel.

Debug tools that made this findable, all loader-side:
`fastboot getvar result` → `linux-fail N` (numbered bail points), a
reset-surviving breadcrumb word at `0xF3FF0000` printed at loader start,
`fastboot oem ramoops` (the kernel's RAM console) and `fastboot oem screenshot`
(raw framebuffer; `getvar screen` = `1080 2400 4320`, BGRA).

### The `tqftpserv` error spam

`failed to create tqftpserv readwrite directory` repeated in `dmesg` came from
our own initramfs: `init.c` starts `rmtfs`, `pd-mapper` and `tqftpserv` (they
feed the DSPs their PD map, EFS and firmware over QRTR/TFTP), but the cpio had
no `/var/lib` at all, so `tqftpserv` could not create its
`/var/lib/tqftpserv/readwrite` directory and logged on every attempt.

Fixed in `tools/linux/mkinitramfs.sh`:
`/var/lib/tqftpserv/{readonly,readwrite}` and `/var/lib/pd-mapper`.

Verified: the spam is gone (`dmesg tqftpserv` now shows only the service start),
and with the three services actually running this boot also shows
`pd-mapper: domain list request from node 0/1/5` (the remotes asking for their PD
maps), `rmtfs_mem1 present`, and `remoteproc1: remote processor modem is now up`
with `state: modem = running`.

The ADSP still hits `USER-PD DOG detects stalled initialization` a few times
after boot and recovers to `running` — slower than the ~20 s cycle before, but
not fixed: the sensor PD inside the Xiaomi image wants more than pd-mapper,
rmtfs and tqftpserv.

### Fedora 44 aarch64 userspace boots to multi-user.target (2026-11-14)

`Startup finished in 5.53 s (kernel) + 3.72 s (userspace) = 9.31 s`, with
`graphical.target`, `multi-user.target`, `sockets.target`, plymouth and
systemd-ssh-generator reached, from a 598 MB gzip cpio unpacked into the
initramfs — i.e. a real distro in RAM on the phone, no UFS writes.

How it was built (this host, no root):

```sh
# aarch64 root, solved natively and installed for aarch64 (scriptlets skipped:
# their aarch64 %posts run under qemu and take tens of minutes each)
docker run --rm --platform linux/amd64 -v /tmp/luma:/luma:ro -v /tmp/armroot3:/out \
  fedora:44 dnf -y --installroot=/out --releasever=44 --forcearch=aarch64 \
  --use-host-config --setopt=install_weak_deps=False --setopt=tsflags=noscripts \
  install --skip-broken $(grep -vE '^\s*(#|$)' /luma/config/mobile/packages.txt) \
  fedora-release systemd-udev util-linux kmod
# then, in a root container:  usr/sbin + sbin symlink, /dev /proc /sys /run /tmp,
# empty /etc/machine-id, dbus.service alias (lost with noscripts), users via
# systemd-sysusers --root, the usbacm.service unit + serial-getty@ttyGS0 autologin
tools/linux/pack-rootfs.sh   # or the same cpio/gzip inside the container
```

Remaining failures in that boot, all understood: `dbus.socket: service not
loaded` (alias missing because the scriptlets were skipped — now created),
`systemd-logind` FAILURE (follows from dbus), `modem.mdt failed with error -2`
(no firmware partition mounted), `dpu_crtc_atomic_flush: crtc pending` on the
DPU, and `snd-sm8250: MultiMedia1: error getting cpu dai name`.

`tools/linux/usbacm.c` brings the CDC-ACM gadget up from systemd
(`usbacm.service`, stays resident) and `serial-getty@ttyGS0` autologs root, so
the Fedora userspace is reachable over the same USB cable (1d6b:0104).

### Fedora userspace: root shell over USB, and logs on UFS

- `usbacm.service` (Type=oneshot, RemainAfterExit) binds the CDC-ACM gadget and
  `ttygs-shell.service` runs `bash --login` on `/dev/ttyGS0`. `serial-getty@`
  is disabled: `login -f root` on this tty fails with `Permission denied`
  (PAM, booted with `selinux=0`), and a plain shell is what iteration needs.
- Reached on the device: `[root@fedora /]#`, `nproc` = 8, `free -m` = 5296 MB
  total / 1463 used, `/etc/os-release` = "Fedora Linux 44 (Forty Four)".
- **Logs on UFS.** `userdata` is *not* usable from Linux: it is
  metadata-encrypted, so f2fs reports `Magic Mismatch ... 0x572000`.
  `log-to-ufs.sh` (timer, every 30 s) writes `journal-<stamp>.log`,
  `dmesg-<stamp>.log` and `cmdline-<stamp>.txt` to `/picasso-logs/` on the
  Android **cache** partition (ext4, 384 MB, 318 MB free — verified mounted rw,
  written, `sync`, unmounted), keeping the newest 20 boots.

### Fedora: firmware for the DSPs and the GPU, and the first GL frame

- Missing firmware was the last real blocker for the DSPs and the GPU:
  `Direct firmware load for qcom/sm7250/xiaomi/picasso/image/modem.mdt failed
  with error -2`, `failed to load a650_sqe.fw` (so Mesa fell back to llvmpipe —
  kmscube ran at 30 fps on 2 frames with software rendering).
- `fw-setup.service` (`/usr/local/bin/fw-setup.sh`) mounts the Android `modem`,
  `dsp` and `bluetooth` partitions read-only, links their `image/*` files into
  `/lib/firmware/qcom/sm7250/xiaomi/picasso/image` (and `/lib/firmware/qca`),
  then unbinds/rebinds `qcom_q6v5_pas` and `ath10k_snoc` so the second probe
  finds them.
- The GPU blobs from `prebuilt/fw-gpu/` (a620 zap split, a650 GMU/SQE) are now
  copied into the rootfs `/lib/firmware/qcom`.
- `kmscube -D /dev/dri/card1` runs (EGL/GBM on the DPU), `renderD128` present;
  the DPU prints `dpu_crtc_atomic_flush: crtc73 pending 0x1` on fast commits —
  a benign warning from the port's flush-bit handling (fb0 still registers and
  scans out).

### The three open items, closed (2026-11-14)

1. **Firmware partitions.** `fw-setup.service` now waits for
   `/dev/disk/by-partlabel/modem` (udev is asynchronous — without the wait the
   mounts silently failed and every remoteproc kept `-2`), mounts `modem`,
   `dsp` and `bluetooth` read-only, links 327 `image/*` files into
   `/lib/firmware/qcom/sm7250/xiaomi/picasso/image`, then unbinds/binds
   `qcom_q6v5_pas`. Result on the phone: `adsp is now up` **and**
   `modem is now up` in the same boot.
2. **DPU `crtc73 pending 0x1`.** Confirmed benign for the port: it is a warning
   from `dpu_crtc_atomic_flush` when commits arrive faster than the flush bit
   clears; `DSI-1`/crtc 73 still scan out (`kmscube` renders, weston enables
   the mode). No change made.
3. **Graphical session.** `weston.service` (`PAMName=login`, DRM backend) comes
   up: `Output 'DSI-1' (crtc 73) video modes` → `Output 'DSI-1' enabled with
   head(s) DSI-1`, inputs (pwrkey/resin/gpio-keys/NT36672C) associated, a
   logind session exists, and a Wayland client (`weston-terminal`) runs on it.
   Two facts learned the hard way: libseat needs a **logind session**
   (`PAMName=login` on the unit; `systemd-run --scope` rejects `PAMName`), and
   the rootfs needs `/etc/pam.d/system-auth` + `postlogin` with
   `pam_systemd.so` — the `tsflags=noscripts` build has none, and without them
   even the session-open fails. Weston 15 dropped `--tty`.
   GPU is hardware: `renderer: FD620`, `kmscube` at ~10.7 fps once
   `prebuilt/fw-gpu/*` (a650 GMU/SQE, a620 zap) is in `/lib/firmware/qcom`.

### Wi-Fi (WCN3990, modem-hosted) — QRTR stack works, modem init is the wall

What is proven on hardware today:

- `pd-mapper` (static, from `tools/linux/bin`), `rmtfs` (seeded from the phone's
  own `modemst1`/`modemst2`/`fsc`/`fsg` via `rmtfs-seed.service`) and
  `tqftpserv` all run in Fedora; `qrtr-lookup` shows the expected node 1
  services (14 remote-fs, 4096 TFTP, 64 registry locator).
- With the modem started after those services: **`69 1 0 0 93 ATH10k WLAN
  firmware service`** appears and `ath10k_snoc` completes the QMI handshake —
  `qmi chip_id 0x320 chip_family 0x4001 soc_id 0x400b0000`,
  `fw_version 0x321b002a … WLAN.HL.3.2.1.c2-00042-QCAHLSWMTPLZ-1.395128.1`.
  So the WLFW path itself works on mainline once the modem is up.
- It then wanted board data: `failed to fetch board-2.bin or board.bin from
  ath10k/WCN3990/hw1.0`; `fw-setup` now links `bdf_g7a.bin` from the modem
  partition there (`board.bin`).
- **Blocked:** the modem itself keeps dying during init —
  `qcom_q6v5_pas … watchdog received: dog_hal_common.c:180:DOG detects stalled
  initialization, triage with IMAGE OWNER`, then `can't start rproc modem:
  -110`. Once that happens the WLFW service is gone and `ath10k` cannot
  handshake, so no `wlan0` and no scan.
  Next suspects, in order: (1) the EFS content rmtfs serves (our seed is a raw
  copy of the partitions — the modem may need its `modem_fs*` to be the exact
  sizes Android uses); (2) the PD map from pd-mapper (it serves what it finds
  under /lib/firmware; the vendor's `.jsn` files may be missing); (3) start
  ordering — the modem must come up *after* both are serving and must not be
  touched afterwards (a stop/start of a crashed modem returns `-EINVAL`).

### SSH over the USB cable (done) and where Wi-Fi stands

**SSH works, verified after a clean boot:**

```
ssh -i <key> root@fe80::11:22ff:fe33:4455%enx021122334466   # host side
→ SSH_OK, uid=0(root), Linux 7.3.0-rc7 aarch64, 8 cpus, 5296 MB RAM
```

- The gadget now carries two functions: `acm.GS0` (console) and `ecm.usb0`
  (`usbacm.c`), so one cable gives a console and an IP link (`usb0`, carrier
  follows the phone-side link). `usb0-up.service` just does
  `ip link set usb0 up`; both ends then have IPv6 link-local and ssh runs over
  that — no DHCP, no routing, no host root needed (`fe80::11:22ff:fe33:4455`
  from `dev_addr 02:11:22:33:44:55`).
- Two rootfs traps this exposed, both now fixed in the image and the README:
  `/usr/sbin` must be a **symlink to `bin`** (Fedora 44 merges it; our earlier
  hand-made empty directory hid every sbin binary, `sshd` included), and the PAM
  stack needs **`/etc/pam.d/password-auth`** as well as `system-auth` +
  `postlogin` (without it sshd logs `Access denied for user root by PAM account
  configuration`).
- `sshd` listens on `0.0.0.0:22` and `[::]:22`; `/root/.ssh/authorized_keys`
  holds the host's ed25519 key (the corresponding private key lives on the host
  at `/tmp/picasso_key`, regenerate if the phone image is rebuilt elsewhere).

**Wi-Fi:** `pd-mapper` in the foreground *does* find all six PD-map files
(`adspua/adsps/adspr`, `cdspr`, `modemr/modemuw`) and serves them — the modem
(node 5) then hammers it with domain-list requests and its own watchdog fires
(`dog_hal_common.c:180 DOG detects stalled initialization`). `modemr.jsn`
declares only the `modem` domain (no `wlan` entry; the WLAN PD is a *subdomain*
of the modem), so the remaining work is the modem's init sequence itself, not
the map's presence. That is the same class of problem the XNU port hit as
"WLFW never registers".

### Wi-Fi, continued: the modem dies before it talks to the host

Ordering is now right, and the evidence points inside the modem:

1. `fw-mount.service` (new, `Before=pd-mapper rmtfs tqftpserv`) mounts the
   Android `modem`/`dsp`/`bluetooth` partitions, links the `image/*` files into
   `/lib/firmware` (board.bin for ath10k included) **and** into
   `/var/lib/tqftpserv/{readonly,readwrite}` (327 files) — so pd-mapper now
   finds all six PD maps on its **first** start (before: it started with nothing
   mounted, failed, and restarted after the modem was already up).
2. `fw-start.service` then unbinds/binds `qcom_q6v5_pas` (after a 3 s settle)
   and starts the modem; `rmtfs` runs with `-v` over the seeded EFS.
3. Result: `remoteproc1: remote processor modem is now up` — and then, ~40 s
   later, `qcom_q6v5_pas 4080000.remoteproc: watchdog received:
   dog_hal_common.c:180:DOG detects stalled initialization` (a *different* dog
   from the ADSP's `sensor_process` one). 3 such DOGs per boot; both remotes end
   up `offline`.
4. **Neither `rmtfs` nor `tqftpserv` ever logs a request** (`rmtfs -v` included)
   → the modem stalls *before* it asks the host for EFS, the PD map or TFTP
   files, so the host-side stack is not the gate any more.
5. One earlier boot did reach `69 1 0 0 93 ATH10k WLAN firmware service` and
   `ath10k_snoc: qmi chip_id 0x320 … fw_version 0x321b002a … WLAN.HL.3.2.1.c2`,
   so everything above the modem is correct.

Next lever, unexplored: **remoteproc coredump** — the kernel exposes
`/sys/kernel/debug/remoteproc/remoteproc1/coredump`
(+ `crash`, `carveout_memories`), i.e. the modem's memory after the watchdog
reset, the same data the vendor's rawdump carries. Capturing it and symbolising
against the `modem.bNN` segments from the modem partition is how the stalled PD
gets named.

### Wi-Fi, third pass: PD map does carry `wlan_pd`; coredump stays empty

- `modemuw.jsn` (found and served by pd-mapper) declares
  `"subdomain": "wlan_pd"` with `"provider": "wlan"` — so the modem *can*
  resolve the WLAN PD; `modemr.jsn` only carries the `modem` root domain
  (`qmi_instance_id` 180). Nothing is missing from the map.
- `ath10k_snoc` is **bound** to `18800000.wifi` (not in `devices_deferred`, only
  `sound` is) — it sits inside probe waiting for the WLFW QMI service, exactly
  as designed.
- The modem still DOGs 3× per boot (`dog_hal_common.c:180`), and **neither
  rmtfs (with `-v`) nor tqftpserv logs a single request** — the stall is before
  any host-service exchange.
- `rproc-coredump.service` (new) enables
  `/sys/kernel/debug/remoteproc/remoteprocN/coredump` at boot, but after the
  watchdog crashes the file stays **0 bytes**: the q6v5_pas dump path does not
  produce a dump for this crash (it needs the segments to still be mapped when
  recovery runs). Capturing the modem's memory that way would need work inside
  the driver's dump path, or the vendor's rawdump.
- Everything needed to reach the phone while iterating is in place:
  `ssh -i /tmp/picasso_key root@fe80::11:22ff:fe33:4455%enx021122334466`.

### Wi-Fi: the modem blocker was a missing PD map entry — WLFW now comes up

Found by making pd-mapper log *what the modem asks for* and *what it answers*
(a 20-line patch to `pd-mapper.c`, built static and baked into the image):

```
request from node 5: 'tms/pdr_enabled'      -> 1 domain  (msm/modem/root_pd, 180)
request from node 5: 'tms/pddump_disabled'  -> 0 domains   <-- here
request from node 5: 'avs/audio'            -> 1 domain  (msm/adsp/audio_pd, 74)
```

Xiaomi's own PD maps carry `tms/pdr_enabled` and `tms/servreg` but **not**
`tms/pddump_disabled`, so the modem got an empty list for it and its PD watchdog
fired 3× per boot (`dog_hal_common.c:180`). `fw-mount.sh` now writes three extra
maps (`tms-pddump-{modem,adsp,cdsp}-*.jsn`) into a **tmpfs copy of the image
directory** (`/run/fw-image`, symlinked as `/lib/firmware/.../image`) — the
mounted ext4 is read-only and pd-mapper only scans that directory — and things
finally move:

- modem stays **running** (one DOG left, from the ADSP's unrelated `sensor_process`)
- `69 1 0 0 92 ATH10k WLAN firmware service` appears on QRTR
- `ath10k_snoc: qmi chip_id 0x320 board_id 0xff soc_id 0x400b0000`,
  `qmi fw_version 0x321b002a … WLAN.HL.3.2.1.c2` — the QMI handshake completes
- `board.bin` (= `bdf_g7a.bin` from the modem partition) is accepted

**Last missing artifact:** `ath10k/WCN3990/hw1.0/firmware-5.bin` (API-2/5 WMI
metadata): `Failed to find firmware-N.bin (N between 2 and 6)`. Every local
source ships it as a **60-byte LFS pointer** — kernel.org's linux-firmware repo,
Fedora's and Debian's `linux-firmware` packages, and this host's own
`/lib/firmware` all contain `QCA-ATH10K` pointer stubs. A real copy (≈1.3 MB)
has to come from a linux-firmware release tarball or a machine that still has
the pre-LFS blob; drop it in and `wlan0` should appear.

### Wi-Fi WORKS: wlan0 up, scanning real access points

Two more pieces after the PD-map fix:

1. **`firmware-5.bin`** (WCN3990, `ath10k/WCN3990/hw1.0/`) is a **60-byte file**
   whose content *is* the firmware header (`QCA-ATH10K\0` + TLV entries) — not a
   git-LFS pointer, which is what every copy looked like at first glance.
   Shipped in `prebuilt/fw-wcn3990/` and installed into the rootfs; after that:
   `wcn3990 hw1.0 target 0x00000008`, `firmware ver  api 5 … crc32 b3d4b790`,
   `htt-ver 3.81 … hwcrypto 1`, `invalid MAC address; choosing random`, and
   **`wlan0`** exists (`phy0` registered).
2. `wpa_supplicant` + `iw` in the userspace image (NM needs the supplicant; the
   package list had neither).

`iw dev wlan0 scan` returns live access points:

| SSID | best signal |
| --- | --- |
| <ssid> | −46 dBm |
| <ssid> | −46 dBm |
| (hidden) | −67 dBm |
| VIETTEL_2dH95J | −69 dBm |
| SCADA-INDUSTRIES | −81 dBm |
| <ssid> | −86 dBm |
| vivo Y53s | −89 dBm |

NetworkManager still reports `wlan0: unmanaged` (it was started before
wpa_supplicant existed and does not pick the device up afterwards), so
connecting is done directly with `wpa_supplicant` next; `sshd` already listens
on 22 and the USB-ECM link keeps working meanwhile.

### Wi-Fi connected, DHCP, ssh over Wi-Fi (verified after a clean boot)

```
wlan0  UP  <lan-ip>/22  (DHCP from <lan-ip>)
iw dev wlan0 link -> Connected to <bssid>, SSID <ssid>, 5765 MHz, -50 dBm
ssh root@<lan-ip> -> SSH_OVER_WIFI_OK
```

The missing piece for a *managed* device was `NetworkManager-wifi` (the plugin
lives in a separate Fedora package; without it NM logs
`'wifi' plugin not available; creating generic device` and leaves wlan0
`unmanaged`). With `NetworkManager-wifi` + `wpa_supplicant` + `iw` installed,
`nmcli dev wifi connect` associates and does DHCP with NM's internal client
(`nm-dhcp-helper`; there is no dhclient/udhcpc in this image).

The profile is baked into the image (`userland/fedora/<ssid>.nmconnection`,
`/etc/NetworkManager/system-connections/`, mode 600, autoconnect) and
`NetworkManager.service` is enabled, so a freshly booted phone joins the
network by itself and is reachable over Wi-Fi as well as over the USB-ECM link.
