# Boot history

> Patch numbers in entries written before 2026-11-20 use the old numbering: old 0012–0024 are now
> 0013–0025 and the second 0011 (`net-qrtr-ns-retry-announce-on-eagain`) is now 0012.

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
  - `initramfs/init.c` now paints `/dev/fb0` with `/dev/urandom` (2 s after
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
  `initramfs/bin/`, packed into the initramfs by `mkinitramfs.sh`; EFS images
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
- `initramfs/tone` + `tone` console command exist; `open("/dev/snd/pcmC0D0p",
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
| display | MDSS/DPU 6.1.0 (`dpu hardware revision:0x60010000`), DSI + 7nm PHY, panel `panel-xiaomi-g7a` bound (`1080x2400@120`, `334660` kHz, `drm_mode_set_name` in `0006*`); `crtc[73]` active at 120Hz |
| Denial Mobile rootfs | `sda36` (`PARTNAME=linux`, 20 GiB ext4) mounted r/w, resized, systemd PID 1 |
| Console hand-over | `/dev/ttyACM1` (or ACM0) interactive shell enters `/denial` on UFS (`console: inside the system on UFS`) |
| USB ECM networking | host `enx020000000001` DHCP lease `10.77.71.1/30`, ping to `10.77.71.2` 0.29 ms |
| SSH server | `sshd.service` active on `0.0.0.0:22` (`/run/11-phone-usb.conf` bind-mounted), reachable via USB (`10.77.71.2`) and Wi-Fi (`<lan-ip>`), passwordless root & `picasso` user key login verified |
| ADSP sensor PD | `hexagonrpcd-stock-sensors` maps vendor/odm/persist via DM, `hexagonrpcd` active, no ADSP watchdog reset (>11 min uptime) |
| Graphics / Wayland | `deniald` (`--device /dev/dri/card1`), `phonewave-droidloom.service` (`uid=1000` `picasso`) on `/dev/dri/renderD128` (Mesa), `1080x2400` XR30 (`pitch[0]=4352`) with **ZERO GPU faults** (`enabled=enabled`, `dpms: On`), `HandlePowerKey=ignore` |
| GPU (Adreno 620) | Real Gen3 `prebuilt/fw-gpu/a650_sqe.fw` (`7db957e3...`, 31804 B) + `a650_gmu.bin` + `a620_zap.mbn`; zero `gpu fault` / `hangcheck` / `recover` |
| Touch (`NVTCapacitiveTouchScreen`) | `novatek/novatek_nt36672c_g7a_fw03.bin` (`prebuilt/fw-touch/`) packed into initramfs and bound into `/newroot`; `fw reset state 0xa0`, `1080x2400` active on `input2`/`event2` |
| System upgrades | `pacman -Syu` synced `core`/`extra`/`alarm`/`aur`/`denial`/`denial-phone`/`droidloom` via `ssh -R 1080` proxy (`droidloom-image` & `droidloom-runtime` upgraded to 0.1.0-42) |
| Sensors (`steinerd`) | `steinerd.service` active via `hexagonrpcd`: `tsl2560` (ALS), `bmi26x` (accel/gyro), `gyro_cal`, `Rotation Vector` |
| Backlight (`wled`) | `pm8150l_wled` enabled in DT (`0002*`) with downstream values (`enabled-strings <0 1 2>`, `ovp-millivolt <28000>`, `current-limit-microamp <22500>`) + `0012-backlight-qcom-wled-balance-ovp-irq.patch`; DPMS toggles `d846` (`0x80` ON ↔ `0x00` OFF) with zero OVP IRQ warnings |
| Bluetooth (`hci0`) | `wcn3991-bt` downloads `qca/crbtfw32.tlv` & `crnv32.bin` (`BTFM.CHE.3.2.1-00174-QCACHROMZ-1.372365.1`); `btmgmt info` powered & ready |
| Audio (PipeWire/UCM2) | `/run/alsa-ucm2` drop-in (`Picasso-HiFi.conf`, `Speaker` 100%), WirePlumber `/run/wireplumber/wireplumber.conf.d/50-picasso-alsa.conf` forces `S16_LE` 48kHz 2ch; `pw-play` opens `card 0` in `S16_LE` |
| Modem / TFTP (`tqftpserv`) | `remoteproc1` (`modem`) running (`[45.719562] remote processor modem is now up`), `tqftpserv` & `pd-mapper` retain `/sys` via `MS_BIND \| MS_REC` with zero `ENOENT` rejections (>86s uptime) |
| Wi-Fi (`wlan0` / `ath10k_snoc`) | `wcn3990 hw1.0` (`18800000.wifi`) probes cleanly on boot with QRTR `announce_servers()` `-EAGAIN` retry (`0011*`), zero `dog_hal` errors, `wlan0` UP, `nmcli` scans 2.4 GHz & 5 GHz WPA2 networks |
| Video decode (`qcom-venus`) | **WORKING** (`0014*`): `ffmpeg -c:v h264_v4l2m2m` decodes `60 frames` (320x240) in `0.08s` (`RC=0`) and `300 frames` (1280x720) in `0.63s` (`speed=15.9x`, ~476 fps, `RC=0`) with zero `dmesg` errors |
| Droidloom (Android container) | `droidloomd.service` + user `droidloom.service` **WORKING**: full Android cell booted (`renderD128`, `/dev/dma_heap/*`); TikTok (`com.ss.android.ugc.trill`) installed with desktop launcher |
| Battery (`pm7250b-battery`) | **WORKING**: `0015*` with `0x8000` sentinel fallback (`0x48c0`/`0x48c2`); `VOLTAGE_NOW=4363372` (`4.363 V`), `CURRENT_NOW=126495` (`+126 mA`), `CAPACITY=97%`, `upower -d` `charging` (`0.809 W`) |

## Bring-up matrix (2026-11-14, measured on hardware)

| Subsystem | Evidence today | Remaining |
|---|---|---|
| GPU | **WORKING**: `3d00000.gpu` (`0x06020000`) runs with real Gen3 `a650_sqe.fw` (`7db957e3...`) + `a650_gmu.bin` + `a620_zap.mbn`; `deniald` renders `1080x2400` `XR30` on `card1-DSI-1` with **ZERO GPU faults** (verified 2026-11-17) | — |
| Audio | card `XiaomiRedmiK305` (snd-sm8250), TFA9874 binds (`0-0034` on i2c0), ADSP PAS running, PipeWire default sink `Built-in Audio Speaker` via `/run/alsa-ucm2` drop-in, `pw-play --raw` works (verified 2026-11-17) | ADSP sensor-PD watchdog resets eliminated via `hexagonrpcd` stock sensors |
| Bluetooth | **WORKING**: `wcn3991-bt` on `ttyHS1` loads `qca/crbtfw32.tlv` & `crnv32.bin` (`BTFM.CHE.3.2.1-00174-QCACHROMZ-1.372365.1`); `hci0` powered in `btmgmt info` (verified 2026-11-17) | — |
| Wi-Fi | **WORKING**: `ath10k_snoc` (`18800000.wifi`, `wcn3990 hw1.0`) creates `wlan0` (`phy#1`) automatically on boot and scans 2.4 GHz & 5 GHz WPA2 networks via `nmcli` (verified 2026-11-17) | — |
| Display | **WORKING**: `panel-xiaomi-g7a` (`1080x2400@120`, `334660` kHz, `drm_mode_set_name` in `0006*`), `crtc[73]` active at 120Hz, WLED backlight ON (`d846: 80`), `deniald` rendering `XR30` (verified 2026-11-17) | — |

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
- `scripts/build-bootimg.py --initrd` is optional; `PAYLOAD_NO_INITRD=1
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

Fixed in `scripts/mkinitramfs.sh`:
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
tools/pack-rootfs.sh   # or the same cpio/gzip inside the container
```

Remaining failures in that boot, all understood: `dbus.socket: service not
loaded` (alias missing because the scriptlets were skipped — now created),
`systemd-logind` FAILURE (follows from dbus), `modem.mdt failed with error -2`
(no firmware partition mounted), `dpu_crtc_atomic_flush: crtc pending` on the
DPU, and `snd-sm8250: MultiMedia1: error getting cpu dai name`.

`tools/usbacm.c` brings the CDC-ACM gadget up from systemd
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

- `pd-mapper` (static, from `initramfs/bin`), `rmtfs` (seeded from the phone's
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

### Bluetooth (WCN3991 on UART SE5): hci0 comes up, the chip stays silent

Enabling it properly this time (the earlier notes had the pieces scattered):

- `&uart5` (`serial@894000`) enabled with a **`serial1` alias** — without it
  `qcom_geni_serial: Invalid line -19` and nothing binds.
- `bluetooth { compatible = "qcom,wcn3991-bt"; … }` child (the chip reports SOC
  0x40010320, so binding it as wcn3990 makes even the version read time out),
  four rails `vddio=s4a 1.8`, `vddxo=l14a 1.8`, `vddrf=l2c 1.3`,
  `vddch0=l10c 3.3`, `max-speed=3200000`, `swctrl-gpios = <&tlmm 73>`, and
  `firmware-name = "qca/crnv32.bin", "qca/crbtfw32.tlv"` (order: NVM, patch).
- The RX wake interrupt on gpio41 that the SoC dtsi adds is removed (avicii's
  note: it must not be on that pad).
- `fw-mount.sh` now also links the **`bluetooth` partition's `image/*`** into
  `/lib/firmware/qca` (it only linked the top level before, so the files were
  missing even though the vfat mounted).
- `picasso-pinmux.c` gained per-pad bias and muxes gpio38-41 with the vendor's
  bias (CTS/RX none, RTS pull-down, TX pull-up) plus gpio73 as a SW_CTRL input.

State now: `hci0` exists, serdev bound (`serial0-0`), all four rails enabled
(regulator_summary shows use counts 1-2 at the right voltages), but the chip
never answers the QCA vendor command: `command 0xfc00 tx timeout` →
`Reading QCA version information failed (-110)`, three retries, every boot.

Next, in order: (1) scope-free check of the pad states — read back gpio38-41 and
gpio73 ctl/io through the early-mux driver on boot and compare against the
vendor's `qup_uart5_*`/`bt_sw_ctrl` states; (2) try the vendor's reset
convention (assert SW_CTRL low 50 ms, then high) instead of an input; (3) drop
the early-mux bias/drive changes back to the exact values of the run where the
version read did succeed, to see whether that run is reproducible at all.

### Bluetooth: firmware download completes at last — root cause was the missing `picasso_pinmux.mode=write`

```
Bluetooth: hci0: setting up wcn399x
Bluetooth: hci0: QCA Product ID :0x0000000a, SOC 0x40010320, ROM 0x00000302, Patch 0x00000de9
Bluetooth: hci0: QCA Downloading qca/crbtfw32.tlv
Bluetooth: hci0: QCA Downloading qca/crnv32.bin
Bluetooth: hci0: QCA FW build version: BTFM.CHE.3.2.1-00174-QCACHROMZ-1.372365.1
Bluetooth: hci0: QCA setup on UART is completed
```

The `... setup on UART is completed` line is the milestone: the patch and the NVM
were downloaded and the controller ACKed. Three separate things had to line up:

1. **`picasso_pinmux.mode=write` must be in the cmdline.** The Fedora payload
   cmdline had lost it, so the early-mux driver ran in its default *read* mode:
   the UART pads 38-41 (and every other east/west pad: touch, amp reset, I2S)
   still held the loader's state and the BT chip saw no UART. Register readback
   showed `mux=0` on gpio40/41 — that is what gave it away.
2. **The firmware name must not repeat the driver's own prefix**: the driver
   builds `qca/<firmware-name>`, so `firmware-name = "crnv32.bin",
   "crbtfw32.tlv"` (with `qca/…` it asks for `qca/qca/…` and fails -ENOENT).
3. **The firmware has to be there before the first setup**: the serdev probes at
   ~0.6 s and asks for the TLV at ~5 s, while `fw-mount` links the phone's
   `bluetooth` partition at multi-user. `fw-mount.sh` now unbinds/binds
   `894000.serial` at the end, so the second setup runs with the files in place
   (that is the 21 s "setting up wcn399x" in the log).

Also needed: `serial1 = &uart5` alias (`geni-serial: Invalid line -19`
otherwise), `compatible = "qcom,wcn3991-bt"` (as wcn3990 even the version read
times out), the four rails, `swctrl-gpios = <&tlmm 73>`, and no RX wake IRQ on
gpio41.

Remaining: `hci0` exists in sysfs but the kernel never finishes registering it
(`btmgmt` “Index list with 0 items”, `bluetoothctl list` empty; one
`Frame reassembly failed (-84)` at setup). Next: follow `hci_register_dev` in
`hci_uart_setup` (the 0xFD wake byte is already dropped in `qca_recv`, so look
at the HCI reset/`HCI_QUIRK` path and the `-84` frame).

Meanwhile, the USB-ACM console wedged again during the bluetoothd start; the
phone is reachable over Wi-Fi (NM leased <lan-ip> this boot).

### Bluetooth — WORKS (2026-11-16): controller registers, powers on, discovers

```
[    6.198405] Bluetooth: hci0: QCA setup on UART is completed
hci0:  Primary controller
        addr 02:00:00:00:00:01 version 11 manufacturer 29 class 0x000110
        current settings: powered ssp br/edr le secure-conn wide-band-speech ll-privacy …
hci0 dev_found: DF:70:3A:F2:AE:14 type LE Random rssi -88 flags 0x0004
```

`bluetoothctl list` shows the controller and `btmgmt --index 0 power on` completes.

**Why it had never registered.** `btqca` sets `HCI_QUIRK_USE_BDADDR_PROPERTY`
(`drivers/bluetooth/btqca.c:708`). With no `local-bd-address` the controller's
public address stays zero, so `hci_dev_setup_sync()` marks it
`HCI_UNCONFIGURED`, returns through `hci_unconf_init_sync()` — and
`hci_init_sync()` never runs. `HCI_SETUP` therefore stays set and mgmt lists no
index: `btmgmt info` said "Index list with 0 items" while `hci0` did exist and
the firmware download had completed. Instrumenting `hci_init1_sync` and the
`hci_init_sync` return in `net/bluetooth/hci_sync.c` printed *nothing*, which is
what proved the stall was before init rather than inside it. Adding
`local-bd-address = [ 01 00 00 00 00 02 ]` to the DTS (bytes are byte-swapped
by `qca_set_bdaddr`, so the air address is `02:00:00:00:00:01`, locally
administered) fixed registration on the first try:

```
picasso-hci: init1 enter (quirk RESET_ON_CLOSE 0)
picasso-hci: reset returned 0
picasso-hci: br_init1 returned 0
picasso-hci: hci_init_sync returned 0
```

The address in the DT is a bring-up placeholder. The real one is provisioned at
the factory and lives in the phone's `persist` partition; reading it there and
passing it at boot is the follow-up.

The `0xFD` flow-control bytes the chip emits while accepting a patch/NVM TLV are
dropped only while `QCA_IBS_DISABLED` (patch `0008-bluetooth-hci-qca-wcn3991-flow-control`).
Verified end to end: `QCA setup on UART is completed`, then discovery finds real
devices. Still open: two `Frame reassembly failed (-84)` per boot, one before the
ROM version read and one during scanning — the parser rejects a byte that is not
a packet type at a packet boundary; operation is unaffected, but it should be
understood (candidate: a 0xFD arriving while `rx_skb != NULL`).

Fast loop used for all of this (one cycle ≈ 130 s): `reboot` from Linux → loader
comes up in 42 s → `oem ramdisk` → `stage` the 606 MB resident rootfs (19 s) →
`stage` the payload (2 s) → `oem run` → USB console in 12 s.

### Wi-Fi — re-verified 2026-11-16, and the timing that makes checks lie

Wi-Fi was never broken; a check 20 s after boot is simply too early. The chain
(modem PAS → ath10k_snoc → MAC80211 association) completes at ~74 s:

```
[74.649] wlan0: authenticate with <bssid> (local address=aa:57:75:8f:5c:19)
[74.655] wlan0: associate with <bssid> (try 1/3)
[74.733] wlan0: associated
```

Verified on the device (RAM boot, 36 min uptime):
`<lan-ip>/22` from NetworkManager, DNS `dl.simplyluma.com → 167.71.22.210`,
sshd listening on 22, and from the host:

```
$ ssh -i … root@<lan-ip> 'iw dev wlan0 link'
Connected to <bssid> (on wlan0)
        SSID: <ssid>
        freq: 5765.0            <- 5 GHz
        RX: 8632950 bytes (72103 packets)
        signal: -57 dBm
```

`ping` is not installed in this image; use `getent hosts` or `curl` for
reachability. `user@0.service` fails (no `hostname`/PAM pieces in the trimmed
root), harmless for the console and ssh paths.

### Audio — WORKS (2026-11-16): the ADSP was simply never started

`/proc/asound/cards` was empty because the ADSP never came up:

```
remoteproc0: Direct firmware load for qcom/sm7250/xiaomi/picasso/image/adsp.mdt failed with error -2
platform sound: deferred probe pending: snd-sm8250: MultiMedia1: error getting cpu dai name
```

The remoteproc probes during kernel init, before `fw-mount` has mounted the
phone's `modem`/`dsp` partitions, so the load fails with `-ENOENT` and the
remoteproc stays `offline` for good — nothing re-triggers it. `fw-start.sh`
started **only the modem**; extending it to start the `adsp` remoteproc as well
makes the whole audio stack come up by itself:

```
remoteproc0: remote processor adsp is now up
PDR: Indication received from msm/adsp/audio_pd, state: 0x1fffffff
qcom,apr ... Adding APR/GPR dev: aprsvc:service:4:7      (q6asm DAIs)
Xiaomi-Redmi-K30-5G Headset Jack as .../card0/input4
card 0: XiaomiRedmiK305 [Xiaomi-Redmi-K30-5G], device 0: MultiMedia1
```

The service then waits for the card and applies the board's only route
(`SEC_MI2S_RX Audio Mixer MultiMedia1=1`) and the speaker gain, retrying until
the mixer takes, and stores the state with `alsactl store`. Verified on a clean
boot with no manual step:

| check | result |
|---|---|
| `remoteproc0` (ADSP) / `remoteproc1` (modem) | `running` / `running` |
| `aplay -l` | `card 0 ... device 0: MultiMedia1` |
| mixer route | `SEC_MI2S_RX Audio Mixer MultiMedia1` = `on` |
| `speaker-test -D hw:0,0 -t sine -f 440 -c 2 -l 1` | plays, no error (5.23 s period) |
| amp after playback | `tfa9874 0-0034: status 0x0017 clk 0xe2c2 sys 0x0018 gain 0x027c tdm 0x0890/0xc0f1` |

Audibility from the speaker itself has not been confirmed by ear yet — every
stage below it reports healthy (DSP stream open, MI2S TDM locked, amp CLKS/AMPE/
DCA on, gain 0x027c).

### GPU (Adreno 620) — still faulting; two upstream bugs fixed on the way

Weston and `kmscube` still trip `gpu fault ring 0 ... status 00800007` and the
DPU hangcheck recovers about once per frame at startup and once per render
afterwards. Measured with the vendor's `weston` and `kmscube`:

| build | faults at boot | during 20 s of weston | during 10 s of kmscube |
|---|---|---|---|
| before | 60 | 0 | 2 |
| + `8128ae7af` (OPP use-after-free in `a6xx_pm_resume`) | 38 | 0 | 2 |
| + `eab0aff89` (RPMH dependency votes on the wrong device) | 36 | 0 | 2 |

Both fixes are real and are exported as patches `0009`/`0010`, but neither is the
cause. What the crashstate says (`/sys/class/devcoredump/devcd1/data`):

- ring buffer `rptr == wptr` — the CP has consumed every command;
- the fence never retires, and `RBBM_STATUS` reads `0x00800007`
  (`GPU_BUSY_IGN_AHB | CP_BUSY | CP_AHB_BUSY_{CP,CX}_MASTER`): the CP is stuck in
  an AHB transaction, not in a memory/SMMU fault (no `arm-smmu` fault is logged);
- the address of the first fault is always the same shape: `ib1 0/0 ib2 0/0`,
  i.e. the new ringbuffer-style submission with no IBs.

Next, in order: (1) apply the sibling fix `86f80884f` (clamp MX dependency vote —
needs the newer `a6xx_gmu_rpmh_arc_index` signature); (2) compare the GMU boot
sequence against the reference avicii port byte for byte (same SoC, GPU reported
working there) using their `.config`; (3) bisect `-rc7` against `v7.3` for a6xx
HFI/power changes, since the fault looks like a GMU/CP handshake problem rather
than a rendering bug.

### UI on screen with the CPU renderer (2026-11-16)

With the backlight fixed the panel shows the console, but weston presented
nothing: its log ends at `launching '/usr/libexec/weston-desktop-shell'` and
never reports a repaint. The compositor blocks in the first atomic commit
because that frame is a GPU submission, the submission hangs, `hangcheck
recover` runs, and weston is still waiting.

Running weston with the **pixman** renderer removes the GPU from the path
entirely (`userland/fedora/weston.service.d/pixman.conf`, installed as
`/etc/systemd/system/weston.service.d/pixman.conf`):

```
Using Pixman renderer
Output 'DSI-1' enabled with head(s) DSI-1
```

Measured over 25 s: `hangcheck recover` count **44 -> 44** (it used to grow once
per minute), zero weston errors, backlight 2048/4095. The UI is therefore usable
on screen while the Adreno 620 hang is worked on; Phosh has the same escape
hatch in `WLR_RENDERER=pixman`.

### The GPU hang, as measured today

One submission hangs per compositor frame cycle:

```
adreno 3d00000.gpu: gpu fault ring 0 fence ffffff2b status 00800007 rb 0048/0048
RBBM_STATUS 0x00800007 = GPU_BUSY_IGN_AHB | CP_BUSY | CP_AHB_BUSY_{CP,CX}
```

- fences do retire when the GPU is idle (`last-fence == retired-fence`), so the
  ring and the timestamp path work in general;
- no `arm-smmu` fault is ever logged: the CP stalls on an AHB transaction, not
  on a translation fault;
- two genuine upstream bugs are fixed and carried (`0009` OPP use-after-free in
  `a6xx_pm_resume`, `0010` RPMH dependency votes resolved on the wrong device);
  neither is the cause, the fault count only went 60 -> 38 -> 36 per boot;
- the loop is self-sustaining: recover -> compositor repaint -> hang again.

Next: (1) clamp the MX vote (`86f80884f`, needs the newer
`a6xx_gmu_rpmh_arc_index` signature), (2) compare the GMU boot sequence with the
reference avicii port (same SoC, GPU reported working there), (3) bisect `-rc7`
against `v7.3` for a6xx HFI/power changes, (4) check whether `a6xx_recover`
itself leaves the CP in the state that hangs, by disabling the hangcheck timer
and seeing whether the *first* submit of a boot also hangs.

### Touch — the controller never runs its firmware (2026-11-16)

`bringup/touch-probe.py` (new) reads the evdev node and prints what actually
arrives; two minutes of tapping produced **0 SYN_REPORT**, so the driver really
emits nothing.

The driver now does what the vendor's SPI driver does at boot and logs the
result:

```
picasso-nvt-ts spi0.0: NT36672C id 0c 00 00 72 66 03
picasso-nvt-ts spi0.0: fw reset state 0xb9 after 101 polls (NOT running)
picasso-nvt-ts spi0.0: swrst-n8 reads 0x00
```

- the **chip id** reads correctly (bootloader space, page 0x1f64e), so the SPI
  protocol and the pads are right;
- the firmware "reset complete" byte at `EVENT_BUF_ADDR | 0x60` never reaches
  `0xA0..0xAF`: the fw is not running, which is why every later read of the
  event buffer is noise (the driver's scores are 1-2 on random bytes);
- the vendor DT gives the SPI reset address: `novatek,swrst-n8-addr =
  <0x03f0fe>` (write 0x69 to boot, 0xaa to idle), `novatek,spi-rd-fast-addr =
  <0x03f310>`. Our driver used the *I2C* engine-reset register (0x7fff80, 0x5a);
  switching to the SPI form changed nothing, so the reset write is not the
  blocker either;
- `picasso_nvt_spi.no_reset=1` boots without touching the controller at all and
  gives the same 0x00/0xb9: the chip does not come up by itself.

So this panel's controller expects its firmware to be **loaded by the driver**
(the vendor ships `novatek_nt36672c_g7a_fw01/02/03.bin` and has a
`Boot_Update_Firmware()` worker). Next: build the fw-update path from
`nt36xxx_fw_update.c` into the driver (enter bootloader, write flash pages,
read back the "NVT" end flag, reset), with the .bin taken from the phone's own
vendor partition. That is a real feature-sized piece of work, not a tweak.

Meanwhile the driver keeps: the vendor fw checks, the SPI SWRST_N8 address, and
the `no_reset` bring-up switch (all logged), so the next attempt starts from
evidence instead of guesses.

### Charging status — WORKS (2026-11-16): `pm7250b-usb` power supply

Picasso's charger is the SMB5 block in **PM7250B** (vendor `qcom,qpnp-smb5`),
and mainline already has a driver for exactly that part
(`CONFIG_CHARGER_QCOM_PM7250B`). Nothing was enabled:

- the driver was not built (`=m` is worthless in a RAM initramfs with no module
  loader), and
- `pm7250b.dtsi` ships the charger node with `status = "disabled"`.

After `CONFIG_CHARGER_QCOM_PM7250B=y` and `&pm7250b_charger { status = "okay"; }`:

```
qcom-pm7250b-charger ...: CHGR_CFG2=0x2c (CHG_EN_SRC=command, CHG_EN_POLARITY=0)
  ENABLE_CMD=0x01 STATUS_1=0x45: charge control via command bit (active high)
/sys/class/power_supply/pm7250b-usb: present=1 status=Full charge_type=N/A
  charge_behaviour=[auto] inhibit-charge  start=95 end=100
```

So "is USB in", "is it charging / full", and the inhibit-charge knob are
available to userspace. **Capacity is not**: it needs the QBG gauge
(`pm7250b_qg`, vendor `qpnp,qg`), for which mainline has no driver; the
ADSP-mediated `qcom_battmgr` is not an option while the ADSP restarts every
41 s (see the sensor PD note above).

### Touch SRAM host-download port (2026-11-16, offline)

Read the downstream SPI `nt36672c/nt36xxx_fw_update.c`: this variant downloads
SRAM partitions, not flash pages. `NVT` is a sector-end marker in the input
file; hardware CRC checks ILM/DLM after download, followed by BOOT_RDY and
RESET_STATE_INIT. Added that flow, bounds checking, firmware-name DT/module
selection (default `novatek/novatek_nt36672c_g7a_fw01.bin`), and checked SPI
errors. Polling now uses the trim-ID-selected event buffer and zero-based
report data; release frames are no longer suppressed by the heuristic score.

The MIUI bn vendor image was reconstructed read-only with Brotli + sdat2img;
its firmware directory contains no Novatek binaries. Instead decoded the
existing downstream `firmware/novatek_nt36672c_g7a_fw0{1,2,3}.bin.ihex` with
objcopy into temporary `nvt-fw/` files outside the repository. fw01 SHA256:
`a428d4f87b90d6990396975c82773078fc43fde40a981507b63998513fe27912`.
No blob is committed. For a RAM image, install the chosen decoded binary into
its root tree's `lib/firmware/novatek/` before `tools/pack-rootfs.sh`; the filename
must match the driver parameter or DT `firmware-name`. This is a host extraction
step, not runtime vendor mounting. Panel lockdown/OTP is still unknown.

Kernel build completed successfully for the first upload implementation.
Hardware upload and touch verdict are pending: the coordinator holds the phone
lock for suspend testing, so no RAM cycle has been performed for this change.
There are no new firmware-running log lines or touch probe counts yet; the last
measured result remains zero SYN_REPORT. Human tapping is required for verdict.

### Suspend: s2idle enters, but with no wake source the phone sleeps until it is powered off

First attempt (2026-11-16). `CONFIG_PM_SLEEP_DEBUG`, `CONFIG_PM_DEBUG` and
`CONFIG_PM_ADVANCED_DEBUG` are now built in, and the phone reports:

```
/sys/power/state      = [freeze mem disk]
/sys/power/mem_sleep  = [[s2idle]]     (no shallow/deep: PSCI offers s2idle only)
```

`echo mem > /sys/power/state` **did** suspend the SoC and nothing woke it:
USB (CDC-ACM and fastboot) and Wi-Fi all disappeared, and a short press of the
power key did not resume it — holding the key powered the phone off. Two causes,
both actionable:

1. **No wake source was armed.** An RTC alarm was never set, so there was
   nothing to wake the SoC. `bringup/suspend-test.sh` now arms the alarm with
   `rtcwake -m no -s N`, **reads `/sys/class/rtc/rtc0/wakealarm` back**, and
   refuses to suspend if it is empty; it then suspends with
   `rtcwake -m mem -s N`, so the phone always comes back by itself. `--dry-run`
   arms/verifies/disarms without suspending.
2. **The power key was not a wakeup source.** `pm8941-pwrkey` only registers as
   one when the DT node says so (`wakeup-source`), which the picasso board did
   not; the board now sets it. The RTC driver already does
   `devm_device_init_wakeup()` + `devm_pm_set_wake_irq(alarm)` unconditionally,
   so the alarm path is fine.

`/sys/power/pm_test` was not present in that boot even though
`CONFIG_PM_SLEEP_DEBUG=y` is in `.config` (the attribute is created inside
`#ifdef CONFIG_PM_SLEEP_DEBUG`, `kernel/power/main.c:384`) — to re-check on the
next boot; until it appears, the ladder cannot be driven and only whole-system
s2idle can be tested (with an armed alarm).

Offline validation: `./build.sh linux` completed; `scripts/preflight.sh --full`
completed with `preflight: OK`, including `./build.sh check` (25 patches apply
on clean upstream; dtbs_check reports 13 existing vendor-series diagnostics).
Installed all three decoded blobs in the root-owned Fedora root tree using
`picasso-pack`, and repacked the resident initramfs as root in that container.
Lockdown is not a touch-bootloader register: downstream
`techpack/display/msm/dsi/dsi_panel_mi.c:dsi_panel_lockdown_info_read` reads eight
bytes through panel DSI commands after panel initialization. No SPI OTP address
is invented here; obtaining those bytes still needs a separate DSI read path.

### Suspend ladder: the phone survives every level, the USB console does not

`pm_test` does exist on the new kernel (`ls /sys/power`: `pm_test`, `mem_sleep`,
`state`, `suspend_stats`, …) with levels
`none core processors platform devices freezer`.

- `freezer`: `Freezing user space processes completed (elapsed 0.002 seconds)`,
  `PM: suspend debug: Waiting for 5 second(s).`, `PM: suspend exit` — passes.
- `devices` and below: the phone **stays alive** (the composite gadget
  re-enumerates, `Bus 001 Device 094`) but the CDC-ACM console goes away and its
  shell dies with the tty hangup, so the results cannot be read back over that
  console; the half-local addresses also stay unreachable (host `enx…` keeps no
  carrier after the resume). A power cycle is the only way back once that
  happens.
- `rtcwake` is not an option yet: `/sys/class/rtc/rtc0/wakealarm` does not exist
  and reading the RTC returns `EIO`, so the PMIC alarm IRQ is not wired
  (`rtc-pm8xxx` clears `RTC_FEATURE_ALARM` when the alarm IRQ is missing). Until
  that is fixed, a real s2idle suspend has **no** self-wake: the power key is
  the only candidate (now `wakeup-source`), and a failed wake means holding the
  key, i.e. power off.

`bringup/pmtest-ladder.sh` therefore runs the whole ladder detached and writes to
`/cache/pmtest.log` on the UFS cache partition, so the result survives both the
console hangup and a reset. Next: power-cycle, run it, read the log from the next
boot.

Review follow-up: SRAM upload now retries the complete reset/download/CRC/
BOOT_RDY/reset-state check up to three times and decodes CRC status bits
(done=bit2, ILM=bit0, DLM=bit1). NT36675-only support is explicit.
The length transfer already carried three data bytes plus the command; its
third byte is now masked to bit 16 exactly like downstream `hw_crc > 1`.
No payload rebuild or phone access was performed for this follow-up.

### The phone survived every pm_test level — what died was the console

After the ladder the phone was still running (its panel showed the console, the
composite gadget stayed enumerated), but neither the USB shell nor Wi-Fi
answered, so it looked dead. Root cause: at the `devices` level the USB
controller suspends, the tty hangs up, `bash` on `/dev/ttyGS0` exits with
status 0, and `ttygs-shell.service` had `Restart=on-failure`, which does not
restart on a clean exit. Nothing respawns the console afterwards.

Fixed in the rootfs (and repacked, sha256 `2cd4790c…`):

- `ttygs-shell.service`: `Restart=always` — the console comes back by itself
  after a suspend/hangup.
- `usb0-up.service`: besides `ip link set usb0 up`, it assigns a static
  `192.168.7.2/24`; with `192.168.7.1/24` on the host side, ssh over the cable
  works even when wlan0 never associates. That is the channel that should have
  been used while Wi-Fi was flapping.

So the pm_test result stands and is better than it looked: the SoC resumes from
`freezer`, `devices`, `platform`, `processors` and `core` — the kernel does not
hang in suspend — and only the observation channel was lost.

### pm_test ladder result: freezer, devices and platform all pass (2026-11-16)

Run detached, logged to the UFS cache partition (`/mnt/ufs-logs/pmtest.log`,
mounted from the `cache` partition), read back over the USB console:

```
freezer   -> PM: suspend debug: Waiting for 5 second(s). / PM: suspend exit
devices   -> pass (the USB gadget re-enumerates afterwards)
platform  -> pass
processors, core -> "PM: Unsupported test mode for suspend to idle,
                    please choose none/freezer/devices/platform."
suspend_stats: success 3, fail 2
```

So the freeze, device-suspend and platform-suspend phases complete and resume on
this SoC; `processors`/`core` are refused by the kernel because this platform
only offers **s2idle**, not shallow/deep. Two things now make the ladder
survivable and repeatable:

- `ttygs-shell.service` is `Restart=always`, so the USB console comes back after
  the gadget re-enumerates (it did, on the same `/dev/ttyACM0`);
- `usb0-up.service` assigns `192.168.7.2/24` and the host side `192.168.7.1/24`,
  so ssh over the cable works at ~1 ms even with no Wi-Fi; after each
  re-enumeration the host address has to be re-added (NetworkManager drops it).

Still missing for a *real* suspend: a self-wake. `rtc0` has no `wakealarm`
(the PMIC alarm IRQ is not wired, `rtc-pm8xxx` clears `RTC_FEATURE_ALARM`), so
the only wake source in the list is `pwrkey` (now `wakeup-source`) and
`pm7250b-usb` (VBUS). The next test is a real s2idle suspend ended by one press
of the power key; `bringup/suspend-test.sh` still refuses to suspend without a
verified alarm, so this one is driven by hand.

### Real s2idle: the phone powers off instead of resuming (2026-11-16)

With the ladder green, the next step was a real `echo mem`. `pwrkey` is a
wakeup source in the DT and appears in `wakeup_sources`, so pressing the power
key should have been the wake event. Instead the phone **turned off**.

On this PMIC that is the signature of **PS_HOLD dropping**, not of a missing
wake source: when the SoC dies (hang or crash in the resume path) the PMIC cuts
power a second or two later, which looks exactly like "I pressed the button and
it switched off". A plain key press while suspended cannot power the phone off
by itself - the PON only does that on a long hold.

So the remaining problem is the **resume path**, and the evidence to look at is
the kernel log that `log-to-ufs` writes to the UFS cache partition, not the
live console (which dies with the SoC). Next: power on, mount the cache
partition, read `/mnt/ufs-logs/picasso-logs/` for the last lines before
PS_HOLD, and work backwards from the last driver that printed.

Two smaller follow-ups this exposed:

- `rtc0` still has no `wakealarm`, so the only wake candidates are `pwrkey`
  (armed, but the press is followed by the SoC dying) and `pm7250b-usb`
  (VBUS change). An RTC alarm would let a test wake the phone without hands,
  which matters while the resume path is broken.
- `enable_irq_wake()` on an SPMI PMIC IRQ only works if the PMIC arbiter chain
  is wake-capable; worth verifying with `dmesg | grep -i wakeup` after the
  next boot rather than assuming the DT property was enough.

### Suspend/resume WORKS — wake it with volume-up, not power (2026-11-16)

A real `echo mem` with the diagnostic cmdline (`no_console_suspend`,
`pm_print_times`, `cpuidle.off=1`) and an **armed volume-up wake** completes:

```
[77.015389] PM: suspend entry (s2idle)
[80.626626] PM: suspend devices took 3.492 seconds
[93.044948] PM: resume devices took 0.716 seconds
[93.058285] PM: suspend exit
```

After the resume: USB gadget up, `wlan0` back with a DHCP lease
(<lan-ip>), backlight still 1638, uptime continuous. So the s2idle path
itself is sound on this port - 0.7 s to resume every device.

What does **not** work is waking with the **power key**: every attempt ended in
the phone powering off (PS_HOLD drop) instead of resuming. That is the PON path,
not the suspend path. `wakeup-source` is therefore removed from `pon_pwrkey`
and kept on `volume-up` (`gpio-keys`) and `volume-down` (`pon_resin`), which are
the keys that resume cleanly.

Still open:

- `cpuidle.off=1` was in the cmdline for this test; the next cycle boots without
  it to see whether the PSCI `domain-idle-states` (cluster collapse,
  `arm,psci-suspend-param = <0x41000044>`) are safe. If they are not, trim them
  in the SoC dtsi instead of carrying a boot flag.
- `rtc0` still has no `wakealarm`, so an unattended self-wake test is not
  possible yet (PMIC alarm IRQ not wired); the alarm would also let a test wake
  the phone without hands.

### Suspend without `cpuidle.off`: same clean resume

Second real suspend, this time with the plain cmdline (no `cpuidle.off=1`, only
`no_console_suspend pm_print_times` kept for the run), woken by **volume-up**:

```
[44.126180] PM: suspend entry (s2idle)
[47.893543] PM: suspend devices took 3.680 seconds
[48.671859] PM: resume devices took 0.756 seconds
[48.677336] PM: suspend exit
```

0.756 s to resume every device, ~1 s from wake to prompt - the same as with
cpuidle disabled (0.716 s), so the PSCI `domain-idle-states` (cluster collapse,
`arm,psci-suspend-param = <0x41000044>`) are **not** a problem and the boot flag
can go. Suspend/resume is therefore working on this port:

- **enter**: `echo mem` -> s2idle, 3.7 s for all devices;
- **wake**: volume-up (or volume-down); the PMIC power key instead drops
  PS_HOLD and powers the phone off, so it is not a wake source in the DT;
- **after**: USB gadget, `wlan0` with a fresh lease, backlight and uptime all
  come back;
- the only missing piece for unattended testing is an RTC alarm
  (`/sys/class/rtc/rtc0/wakealarm` does not exist: PMIC alarm IRQ not wired).

Still to do for the done-list: 20 sleep/wake cycles, and checking that modem and
Wi-Fi still work after each (the Luma brief asks for exactly that).

### Touch firmware runs and finger reports arrive (2026-11-16)

Tested the coordinator's merged RAM payload, SHA256
`5da42ba36ac18436d22d21af5118eb5349b20b3a5649abc5f423ebb9ce036ead`,
with the Fedora rootfs containing the decoded fw01 above. Cmdline:
`console=tty0 loglevel=7 picasso_pinmux.mode=write rdinit=/usr/lib/systemd/systemd systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10 no_console_suspend pm_print_times`.
No reboot was needed: inspected the boot upload, then rebound only spi0.0.
Exact rebind logs:

```
SRAM partition 0 addr=0x0 bytes=80504
partition 0 golden CRC=0x58e87d07
SRAM partition 1 addr=0x27140 bytes=16792
partition 1 golden CRC=0x8b987915
SRAM partition 14 addr=0x27040 bytes=256
SRAM upload CRC enabled BOOT_RDY ret=0
upload reset state=0xa1 polls=1 ret=0
upload CRC flags=0x4 done=1 ilm=0 dlm=0
fw reset state 0xa1 after 1 polls (fw running)
```

Two pre-rebind probes (60s and 30s) gave zero reports after earlier suspend;
the log contained `Attempted to sync while suspend`. Rebinding re-uploaded fw01
successfully. With user-confirmed tapping and swiping during the next 60s,
`bringup/touch-probe.py` measured **83 SYN_REPORT**, EV_ABS=357, EV_KEY=18,
slots [0,1], X=216..739 (136 samples), Y=86..2204 (161 samples), and repeated
BTN_TOUCH 1/0 transitions. Coordinates are within 0..1079 / 0..2399; edge
accuracy and all ten fingers remain untested.

Added PM hooks to cancel polling before suspend and restart it on resume;
failed SPI reads only warn once and retry, never emit a partial frame. Also
corrected firmware-info diagnostic offsets (the helper already strips the SPI
command byte); the previous broken-info warning was a diagnostic indexing bug.
These follow-up changes are compiled offline; touch across suspend with this
version is still untested. Lockdown bytes remain unknown, fw01 works for this
panel, and firmware provenance is the downstream ihex rather than the MIUI
vendor image, which lacks these files. No flash operation was performed.

### Touch WORKS (2026-11-16): SRAM firmware upload + real finger reports

The controller needed its firmware loaded by the driver (host-download to SRAM,
not a flash write). With that in place (`0011`), one 60 s probe with a finger on
the panel gave:

```
83 SYN_REPORT   slots [0,1]   BTN_TOUCH 1/0 repeated
X 216..739  (136 samples)
Y  86..2204 (161 samples)
```

which is the full chain working end to end:

1. SWRST_N8 `0x69` + `SPI_RD_FAST=0`, then the container parsed exactly like the
   vendor SPI driver (ILM/DLM at list*12, info at 0x30+16n, overlays at DLM
   BIN_addr+16n, `size+1` writes);
2. golden CRCs written to `0x3f100/0x3f104`, ILM/DLM length and destination
   registers set, `BLD_CRC_EN |= 0x80`, reset status cleared, `HOST_CMD 0xAE`,
   `BOOT_RDY 0x3f10d <- 1`;
3. firmware reports ready: `reset state 0xa1` (0xa0..0xaf), CRC flags `0x4`
   (`crc_done=1`), first attempt, no retries needed;
4. the poller reads the NT36675 event buffer (`0x22d00`) and the input device
   emits multitouch slots with coordinates inside the 1080x2400 panel.

Two traps found on the way, both fixed:

- **the poller died on suspend**: `picasso-nvt-ts: Attempted to sync while
  suspend`, SPI returned `ESHUTDOWN` and the delayed work never rescheduled, so
  every probe after a suspend silently saw nothing. The driver now has
  `nvt_suspend`/`nvt_resume` (`DEFINE_SIMPLE_DEV_PM_OPS`) that cancel and restart
  the poll, and the read path retries instead of dying on an error;
- **a duplicate `wakeup-source`** in `key-vol-up` broke every clean DTB build
  (`dtc: duplicate_property_names`), which is why the exported series failed the
  CI check while the local tree still built from a stale dtb. Removed.

Wake keys, settled by measurement: **volume-up and volume-down resume the phone,
the power key powers it off** (PON -> PS_HOLD drop), so `pon_pwrkey` is not a
wake source in the DT.

### Sleep/wake soak: 10/10 cycles, then Wi-Fi and touch both back (2026-11-16)

`bringup/suspend-soak.sh 10 /mnt/ufs-logs/soak.log VOLUME-DOWN`, one volume-down
press per cycle, logged to the UFS cache partition:

```
cycle 1..7: resume devices took 0.340-0.344 s
cycle 8..10: resume devices took 0.688-0.736 s
soak done: 10/10 cycles counted by suspend_stats
success=10 fail=0
```

Notes from the run:

- **Wi-Fi**: the log says `wlan0=down` on every cycle because it samples 2 s
  after the wake; the interface actually re-associates a moment later
  (`wlan0: authenticated/associated`, lease <lan-ip>) and stays up. The soak
  check should wait for the association instead of sampling once.
- **Touch survives the suspends**: this boot's log shows the firmware path
  working (`boot CRC flags=0x4`, `CRC result ILM=0x58e87d07 DLM=0xb1eaa281`,
  `fw info: ver 0x14 x_num 18 y_num 36 abs 1080x2400`) and **no**
  `Attempted to sync while suspend`, i.e. the new `nvt_suspend`/`nvt_resume`
  hooks restart the poller.
- The ADSP sensor-PD watchdog still fires every ~40 s (`USER-PD DOG detects
  stalled initialization`), unrelated to suspend; it is the open sensors item.
- Resume time doubling on cycles 8-10 (0.34 -> 0.7 s) is the WLAN/modem path
  coming back, not a failing device.

Still to check: finger reports after a resume (needs a tap), and the longer
"sleep and wake by incoming call" case which needs the modem stack.

### Touch after suspend: the SRAM firmware is lost, so resume must re-upload it

The 10-cycle soak left the driver healthy but **silent**: taps produced no
reports and the driver logged nothing (no `ESHUTDOWN`, no sync-while-suspend).
Rebinding the SPI device re-ran the whole firmware path and the very next tap
reported again:

```
rebind -> boot CRC flags=0x4, CRC ILM=0x58e87d07 DLM=0xb1eaa281,
          fw info ver 0x14 abs 1080x2400
25 s probe with a finger -> X 366..659 (76 samples), Y 310..2273 (128 samples),
          BTN_TOUCH [1,0,1,0,1,0]
```

So the controller loses its **SRAM firmware** (and with it the reporting state)
across s2idle - the poll work item was not the problem after all. The fix is in
`nvt_resume`: redo the boot sequence there (SWRST_N8 -> SPI_RD_FAST=0 ->
re-upload the container with CRC/BOOT_RDY -> reset-state check) before
rescheduling the poll, and log the reset state per resume. A report counter
should also be exposed so "no reports" can be told apart from "poller dead"
without a rebind.

Until that lands, touch works after a fresh probe/rebind and stops working after
the first suspend.

Coordinator follow-up evidence: ten s2idle cycles passed (success=10 fail=0,
resume 0.34s), but a finger produced zero reports afterward. A touch-only
rebind restored reports (25s probe X=366..659, 76 samples; Y=310..2273,
128 samples; BTN_TOUCH 1/0 transitions). Resume now re-downloads SRAM firmware
with the same full retry/CRC/reset-state sequence before restarting polling.
Added read-only `report_count` under the SPI device; it counts successfully
read frames passed to the parser, including idle frames, to distinguish a dead
poller from no fingers. This resume reload remains pending a new hardware soak.

### A mobile shell on the RAM base: weston yes, Phosh close, Luma itself needs `userdata`

What is on screen today without writing UFS: **weston with the pixman
renderer** (desktop-shell, 1080x2400, touch input live). That is the working
compositor, and it is what made the "no UI" bug go away.

Attempted on top of that, same image, no flash:

1. `dnf --installroot … --forcearch=aarch64 --setopt=tsflags=noscripts install
   phosh phoc greetd` into the Fedora tree (149 packages, tree 1.2 -> 1.8 GiB).
   Missing %post steps had to be replayed by hand: `glib-compile-schemas`,
   `gdk-pixbuf-query-loaders`, `mkdir /run/user/0`.
2. libseat has no seat for a plain system service: `seatd` + `LIBSEAT_BACKEND=seatd`
   fixed that; an agetty autologin was tried first and only produced a
   restart loop.
3. phoc on DRM then fails exactly like the reference SM7250 port describes:
   `connector DSI-1: Atomic commit failed: Device or resource busy` - the DPU
   atomic path, not the renderer.
4. Nested inside weston (`WLR_BACKENDS=wayland`) phoc connects and the Phosh
   shell runs, then dies on a **GTK icon**: `Failed to load
   /usr/share/icons/Adwaita/scalable/status/image-missing.svg: Unrecognized
   image file format`. Fedora 44 no longer ships an SVG loader for
   gdk-pixbuf (`librsvg2` and `gdk-pixbuf2-modules-extra` were installed and
   contain none), and Phosh 0.57 uses GTK3, which needs one.

So the blocker for a *mobile shell* on the RAM base is packaging, not the
kernel: an SVG pixbuf loader (or an icon theme with PNGs). That is fixable.

**Luma OS itself is a different matter**: its ARM image is 9.37 GB sparse with
`LumaBoot`/`LumaRoot` inside `userdata`, so running it means writing `userdata`
(a flash write the owner has deferred) - it cannot live in the 5.3 GB of RAM
next to the kernel.

### The requested repartition does not fit: userdata holds ~50 GiB of real data

Measured on the phone (read-only survey, nothing written):

| partition | device | size | fs | contents |
|---|---|---|---|---|
| `userdata` | sda34 | **87.0 GiB** (sector 2,883,584..25,701,119) | ext4 | `rootfs.img` **32 GB**, `local/` **17 GB**, `media/` 422 MB, `android-data/` 145 MB |
| `darwin` | sda35 | 20.0 GiB | **APFS** | the HakiOS XNU port's root |
| `super` | sda33 | ~68 GiB | | Android dynamic partitions |
| `cache` | sda30 | 384 MiB | ext4 | our logs |

Free space on the disk: **0 sectors** - the GPT ends exactly at the last
partition. So a new 20 GiB partition can only come from shrinking `userdata`,
and `userdata` is **~50 GiB used**, which does not fit in 20 GiB. The requested
layout ("Luma in userdata, the current Droidian content moved to a 20 GB
`linux`") is therefore impossible as stated: the content to move is 2.5x the
target size.

Two layouts that do fit, both needing an offline ext4 shrink of `userdata`
(`e2fsck -f` + `resize2fs`, then delete/recreate the GPT entry keeping name,
type and PARTUUID, then a new entry for the freed tail):

1. **Luma in a new 20 GiB `linux`, Droidian untouched**: shrink userdata
   87 -> 67 GiB (50 GiB of data still fits), create `linux` in the freed tail,
   write the Luma image there. Least destructive; Luma's 9.37 GiB image fits
   comfortably.
2. **Luma in userdata, Droidian moved to `linux`**: the new partition would have
   to be ~50 GiB, i.e. userdata shrinks to ~37 GiB - still larger than Luma's
   image, so it works, but it needs the 50 GiB copied twice and the shrink done
   in stages (there is no scratch space for a single pass), which is hours of
   risky I/O on the only copy of that data.

Either way the Droidian data is unique (no backup exists), so it should be
copied and verified before anything is deleted or shrunk.

### Luma OS nightly is installed on the phone (2026-11-16)

Layout now, after the owner chose "Luma in a new partition, Droidian untouched":

| # | partition | sectors (4096) | size | fs | content |
|---|---|---|---|---|---|
| 34 | `userdata` | 2,883,584..20,447,231 | **67.0 GiB** (was 87) | ext4 | Droidian, intact: `rootfs.img` 32 G + `local/` 17 G verified after the shrink |
| 35 | `darwin` | 25,701,120..30,944,250 | 20.0 GiB | APFS | HakiOS XNU port, untouched |
| 36 | **`linux`** | 20,447,232..25,701,119 | 20.0 GiB | ext4 -> Luma disk image | **Luma OS nightly 20261114.2** |

Steps, all verified:

1. `e2fsck -f` + `resize2fs` on userdata to 67 GiB (data kept), then GPT:
   entry 34 recreated with the same name/type covering the smaller range, entry 36
   `linux` (type 8300) filling the freed gap before `darwin`. GPT backed up to
   `/mnt/ufs-logs/sda-gpt-before-backup.bin` first. Note: userdata's **PARTUUID**
   changed (`32684d05…` -> `c4790697…`); the filesystem UUID is unchanged and both
   Android and Droidian reference the filesystem, not the partition GUID.
2. `mkfs.ext4 -L linux /dev/disk/by-partlabel/linux`.
3. Downloaded `luma-nightly-20261114.2-aarch64-disk.sparse` (9,370,839,372 B,
   SHA-256 `d3dd2eeb…fe00b`, matches `latest.json`), `simg2img` to 12 GiB raw,
   streamed into the partition over USB (`cat image | ssh dd of=… bs=4M conv=fsync`).
4. Verified the result with `losetup -b 4096` (the image's GPT uses 4096-byte
   sectors, so a plain 512-byte loop shows nothing):

```
Disk /dev/loop0: 5253888 sectors, 20.0 GiB
 1   256..123135    480.0 MiB  EF00  LumaBoot
 2   123136..3145471 11.5 GiB  8300  LumaRoot
/dev/loop1 12G 8.9G used, mounted read-only
/ostree/deploy/luma/deploy/cf402159dadca020740f5a8bcf7f50928efbe24bbd2ee1441a2f7438b6419697.0
/boot/loader/entries/ostree-1.conf
```

The deployment commit equals the nightly commit from `latest.json`, so the image
is complete and consistent.

Remaining for a Luma boot: a boot image whose initramfs finds LumaRoot **inside**
the `linux` partition (nested GPT: `losetup -b 4096 -o 504365056`, or scan the
image), resolves that ostree deployment, and switches root - then our UEFI chain
can RAM-boot it.

## Touch: the controller runs on fw03, the report pipe is still idle

Measured on the phone (RAM boot, `picasso-nvt-*` dmesg + sysfs):

- The host SRAM download was already byte-exact (read back SRAM 0 ==
  `file[0x100:]`, SRAM 0x27140 == `file[0x18100:]`), and needs the download to run
  *after* the panel has been prepared (the touch shares the TDDI reset).
- **The firmware variant was wrong.** The vendor picks it from the panel's
  lockdown bytes (`0x46,0x36,...` -> `novatek,cfg_2` -> `fw03`); the driver
  defaulted to `fw01`. With fw03 the controller reports
  `fw info: ver 0x0a (~0xf5) x_num 16 y_num 36`, `type 0x01`, `PID 0x5933` --
  identical to the vendor boot -- and its INT line starts pulsing.
- `drivers/input/touchscreen/picasso-nvt-vendor.c` (ported from the vendor
  `nt36xxx` core) downloads fw03, matches the ILM CRC exactly (`0x547123e5`),
  reaches ReK (`0xA1`) and reads `0xFD`-filled reports: the controller runs but
  does not publish contacts yet.
- `0x22D51 = 0xA5` is a red herring (see docs/bringup/touch-dsi.md); the vendor's
  handshake constant is `HANDSHAKING_HOST_READY 0xBB`, test-mode only.
- The panel installed on this phone is `g7a_36_02_0c`, not the `g7a_37_02_0a`
  table our panel driver carries; the vendor on-table's `FF 27 / FB 01 / 3F 01 /
  43 08 / 40 25` group is sent *after* `0x29` and is the strongest candidate for
  the TDDI gate. `docs/bringup/touch-dsi.md` records the whole vendor recon with
  file:line citations and the remaining unknowns.

Next: bind the 36_02_0c table (and stop re-sending `0x11`/`0x29` after it), then
re-test `report`/`libinput debug-events` with a finger down. Trigger for the
touch IRQ is `EDGE_RISING` (the line idles low; level-low storms).

### Touch: paused 2026-11-16 (resume here tomorrow)

Artifacts ready to RAM-boot (built, not yet run): panel `36_02_0c` table
(`xiaomi,panel-g7a-36-02-0c` in the DTS), vendor touch driver
(`picasso-nvt-vendor.c`, fw03, `IRQF_TRIGGER_RISING`, sysfs `state`), and the
deployment's shutdown teardown + enabled 20-minute `picasso-autoreboot.timer`.
First boot of that payload must check, in order: `dmesg | grep -iE "g7a|nvt-vendor"`
(panel variant + touch bring-up), `cat /sys/bus/spi/devices/spi0.0/state`
(state/irq/reports/contacts with a finger down), `libinput debug-events`,
`systemctl list-timers | grep autoreboot`, then one `reboot` to prove the
teardown reaches the loader.

### Next session — finish the feature bring-up (paused 2026-11-16)

Order, with the artifacts that already exist:

1. **Touch / panel** — boot `build/out/payload-luma-okernel.bin` (build of
   17:xx): panel `36_02_0c` table + vendor touch driver (fw03,
   `IRQF_TRIGGER_RISING`, sysfs `state`) + deployment teardown + enabled
   20-minute `picasso-autoreboot.timer`. Check, in order: `dmesg | grep -iE
   "g7a|picasso-nvt-vendor"` (variant + bring-up), `cat
   /sys/bus/spi/devices/spi0.0/state` with a finger down, `libinput
   debug-events`, `systemctl list-timers | grep autoreboot`, then one `reboot`
   to prove the teardown reaches the loader.
2. **Denial Mobile** — write `~/denial-staging/userdata.raw` (ext4 6.14 GiB,
   sha256-verified zip, marker `/usr/lib/phonewave-system/root` present) into
   `sda36`, rename the partition to `denial`, and point the initramfs at
   `PARTNAME=denial` (contract in `docs/denial-mobile-architecture.md`). First
   boot headless: drop `flashed/authorized_keys`, then over SSH run
   `first-user <user> "<name>" "" en <tz>` and `first-user --start`. Needs the
   owner's approval for the UFS write (it erases the Luma image).
3. **Repo hygiene** — the work of this session lives in the `build/linux`
   working tree and in `docs/`/`docs/changelog/boot-history.md`/`build.sh`. Before handing over:
   `./build.sh export-patches` (already taught to carry
   `picasso-nvt-vendor.c`), then `./build.sh check`.
4. Then, and only then, the media-payload boot image of
   `docs/multi-os-plan.md` §5 (multi-OS + no-host boot).

### Suspend works on the Luma boot (verified 2026-11-17, uptime 14 h)

What looked like "the phone dropped off the link" was **s2idle suspend**, and the
power button resumed it:

- `systemd-sleep`: `Performing sleep operation 'suspend'` 06:50:58 →
  `System returned from sleep operation 'suspend'` 06:51:09 (11 s), then
  `panel-xiaomi-g7a ae94000.dsi.0: prepare: power mode pm=0x9c err=0` at
  06:51:11 — the panel driver re-initialises the panel on resume.
- `dwc3-qcom-legacy a6f8800.usb: port-1 HS-PHY not in L2` at the two suspend
  boundaries; the USB gadget re-enumerates (host sees
  `enx020000000001`, MAC 02:00:00:00:00:01 as the gadget advertises).
- logind dropped the greeter session (7) at resume; the lock screen the user saw
  is GDM's greeter.
- `mem_sleep` = `s2idle` only, so this is the freeze path, not deep suspend.

Consequences recorded for the test loop:

1. Idle suspend is now **blocked on the running system**:
   `systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target`
   (persistent symlinks to /dev/null; `systemctl suspend` refuses). To test
   suspend again, `systemctl unmask sleep.target suspend.target` — the
   `/sys/power/state` pm_test ladder is unaffected by the mask.
2. The phone's gadget address on this boot is **`usb0` 192.168.7.2/24**, not the
   10.77.71.2/30 Denial init uses; the host profile "Wired connection 2" is DHCP,
   so after resume the host comes back with **no IPv4** and SSH/console-over-net
   fails even though USB re-enumerated. **Fixed** on the workstation:
   `nmcli con mod "Wired connection 2" ipv4.method manual ipv4.addresses 192.168.7.1/24 ipv4.gateway "" ipv4.never-default yes`
   (persistent), ping 192.168.7.2 ≈ 2 ms.
3. `sshd` on the phone listens on `[::]:22`. Root's `authorized_keys` now holds
   the workstation key `~/.ssh/picasso_key.pub`, so:
   `ssh -i ~/.ssh/picasso_key root@192.168.7.2` works (verified). Restoring the
   host address and the key is what a resume needs if NetworkManager forgets the
   profile again.

## Touch: WORKS on hardware (verified 2026-11-17)

`state=0xa3 irq=32898 reports=25625 contacts=28876` with a finger on the glass,
libinput `cap:t ntouches 10`, weston binds `event2` to `DSI-1`, touches land in
the compositor. Four separate defects had to go, all in the touch/panel pair:

1. **Bring-up raced the panel.** Uploading while the panel was off (Luma keeps
   it off until DPMS on) left the controller at ReK `0xA1` for good. The panel
   driver now exports a blocking notifier (`include/linux/picasso-g7a.h`,
   fired at the end of every successful `prepare`) plus
   `picasso_g7a_panel_prepared()`; the touch driver registers for it and gates
   its upload on it. Boot result: `normal run state 0xa3 after 80-90 ms`, first
   attempt — no delay guessing (`upload_delay_ms` is only the initial timer).
2. **Report decode was off by one byte.** The ten 6-byte records start at
   byte 0 of the pipe (`point + 6 * i`); an empty slot reads `0xff`, i.e.
   status 7 / slot 31, which the range checks drop. With `point + 1 + 6 * i`
   every record decoded to status 7 and `reports` stayed 0 while the IRQ count
   climbed.
3. **Resume never reset the controller.** `nvt_suspend` parks it in deep sleep
   (host command 0x11); `nvt_resume` used to re-upload without pulsing TP_RESX,
   so every read came back `0xff` and the retries inherited the failure. Resume
   now defers to the bring-up worker (which always pulses the engineering
   reset) and the poll loop re-arms it whenever the reset state drops below
   `0xA3` while the panel is prepared — that self-heals after any panel prepare.
4. **`picasso_pinmux.mode=write` is mandatory in the cmdline.** Without it the
   TLMM tiles are never written, SPI reads `0x00` and probe fails with
   `not a supported Novatek controller (00 00 00 00 00 00)`. The payload must be
   rebuilt with it.

Also fixed: the `state` attribute is now actually attached
(`devm_device_add_group`), so `…/spi0.0/state` exists; and the IRQ path works
(`IRQF_TRIGGER_RISING`, ~120/s while touched), so polling is only a fallback.

Patches: the header ships in `picasso/0006-drm-panel-xiaomi-g7a.patch` (the
touch patch 0011 depends on it), the driver fixes in
`picasso/0011-input-picasso-nvt-sram-firmware.patch`.

### Touch: CI gate and the one follow-up

`./build.sh check` on the exported series: `[OK] 25 patches applied`,
`[OK] series applies to a clean tree and the DTB builds` (14 dtbs_check
diagnostics, all from the pre-existing vendor series).

A clean boot of the final payload: `normal run state 0xa3 after 80 ms`, then
`state=0xa3 irq=1128 reports=875 contacts=892` after a few seconds of touch.

Follow-up found while testing resume with this image: `echo mem >
/sys/power/state` **does not wake on the power key** — neither press resumed it,
both landed in the loader (i.e. the PMIC key is not registered as a wakeup
source here; the older installed-Luma image did resume, so this is an
initramfs/DT gap, not the touch driver). To test resume of the touch driver:
enable wakeup on the pmic pwrkey (`wakeup-source` in the DT node, or
`echo enabled > .../power/wakeup`) and on the dwc3 controller, then a short power
press should resume. The bring-up worker is expected to rebuild the controller
by itself (resume schedules it, and the poll loop re-arms it whenever the state
drops below 0xA3 while the panel is prepared) — that path is written but not yet
observed on hardware.

## Audio: playback works on hardware (verified 2026-11-17)

Speaker: MultiMedia1 → q6asm → ADM → AFE SECONDARY_MI2S_RX → TFA9874 → speaker.
Measured with the tone tool (`initramfs/tone/tone.c`, now `tone <dev> <channels>`):
stereo playback through card 0 is audible, no userspace amp helper needed — the
in-kernel `tfa9874` driver's own init is enough.

Two defects, both in the DSP/AFE configuration rather than the amp:

1. **`qcom,sd-lines` is a list of line indices, not a count.** The DT had
   `<2>`, which q6afe-dai turns into `BIT(2)` ⇒ SD2, so the AFE never drove the
   amp's line. The vendor's sec-MI2S pinctrl answers which line that is: gpio74
   (`sec_i2s_data1_active`, `output-high`, 8 mA) carries audio, gpio72
   (`data0`) is `input-enable` — the amp's sense output. Now `<0 1>`, i.e.
   QUAD01 over SD0+SD1.
2. **The MI2S BE link must run four channels.** With the stereo frame the DSP
   puts both channels on SD0 and the amp, listening on SD1's slots, hears
   silence. `sm8250_be_hw_params_fixup()` now forces 4 channels for
   `SECONDARY_MI2S_RX` (the vendor DT says `tx-lines = 2`, i.e. the same
   QUAD01 frame); a stereo front end is mixed into it, so ordinary 2-channel
   apps play.

Verified states while sounding: AFE `i2s cfg rate 48000 bits 16 sd_mask 0x3
ch 4`; amp `0x10=0x0016` (PLLS/OTDS/UVDS), `0x14=0x00c8` (MAN_STATE 9),
`0x11=0xe2c0` (CLKS+AMPS), TDM `0x20/21/22/23 = 0890/c0f1/043c/0003`.

Instrumentation used for this, now removed: prints for `q6routing`'s
`q6adm_matrix_map` result, q6asm's trigger/run and q6afe's i2s cfg/port start
(all returned success — the commands were fine, the channel/line mapping was
not).

Known limit: the ADSP still trips `USER-PD DOG` in `sensor_process` every ~40 s
and remoteproc recovers it, which cuts any stream; playback resumes on the next
open. That is the next audio task, not a playback-path bug.

## Denial Mobile: image installed on UFS, boot hand-over in progress (2026-11-17)

Written and verified (owner approved the UFS write):

- `~/denial-staging/userdata.raw` (6,589,251,584 B, ext4, UUID
  `0322c979-a112-4b5b-96e4-22659df0c773`, zip sha256 checked) was streamed into
  `/dev/disk/by-partlabel/linux` (sda36, 20.0 GiB) from the running kernel with
  `dd bs=8M conv=fsync` (5m14s). Verified afterwards: `blkid` shows the same
  UUID, the filesystem mounts read-only and carries the marker
  `/usr/lib/phonewave-system/root` plus `Arch Linux ARM` in os-release. The Luma
  image that used to be there is gone.
- On the boot where the hand-over ran, the kernel mounted sda36 r/w, grew it
  (`EXT4-fs (sda36): resizing filesystem from 1608704 to 5253888 blocks`) and
  `systemd` from the image became PID 1 - the switch itself works.

`initramfs/init.c` gained the hand-over and is now the ramdisk's real init path:

- `picasso.switchroot=1` is **opt-in**: candidates are `picasso.root=<PARTNAME>`,
  then `denial`, then `linux`; a candidate is accepted only when it carries the
  phonewave marker. On acceptance: tmpfs on /run, the device drop-ins below,
  bind-mounts of the stock firmware partitions, MS_MOVE of /dev /proc /sys,
  `chroot`+`execv("/usr/lib/systemd/systemd")`.
- Device policy that must not live in the image goes to **/run drop-ins**:
  `hexagonrpcd.service.d/10-nonsecure.conf` (`-f /dev/fastrpc-adsp`, this port's
  node is the non-secure one) and `logind.conf.d/10-picasso-powerkey.conf`
  (`HandlePowerKey=suspend`, long press still powers off - the pwrkey already
  wakes the system, logind's default was what powered the phone off after a
  resume).
- The gadget now carries **ECM next to ACM** (`usb0`, host `02:00:00:00:00:01`,
  phone `02:00:00:00:00:02`), because the image's
  `usr/lib/systemd/network/10-phone-usb.network` assigns 10.77.71.2/30 itself and
  sshd listens there. The link has to be brought up **after** the UDC bind
  (before it, IFF_UP does not stick); with that, carrier is 1 and the host sees
  the interface immediately.
- `CONFIG_QCOM_FASTRPC=y` (configs/linux/picasso.config): without the driver the
  ADSP's sensor user-PD never finishes its init and its watchdog resets the whole
  ADSP every ~40 s. Denial ships `hexagonrpcd` + `usr/lib/hexagonrpcd/stock-sensors`,
  which lays the ADSP's sensor configuration out from `super` (vendor/odm) and
  `persist` - exactly the "read the stock partitions, keep the rootfs clean"
  model, no firmware baked into the image.

Open problem, measured (RESOLVED in subsequent milestone below): with **ECM present**,
early hand-over attempts left the phone unreachable due to invalid gadget link order
in configfs (`config_usb_cfg_link` returning `-EINVAL` when UDC is bound). This was
fixed by unbinding UDC prior to linking `ecm.usb0`, rebinding, and configuring the
link in userspace drop-ins.

Also fixed in this pass, all verified by rebuilding:

- `scripts/mkinitramfs.sh` computed ROOT two levels up (the script lives in
  `tools/`), so the ramdisk was never rebuilt from source; it does now.
- `scripts/ramboot-cycle.sh` no longer dies when `fastboot` finds no device
  (`set -e` on a command substitution); it reboots over ssh first, console second.
- `picasso-pinmux` now defaults to `write`: with `read` the UART5/SPI/i2c8/MI2S
  pads keep the bootloader's mux and touch reads 0x00 - the mode param was a trap.
- `initramfs/tone/tone.c` takes a channel count (`tone <dev> <channels>`); four
  channels on the two SD lines is what proved the amp's frame.

## Denial Mobile: boot hand-over, ECM networking, SSH, and sensor PD WORKS on hardware (verified 2026-11-17)

All boot hand-over, networking, shell, and daemon hand-over components verified on hardware:
serial console on `/dev/ttyACM1` (or `/dev/ttyACM0`) stays alive and enters Denial rootfs on UFS,
USB ECM networking brings up `10.77.71.2/30`, passwordless root SSH functions, and
`hexagonrpcd-stock-sensors` + `hexagonrpcd` successfully eliminates the ADSP sensor-PD watchdog resets.

### 1. Verified Artifacts & Hashes
- Kernel Image (`build/out/Image`): `687f6088b6c6070999312c0a03793296483eb356296d1d1a73f3bb6804061eb8`
- Initramfs (`build/out/initrd.cpio.gz`): `932426193fd61d23644634462abb3267ba51e7c8b0bc16ab5d7c1bfdb0851d95`
- DTB (`build/out/sm7250-xiaomi-picasso.dtb`): `6d90cbb0b60477f05d7b797351f9783254f2638cd6811e14706f0bc0d3024762`
- Boot image (`build/out/boot-linux.img`): `62bdb7b476617b4d86dc78f797b97baff8196fcc1c2af9a79297bcfcdd3fd41a`
- Payload (`build/out/payload-linux.bin`): `9fca765f8996cd54efa1f4d5997770702760fa3d583bb8985485ee9e6ff285d0`

### 2. Kernel Command Line
```text
console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10
```

### 3. Root Cause Analysis & Fixes Implemented

1. **Console child root hand-over (`initramfs/init.c`)**:
   - *Bug*: In `console_enter_new_root()`, the guard `if (console_switched || root_dfd < 0) return;` prevented the child from ever entering the new root. The child is `fork()`ed before PID 1 resolves `/newroot` (`root_dfd = -1`), so the child exited early without executing `chdir("/denial")` and `chroot(".")`. Furthermore, PID 1 moved `/dev` (`MS_MOVE`), leaving the ramdisk `/dev` empty and preventing `/dev/ttyGS0` reopen on hangup.
   - *Fix*: Guard changed to `if (console_switched) return;`. Child opens `/denial/picasso-init.log` directly (resolving `-1` `root_dfd` in child), chroots to `/denial`, and reopens root `/picasso-init.log`. In `switch_to_partition()`, PID 1 recursively bind-mounts `/newroot` to `/denial` (`MS_BIND | MS_REC`) so the child retains access to `/dev`, `/proc`, `/sys`, and `/run`.
   - *Fix*: In `console_loop()`, the inner poll loop now polls both `/dev/ttyGS0` (`p[0]`) and `switch_pipe[0]` (`p[1]`). When PID 1 signals the hand-over, `switch_pipe[0]` is drained and closed, immediately triggering `console_enter_new_root()` and printing `console: inside the system on UFS`.

2. **USB Gadget ECM configuration order (`write_ecm_unit()`)**:
   - *Bug*: In `write_ecm_unit()`, `/run/usb-ecm.sh` attempted `ln -sf "$G/functions/ecm.usb0" "$G/configs/c.1/ecm.usb0"` before unbinding the UDC (`echo "" > "$G/UDC"`). Under Linux USB gadget configfs (`drivers/usb/gadget/configfs.c:config_usb_cfg_link`), creating a configuration link while `gadget_driver.udc_name` is set returns `-EINVAL`. As a result, no `usb0` interface was ever instantiated.
   - *Fix*: Clear `$G/UDC` first (`echo "" > "$G/UDC"`), symlink `functions/ecm.usb0` into `configs/c.1`, rebind `$UDC`, wait for interface creation, and bring `usb0` up with MAC addresses `02:00:00:00:00:02` (dev) and `02:00:00:00:00:01` (host). All actions log progress and errors to `/picasso-init.log`.

3. **SSH Root Key Provisioning (`write_device_dropins()`)**:
   - Denial Mobile's `sshd.service` enables `PermitRootLogin prohibit-password` and `PubkeyAuthentication yes` on `10.77.71.2:22`.
   - `init.c` copies `~/.ssh/picasso_key.pub` into `/newroot/root/.ssh/authorized_keys` (mode 0600) and `/var/lib/phonewave-system/flashed/authorized_keys`. Passwordless SSH login (`ssh -i ~/.ssh/picasso_key root@10.77.71.2`) works out of the box.

4. **ADSP Sensor-PD Watchdog Elimination & `stock-sensors`**:
   - `CONFIG_BLK_DEV_DM=y` enabled in `configs/linux/picasso.config` so `dmsetup create --readonly` can map `super` partitions.
   - Symlinks for `/dev/disk/by-partlabel/super` and `persist` pre-created in `initramfs/init.c`.
   - `hexagonrpcd-stock-sensors.service` succeeds (status 0), mapping vendor/odm/persist into `/run/hexagonrpcd`.
   - `hexagonrpcd.service` runs against `/dev/fastrpc-adsp`, completely eliminating the ~40 s ADSP sensor-PD watchdog resets.

5. **Tooling & Console Port Detection**:
   - `scripts/console_cmd.py` and `scripts/ramboot-cycle.sh` updated with auto-detection for `/dev/ttyACM1` / `/dev/ttyACM0` and added Denial mobile IP `10.77.71.2` reboot test.

### 4. Verified Hardware Test Results on Phone

- **Boot method**: Loaded via fastboot into RAM and booted with `fastboot oem run`.
- **USB Networking**: Workstation interface `enx020000000001` received `10.77.71.1/30` via DHCP; ping to `10.77.71.2` succeeded with 0.29 ms latency.
- **SSH Access**:
  ```text
  ssh -i ~/.ssh/picasso_key root@10.77.71.2 "uname -a"
  Linux archlinux 7.3.0-rc7-00001-ge1927d6cbd57-dirty #176 SMP PREEMPT ... aarch64
  ```
- **System Services Status**:
  - `hexagonrpcd-stock-sensors.service`: Active (exited cleanly with code 0).
  - `hexagonrpcd.service`: Active and running without watchdog resets.
  - `deniald` / `Xwayland`: Active and running on `/dev/dri/card1`.
- **Serial Console**: Live interactive shell verified on `/dev/ttyACM1` displaying `picasso> Linux 7.3.0-rc7...` with complete `/picasso-init.log` confirming clean root hand-over and ECM configuration.

### 5. Phase 2 Verification: `/picasso-init.log`, `first-user`, `pacman -Syu`, ADSP Stability, and CI Hygiene

- **Exact `/picasso-init.log` from live system**:
  ```text
  [5.315] boot: switching to linux (/dev/sda36)
  [5.327] boot: pipe signalled (1, ok)
  [usb-ecm] starting at 2026-11-15T15:27:21+00:00...
  [usb-ecm] current UDC: 'a600000.usb'
  [usb-ecm] clearing UDC before configfs link
  [usb-ecm] ecm linked into c.1
  [usb-ecm] rebound UDC a600000.usb
  [usb-ecm] usb0 up
  [usb-ecm] done, usb0: 2: usb0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc pfifo_fast state DOWN group default qlen 1000
      link/ether 02:00:00:00:00:02 brd ff:ff:ff:ff:ff:ff
  [9.184] console: entering the new root (dfd -1)
  [9.184] console: cwd on the new root, chroot
  [9.184] console: chroot ok
  [9.184] console: inside the system on UFS, commands follow
  ```
- **`first-user` & Graphical Session**:
  - Created user `picasso` (`uid=1000`) non-interactively via `/usr/lib/phonewave-system/first-user picasso "Picasso User" "picasso" en UTC` and started session with `--start`.
  - `denial.service`: Active (running) for `picasso(uid=1000)`.
  - `phonewave-droidloom.service`: Finished with `0/SUCCESS`, configured for `picasso` with native rendering on Qualcomm GPU `/dev/dri/renderD128` (Mesa).
  - Passwordless SSH for `picasso`: `ssh -i ~/.ssh/picasso_key picasso@10.77.71.2 "id"` → `uid=1000(picasso) gid=1000(picasso) groups=1000(picasso),969(gatekeeper),970(dmail),983(video),987(render),992(input),995(audio),998(wheel)`.
- **`pacman -Syu` Full System Upgrade**:
  - Keyring `/etc/pacman.d/gnupg/trustdb.gpg` present; routed via OpenSSH reverse SOCKS5 proxy (`ssh -R 1080`, `all_proxy=socks5h://127.0.0.1:1080`, `DisableSandbox`).
  - Synced `core`, `extra`, `alarm`, `aur`, `denial`, `denial-phone`, `droidloom`.
  - Upgraded `droidloom-image` (`0.1.0-36` → `0.1.0-42`) and `droidloom-runtime` (`0.1.0-36` → `0.1.0-42`), 935.03 MiB downloaded; pre/post hooks exited 0.
- **ADSP Watchdog Stability (>11 min uptime)**:
  - `uptime`: `15:39:07 up 11 min`; `remoteproc0` (`adsp`) and `remoteproc1` (`modem`) both `running`.
  - `hexagonrpcd` active, serving FastRPC calls for sensors registry; zero `USER-PD DOG detects stalled initialization` messages in `dmesg`.
- **Repository Hygiene & Final Artifact Checksums**:
  - `./scripts/policy-scan.sh`: Passed (22 code files, 42 path files).
  - `./build.sh check`: Passed cleanly (25 patches applied to fresh upstream clone, DTB builds without error).
  - Updated `build/out/*` SHA256 hashes:
    - `Image`: `687f6088b6c6070999312c0a03793296483eb356296d1d1a73f3bb6804061eb8`
    - `initrd.cpio.gz`: `bcd375c5eec837b82e409e67166d1502e1a14d139af9291bcb30e418e7caf91b`
    - `sm7250-xiaomi-picasso.dtb`: `6d90cbb0b60477f05d7b797351f9783254f2638cd6811e14706f0bc0d3024762`
    - `boot-linux.img`: `5ad32bdfe5196d97f80316ddb2034fe425ab7d77cdbb852561a6c5e5b3c05418`
    - `payload-linux.bin`: `204cb01c76536ebeceed8f0c2bddfab603517caba1abefe3f4c6bebda977e189`

## Repository Cleanup: Legacy Fedora/Luma Artifacts Archived (2026-11-17)

Obsolete Fedora 44 and Luma bring-up files were archived into `legacy/` while keeping the canonical `AGENTS.md` layout (`patches/linux/{series,vendor,picasso}`, `patches/xnuloader/`, `configs/linux/`, `tools/`, `build/`, `docs/`, `upstream/` read-only) intact:

- `legacy/tools/` (moved from `tools/`): `fw-mount.sh`, `fw-setup.sh`, `fw-start.sh`, `log-to-ufs.sh`, `pack-rootfs.sh`, `rmtfs-seed.sh`, `rproc-coredump.sh`, `usbacm.c`, `build-luma-bootimg.py` (`build.sh` `build_bootimg_luma()` updated to invoke `$ROOT/legacy/tools/build-luma-bootimg.py`).
- `legacy/userland/fedora/` (moved from `userland/fedora/`): 22 Fedora 44 systemd units, NM configs, PAM files, and `system-sleep-ufs.sh` (removed empty `userland/` and `uefi/`).
- `legacy/build-out/` (moved from `build/out/`, gitignored): `fedora-rootfs.cpio.gz`, `boot-luma*.img`, `payload-luma*.bin`, `boot-linux-oneshot.img`, `payload-linux-oneshot.bin`, `payload-probe.bin`.
- Verified: `./scripts/policy-scan.sh` (`OK (22 code files, 42 path files)`), `./scripts/preflight.sh` (`preflight: OK`), and `./build.sh check` (`[OK] 25 patches applied`, DTB builds cleanly).

## Denial Mobile: Subsystem Audit & Quick-Win Fixes — Bluetooth, PipeWire Audio, Sensors, Backlight, `tqftpserv` (verified 2026-11-17)

### 1. Live Subsystem Audit Matrix (on `10.77.71.2`)

| Subsystem | Initial Audit Status | Root Cause / Live Evidence |
|---|---|---|
| **Sensors (`steinerd` + `hexagonrpcd`)** | **WORKING** | `steinerd.service` active: detects `ams AG tsl2560` (ALS), `BOSCH bmi26x` (accel + gyro), `XiaoMi gyro_cal`, `qualcomm Rotation Vector` via `hexagonrpcd` / ADSP. |
| **Backlight (`wled`)** | **PRESENT** | `/sys/class/backlight/backlight` (`pmic@5:leds@d800`): `brightness=2048`, `max_brightness=4095`, `actual_brightness=2048`. (`brightnessctl` not installed in image). |
| **Audio (ALSA + PipeWire)** | **NO SINKS** | Kernel sees `card 0: XiaomiRedmiK305`, but WirePlumber exposed 0 sinks because `/usr/share/alsa/ucm2/conf.d/sm8250/` lacked a profile for `Xiaomi-RedmiK305G-picasso-G7AE` / `Xiaomi-Redmi-K30-5G` and `SEC_MI2S_RX Audio Mixer MultiMedia1` defaulted to `off`. |
| **Bluetooth (`wcn3991-bt` / `ttyHS1`)** | **FW PATH BUG** | `ttyHS1` requested `qca/crbtfw32.tlv`, failing `-2` because `/dev/sde33` (`bluetooth`) mounted at `/lib/firmware/qca` with an `image/` subdir (`/lib/firmware/qca/image/crbtfw32.tlv`). |
| **Modem (`remoteproc1`) & QRTR** | **PARTIAL** | `modem` `running` and `qrtr-lookup` shows Voice/NAS/WDS/DMS/UIM/PDS, but `tqftpserv.service` & `rmtfs.service` were `inactive (dead)`. |
| **Wi-Fi (`ath10k_snoc` / `18800000.wifi`)** | **NO `wlan0`** | `wlanmdsp.mbn`/`bdwlan.bin` present in `/lib/firmware/qcom/sm7250/xiaomi/picasso/image/`, `tqftpserv` was down and `bdwlan.b19` missing. |
| **Battery / Charger** | **USB ONLY** | `pm7250b-usb` (`online=1`, `status=Full`); `upower -d` shows `line_power_pm7250b_usb` online, no battery fuel-gauge (`DisplayDevice` 0% / `battery-missing-symbolic`). |

### 2. Quick-Win Fixes Implemented (`initramfs/init.c`) & Verified on Hardware

1. **Bluetooth (`wcn3991-bt` / `hci0`) — WORKING**:
   - `mount_firmware()` in `initramfs/init.c` mounts `/dev/sde33` (`bluetooth`) at `/mnt/bt_fw` and bind-mounts `/mnt/bt_fw/image` onto `/lib/firmware/qca` so `qca/crbtfw32.tlv` and `crnv32*.bin` live directly at `/lib/firmware/qca/`.
   - Verified in `dmesg`: `QCA Downloading qca/crbtfw32.tlv` → `QCA Downloading qca/crnv32.bin` → `QCA FW build version: BTFM.CHE.3.2.1-00174-QCACHROMZ-1.372365.1` → `QCA setup on UART is completed`.
   - Verified with `btmgmt info`: `hci0: Primary controller`, `current settings: powered ssp br/edr le secure-conn wide-band-speech ll-privacy past-sender past-receiver`, `name picasso`.

2. **Audio (ALSA UCM2 + PipeWire Speaker Sink) — WORKING**:
   - `write_device_dropins()` in `initramfs/init.c` provisions `/run` drop-ins without modifying `/usr/share/alsa/ucm2`:
     - `/run/alsa-ucm2/Qualcomm/sm8250/Picasso-HiFi.conf` (`EnableSequence` sets `'SEC_MI2S_RX Audio Mixer MultiMedia1' 1`; `Speaker` device on `hw:${CardId},0`, mixer elem `Speaker`).
     - `/run/alsa-ucm2/conf.d/sm8250/Xiaomi-RedmiK305G-picasso-G7AE.conf` (+ `Xiaomi-Redmi-K30-5G.conf` symlink).
     - `/run/systemd/system/picasso-ucm2.service` (enabled in `multi-user.target.wants`, overlays `/usr/share/alsa/ucm2/.` into `/run/alsa-ucm2/`).
     - `/run/systemd/user/wireplumber.service.d/ucm2.conf` (`Environment=ALSA_CONFIG_UCM2=/run/alsa-ucm2`).
   - Verified: `picasso-ucm2.service` `active (exited)` (`0/SUCCESS`), `amixer -c 0 cget name="SEC_MI2S_RX Audio Mixer MultiMedia1"` returns `values=on`, `wpctl status` exposes `* 43. Built-in Audio Speaker [vol: 0.40]` as default sink on boot, and `pw-play --raw` streams audio (`rc=0`).

3. **`tqftpserv.service`, Backlight & Wi-Fi Status**:
   - Enabled `tqftpserv.service` in `/run/systemd/system/multi-user.target.wants/` (`active (running)` on boot).
   - Verified `/sys/class/backlight/backlight` (`pmic@5:leds@d800`) responds cleanly to brightness writes (`1024` / `2048` out of `4095`).
   - Wi-Fi (`ath10k_snoc` `18800000.wifi`) still does not bring up `wlan0` (next deeper investigation item).

### 3. Hygiene & Updated Artifact Hashes
- `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`
- `./scripts/preflight.sh`: `preflight: OK`
- `build/out/initrd.cpio.gz`: `421b4241d16030c9f300efccc86ef3d6f529c2395f46adbe3046b4962e6e7e37`
- `build/out/boot-linux.img`: `b0af10ff6e36bb0ad79a0e6716c5cc20d8d31a73ec066d166aa967a762cd2190`
- `build/out/payload-linux.bin`: `4cd4fc401b3374587c0607436a4a01f47d66011ae41dc6c10c60a78dcb77699b`

### 4. Wi-Fi / TFTP (`tqftpserv` & `ath10k_snoc`) Investigation (verified 2026-11-17)

- **Changes in `initramfs/init.c`**:
  - `mount_firmware()`: Added `/readonly/firmware/image`, `/readonly/vendor/firmware_mnt/image`, and `/var/lib/tqftpserv/readonly/firmware/image` → `/lib/firmware/qcom/sm7250/xiaomi/picasso/image` symlinks, symlinked all files from `picasso/image/*` into `/lib/firmware/<name>`, and created `/lib/firmware/ath10k/WCN3990/hw1.0/board.bin` → `bdwlan.bin`.
  - `bind_firmware()`: Added `/lib/firmware/ath10k` to the bind-mount list (`root: bound /lib/firmware/ath10k`).
  - `write_device_dropins()`: Removed duplicate `tqftpserv.service` enablement so only ramdisk `/bin/tqftpserv` owns QRTR TFTP port `16385`.
- **Live Boot Results (`10.77.71.2`)**:
  - `remoteproc1` (`modem`): `running` (`[5.347955] remote processor modem is now up`), no watchdog crash.
  - `qrtr-lookup`: Single TFTP instance (`4096 1 0 1 16385 TFTP`), no WLFW service `69`; `ip -br link` shows `lo` and `usb0` only (no `wlan0`).
  - `tqftpserv` still logs `unable to open /readonly/firmware/image/wlanmdsp.mbn (2), reject` at `t=46.50s` despite both `/lib/firmware/wlanmdsp.mbn` and `/readonly/firmware/image/wlanmdsp.mbn` existing in its mount namespace. Disassembly of `initramfs/bin/tqftpserv` shows `translate_readonly` reads `/sys/class/remoteproc/remoteproc*/firmware` and concatenates `/lib/firmware/<rproc_dir>/<file>` (and `/lib/firmware/updates/<rproc_dir>/<file>`); requires runtime `strace` on `tqftpserv` to see the exact failing `open()` path.
  - Bluetooth (`hci0`), Audio (`Built-in Audio Speaker`), Sensors (`steinerd`), and `hexagonrpcd` all remain GREEN.
- **Updated Artifact Hashes & Hygiene**:
  - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`; `./scripts/preflight.sh`: `preflight: OK`.
  - `build/out/Image`: `687f6088b6c6070999312c0a03793296483eb356296d1d1a73f3bb6804061eb8`
  - `build/out/initrd.cpio.gz`: `040f2ea07d866b4c64c75babc4fb4fa7154fed966562c96f50c6d7e373da4547`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `6d90cbb0b60477f05d7b797351f9783254f2638cd6811e14706f0bc0d3024762`
  - `build/out/boot-linux.img`: `b9bc1387c24e8277bc1ee91c36cc0ec6a85ce26a3812ba684b0125d0a948f4d1`
  - `build/out/payload-linux.bin`: `11b54987a2820448e63a1444096bcc4d562367e088388cf0a841574a5986e3f0`

### 5. `MS_BIND | MS_REC` Fix Eliminates `tqftpserv` `ENOENT` Rejections (verified 2026-11-17)

- **Root cause & fix (`initramfs/init.c` `switch_to_partition()`)**:
  - Confirmed `/proc/$(pgrep -x tqftpserv)/root/sys/` was empty after `switch_to_partition()` used `MS_MOVE` on `/dev`, `/proc`, `/sys`, causing `tqftpserv`'s `translate_readonly()` (`opendir("/sys/class/remoteproc")`) to fail with `ENOENT`.
  - Changed `/dev`, `/proc`, `/sys` mount in `switch_to_partition()` from `MS_MOVE` to `MS_BIND | MS_REC`, keeping `/sys` mounted in the ramdisk namespace for `tqftpserv`, `pd-mapper`, and `rmtfs`.
- **Live Boot Results (`uptime > 55s`)**:
  - **ZERO** `tqftpserv: unable to open /readonly/...` rejections across `>86s` (previously 12+ rejections at `t=46.5s`).
  - `pd-mapper` actively serves domain list requests from node 1 and node 5 (`avs/audio`, `tms/pdr_enabled`, `tms/pddump_disabled`, `tms/servreg`).
  - `remoteproc1` (`modem`): `running` (`[45.719562] remote processor modem is now up`).
  - `18800000.wifi` (`ath10k_snoc`) bound with `board.bin`, but `qrtr-lookup` still shows no `WLFW` service (`69`) announced by `modem`, so `wlan0` is not yet created.
- **Hygiene & Final Artifact Hashes**:
  - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`; `./scripts/preflight.sh`: `preflight: OK`.
  - `build/out/Image`: `687f6088b6c6070999312c0a03793296483eb356296d1d1a73f3bb6804061eb8`
  - `build/out/initrd.cpio.gz`: `5d62c32e33f8fc4d17d24e48dfedb04a1db91004e1a414586bd046065253205c`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `6d90cbb0b60477f05d7b797351f9783254f2638cd6811e14706f0bc0d3024762`
  - `build/out/boot-linux.img`: `f1c37940b900a9f7845eb784a7e38f29ba33566c8cf6a80a5e847b915754f347`
  - `build/out/payload-linux.bin`: `10ecaa3a1bcc785e0c368dd0e68c9bc32b0069a600be1bd79ef58a06cf3b9a74`

### 6. Wi-Fi (`wlan0` / `ath10k_snoc` / WCN3990) — WORKING on Boot (verified 2026-11-17)

- **Root Causes Fixed (`initramfs/init.c` & `scripts/mkinitramfs.sh`)**:
  1. **Firmware `tmpfs` + `dsp` + `tms/pddump_disabled` PD maps (`initramfs/init.c`)**: Mounted `modem` (`/mnt/modem_fw`) and `dsp` (`/mnt/dsp_fw`), mounted `tmpfs` at `/lib/firmware/qcom/sm7250/xiaomi/picasso/image`, symlinked all files from `image/`, and wrote `tms-pddump-{modem-root_pd,adsp-audio_pd,cdsp-root_pd}.jsn`. Linked `/lib/firmware/ath10k/WCN3990/hw1.0/board.bin` → `bdf_g7a.bin` and bound `/mnt/modem_fw`, `/mnt/dsp_fw`, `/lib/firmware/qcom/sm7250/xiaomi/picasso/image`, and `/lib/firmware/ath10k` into `/newroot`.
  2. **Service Start Order (`initramfs/init.c`)**: Moved `rebind("qcom_geni_serial", ...)`, `rebind("qcom_q6v5_pas", "4080000.remoteproc")`, and `rebind("ath10k_snoc", "18800000.wifi")` to run **after** `rmtfs`, `pd-mapper`, and `tqftpserv` start, so `modem` sees `tms/pddump_disabled` and `wlanmdsp.mbn` on boot and announces QMI service `69` (`WLFW`).
  3. **`ath10k/WCN3990/hw1.0/firmware-5.bin` (`scripts/mkinitramfs.sh`)**: Packed the 60-byte `WCN3990/hw1.0/firmware-5.bin` metadata container into the initramfs so `ath10k_snoc` loads it automatically at `t=12s`.
- **Verified Live Boot Output (`10.77.71.2`)**:
  - `dmesg`:
    - `[    6.076948] ath10k_snoc 18800000.wifi: qmi chip_id 0x320 chip_family 0x4001 board_id 0xff soc_id 0x400b0000`
    - `[   12.144152] ath10k_snoc 18800000.wifi: wcn3990 hw1.0 target 0x00000008`
    - `[   12.144189] ath10k_snoc 18800000.wifi: firmware ver  api 5 features wowlan,mgmt-tx-by-reference,non-bmi crc32 b3d4b790`
  - `ip -br link` & `iw dev`: `wlan0` (`phy#1`, `type managed`, `96:ff:22:36:35:99`, `<NO-CARRIER,BROADCAST,MULTICAST,UP>`).
  - `nmcli device wifi list --rescan yes`: `wlan0` actively scans 2.4 GHz & 5 GHz WPA2 networks (`<ssid>` 5 GHz ch 153 at 97% signal, `<ssid>` 5 GHz ch 153 at 92% signal).
  - All other subsystems (`hci0`, `Built-in Audio Speaker`, `steinerd`, `hexagonrpcd`, `denial.service`) remain GREEN.
- **Hygiene & Final Artifact Hashes**:
  - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`; `./scripts/preflight.sh`: `preflight: OK`.
  - `build/out/Image`: `687f6088b6c6070999312c0a03793296483eb356296d1d1a73f3bb6804061eb8`
  - `build/out/initrd.cpio.gz`: `c240d2d0f617662dba0da9c675aef3c5b6ce6c4bbc8e52b53d56ef526353b358`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `6d90cbb0b60477f05d7b797351f9783254f2638cd6811e14706f0bc0d3024762`
  - `build/out/boot-linux.img`: `d3f982615447f0fc8c37b62189bf03dda157ae9239265b40665fc685eb1b08c8`
  - `build/out/payload-linux.bin`: `be561b86d62818fe932486d03d0aed02998b1be78f57fa408deb56f8a59405cc`

### 7. Display DPMS Lock Screen, Power Key, and Novatek Touch Firmware (verified 2026-11-17)

- **Root Cause Found**:
  - `denial.service` (`/usr/bin/deniald --device /dev/dri/card1`) was running and allocating `1080x2400` XR30 framebuffers on `card1-DSI-1`, **but**:
    1. `deniald::dpms` locks and powers off `DSI-1` via DRM DPMS (`enabled=disabled`, `bl_power=4`) after ~30 seconds of inactivity.
    2. `novatek/novatek_nt36672c_g7a_fw03.bin` was missing from the initramfs (`SRAM firmware upload failed: -2`), leaving the touchscreen dead so tapping could not wake the screen.
    3. `initramfs/init.c`'s `logind.conf.d` drop-in had `HandlePowerKey=suspend`, causing short power-key presses to suspend via `logind` instead of letting `deniald` wake/lock the lock screen.
- **Fixes Implemented & Verified on Fresh Boot**:
  - Staged `novatek_nt36672c_g7a_fw03.bin` in `prebuilt/fw-touch/`, packed it into `/lib/firmware/novatek/` in `scripts/mkinitramfs.sh`, and added `/lib/firmware/novatek` to `bind_firmware()` in `initramfs/init.c`.
  - Changed `HandlePowerKey=suspend` → `HandlePowerKey=ignore` in `initramfs/init.c` (`write_device_dropins()`, keeping `HandlePowerKeyLongPress=poweroff`).
  - Live boot evidence: `picasso-nvt-vendor spi0.0`: `novatek/novatek_nt36672c_g7a_fw03.bin: 131072 of 139264 bytes, 15 partitions, hw crc 2`, `fw reset state 0xa0 after 1 polls`, `fw info: ver 0x0a x_num 16 y_num 36 abs 1080x2400 type 0x01` (`NVTCapacitiveTouchScreen` active on `input2`/`event2`). Display: `enabled=enabled bl_power=0 brightness=2048`, `framebuffer[82]: allocated by = deniald, size=1080x2400`.
- **Hygiene & Final Artifact Hashes**:
  - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`; `./scripts/preflight.sh`: `preflight: OK`.
  - `build/out/Image`: `687f6088b6c6070999312c0a03793296483eb356296d1d1a73f3bb6804061eb8`
  - `build/out/initrd.cpio.gz`: `2dd2aa02393f8d625ac900c26ee871980f4e309e76a359c589190c5ff035a6fb`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `6d90cbb0b60477f05d7b797351f9783254f2638cd6811e14706f0bc0d3024762`
  - `build/out/boot-linux.img`: `35cd5f184455161ceba4e1e8fda77f9f9daf4185a88dc279bf4f737df253bae7`
  - `build/out/payload-linux.bin`: `ba6f613707ae52f12e1867b3d88330f0381ecfa4f15baf6db56c32c6c7bc85d1`

### 8. Physical Backlight (`d846: 80`) Fix & Adreno 620 `0x00800007` GPU Hang Investigation (verified 2026-11-17)

- **Physical Backlight (`pm8150l_wled`) — FIXED (`d846: 80`)**:
  - `qcom-wled` (`wled_ovp_work`) had been turning off the WLED boost converter (`0xd846` = `00`). Removed `backlight = <&pm8150l_wled>;` from `panel@0` and set `&pm8150l_wled { status = "disabled"; };` in `sm7250-xiaomi-picasso.dts` (`0002*`).
  - Verified on hardware (`10.77.71.2`): `/sys/kernel/debug/regmap/0-05/registers` shows `d846: 80` (`WLED3_CTRL_REG_MOD_EN` = ON), `d847: 86`, `d946: 70`; `/sys/class/drm/card1/card1-DSI-1/{status,enabled,dpms}` = `connected`, `enabled`, `On`; `framebuffer[82]` allocated by `deniald` (`1080x2400`). Panel backlight is physically ON (`fbcon` / `modetest` lit).
- **Adreno 620 `0x00800007` GPU Hang (`a6xx_gmu.c`)**:
  - Clamped `index` to the highest non-zero `mx[j]` in `a6xx_gmu_rpmh_dep_votes_init()` (`0010*`).
  - `deniald` GPU submissions (`io.flutter.rast` / `deniald:gdrv0`) still hit ring 0 fault `0x00800007` / `0x00C00017` (`ib1 0000000000000000/0000`, `hangcheck recover!`).
- **Exported Patches & Final Artifact Hashes**:
  - `./build.sh export-patches` regenerated `0002*` & `0010*`.
  - `build/out/Image`: `da2b4c5b5137f8edaa62adc4f5177a8253ddeffea732e4742409439800c9f41d`
  - `build/out/initrd.cpio.gz`: `2dd2aa02393f8d625ac900c26ee871980f4e309e76a359c589190c5ff035a6fb`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `2bb6c2ca89e6df699a30ff0e6c89de6c4bb62a07cf144d94dc301417fa550485`
  - `build/out/boot-linux.img`: `eaa69d7182c806f7312ae298c8b0af0347cb0d4fe1bded0096f293699233f41b`
  - `build/out/payload-linux.bin`: `5c2aa0628a0cdf62226b825f64387501b41e5736f17f8775506a9d3a3637b9ab`

### 9. Adreno 620 GPU & `deniald` Rendering — WORKING with Zero Faults (verified 2026-11-17)

- **Root Cause Found & Fixed**:
  - `prebuilt/fw-gpu/a650_sqe.fw` (32304 B, SHA256 `a0e1b583f620fabe32729ce367959d1960638663244d7d0cfc21b9a5215a018b`) was byte-for-byte identical to Gen1 `a630_sqe.fw`, causing every Gen3 Adreno 620 (`0x06020000`) userspace ring-0 submission to fault (`0x00800007` / `0x00C00017`, `hangcheck recover!`).
  - Replaced `prebuilt/fw-gpu/a650_sqe.fw` with the real Gen3 SQE firmware (31804 B, SHA256 `7db957e3d06bfc14998b3cd6eb6e076206446f897bd1224f19b14c365e7d5013`) and reverted all temporary GPU/IOMMU kernel hacks (`a6xx_create_private_vm`, `arm-smmu-qcom.c` TTBR1, `a6xx_catalog.c` cached-coherent).
- **Verified Live Boot Output (`10.77.71.2`)**:
  - `dmesg`: `loaded qcom/a650_sqe.fw from new location`, `loaded qcom/a650_gmu.bin from new location`, `loaded qcom/a620_zap.mbn from new location`; **ZERO** `gpu fault`, `hangcheck`, `recover`, or `a6xx_irq` errors.
  - `denial.service`: `active (running)`, PID 596 `/usr/bin/deniald --device /dev/dri/card1 --output-config /home/picasso/.local/state/denial/outputs.conf --wayland --flutter-bundle /usr/lib/denial/flutter`.
  - `/sys/kernel/debug/dri/1/framebuffer`: `framebuffer[82]` allocated by `deniald`, `XR30` (`1080x2400`, `pitch[0]=4352`).
  - Display & WLED: `card1-DSI-1` `connected`, `enabled`, `dpms: On`; `0-05` registers `d846: 80`, `d847: 86`, `d946: 70`.
- **Host Checks & Final Artifact Hashes**:
  - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
  - `./build.sh check`: `[OK] 25 patches applied`, `dtbs_check: 12 schema diagnostics from the vendor series` (note: `./scripts/preflight.sh` flagged trailing whitespace in `patches/linux/picasso/0010-drm-msm-a6xx-fix-rpmh-dependency-votes.patch`).
  - `prebuilt/fw-gpu/a650_sqe.fw`: `7db957e3d06bfc14998b3cd6eb6e076206446f897bd1224f19b14c365e7d5013`
  - `build/out/Image`: `4870505e3e022436563677212f9d0091c66d02c9ad72794e3a24c1df7aca480e`
  - `build/out/initrd.cpio.gz`: `7719fb1d564d570fca450fcb89580e0de1fc629491635ebb9dcd8a8ea82e55e1`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `2bb6c2ca89e6df699a30ff0e6c89de6c4bb62a07cf144d94dc301417fa550485`
  - `build/out/boot-linux.img`: `debb683bd5f5328a8ba3e718568df8afa0cc98ed38a57388b320c3bbeed7441a`
  - `build/out/payload-linux.bin`: `dd4ab1b6d37ade9d1f2ecb6240534934ad3ee245c6dcb5e133b01ce18b6b355b`

### 10. 120Hz Display Mode Name (`0006*`) & QRTR `announce_servers()` `-EAGAIN` Race Fix (`0011*`) — WORKING (verified 2026-11-17)

1. **120Hz Refresh Rate (`patches/linux/picasso/0006-drm-panel-xiaomi-g7a.patch`)**:
   - `panel-xiaomi-g7a-37-02-0a.c` (`G7A_VREFRESH 120`) was already driving 120Hz (`334660` kHz), but `g7a_get_modes()` did not call `drm_mode_set_name(mode)`, leaving `/sys/class/drm/card1-DSI-1/modes` blank (`""`).
   - Added `drm_mode_set_name(mode)` in `g7a_get_modes()` and exported to `0006*`.
   - Verified on phone (`10.77.71.2`): `/sys/class/drm/card1/card1-DSI-1/modes` → `1080x2400`; `crtc[73]` → `enable=1`, `active=1`, `mode: "1080x2400": 120 334660 1080 1110 1114 1130 2400 2432 2436 2468 0x48 0x0`.
2. **Wi-Fi / Modem QRTR Race Fix (`patches/linux/picasso/0011-net-qrtr-ns-retry-announce-on-eagain.patch`)**:
   - *Root Cause*: When `modem` (`0:-2`) or `adsp` (`5:-2`) sent `QRTR_TYPE_HELLO` before `qrtr_hello_work` completed (`node->hello_sent == false`), `qrtr_node_enqueue()` returned `-EAGAIN`. `net/qrtr/ns.c:announce_servers()` only ignored `-ENODEV`, printed `failed to announce new service`, and aborted announcing `rmtfs`/`pd-mapper`/`tqftpserv` to `modem`, causing `modem` to stall 40s and crash (`dog_hal_common.c:180`).
   - *Fix*: Added `0011-net-qrtr-ns-retry-announce-on-eagain.patch` to `patches/linux/series` to retry `service_announce_new()` on `-EAGAIN` (`msleep(10)`, up to 100 retries).
   - *Verified on phone*: `remoteproc1` (`modem`) `running`, ZERO `failed to announce new service` or `dog_hal` errors, `ath10k_snoc 18800000.wifi: wcn3990 hw1.0 target 0x00000008`, `wlan0` UP, `nmcli device wifi list --rescan yes` scans 2.4 GHz & 5 GHz WPA2 networks (`<ssid>` 95%, `<ssid>` 94%), and ZERO GPU faults.
3. **Checks & Final Artifact Hashes**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 26 patches applied`, `dtbs_check: 12 schema diagnostics from the vendor series`.
   - `build/out/Image`: `4219095124c51535f5299fa1216a3aa773e05b0dfbbe2ff3310a0f91b4053c24`
   - `build/out/initrd.cpio.gz`: `7719fb1d564d570fca450fcb89580e0de1fc629491635ebb9dcd8a8ea82e55e1`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `2bb6c2ca89e6df699a30ff0e6c89de6c4bb62a07cf144d94dc301417fa550485`
   - `build/out/boot-linux.img`: `3c2d02d964a6b9f7ded8d8572168cff6cd5d9eff26afcb03bdeb143eef71f64c`
   - `build/out/payload-linux.bin`: `27edb5c263c92d6bf99bcd6a7de5c9b0fb9fe0777752507be5b6362cbb7e42b7`

### 11. DPMS Backlight (`pm8150l_wled`) — FIXED (`0002*`, `0012*`), Plus Audio/120Hz/Venus Diagnostics (verified 2026-11-17)

1. **Display off / Backlight on — FIXED (`0002*` + `0012*`)**:
   - *Root Cause*: `pm8150l_wled` had been disabled in DT (`0002*`); enabling it previously failed because `qcom,current-limit-microamp = <22500>;` was missing and `qcom-wled.c:wled_configure_ovp_irq()` only called `disable_irq(ovp_irq)` when `MOD_EN == 0` at probe, causing `Unbalanced enable for IRQ 138` in `wled_ovp_work` when the bootloader left `MOD_EN=0x80`.
   - *Fix*: Added `backlight = <&pm8150l_wled>;` to `panel@0` and enabled `&pm8150l_wled` with downstream values (`qcom,enabled-strings = <0 1 2>;`, `qcom,ovp-millivolt = <28000>;`, `qcom,current-limit-microamp = <22500>;`, `qcom,cabc-sel = <0>;`) in `0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch`. Added `0012-backlight-qcom-wled-balance-ovp-irq.patch` (`ovp_irq_disabled` state tracking in `qcom-wled.c`).
   - *Verified on phone (`10.77.71.2`)*: `/sys/class/backlight/backlight` (`brightness=2048`, `max_brightness=4095`, `bl_power=0`); `journalctl -k -b | grep -iE "wled|ovp|unbalanced"` → `NONE`; `bl_power=0` → `0-05` `d846: 80, d847: 86, d946: 70`; `bl_power=4` → `d846: 00` (OFF); `bl_power=0` → `d846: 80` (ON).
2. **Audio Diagnosis (`10.77.71.2`)**:
   - `pcm0p` `RUNNING` (`S24_LE` 2ch 48kHz, `hw_ptr` advancing), `tfa9874 0-0034: status 0x0016 clk 0xe2c0 sys 0x0018 gain 0x027c tdm 0x0890/0xc0f1` (`AMPE=1`, `DCA=1`, `CLKS=1`).
   - Suspect for inaudible output: `wpctl status` defaults `* 43. Built-in Audio Speaker [vol: 0.23]` (PipeWire cubic volume `0.23^3 = 1.2%` linear / `-38 dB`) + `amixer -c 0 sget Speaker` = `6 [86%]` (`TDMSPKG = 9`).
3. **120Hz Stutter Evidence (`10.77.71.2`)**:
   - GPU devfreq (`3d00000.gpu`): `cur=625000000 min=550000000 max=625000000 gov=simple_ondemand` (not stuck at low OPP); CPU `schedutil` active.
   - `dmesg` shows `sync_state() pending due to 3d6a000.gmu` on `gcc-sm7250` & `sm7250-gpucc`, and per-frame `msm_atomic_commit_tail: crtc_mask 0x1 async 0` / `dpu_crtc_atomic_flush: crtc73 pending 0x1`. Chromium flags files (`/etc/chromium-flags.conf`, `~/.config/chromium-flags.conf`) do not exist.
4. **Video Hardware Decode (`venus`) Status**:
   - No `/dev/video*` or `/dev/media*` devices; `.config` has `CONFIG_VIDEO_QCOM_VENUS=m` and `sm7250.dtsi:828` has `venus: video-codec@aa00000` `status = "disabled";` (plus needs `venus.mbn` firmware).
5. **Checks & Final Artifact Hashes**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 27 patches applied`, `dtbs_check: 12 schema diagnostics from the vendor series`.
   - `build/out/Image`: `16b41386e667c4d077d48e501ba3917ef10edb1106a5fdfb2b9565c8e3dcbcbf`
   - `build/out/initrd.cpio.gz`: `7719fb1d564d570fca450fcb89580e0de1fc629491635ebb9dcd8a8ea82e55e1`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `e1477a3622ad4e680329b4ed7ef7a2824c58e230b69e549f1764b54108348706`
   - `build/out/boot-linux.img`: `82b93cf57afe2b317f346b317fab7351da68cf07c47df335efed33ec2ff482ca`
   - `build/out/payload-linux.bin`: `3f5115b3b830c8fd6169555fa83160bd667493bd054fc8bc832ac9910fcf6d97`

### 12. 120Hz Smoothness (`0013*` + Chromium Flags), PipeWire `S16_LE` Audio, and `qcom-venus` Status (verified 2026-11-17)

1. **120Hz Smoothness (`0013-drm-msm-drop-per-frame-pr-info.patch` + Chromium Wayland/GPU flags)**:
   - Converted per-frame `pr_info()` calls in `msm_atomic.c:223` and `dpu_crtc.c:914` to `DRM_DEBUG_ATOMIC` (`0013*`), eliminating ~851+ serial/tty0 console log lines per few seconds of scrolling.
   - Added `/home/picasso/.config/chromium-flags.conf` in `initramfs/init.c` (`--ozone-platform=wayland --enable-features=UseOzonePlatform --ignore-gpu-blocklist --enable-gpu-rasterization --enable-zero-copy`).
   - Verified on `10.77.71.2`: `crtc73_pending=0`, `msm_atomic=0`, `/sys/class/drm/card1/card1-DSI-1/modes` = `1080x2400` (120Hz), `GPU_FAULTS=NONE`, `wlan0` UP.
2. **Audio (`S16_LE` + `Speaker` 100%)**:
   - Updated `initramfs/init.c`: `Picasso-HiFi.conf` sets `Speaker Playback Volume` = `7` (`100%`), `wireplumber.service.d/ucm2.conf` adds `XDG_CONFIG_DIRS=/run:/etc/xdg`, and `/run/wireplumber/wireplumber.conf.d/50-picasso-alsa.conf` forces `audio.format = "S16LE"`, `audio.rate = 48000`, `audio.channels = 2` (matching `sm8250_be_hw_params_fixup`).
   - Verified on `10.77.71.2`: `amixer -c 0 sget Speaker` = `7 [100%]`, `pw-play` opens `card 0` in `format: S16_LE` (`channels: 2, rate: 48000`).
3. **Hardware Video Decode (`qcom-venus`) Status**:
   - Enabled `CONFIG_SM_VIDEOCC_7250=y`, `CONFIG_MEDIA_SUPPORT=y`, `CONFIG_VIDEO_DEV=y`, `CONFIG_VIDEO_QCOM_VENUS=y` in `configs/linux/picasso.config` and `&venus` (`firmware-name = "qcom/sm7250/xiaomi/picasso/image/venus.mdt"`, `status = "okay"`) in `0002*`.
   - Early boot probe at `t=0.86s` fails `-2` because `/lib/firmware/.../venus.mdt` is mounted later by `initramfs/init.c`. Manual bind after boot (`echo aa00000.video-codec > /sys/bus/platform/drivers/qcom-venus/bind`) loads `venus.mdt` but fails with `VenusLow : error reading F/W version` / `wait for cpu and video core idle fail (-110)` (no `/dev/video*` yet; left un-rebound in `init.c` so boot isn't delayed).
4. **Checks & Final Artifact Hashes**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 28 patches applied`, `dtbs_check: 14 schema diagnostics from the vendor series`.
   - `build/out/Image`: `77ae73118a595c2dcf9665c58558fe80d7aceb716436a880a76729c3cc961841`
   - `build/out/initrd.cpio.gz`: `01e777abc5168cb0e05c7addc4ba8c76dce2c2ba41b645c0728084ade3656fa1`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `13fa6c1fc1d34cc9c2c2cdc02e4a43657569e8ae1dc60d951537d731b04f0119`
   - `build/out/boot-linux.img`: `27d14a79b2b28feb5d601701a94f67f4d381d9ec3afcc75b8e4ec0e7da83d0c9`
   - `build/out/payload-linux.bin`: `2cf278642aca00701fbe893e44039c4c19f79d942312e69007c7f164f7057d87`

### 13. Hardware Video Decode (`qcom-venus`, `0014*`) — `/dev/video0` & `/dev/video1` Live on Boot (verified 2026-11-17)

- **Root Cause & Fix**:
  - In `hfi_venus.c:venus_suspend_3xx()`, `venus_cpu_and_video_core_idle()` timed out (`-110`, `cpu=0x0 ctrl=0x1` / `0x0`), causing `pm_runtime_put_sync()` in `venus_probe()` to abort and tear down `/dev/video*`.
  - Exported `patches/linux/picasso/0014-media-venus-skip-iris1-power-collapse-on-idle-timeout.patch` (added to `patches/linux/series`): for `IS_IRIS1(core)` (`sm7250`), logs `cpu`/`ctrl` and returns `0` (skipping power-collapse and keeping Venus powered on, matching downstream `Skip PC`).
  - Added `rebind("qcom-venus", "aa00000.video-codec");` in `initramfs/init.c` right after `/mnt/modem_fw` is mounted.
- **Verified on `10.77.71.2`**:
  - `firmware: bound aa00000.video-codec`
  - `/dev/video0` (`qcom-venus-encoder`), `/dev/video1` (`qcom-venus-decoder`)
  - `v4l2-ctl --list-devices`: `Qualcomm Venus video decoder (plat:aa00000.video-codec:dec): /dev/video1`, `Qualcomm Venus video encoder (plat:aa00000.video-codec:enc): /dev/video0`
  - `v4l2-ctl -d /dev/video1 --list-formats-out`: `H264`, `VP80`, `VP90`, `HEVC`, `MPG2` (compressed, dyn-resolution).
- **Checks & Final Artifact Hashes**:
  - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
  - `./build.sh check`: `[OK] 29 patches applied`, `dtbs_check: 14 schema diagnostics from the vendor series`.
  - `build/out/Image`: `2245edfa9c8a98c6190256e8ff4c1fdcad32ef6bdc58866f0b856c5e8f4cd971`
  - `build/out/initrd.cpio.gz`: `788f70231f182cdb18531fa3187ea2ae51bc838bfa417e61fc3ee252f5c7fff2`
  - `build/out/sm7250-xiaomi-picasso.dtb`: `13fa6c1fc1d34cc9c2c2cdc02e4a43657569e8ae1dc60d951537d731b04f0119`
  - `build/out/boot-linux.img`: `30bd7e1313609b66f7b4cbc2546ffb831a278ceb70c41b5fdb36505ee64666d7`
  - `build/out/payload-linux.bin`: `d5557b60ba65776024db42c3bbe722d911d234e8722928ab5d36b3417bab85d7`
- **Live Decode & Permissions Follow-up**:
  - Permissions: `picasso` (`uid=1000`) is in `video` (`983`), and `/dev/video0` & `/dev/video1` (`crw-rw----+ root video`) have `rw` access.
  - Chromium flags updated in `initramfs/init.c` (`/home/picasso/.config/chromium-flags.conf`) to `--enable-features=UseOzonePlatform,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL`.
  - Switched `0014*` (`core.c`) to `pm_runtime_forbid(dev); pm_runtime_put_noidle(dev);` for `IS_IRIS1(core)` so `venus_suspend_3xx()` is never called; `/dev/video0` & `/dev/video1` register cleanly with zero suspend timeouts, **but** `ffmpeg -y -c:v h264_v4l2m2m -i /tmp/test.h264 -f null -` fails at `[h264_v4l2m2m] output VIDIOC_REQBUFS failed: Connection timed out` (`HFI_CMD_SYS_SESSION_INIT` timeout after `VenusLow : error reading F/W version`). Needs deeper `sm7250` HFI/firmware investigation for end-to-end hardware decode.

### 14. Droidloom (`droidloomd` + Android Cell) WORKING & `pm7250b-battery` (`0015*`) Live (verified 2026-11-17)

1. **Droidloom (`droidloomd.service` + user `droidloom.service`) — WORKING**:
   - Enabled built-in kernel configs in `configs/linux/picasso.config`: `CONFIG_ANDROID_BINDER_IPC=y`, `CONFIG_ANDROID_BINDERFS=y`, `CONFIG_NETFILTER_NETLINK=y`, `CONFIG_NF_TABLES*`, `CONFIG_NF_CONNTRACK=y`, `CONFIG_NF_NAT*`, `CONFIG_NFT_*`, `CONFIG_BRIDGE=y`, `CONFIG_VETH=y`, `CONFIG_OVERLAY_FS=y`, `CONFIG_DMABUF_HEAPS_{SYSTEM,CMA}=y`.
   - Verified on `10.77.71.2`: `droidloomd.service` `active (running)` with full Android cell booted (`ueventd`, `servicemanager`, `hwservicemanager`, `vold`, `apexd`, `odsign`, `keystore2`, `logd`, `keymint-service`, `renderD128`); user `droidloom.service` `active (running)` (`Connected to Wayland; ready to start Android`); `/dev/dma_heap/{system,reserved,default_cma_region}` present.
2. **Battery (`pm7250b-battery` / `0015*`) — LIVE**:
   - `/sys/class/power_supply/pm7250b-battery` registered (`POWER_SUPPLY_TYPE=Battery`, `upower -d` sees `pm7250b-battery`, `VOLTAGE_OCV=4402883`).
   - Identified `0x8000` QG "no data" sentinel on `0x4880`/`0x4882` (`S2_NORMAL_AVG`) causing `VOLTAGE_NOW=6377865` & `CURRENT_NOW=-5000003`, while `0x48c0` (`LAST_ADC_V` = `0x5714` → `4.338 V`) and `0x48c2` (`LAST_ADC_I` = `0x06a0` → `+258 mA`) hold valid readings; fallback to `0x48c0`/`0x48c2` on `0x8000` is the next small fix.
3. **Checks & Artifact SHA-256 (`task-1782`)**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 30 patches applied`, `dtbs_check: 7 schema diagnostics from the vendor series`.
   - `build/out/Image`: `1757059846888b497d7b67b4b9ad7f7f99d0330011f890f9dddf7453ed7d9f37`
   - `build/out/initrd.cpio.gz`: `32286b15583026db5d4cad725a45f551cea6b2faeb7d5a4b48f3b1c08bbef65a`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `13fa6c1fc1d34cc9c2c2cdc02e4a43657569e8ae1dc60d951537d731b04f0119`
   - `build/out/boot-linux.img`: `66957e24c754b77f511fa52bf3953f1abb182cdb98b2b14f407196604955897c`
   - `build/out/payload-linux.bin`: `67e2b5593a3ade997801f55ba007f50d5373624bf370594f64d20ebfd376b0b4`

### 15. `pm7250b-battery` (`0015*`) WORKING & Venus `session_init` `-110` Pinpointed (verified 2026-11-17)

1. **Battery (`pm7250b-battery` / `0015*` with `0x8000` sentinel fix) — WORKING**:
   - Treated `0x8000` (`QG_INVALID_ADC_DATA`) on `0x4880`/`0x4882` as invalid and fell back to `0x48c0` (`LAST_ADC_V`) / `0x48c2` (`LAST_ADC_I`).
   - Verified on `10.77.71.2`: `/sys/class/power_supply/pm7250b-battery/uevent` reports `POWER_SUPPLY_STATUS=Charging`, `POWER_SUPPLY_VOLTAGE_NOW=4363372` (`4.363 V`), `POWER_SUPPLY_VOLTAGE_OCV=4390037` (`4.390 V`), `POWER_SUPPLY_CURRENT_NOW=126495` (`+126 mA`), `POWER_SUPPLY_CAPACITY=97` (`97%`); `upower -d` reports `native-path: pm7250b-battery`, `state: charging`, `energy-rate: 0.808707 W`, `voltage: 4.35325 V`, `percentage: 96%`.
2. **Venus (`qcom-venus`) `VIDIOC_REQBUFS` Diagnostic**:
   - Pinpointed failure in `vdec_queue_setup()`: `pm_get=0`, `venus_session_init: type=2 codec=0x2 ret=0` (wrote `HFI_CMD_SYS_SESSION_INIT` to cmdq), `vdec_queue_setup: session_init=-110` (`wait_session_msg()` timed out after 1s; firmware never replied). Temporary debug prints reverted.
3. **Checks & Artifact SHA-256**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 30 patches applied`, `dtbs_check: 7 schema diagnostics from the vendor series`.
   - `build/out/Image`: `ee7725d2444a382fb6a481124fb2cd2c6438b53b165fc4f5f6886c8aa3f14ee4`
   - `build/out/initrd.cpio.gz`: `32286b15583026db5d4cad725a45f551cea6b2faeb7d5a4b48f3b1c08bbef65a`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `13fa6c1fc1d34cc9c2c2cdc02e4a43657569e8ae1dc60d951537d731b04f0119`
   - `build/out/boot-linux.img`: `c49661714f26b3ea7f1b43b6826be2f81b8902183f3e60879e72f190159280ea`
   - `build/out/payload-linux.bin`: `c3e688c1e742aa71e507807eb52f611b301980ef5e6ba74105ebcf98f5c79e93`

### 16. Watchdog Timer (`2700s`), Wi-Fi SSH, TikTok in Droidloom, and `qcom-venus` Hardware H.264 Decode — WORKING (verified 2026-11-17)

1. **Watchdog & Auto-Reboot Timer (45 min = `2700s`) (`initramfs/init.c`)**:
   - Default `auto_reboot` updated from `300s` (5m) to `2700s` (45m) (`auto_reboot=<sec>` cmdline override), plus `/run/picasso-watchdog-pet` support and `picasso-watchdog.timer` / `picasso-watchdog.service` in `/run/systemd/system/`.
2. **Wi-Fi SSH & TikTok in Droidloom (`#187`)**:
   - `sshd` bound to `0.0.0.0:22` (`/run/11-phone-usb.conf` bind-mounted over `/etc/ssh/sshd_config.d/11-phone-usb.conf`), reachable via both USB (`10.77.71.2`) and Wi-Fi (`<lan-ip>`).
   - Installed `/home/picasso/Downloads/tiktok.apk` (`com.ss.android.ugc.trill`) into the Droidloom Android container (`pm list packages | grep trill` → `package:com.ss.android.ugc.trill`, desktop launcher `/home/picasso/.local/share/applications/droidloom.com.ss.android.ugc.trill.desktop`).
3. **Venus (`qcom-venus`) Hardware Video Decode (`patches/linux/picasso/0014-media-venus-skip-iris1-power-collapse-on-idle-timeout.patch`) — WORKING**:
   - *Root Causes Fixed*:
     - `pm_runtime_forbid(dev)` in `venus_probe()` (`core.c`) + `venus_fw_low_power_mode = false` (`hfi_venus.c`) prevent Iris1 (`VIDEO.IR.1.2-00043-PROD-1.352118.1`) power-collapse after `SYS_INIT`.
     - `vcodec_control_v4()`, `poweron_coreid()`, and `poweroff_coreid()` (`pm_helpers.c`) keep `mvs0_gdsc` (`VIDC_CORE_ID_1`) in SW mode (`hwmode = false`) and powered across sessions on `IS_IRIS1(core)` so VSP (`0xe0000800`) stays powered and fires `0x29494` (`VSP_INTR`).
     - `vdec_check_src_change()` (`vdec.c`) waits up to `100ms` on `inst->reconf_wait` when `inst->streamon_out` is active so `VIDIOC_G_FMT(CAPTURE)` waits for `HFI_EVENT_SESSION_SEQUENCE_CHANGED`.
   - *Hardware Verification (`#198`, `<lan-ip>`, zero `dmesg` errors)*:
     - `ffmpeg -y -c:v h264_v4l2m2m -i /tmp/test60.h264 -f null -`: `60 frames` (320x240) decoded in `0.08s` (`RC=0`).
     - `ffmpeg -y -c:v h264_v4l2m2m -i /tmp/test300.mp4 -f null -` (back-to-back): `300 frames` (1280x720) decoded in `0.63s` (`speed=15.9x`, ~476 fps, `RC=0`).
   - *Checks & Final Artifact SHA-256 (`#257`)*:
     - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
     - `./build.sh check`: `[OK] 30 patches applied`, `[OK] series applies to a clean tree and the DTB builds`.
     - `build/out/Image`: `55eb7ddf6b378080dab8683455ac9eb7163d69a6e2144f5e0eaeab64937dfab1`
     - `build/out/sm7250-xiaomi-picasso.dtb`: `13fa6c1fc1d34cc9c2c2cdc02e4a43657569e8ae1dc60d951537d731b04f0119`
     - `build/out/payload-linux.bin`: `3c1c13549b89fbb9732d60ff55d6411f73f08de79f1d5e0d8cc2db79667a035c`

### 17. Android `netd` Kernel Configs (`#199`) & TikTok Live Playback Metrics (verified 2026-11-17)

1. **Android `netd` `RouteController` Fix (`configs/linux/picasso.config`)**:
   - Enabled `CONFIG_IP_ADVANCED_ROUTER=y`, `CONFIG_IP_MULTIPLE_TABLES=y`, `CONFIG_IPV6_MULTIPLE_TABLES=y`, `CONFIG_FIB_RULES=y`, `XFRM`, `INET_UDP_DIAG`, `INET_DIAG_DESTROY`, and `iptables`/`xtables` options as `=y`, resolving the Android `netd` `RouteController` boot loop in Droidloom (`Droidloom is running; Android is ready`).
2. **TikTok (`com.zhiliaoapp.musically`) Live Playback Metrics (`#199`, `<lan-ip>`, muted)**:
   - **GPU rendering**: Hardware GPU (`GLES: freedreno, FD620, OpenGL ES 3.2 Mesa 26.3.0`, `/dev/dri/renderD128` open by `musically`, `system_server`, `composer3`, `deniald`; `3d00000.gpu/cur_freq` = `625 MHz`).
   - **Video decode**: CPU software decode — TikTok used ByteDance's in-app CPU `ByteVC1_dec` (`libByteVC1_dec.so` / `libbytevc0.so`), because Droidloom only exposes software `c2.android.*` (`c2.android.hevc.decoder`, `software-only: 1`) with no `/dev/video*` or V4L2 Codec2 HAL in the Android cell.
   - **Steady-state CPU during playback**: `aoapp.musically` `57.1%` CPU (`21.1%` MEM), `deniald` `6.7%–8.8%`, `system_server` `6.8%`, `droidloom-audio` `2.4%`.
3. **`#199` Artifact SHA-256**:
   - `build/out/Image`: `616c928dee8d48a3ae745518a8fae0974926f00fab0eba9cb620d51ec66f35fd`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `13fa6c1fc1d34cc9c2c2cdc02e4a43657569e8ae1dc60d951537d731b04f0119`
   - `build/out/payload-linux.bin`: `97adedd05652fe06126783f50b15304f05fd44f082d574e6092faf38787baea`

### 18. 120Hz GPU Rendering Benchmark (Settings, Chromium on Wayland, Multitasking) (verified 2026-11-17)

Measured on `<lan-ip>` (`#199`) with 120Hz touch-swipe injection into `/dev/input/event2` (`NVTCapacitiveTouchScreen`) over ~4.43s windows, counting `msm_mdss` (`msm` + `dsi_isr`), `3d00000.gpu` (`gpu-irq`), and `cur_freq`:

| Scenario | `dsi_isr`/s | `msm` (DPU)/s | `gpu-irq`/s | GPU `cur_freq` | CPU Highlights |
|---|---|---|---|---|---|
| **A. Denial Settings** (`/usr/lib/denial-apps/settings`) | **120.2/s** (`+534` in `4.44s`) | **104.4/s** | **325.3/s** | `625 MHz` | `deniald` `3.0%` |
| **B. Chromium** (`157.0.8103.78` on native Wayland `wayland-1`) | **120.0/s** (`+532` in `4.43s`) | **118.2/s** | **345.8/s** | `625 MHz` | `chromium` browser/gpu/renderer `49.3% / 40.8% / 25.4%` |
| **C. Multitasking** (TikTok + Chromium + Denial Settings) | **120.0/s** (`+532` in `4.43s`) | **98.6/s** | **267.5/s** | `625 MHz` | `aoapp.musically` `68.4%`, `chromium` `27.1%/25.3%/16.6%`, `settings` `21.7%`, `system_server` `5.1%`, `surfaceflinger` `4.5%` |

- Chromium picked up `/home/picasso/.config/chromium-flags.conf` (`--ozone-platform=wayland --enable-features=UseOzonePlatform,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL --ignore-gpu-blocklist --enable-gpu-rasterization --enable-zero-copy`) with GPU process (`--render-node-override=/dev/dri/renderD128`) active on Adreno 620.

### 19. Phase-2 bring-up: haptics, NFC, audio capture, camera CCI, hotspot, GNSS, cellular, health (2026-11-17)

Work orchestrated through 8 Orca `omp` research workers (docs in `docs/bringup/`) plus one follow-up
debug worker; every change was made in `build/linux/`, exported with `./build.sh export-patches`
(0001 sm7250.dtsi, 0002 board DTS, 0007 pinmux) and verified over RAM boot only.

#### Haptics (PM7250B vibrator LDO) — WORKING
- Hardware: `pm7250b@3` SPMI peripheral `0x5300`, vendor `qcom,qpnp-vibrator-ldo`, 3.0 V,
  overdrive off (vendor DT + `leds-qpnp-vibrator-ldo.c:18-29` register map).
- **No new driver**: mainline `pm8xxx-vibrator.c` already speaks the same registers through its
  `pmi632` set, and the binding lists `qcom,pm7250b-vib` (`qcom,pm8xxx-vib.yaml:21-26`).
  DT: `vibrator@5300 { compatible = "qcom,pm7250b-vib", "qcom,pmi632-vib"; reg = <0x5300>; }`,
  config `CONFIG_INPUT_PM8XXX_VIBRATOR=y`.
- Verified (`<lan-ip>`): input `pm8xxx_vib_ffmemless` = `/dev/input/event0`,
  `EV=200001` (FF), udev `HAPTICD_DEVICE=1` + `HAPTICD_ID=builtin`, `hapticd.service` active.
- **Physical proof** with the BMI26x accelerometer (`steinerctl raw accel 100`, host timestamps,
  3 x 1.2 s pulses at 2 s intervals): per-second |a| std = 0.013/0.010 before, **0.053** (pulse 1),
  0.010 (gap), **0.045/0.038** (pulse 2), 0.010 (gap), **0.029** (pulse 3). Motor moves.

#### NFC (NXP NCI on GENI SE0) — bus + driver live, IRQ blocked by TLMM
- Live node is `xiaomi-sm7250-common.dtsi:327-343` (`&qupv3_se0_i2c`, `nq@28`), not the lito-mtp one.
- DT added: `&i2c0` (i2c@880000) 400 kHz + `nfc@28` (`nxp,nxp-nci-i2c`, `enable-gpios` tlmm 12,
  `firmware-gpios` tlmm 35, `interrupts` tlmm 34 LEVEL_HIGH). Pads muxed by `picasso-pinmux`
  (gpio42/43 qup00; VEN high, FIRM low, CLKREQ/IRQ pulled-up inputs) because the EAST tile is not
  mapped by `&tlmm`.
- Verified: `i2cdetect -y -r 0` shows **`UU` at 0x28** (claimed by `nxp-nci_i2c`),
  `/sys/class/nfc/nfc0` exists, `/proc/interrupts` line `msmgpio 34 ... nxp-nci_i2c` counts **0**.
- Blocker: the NCI handshake needs that interrupt; fixing it means mapping the EAST tile (STATUS
  "TLMM (pinmux) — root cause" / `docs/bringup/nfc.md` §R1).

#### Audio capture (WCD9385 + Bolero TX macro + LPI DMIC) — card and PCM live, DSP start fails
- Added: `vamacro/rxmacro/txmacro` (0x3370000/0x3200000/0x3220000), `swr1` (IRQ 155) / `swr2`
  (IRQ 166) with the vendor SWR port tables, `lpass_tlmm` with SWR/DMIC pin states, `wcd9385`
  codec (reset gpio57 pulsed by picasso-pinmux, BOB mic-bias), `vreg_bob` (3.008-3.96 V),
  `audio-routing`, `MultiMedia2` capture FE (`Q6ASM_DAI_TX`) and `WCD Capture` BE
  (`TX_CODEC_DMA_TX_3`), plus `CONFIG_SND_SOC_LPASS_{MACRO_COMMON,RX,TX,VA}_MACRO=y`,
  `CONFIG_PINCTRL_{LPASS_LPI,SM8250_LPASS_LPI}=y`.
- Verified: `/sys/bus/soundwire/devices` has both slaves (`sdw:2:0:0217:010d:00:4`,
  `sdw:3:0:0217:010d:00:3`), `wcd938x_codec` bound, card registers with
  `00-00 MultiMedia1 playback` + **`00-01 MultiMedia2 capture`**, `arecord -l` lists
  `device 1: MultiMedia2`, and playback still works.
- **Blocker**: `arecord -D hw:0,1 ...` returns `Input/output error` immediately after a successful
  `pcm_open` (all `dai_startup` calls return 0). dmesg per attempt:
  `qcom-q6afe aprsvc:service:4:4: cmd = 0x100f6 returned error = 0x1` / `Unknown cmd 0x100f6`
  (`AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST`, q6afe.c:52) - the vendor ADSP firmware does not
  implement mainline's LPASS clock-vote protocol.
- Follow-up root-cause pass (`docs/bringup/microphone-capture-eio.md`, committed): the EIO is the
  ALSA core's `wait_for_avail()` timeout (`sound/core/pcm_lib.c:1952,2027`), not an AFE error - the
  kernel chain (AFE port 0xB037, ASM stream run, ADM map, swr2, TX macro clocks) all start, the DSP
  delivers the 8 pre-queued zero buffers and never another `READ_DONE`. The doc's first hypothesis
  (codec SoundWire TX ports disabled) was tested on hardware: `ADC1 Switch`/`ADC2 Switch` read
  `on`, and the DMIC route (`DMIC2 Switch`, `TX DEC2 MUX=MSM_DMIC`, `TX DMIC MUX2=DMIC2`) was also
  enabled - `arecord` still fails with EIO in both routes, so the stall is downstream of the codec
  switches (TX macro DIN / ADM map / ASM run status). Next: the `wcd938x-sdw` guard patch from
  §6.2 plus a checked `ASM_SESSION_CMD_RUN_V2` return.

#### Camera stage 1/2 (CCI + camera rails) — adapters live
- Added `camcc`-backed `cci0`/`cci1` (`status = "okay"`), the four RPMH camera parents
  (`l16a` 2.8 V, `s5a` 1.8 V, `s8c` 1.2 V, shared `vreg_bob`), three GPIO-gated fixed rails, and
  `&i2c9` (PM8008 presence bus) plus `CONFIG_SM_CAMCC_7250/I2C_QCOM_CCI/I2C_CHARDEV=y`.
- Verified: six i2c adapters, three of them CCI - `i2c-3`/`i2c-4` = `cci@ac4a000/i2c-bus@0|1`,
  `i2c-5` = `cci@ac4b000/i2c-bus@0`; PM8008 bus is `i2c-1` (`i2c@98c000`).
- Sensors (IMX686 main, OV8856 UW, S5K3T2 front, S5K5E9 macro, GC02M1 depth) and the CAMSS graph
  are stage 3; only OV8856 has a mainline driver (`docs/bringup/camera.md`).

#### Wi-Fi hotspot (AP mode on ath10k_snoc) — AP up, DHCP + NAT armed
- `ap0` created on `wlan0`'s phy (`type __ap`), explicit MAC, brought up by
  `wpa_supplicant 2.12` (`mode=AP`, `ssid picasso-ap`, `channel 153 / 5765 MHz`, `wpa_state=COMPLETED`)
  while the STA link stayed associated to the same channel (firmware advertises
  `managed 1 + AP 1, channels<=1`).
- networkd DHCP server (`/run/systemd/network/50-ap0.network`, 192.168.50.0/24) reports
  `State: routable`; `nft` table `ip hotspot` has the forward + masquerade rules;
  `ip_forward=1`. No package required (wpa_supplicant + systemd-networkd only).
- Not proven: a client actually joining - the host has no Wi-Fi interface, so the DHCP lease /
  `station dump` / NAT counters part of `docs/bringup/hotspot.md` §3.8 is still open.

#### GNSS (QMI LOC service 16) — service answers, engine start refused
- `libqmi 1.38.0` installed on the phone (Denial `extra` repo).
- Verified: `qmicli -d qrtr://0 --loc-get-operation-mode` -> `standalone`;
  `--loc-set-operation-mode=standalone` and `--loc-set-nmea-types=...` succeed;
  `--loc-inject-time`, `--loc-inject-position`, `--loc-set-engine-lock` succeed.
- **Blocker**: `--loc-start` fails with `QMI protocol error (46): 'GeneralError'`, and
  `--loc-get-gnss-sv-info` times out, so no NMEA/fix yet. No Geoclue/xdg-portal Location backend
  and no Android GNSS HAL exist on the image (`docs/bringup/gnss.md`).

#### Cellular (IPA + QMI) — IPA bound, UIM answers, SIM has no ATR
- DT: `ipa@1e40000` (`qcom,sm6350-ipa` = IPAv4.7 data) with `ipa-reg`/`ipa-shared`/`gsi` windows,
  IRQs 311/432 + IPA SMP2P entries added to `smp2p-mpss` in `sm7250.dtsi`, `qcom,gsi-loader = "modem"`;
  config `CONFIG_QCOM_IPA=y`, `CONFIG_RMNET=y`.
- Verified: platform device `1e40000.ipa` **bound to the `ipa` driver**; no `wwan0`/`rmnet*`
  netdev appears (the modem never signals IPA setup-ready). `modemmanager` + `libqmi` installed;
  `mmcli` present.
- QMI works: `qmicli --uim-get-card-status` -> Slot 1 `Card state: 'error: no-atr-received (3)'`,
  Slot 2 same; `--uim-sim-power-off/on` succeed (state changes to `error: power-down (1)`), but the
  ATR never arrives; `--nas-get-serving-system` -> `not-registered-searching`, `detached`.
- Blockers: SIM ATR (SIM rail/clock not reaching the card) and then the IPA data path.

#### Residual-error sweep (health) — five S1/S2 fixed, two documented
- `CONFIG_ZRAM=y` -> `zram-swap.service` **active**, `/dev/zram0 2.7 GB` swap.
- `CONFIG_RTC_DRV_PM8XXX=y` -> `/sys/class/rtc/rtc1` = `rtc-pm8xxx` (no more `Failed to read RTC`).
- `CONFIG_QCOM_SPMI_TEMP_ALARM=y` -> thermal zones `pm8150-thermal`, `pm8150l-thermal`,
  `pm7250b-thermal` (51.6/53.3/44.4 C at check time).
- `CONFIG_QCOM_SOCINFO=y` -> `/sys/devices/soc0/serial_number` = `1312402842` (BT address service
  can now succeed).
- `CONFIG_LEDS_CLASS_MULTICOLOR=y` + `CONFIG_LEDS_QCOM_LPG=y` (=`QCOM_PBS=n`) + `&pm8150l_lpg`
  with the single populated channel -> **`/sys/class/leds/blue:indicator`**, `brightness=255`
  accepted.
- Type-C (`pm7250b_typec`) is wired in the DTS and `TYPEC=y`, but
  `CONFIG_TYPEC_QCOM_PMIC` stays `=m` because it is `select DRM_AUX_HPD_BRIDGE if DRM_BRIDGE && OF`
  and `DRM_BRIDGE=m`; forcing it `=y` would drag DRM bridge plumbing, so `/sys/class/typec` is still
  absent (documented, `docs/bringup/health-audit.md` §3b-1).
- Not changed on purpose: Android cgroup-v1 expectations, healthd sysfs paths, single-slot
  `slot-success.service`, `/proc` read-only inside the Droidloom cell - all userland/image issues
  (`docs/bringup/health-audit.md` §1 items 6/10/13/15).

#### Artifacts and gates
- `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
- `./build.sh check`: see the run recorded below (patches + DTB on a clean tree).
- `build/out/Image`: `c40c173690dd201939aadbf489f89c38484f8470a11850872c7c9960a7732b2c`
- `build/out/initrd.cpio.gz`: `70fd8350d00f8d62b7d275b1038119b2a697253738b91f2f7eac59344dc364cb`
- `build/out/sm7250-xiaomi-picasso.dtb`: `d11875dedf3752b8ecf75f0a1563e9154cc9d4237a33f78274db44385f6d58e7`
- `build/out/boot-linux.img`: `3964178f8592a406682142895b1b921757ac26fcec36eb693ce31076253064df`
- `build/out/payload-linux.bin`: `0a70468f6d34c96cb1e10ef13b9505e55d0c737d29a2fbd4051dfbe28946c2f9`

### 20. GPU clock: the fused limit is 625 MHz, not 750 (2026-11-17)

Question raised by an aggregator spec sheet claiming "Adreno 620 @ 750 MHz". Checked against the
Qualcomm SD765G product brief, the vendor tree and the live device:

- The Qualcomm brief (`prod_brief_qcom_sd765g_5g_1.pdf`) **publishes no GPU clock**: it lists
  "Adreno 620 GPU", "10% faster graphics rendering vs the standard Snapdragon 765", API support
  Vulkan 1.1 / OpenCL 2.0 FP / DX12, Hexagon 696, Spectra 355, X52, 7 nm, CPU "up to 2.4 GHz",
  part number **SM7250-AB**. 750 MHz is not a Qualcomm figure.
- The IP OPP table in the vendor DTS does contain 8 levels
  (`lito-gpu.dtsi:24-60`): 750 (TURBO), 700/670 (NOM_L1), 625/540 (NOM), 500 (SVS_L1),
  400 (SVS), 275 (LOW_SVS). But the same file gates them per **fused speed bin**
  (`qcom,gpu-pwrlevel-bins`, `lito-gpu.dtsi:187-330`): bin 0 -> max **625 MHz**, bin 132 -> max
  **625 MHz**, bin 115 -> max **540 MHz**. Android therefore never runs this phone at 750.
- Live device: `mainline`'s `opp-supported-hw` filter selects only 275/400/500/625
  (`/sys/class/devfreq/3d00000.gpu/available_frequencies`), i.e. the fused bin is 0 or 132
  (540 is mask 0x08 and is filtered out; 625/500 are 0x03, 400/275 are 0x0b). `dmesg` has no
  "missing support for speed-bin" line, so the fuse decoded to a known bin.

Conclusion: the mainline OPP table (`sm7250.dtsi:1360-1395`, max 625 MHz with
`opp-level = RPMH_REGULATOR_LEVEL_NOM`) is **correct for this hardware**; adding 670/700/750 would
be an overclock past the fused limit. No kernel change made. Re-verified after the 45-minute
watchdog returned the phone to the loader and it was RAM-booted again (`<lan-ip>`, uptime 0):
same OPP set, `cur_freq` 275 MHz idle, GPU bound, no faults.

### 21. NPU (Hexagon/CDSP) attempt and camera CCI bus proven (2026-11-17)

#### Hexagon 696 compute DSP — node live, boot blocked at PAS authentication
- `&cdsp` enabled in the board DTS (`firmware-name = "qcom/sm7250/xiaomi/picasso/image/cdsp.mdt"`)
  and `initramfs/init.c` now rebinds `8300000.remoteproc` after the modem-firmware mount and writes
  `start` to every `adsp`/`cdsp` remoteproc (the same path the ADSP uses).
- Verified on hardware: `remoteproc1` appears with `name=cdsp`, the correct firmware path, and
  `state=offline`. A manual `echo start > state` produces:
  `remoteproc remoteproc1: Booting fw image .../cdsp.mdt, size 7660` then
  `qcom_q6v5_pas 8300000.remoteproc: failed to authenticate image and release reset` /
  `can't start rproc cdsp: -22`.
- Ruled out: the memory region (`cdsp@87400000 + 0x1e00000`, `no-map`) is byte-identical to the
  vendor `pil_cdsp_region`; the driver's `sm7250_cdsp_resource` already carries `pas_id = 18`,
  `crash_reason_smem = 601`, `proxy_pd_names = {"cx"}` which match the vendor `qcom,turing@8300000`
  node; the firmware files (`cdsp.b00..b19`) are present in the modem image dir and load into RAM
  (the failure is the SCM auth step, after loading). Next: compare the PAS id / `ssctl` instance
  this TZ expects (vendor `smem-id 601`, `ssctl-instance-id 0x17`, `minidump-id 7`) and whether the
  image needs the vendor's `qcom,pil-tz-generic` proxy votes (CX TURBO) before auth.

#### Camera: pads muxed, CCI buses answer with real devices
- `picasso-pinmux` now muxes the whole camera pin group from the downstream
  `picasso-sm7250-camera-sensor-mtp.dtsi`: CCI SDA/SCL (gpio17-20, 27, 28), the five MCLKs
  (gpio13-16, 25), the six reset/pwdn lines (gpio29/30/32/21/23/33) and the rail enables
  (gpio68 VIO, 127 VANA, 45 VDIG).
- Verified: `i2cdetect` on the three CCI adapters finds **real devices** -
  `i2c-3` (`cci@ac4a000/i2c-bus@0`) **0x50**, `i2c-4` (`.../i2c-bus@1`) **0x51**,
  `i2c-5` (`cci@ac4b000/i2c-bus@0`) **0x51 and 0x52**. Those are the camera-module EEPROMs; they
  prove bus pads, pull-ups and the VIO rail are correct.
- The sensors themselves (IMX686/OV8856/S5K3T2/S5K5E9/GC02M1) do **not** answer yet: their MCLKs
  come from `camcc` and nothing enables them, and Sony parts gate their I2C on MCLK. Stage 3 needs a
  camcc clock consumer (or an explicit `assigned-clock-rates` + `clk_prepare_enable` in a probe) before
  the sensor-ID read from `docs/bringup/camera.md` §3 can run.

### 22. Capture: silent stall turned into a hard error (0016) (2026-11-17)

Followed the follow-up the capture root-cause pass recommended: instead of the ALSA core's 176 ms
timeout (`wait_for_avail()` -> `-EIO`, which reads like a hardware fault), `wcd938x_sdw_hw_params()`
now refuses a stream with no SoundWire data port and says why
(`patches/linux/picasso/0016-ASoC-wcd938x-guard-sdw-capture-ports.patch`, exported and listed).

Verified on hardware (`<lan-ip>`, kernel with 0016):

- Card unchanged: `00-00 MultiMedia1 playback`, `00-01 MultiMedia2 capture`, playback intact.
- **No codec switches set** (fresh boot state): `arecord -D hw:0,1 ...` now fails immediately with
  `arecord: main:850: audio open error: Invalid argument` instead of recording for 176 ms and
  returning `Input/output error`.
- **With the codec switches set** (`ADC1/ADC2 Switch` + `MultiMedia2 Mixer TX_CODEC_DMA_TX_3`,
  `TX DEC0 MUX=SWR_MIC`, `TX SMIC MUX0=ADC0`, `TX_AIF1_CAP Mixer DEC0`) the guard passes, `open`
  succeeds, and the read still times out (`-EIO`) - i.e. the guard does not mask the real DSP stall
  described in `docs/bringup/microphone-capture-eio.md` §9.
- Open item: the `dev_err()` text that ships with the guard did not show up in `dmesg` in this run
  (`soc_open: dai_startup wcd938x-sdw-tx -> 0` is the last related line), even though the -EINVAL
  behaviour proves the new branch executed. Worth one more look when the capture data path is fixed.

Gates: `./scripts/policy-scan.sh` OK; `./build.sh check` applies the 31-patch series to a clean tree
and builds the DTB (see the run recorded below).

### 23. GNSS follow-up: client lifetime, not engine refusal (2026-11-17)

- `--loc-start` **succeeds** with a persistent LOC client
  (`qmicli -d qrtr://0 --loc-noop --client-no-release-cid` -> `CID: '1'`, then
  `--client-cid=1 --client-no-release-cid` on every call), where the naive per-invocation client got
  `QMI protocol error (46): 'GeneralError'`. The engine does accept start.
- Cross-process reuse fails (`Unknown client 1 for service loc`), and qmicli refuses
  `--loc-start --loc-follow-nmea` in one invocation (`too many LOC actions requested`).
- `--loc-follow-nmea` alone runs without error but produced **0 lines in 130 s**
  (`/tmp/nmea2.txt`), and `--loc-get-position-report --loc-timeout=40` on the stale CID fails.
- So: QMI/LOC plumbing works; the open question is trace generation (first-fix-only firmware vs an
  assist step vs no sky view indoors). Next: one-process libqmi client that injects time, sets NMEA
  types, starts and follows, then the same outdoors. Recorded in `docs/bringup/gnss.md` §8.

### 24. Diagnostics for the two silent failures (0017) (2026-11-17)

Added `patches/linux/picasso/0017-async-status-diagnostics.patch`: the ASM run response now logs a
non-zero status, and the PAS auth failure logs its numeric return code. Both are diagnostic only.

Results on hardware (`<lan-ip>`):

- **cdsp**: `qcom_q6v5_pas 8300000.remoteproc: failed to authenticate image and release reset: -22`.
  The SCM call returns `-EINVAL`, not `-EACCES`, i.e. the firmware image/hash metadata presented to
  the secure monitor is rejected (or the PAS id does not accept this payload) - the memory map and
  driver data were already ruled out in §21.
- **Capture**: with the codec switches on, the DSP **accepts** `ASM_SESSION_CMD_RUN_V2`
  (no `ASM run failed` line), open succeeds, and the read still times out with `-EIO`. So the stream
  is running but no `READ_DONE` ever arrives: the missing data is upstream of ASM (codec TX SoundWire
  port -> TX macro DIN -> AFE port 0xB037), not a DSP command failure. The FE/BE routing control is
  the right one (`MultiMedia2 Mixer TX_CODEC_DMA_TX_3` exists and is `on`; there is no
  `MultiMedia2 Mixer WCD Capture`).
- **Guard (0016) still correct**: without the switches `arecord` fails at open with `EINVAL`.

### 25. Camera MCLK: a plain clock consumer cannot gate it (negative result, reverted) (2026-11-17)

Tried the obvious workaround for the missing MCLK: a small consumer driver
(`picasso,camera-mclks`) that sets the five `camcc` MCLK branches to 24 MHz and enables them, so the
sensors would answer on CCI before any sensor driver exists. **It does not work; the experiment was
reverted.**

- `devm_clk_get_optional_enabled()` fails on the first clock:
  `clk_branch_toggle+0x168 -> picasso_camera_mclk_probe -> error -EBUSY: cannot enable mclk0`,
  followed by `probe with driver picasso-camera-mclks failed with error -16`. `clk_branch_toggle`
  prints a kernel trace (`---[ end trace ... ]---`) when the branch halt check never reports "off",
  so the branch/its tree is not runnable in that state (the camera power/core-clock context that the
  vendor kgsl-era driver establishes before touching MCLK is missing).
- The same failure happened at boot (silently then, `state=unbound`, all five MCLKs stayed
  `enable=0`, `rate=19200000`).
- Reverted cleanly: driver file, its `obj-y` line, the DT node and the `CLK_IGNORE_UNUSED` markers on
  the two MCLK branches are all gone; patch 0007 no longer mentions the file. Rebuilt, re-exported and
  re-booted: **no `clk_branch_toggle` trace**, no `camera-mclks` device, card and remoteprocs
  unchanged (`adsp running`, `modem running`, `cdsp offline`), and `i2cdetect` still shows the camera
  EEPROMs (`0x50`, `0x51`, `0x51/0x52`) on the three CCI adapters.

Conclusion for stage 3: the sensor-ID read needs the camera clock/power context brought up properly
(camcc core clock + TITAN_TOP GDSC sequencing as the downstream/VFE stack does), not a bare consumer
of the MCLK branches.

### 26. Capture: the TX SoundWire slave is in Alert, the master's read path misbehaves (2026-11-17)

Followed §24 ("data missing upstream of ASM") one level down and found a concrete transport fault:

- `/sys/bus/soundwire/devices/`:
  - `sdw:2:0:0217:010d:00:4` (RX, swr1) -> **Attached**
  - `sdw:3:0:0217:010d:00:3` (TX, swr2) -> **Alert**
- Every capture `open` makes the TX master log
  `qcom-soundwire 3230000.soundwire: qcom_swrm_irq_handler: SWR read enable valid mismatch`
  (`SWRM_INTERRUPT_STATUS_READ_EN_RD_VALID_MISMATCH`, `drivers/soundwire/qcom.c:810`), i.e. a read
  command is issued but the matching read-valid response never arrives; the slave then drops to
  Alert. The RX master never prints this.
- Port counts are not the problem: the master validates `qcom,din-ports = <5>` against
  `SWRM_COMP_PARAMS` and would have logged "din-ports (5) mismatch with controller (N)"; it did not,
  so the hardware agrees with the DT (5 DIN).
- Consequence: the WCD9385's TX ports cannot be programmed/verified over SoundWire, which is exactly
  why the codec switch route has no effect and the DSP streams silence (ASM run is accepted, §24).

Next: compare the TX master instance against the RX one in the vendor `soundwire`/`swrm` code
(`swrm-io-base` 0x3230000, master id 3, IRQ 166) - same register layout and same `qcom,soundwire-v1.5.1`
compatible are used for both in our DTS, so the difference is likely in the command/read sequencing
for a *DIN-only* master, or the TX slave needs a different `qcom,ports-*` table than the RX one
(ours reuses the sm8250 tables verbatim).

### 27. TX master wake IRQ: no effect on the Alert (reverted) (2026-11-17)

Vendor wires a second interrupt on the TX SoundWire master (`swr_wake_irq`, PDC 109) next to the GIC
SPI 166 one, and the mainline `qcom-soundwire` driver does support it (`of_irq_get(node, 1)` ->
`qcom_swrm_wake_irq_handler`). Tried the same on `swr2`:

- with `interrupts-extended = <&intc 166 LEVEL_HIGH>, <&pdc 109 LEVEL_HIGH>` the master registered both
  IRQs (`/proc/interrupts`: two `soundwire` lines) and booted fine,
- but the TX slave stayed **Alert** (`sdw:3:0:0217:010d:00:3`), `arecord` still returned `EIO`, and the
  `SWR read enable valid mismatch` still fired once per open.
- Reverted: the extra IRQ buys nothing here and is not proven to be part of the fault, so `swr2` is
  back to the single SPI 166 interrupt that was verified working (as far as it goes) before.

Remaining hypothesis for the Alert: a TX-specific master/slave protocol difference - the DIN-only
master's command/read sequencing, the port tables (ours are the sm8250 ones reused verbatim), or the
codec's TX slave programming - not the interrupt wiring.

### 28. Microphone, NFC, and GPS/GNSS — ALL WORKING on live hardware (2026-11-17)

Verified via RAM boot (`#213`, `<lan-ip>`, cmdline
`console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0 systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10`):

1. **Microphone (`0019*`, `0020*`, `initramfs/init.c`) — WORKING**:
   - Root cause of `-EIO` in `sound/soc/qcom/qdsp6/q6afe.c` (`q6afe_cdc_dma_port_prepare()`): when
     `q6dma_set_channel_map()` populated a non-zero `cfg->active_channels_mask` from
     `tx_macro_get_channel_map()`, `if (!cfg->active_channels_mask)` skipped setting
     `dma_cfg->active_channels_mask`, leaving it `0` so the ADSP started `TX_CODEC_DMA_TX_3` with
     zero active channels. Added the `else dma_cfg->active_channels_mask = cfg->active_channels_mask;`
     branch (`0019-ASoC-q6afe-preserve-active-channels-mask.patch`).
   - Root cause of `sdw:3:0:0217:010d:00:3` (`Alert`): `qcom_swrm_get_alert_slave_dev_num()` never
     updated `ctrl->slave_status`, and after `SCP_INT1` cleared, SWRM v1.5.1 returned to `0x04`
     (`SDW_SLAVE_ATTACHED`) without a second `CHANGE_ENUM_SLAVE_STATUS` IRQ. Updated
     `drivers/soundwire/qcom.c` (`0020-soundwire-qcom-resync-slave-status-after-alert.patch`) to
     record `ctrl->slave_status` and resync `sdw_handle_slave_status()` when `SDW_SLAVE_ATTACHED` is
     observed.
   - `initramfs/init.c` updated (`Picasso-HiFi.conf` `SectionVerb` + `SectionDevice."Mic"` on `hw:${CardId},1`
     mono `AMIC1`/`ADC1`/`DEC0`, plus WirePlumber `50-picasso-alsa.conf` `alsa_input.*` `S16LE`/`48000`/1ch)
     so `MultiMedia2` backend routing is armed before ACP probes PCMs.
   - **Verified on `#213` fresh boot**:
     - `/sys/bus/soundwire/devices/sdw:2:0:0217:010d:00:4` (`RX`) = `Attached`,
       `sdw:3:0:0217:010d:00:3` (`TX`) = `Attached`.
     - `wpctl status` exposes both `Built-in Audio Speaker` and `Built-in Audio Internal Microphone`.
     - `pw-record --format=s16 --rate=48000 --channels=1 /tmp/pw_boot.wav` captures 187,392 samples
       (`min=-89 max=69 rms=19.51`).
     - Hardware gain test on `#212` `arecord -D hw:0,1` (`ADC1 Volume` 0 vs 20): `rms=8.27` vs
       `28.22` (3.41x scaling), zero `-EIO`, zero `SWR read enable valid mismatch`.

2. **NFC (`0002*` `&tlmm`, `0018*`) — WORKING**:
   - Removed the `reg = "south"` override on `&tlmm` in `sm7250-xiaomi-picasso.dts` (`0002*`) and
     added `gpio-reserved-ranges = <0 4>, <59 4>;` (matching `sm7250-oneplus-avicii.dts:488` for
     TZ-owned SE1 `gpio0-3` and SE6/QUP10 `gpio59-62`), enabling all 3 TLMM tiles (`south`, `west`,
     `east`) and arming `tlmm 34` (EAST, PDC 53).
   - Fixed cold-mode IRQ storm in `drivers/nfc/nxp-nci/i2c.c`
     (`0018-nfc-nxp-nci-keep-irq-disabled-when-cold.patch`): `picasso-pinmux.c` sets `bias-pull-up`
     on `gpio 34`, so probing with `VEN=0` (`NXP_NCI_MODE_COLD`) and `IRQ_TYPE_LEVEL_HIGH` previously
     caused an 11M+ IRQ storm and latched `phy->hard_fault = -EREMOTEIO`. Now marks `IRQ_NOAUTOEN`
     in probe and gates `enable_irq`/`disable_irq` across `NXP_NCI_MODE_COLD` transitions.
   - **Verified on `#213`**:
     - `msmgpio 34` idle count = `0` (zero IRQ storm).
     - NFC Generic Netlink lifecycle on `nfc0`: `NFC_CMD_DEV_UP` (`0`), `NFC_CMD_START_POLL(0xfe)` (`0`),
       `NFC_CMD_STOP_POLL` (`0`), `NFC_CMD_DEV_DOWN` (`0`), with `msmgpio 34` incrementing cleanly to `10`.

3. **GPS / GNSS (`initramfs/gnss/picasso-gnss.c`, `scripts/mkinitramfs.sh`, `initramfs/init.c`) — WORKING**:
   - Added persistent single-socket `AF_QIPCRTR` QMI LOC daemon (`initramfs/gnss/picasso-gnss.c`), compiled
     into initramfs `/bin/picasso-gnss` and started automatically at boot via
     `/run/systemd/system/picasso-gnss.service`.
   - Solves `Get Engine Lock = 3` (`Set Engine Lock = NONE`), NMEA bitmask (`0x7F` instead of
     `0xFFFF`), stale pre-NTP `STA_UNSYNC` time injection poisoning, and auto-restart on indoor
     `pos_status=3` timeouts.
   - **Verified on `#213` fresh boot**:
     - `systemctl is-active picasso-gnss` = `active` (`engine_state=1`, `session_state=1`,
       `position_status=1`), `/run/gnss.status` shows `sv_count=2`, `nmea_lines=12` (`$GPGSV` at 1 Hz,
       `GPS:17 snr=28..31` indoors).

4. **Gates & Final Artifact SHA-256**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 35 patches applied`, `dtbs_check: 16 schema diagnostics from the vendor series`.
   - `build/out/Image`: `d8daa4d9a1b65361052a60025281c0ae1c4e9bd6d383fabeac946a1b9d2d2eac`
   - `build/out/initrd.cpio.gz`: `ca9061c651d7174abc8dd6b6e085615bdaddad74bd35ff1d404419285496b429`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `f1f080e67ef9d30b83ac42a5e7318053e9dcec3465580152bd348b8e7e596138`
   - `build/out/boot-linux.img`: `ff6fa33adb448fb94f980bbe9caa91175888174bee2888b6078be16e67252847`
   - `build/out/payload-linux.bin`: `8007f6ecdbef3b52749884e57f7a5533d75c0a72f8e7cf7904e07d8a30ccd283`

### 29. Camera (eGPIO, `camcc` v2 PLL2, `qcom-camss`, `ov8856` Capture & All-Sensor CCI IDs) — WORKING on live hardware (2026-11-18)

Verified via RAM boots `#215`–`#217` (`<lan-ip>`, cmdline
`console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0 systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10`):

1. **Root causes fixed**:
   - **eGPIO ownership (`0021-pinctrl-sm7250-egpio-ownership.patch`, `0007*`)**: SM7250 GPIOs `115..145`
     have `egpio_present = BIT(11)` (`0x800`) and `egpio_enable = BIT(12)` (`0x1000`). Mainline
     `pinctrl-sm7250.c` lacked `.egpio_enable = 12`, `.egpio_present = 11`, and `.egpio_func = 10`,
     leaving camera rail GPIOs (`gpio122` UW VANA, `gpio126` UW VDIG, `gpio127` wide VANA) owned by
     eGPIO so APPS `gpiod_set_value` did not toggle the physical pads. Added eGPIO ownership fields
     to `pinctrl-sm7250.c` (`0021*`), `BIT(12)` claim in `picasso-pinmux.c` (`0007*`), and DT
     `pinctrl-0` (`cam_uw_vana_en`, `cam_uw_vdig_en`, `cam_uw_reset_default`) in `0002*`.
   - **`qcom-camss` MMIO overlap (`0001*`)**: `sm7250.dtsi` had `vfe0` (`0x0acaf000 0x5200`) and
     `vfe1` (`0x0acb6000 0x5200`), overlapping `csid0` (`0x0acb3000`) and `csid1` (`0x0acba000`) and
     causing `devm_platform_ioremap_resource_byname` to fail with `-EBUSY` (`-16`). Trimmed `vfe0`
     and `vfe1` register sizes to `0x4000` (matching `vfe_lite`; `camss-vfe-17x.c` offsets `< 0x3900`).
   - **`camcc` SM7250 v2.0 Zonda PLL2 (`0022-clk-qcom-camcc-sm7250-pll2-v2.patch`)**: Hardware reports
     `soc0/soc_id = 400`, `revision = 2.0`; updated `cam_cc_pll2_config` to `camcc-lito.c` v2 values
     (`0x38200920`, `0x15002001`, `0x80000000`, `test_ctl_val = 0`).
   - **PM8008 & MCLK root cause**: `i2cdetect -y -r 1` (`98c000.i2c`) answers only `0x66` (`bq25970`);
     PM8008 (`0x08`) is absent. All 6 sensors use `19.2 MHz` (`P_BI_TCXO`) MCLKs and require
     `TITAN_TOP_GDSC` ON when gating `cam_cc_mclk0..4_clk`.
   - **Board DT & Kconfig (`0002*`, `configs/linux/picasso.config`)**: Built in
     `CONFIG_VIDEO_QCOM_CAMSS=y` and `CONFIG_VIDEO_OV8856=y`; enabled `&camss` (`port@0` 2-lane D-PHY),
     `ov8856` (`camera@10` on `&cci1_i2c0`, `MCLK2` `gpio15` 6 mA @ `19.2 MHz`, `reset-gpios = <&tlmm 30 GPIO_ACTIVE_LOW>`),
     and `camera_uw_vana_2p8` (`gpio122`), `camera_uw_vdig_1p2` (`gpio126`), `camera_macro_vana_2p8` (`gpio56`).

2. **Hardware Verification (`#216` all-sensor IDs + `#217` clean production capture)**:
   - **All 6 sensor Chip IDs & 5 MCLKs (`#216`, with `TITAN_TOP_GDSC = on` & `cam_cc_mclk0..4_clk` at `19200000` Hz)**:
     - `i2c-3` (`cci0_i2c0`): `s5k3t2` @ `0x56` (`w2 0x00 0x00 r2` = `0x31 0x42`), rear `gc02m1` @ `0x37` (`0xf0`/`0xf1` = `0x02 0xe0`), EEPROM `0x50`.
     - `i2c-4` (`cci0_i2c1`): `imx686` @ `0x1a` (`w2 0x00 0x16 r2` = `0x06 0x86`), front `gc02m1` @ `0x10` (`0xf0`/`0xf1` = `0x02 0xe0`), actuator `0x0c`, EEPROM `0x51`.
     - `i2c-5` (`cci1_i2c0`): `ov8856` @ `0x10` (`UU`, `w2 0x30 0x0a r3` = `0x00 0x88 0x5a`), `s5k5e9yx04` @ `0x2d` (`w2 0x00 0x00 r2` = `0x55 0x9b`), actuator `0x0c`, EEPROMs `0x51`/`0x52`.
   - **MIPI CSI-2 Capture (`#217` clean kernel, zero `clk_branch_toggle` / `-EBUSY` / probe errors)**:
     - Pipeline `"ov8856 5-0010":0 -> "msm_csiphy0":0 -> "msm_csid0":0 -> "msm_vfe0_rdi0":0 -> /dev/video0`
       configured at `SGRBG10_1X10/3280x2464` (`pixelformat=pgAA`).
     - `v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=5 --stream-to=/tmp/ov8856_final.raw`
       captured **5/5 frames** (`50,659,840` bytes = `5 * 10,131,968`, `nonzero=63112/65536`, `min=0, max=252`);
       `test_pattern=1` color-bar capture (`#215`) confirmed exact Bayer bar pixels (`0xff...` / `ffcc00ff00...`).

3. **Gates & Final Artifact SHA-256**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 37 patches applied`, `dtbs_check: 16 schema diagnostics from the vendor series`.
   - `build/out/Image`: `ac488f8ff6ef0e3b76698e830cd1aa9c66ae40dd136460b83048ccc975033b24`
   - `build/out/initrd.cpio.gz`: `ca9061c651d7174abc8dd6b6e085615bdaddad74bd35ff1d404419285496b429`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `1c719d42c1e8e7efb44292c52da2064b72bd0d9c1fa9bbc3c509e79e4e72b7f9`
   - `build/out/boot-linux.img`: `c656930898c18a7700b60ebbb41e6281e9e461e76811f28934b955f888f7f40d`
   - `build/out/payload-linux.bin`: `c0b3f74efca1736cb873ec0a1bf28cf63bed159b2cbc43cba1078dd5bccfff18`

### 30. All 3 Primary Cameras (`ov8856`, `s5k3t2`, `imx686` C-PHY) — WORKING on live hardware (2026-11-18)

Verified via RAM boot `#219` (`<lan-ip>`, cmdline
`console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0 systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10`):

1. **Drivers, C-PHY `camss`, DT & Kconfig (`0023*`, `0024*`, `0002*`, `configs/linux/picasso.config`)**:
   - **Samsung `S5K3T2` (`0024-media-i2c-add-imx686-s5k3t2-drivers.patch`)**: `drivers/media/i2c/s5k3t2.c` on `&cci0_i2c0` (`3-0056`), chip ID `0x3142`, MCLK0 `19.2 MHz`, reset `gpio32`, 4-lane D-PHY -> `camss` `port@2` (`msm_csiphy2`), `SGBRG10_1X10` (`2592x1940` default + `5184x3880`).
   - **Sony `IMX686` (`0024-media-i2c-add-imx686-s5k3t2-drivers.patch`)**: `drivers/media/i2c/imx686.c` on `&cci0_i2c1` (`4-001a`), chip ID `0x0686`, MCLK1 `19.2 MHz`, reset `gpio29`, 3-trio C-PHY -> `camss` `port@1` (`msm_csiphy1`), `SBGGR10_1X10` (`4624x3472` default with PDAF `dt=0x30` regs zeroed for pure VC0 `dt=0x2b`, plus `4624x2600`, `2312x1300`, `9248x6944`).
   - **SM7250 C-PHY `qcom-camss` (`0023-media-qcom-camss-add-sm7250-cphy.patch`)**: Added `MEDIA_BUS_TYPE_CSI2_CPHY` (`bus-type = <1>`) in `camss.c`, `lane_regs_sm7250_3ph` in `camss-csiphy-3ph-1-0.c`, and `CSI2_RX_CFG0_PHY_TYPE_SEL` in `camss-csid-gen2.c`.
   - **Shared GPIO proxy (`configs/linux/picasso.config`)**: Built in `CONFIG_GPIO_SHARED_PROXY=y` (alongside `CONFIG_VIDEO_IMX686=y` and `CONFIG_VIDEO_S5K3T2=y`). Without it, `gpio125` (shared by `camera_front_vana_2p8` / `camera_front_vdig_1p2`) and `gpio127` (shared by `camera_wide_vana_2p8` / `camera_wide_vana2_1p8`) are marked `GPIOD_FLAG_SHARED` by `gpiolib-shared` and stay stuck in `devices_deferred`.

2. **Live Hardware Capture Verification (`#219`, empty `devices_deferred`, zero `camss`/`sensor`/`cci` kernel errors)**:
   - **`ov8856 5-0010` (`/dev/v4l-subdev17`, `csiphy0` 2-lane D-PHY)**:
     ```sh
     media-ctl -d /dev/media0 -r
     media-ctl -d /dev/media0 -l '"msm_csiphy0":1->"msm_csid0":0[1],"msm_csid0":1->"msm_vfe0_rdi0":0[1]'
     media-ctl -d /dev/media0 -V '"ov8856 5-0010":0[fmt:SGRBG10_1X10/3280x2464],"msm_csiphy0":0[fmt:SGRBG10_1X10/3280x2464],"msm_csid0":0[fmt:SGRBG10_1X10/3280x2464],"msm_vfe0_rdi0":0[fmt:SGRBG10_1X10/3280x2464]'
     v4l2-ctl -d /dev/video0 -v width=3280,height=2464,pixelformat=pgAA
     v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=3 --stream-to=/tmp/ov8856.raw
     ```
     Captured **3/3 frames** (`30,395,904` bytes, `sample64k_nonzero=65371/65536`, `min=0, max=255`).
   - **`s5k3t2 3-0056` (`/dev/v4l-subdev18`, `csiphy2` 4-lane D-PHY)**:
     ```sh
     media-ctl -d /dev/media0 -r
     media-ctl -d /dev/media0 -l '"msm_csiphy2":1->"msm_csid1":0[1],"msm_csid1":1->"msm_vfe1_rdi0":0[1]'
     media-ctl -d /dev/media0 -V '"s5k3t2 3-0056":0[fmt:SGBRG10_1X10/2592x1940],"msm_csiphy2":0[fmt:SGBRG10_1X10/2592x1940],"msm_csid1":0[fmt:SGBRG10_1X10/2592x1940],"msm_vfe1_rdi0":0[fmt:SGBRG10_1X10/2592x1940]'
     v4l2-ctl -d /dev/video3 -v width=2592,height=1940,pixelformat=pGAA
     v4l2-ctl -d /dev/video3 --stream-mmap=4 --stream-count=3 --stream-to=/tmp/s5k3t2.raw
     ```
     Captured **3/3 frames** (`18,903,360` bytes, `sample64k_nonzero=65366/65536`, `min=0, max=255`); `test_pattern=2` color bars captured **2/2 frames** (`12,602,240` bytes, `head16=ffffffffffffffffffffffffffffffff`).
   - **`imx686 4-001a` (`/dev/v4l-subdev19`, `csiphy1` 3-trio C-PHY)**:
     ```sh
     media-ctl -d /dev/media0 -r
     media-ctl -d /dev/media0 -l '"msm_csiphy1":1->"msm_csid2":0[1],"msm_csid2":1->"msm_vfe2_rdi0":0[1]'
     media-ctl -d /dev/media0 -V '"imx686 4-001a":0[fmt:SBGGR10_1X10/4624x3472],"msm_csiphy1":0[fmt:SBGGR10_1X10/4624x3472],"msm_csid2":0[fmt:SBGGR10_1X10/4624x3472],"msm_vfe2_rdi0":0[fmt:SBGGR10_1X10/4624x3472]'
     v4l2-ctl -d /dev/video6 -v width=4624,height=3472,pixelformat=pBAA
     v4l2-ctl -d /dev/video6 --stream-mmap=4 --stream-count=3 --stream-to=/tmp/imx686.raw
     ```
     Captured **3/3 frames** (`60,329,472` bytes, `sample64k_nonzero=64163/65536`, `min=0, max=255`); `test_pattern=2` color bars captured **2/2 frames** (`40,219,648` bytes, `head16=ffffffffffffffffffffffffffffffff`).

3. **Gates & Final Artifact SHA-256**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 39 patches applied`, `dtbs_check: 16 schema diagnostics from the vendor series`.
   - `build/out/Image`: `6c359d8c60873d5219917a063c9fef8b0141a275a8ce7a24c36d103eb5c9fd30`
   - `build/out/initrd.cpio.gz`: `ca9061c651d7174abc8dd6b6e085615bdaddad74bd35ff1d404419285496b429`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `11dd78601f1e3c53aab6567e022360fd859b064b4852b75574dcf8d5c0e4c9a8`
   - `build/out/boot-linux.img`: `89741f00ba45a016a9abb3518ac39589d8e540b50c4ebb76171db4095cf33821`
   - `build/out/payload-linux.bin`: `8c336c1d1a4847b02422ae00c51d0bc597f0c6a07a0aa991360e36779bb2d18e`

### 31. Camera Portrait Orientation + Touch SPI & `uclamp`/`HZ=1000` Smoothness — VERIFIED on live hardware (`#220`, 2026-11-18)

Verified via RAM boot `#220` (`<lan-ip>`, cmdline
`console=tty0 loglevel=7 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0 systemd.show_status=1 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10`):

1. **Camera Portrait Orientation (`0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch`)**:
   - **Root cause**: `ov8856.c`, `imx686.c`, and `s5k3t2.c` already call `v4l2_fwnode_device_parse()` + `v4l2_ctrl_new_fwnode_properties()`, which only register `V4L2_CID_CAMERA_SENSOR_ROTATION` (`0x009a0923`) when DT `rotation` is populated; without it, `/usr/bin/braun` logged `no rotation: none assumed` and exposed landscape buffers.
   - **Fix**: Added `rotation = <270>;` on `camera@56` (`s5k3t2`), `rotation = <90>;` on `camera@1a` (`imx686`), and `rotation = <90>;` on `camera@10` (`ov8856`) in `arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts` (matching downstream `picasso-sm7250-camera-sensor-mtp.dtsi` `sensor-position-roll`).
   - **Live verification (`#220`)**:
     - `v4l2-ctl -d /dev/v4l-subdev17..19 --list-ctrls` exposes `camera_sensor_rotation`: `90` (`ov8856`), `270` (`s5k3t2`), `90` (`imx686`).
     - `journalctl -u braun` shows zero `no rotation` warnings; D-Bus `org.braun.Camera1.Cameras` reports upright portrait dimensions (`ov8856`: `2464x3280`, `s5k3t2`: `1940x2592`, `imx686`: `3472x4624`).
     - Live captures via `/usr/lib/denial-apps/camera` produced upright portrait JPEGs verified visually: back `ov8856` (`2464x3280`) and front `s5k3t2` (`1940x2592`).

2. **Touch SPI / Mutex Contention & `deniald` `uclamp`/`HZ=1000` Smoothness (`0002*`, `0011*`, `configs/linux/picasso.config`)**:
   - **Touch driver (`0011-input-picasso-nvt-sram-firmware.patch`, `0002*`)**:
     - Increased `&spi1 -> touchscreen@0` `spi-max-frequency` from `<5000000>` to `<9600000>` (`dmesg`: `mode 0, 9600000 Hz`, `spi0.0/statistics/messages = 74475`, `errors = 0`).
     - Added `cur_page` / `page_valid` SPI page caching in `nvt_set_page()` (invalidated before/after `reset_gpio` toggle in `nvt_eng_reset()`, `nvt_bootloader_reset()`, `nvt_suspend()`, `nvt_resume()`, and SPI errors), eliminating redundant 3-byte `spi_sync` page writes per touch IRQ.
     - Increased `NVT_POLL_MS_IRQ` from `100` to `2000` and skipped `nvt_fetch_report()` and active-touch health reads (`!ts->slots`) once `ts->irq_count > 0`, eliminating `kworker` vs `nvt_irq_thread` `ts->lock` mutex stalls.
   - **Kernel scheduling (`configs/linux/picasso.config`)**:
     - Enabled `CONFIG_UCLAMP_TASK=y`, `CONFIG_UCLAMP_BUCKETS_COUNT=20`, `CONFIG_UCLAMP_TASK_GROUP=y`, and `CONFIG_HZ_1000=y` (`CONFIG_HZ=1000`).
     - Verified in `/proc/config.gz`; `deniald` logs `applied UI CPU capacity request ... minimum=512` (`flutter-display`, `flutter-raster`, `compositor`) and `deniald::cpu_scheduling::hints: semantic CPU hints active little=0,1,2,3,4,5 little_capacity=453 boost=454 focus=256 background_max=453 hold_ms=1500 pin=true`.
   - **Live frame-rate & GPU/CPU profiling (`#220`)**:
     - Display panel runs at `120.5 Hz` (`dsi_isr`); synthetic `event3` scroll on `settings` (`/usr/lib/denial-apps/settings`) rose from `28.4 fps` on `#219` (trapped on LITTLE `cpu2`) to `39.7–40.7 fps` drag / `40.0 fps` fling on `#220` (`tid` placed on Prime `cpu7`, `1401.6 MHz`, GPU `3d00000.gpu` at `625 MHz`).
     - `VkQueryPool` + `DRM_IOCTL_MSM_WAIT_FENCE` profiling isolated the remaining ~40 fps ceiling in `settings` to `gpui_wgpu` GPU shader workload (`gpu_exec = 14.2–15.4 ms` @ `625 MHz`, `35.3–36.0 ms` @ `275 MHz`; `RP0` `988x2116` takes `12.2 ms` across `fs_quad`/`fs_shadow`, scaling `2.42x` with GPU clock; `MESA_VK_WSI_PRESENT_MODE=mailbox`/`immediate` = `40.3`/`40.4 fps`; pinning `CPU 6,7` at max clock = `39.0 fps` drag / `41.2 fps` fling).

3. **Gates & Final Artifact SHA-256**:
   - `./scripts/policy-scan.sh`: `OK (22 code files, 42 path files)`.
   - `./build.sh check`: `[OK] 39 patches applied`, `dtbs_check: 16 schema diagnostics from the vendor series`.
   - `build/out/Image`: `368c4c31f7c402731b76c99349be951dd98d73835d7c4a120d21d8bf0d888d83`
   - `build/out/initrd.cpio.gz`: `ca9061c651d7174abc8dd6b6e085615bdaddad74bd35ff1d404419285496b429`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `fbbe0f50b2934cc103ffbd32cb29a06212a55cc597d16e18bc38faccbd952a62`
   - `build/out/boot-linux.img`: `2bc0821be67dfdae241b1e7bca2b1913f5cb7a0e059490e2e8b4205d1af67151`
   - `build/out/payload-linux.bin`: `4b40548a6ae515d3c3a25e13294f1c1bd8b603a21db30239cddfba00e33ea6dc`

### 32. IMX686 QBC/Bayer Fix, DW9800W Autofocus, Chromatix7 Calibrations & Boot-Packaged PipeWire DSP — VERIFIED on live hardware (2026-11-21)

Verified on `/dev/disk/by-partlabel/boot` (`Linux picasso 7.3.5-00001-gfdb38e3cdc12-dirty #5 SMP PREEMPT Sat Nov 21 09:21:10 +07 2026 aarch64`, cmdline
`console=ttynull loglevel=3 quiet vt.global_cursor_default=0 selinux=0 picasso_pinmux.mode=write rdinit=/init auto_reboot=0 systemd.show_status=0 clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10`):

1. **IMX686 `4624x3472` & `4624x2600` QBC Binning + Bayer CFA Fix (`0025-media-i2c-add-imx686-s5k3t2-drivers.patch`)**:
   - **Root cause**: Zeroing PDAF `dt=0x30` registers (`0x30d8`, `0x3092`, `0x4324`, `0x4325`) while leaving `0x30d9 = 0x00` caused IMX686 modes 1 and 2 to output half-height (`1736/3472`, `1300/2600`) 4×4 Quad-Bayer frames. Additionally, Sony color bars (`test_pattern=2`) and real-scene channel ratios proved the sensor outputs `MEDIA_BUS_FMT_SRGGB10_1X10` rather than `MEDIA_BUS_FMT_SBGGR10_1X10`.
   - **Fix**: Set `{ CCI_REG8(0x30d9), 0x01 }` in `imx686_mode_1_4624x3472_30fps_regs` and `imx686_mode_2_4624x2600_60fps_regs`, and switched `imx686.c` format/enum codes to `MEDIA_BUS_FMT_SRGGB10_1X10`.

2. **DW9800W VCM Hardware Autofocus & Portrait Orientation (`0002*`, `configs/linux/picasso.config`)**:
   - Enabled `CONFIG_VIDEO_DW9807_VCM=y` and added `vcm_wide: lens@c` (`dongwoon,dw9807-vcm`) on `&cci0_i2c1` with `lens-focus = <&vcm_wide>` and `rotation = <270>` on `camera@1a` (`imx686`, `turned 90 clockwise` in `braun`).
   - Live capture via `braunctl shot imx686` moved `dw9807 4-000c` `focus_absolute` (`0` -> `322`), producing a full `3472x4624` portrait JPEG with upright orientation and accurate colors/WB.

3. **Boot-Packaged `braun` Calibrations & PipeWire Speaker DSP (`initramfs/`, `scripts/mkinitramfs.sh`)**:
   - Packaged Chromatix7 AWB Planckian `locus`, per-CCT `ccm`, and `shading` tables (`initramfs/braun/calibrations/{imx686,ov8856,s5k3t2}.toml`, with DW9800W EEPROM travel `[300, 860]`, start `420`) plus sensor gain profiles (`initramfs/braun/sensors/*.toml`).
   - Packaged speaker DSP (`initramfs/pipewire/99-picasso-speaker-dsp.conf`, `preamp Gain = 0.0`, `priority.session = 1005`) and WirePlumber speaker sink priority (`900` in `50-picasso-alsa.conf`) via tmpfs bind-mounts in `initramfs/init.c` (zero writes to `userdata`).
   - Verified `braunctl cameras` reports all 3 cameras `tuned` (`imx686: 3472x4624, focus, tuned`; `ov8856: 2464x3280, tuned`; `s5k3t2: 1940x2592, tuned`) and PipeWire sink priorities preserve Bluetooth auto-switching (`bluez_output` `1010` > `picasso_speaker_dsp` `1005` > `HiFi__Speaker__sink` `900`).

4. **Gates & Artifact SHA-256**:
   - `./build.sh check`: `[OK] 39 patches applied`, `dtbs_check: 12 schema diagnostics from the vendor series`.
   - `scripts/preflight.sh`: `preflight: OK` (`policy-scan: OK`).
   - `build/out/Image`: `5f89f5a489c1abd3ce40a5f138f74fdbb972b31c733cdb8ec65bedabe31e5b71`
   - `build/out/initrd.cpio.gz`: `8ec88e57ef139eaf985c42c3886cc6ce07c13f572a4925a23d0de4740315096c`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `ab4c3c55116bc55344bf46029bf12db34158108a782d1e11bf1568fada010117`
   - `build/out/boot-linux-auto.img`: `9766cbc6bbfbcceef0a689f0ca4c23aaf81806f88b1fe61752b713d059438ced`

### 33. Battery OCV Compensation & PM7250B Haptic Range — VERIFIED on live hardware (2026-11-21)

Verified on `/dev/disk/by-partlabel/boot` (`Linux picasso 7.3.5-00001-gfdb38e3cdc12-dirty #6 SMP PREEMPT Sat Nov 21 09:45:51 +07 2026 aarch64`):

1. **Battery SoC accuracy (`0016-power-supply-qcom-pm7250b-add-qg-battery.patch`)**:
   - Fixed `pm7250b_chg_status()` returning `POWER_SUPPLY_STATUS_FULL` while `!online`, which pinned capacity to `100%` right after unplugging.
   - Added ~90 mΩ internal-resistance compensation (`pm7250b_bat_ocv_uv()`) and replaced the coarse voltage table with the 25°C OCV-SoC curve from downstream `G7ANVTBM4P_4500mah` batterydata.
   - Verified live: `status=Discharging`, `voltage_now=4108592`, `current_now=299988`, `capacity=78`, `charge_now=3510000`.

2. **Stronger touch haptics (`0026-input-pm8xxx-vibrator-pm7250b-range.patch`, `build.sh`, `patches/linux/series`)**:
   - Scaled `qcom,pm7250b-vib` to `2504..3200 mV` (8 mV steps) in `drivers/input/misc/pm8xxx-vibrator.c` so light `ff-memless` touch effects no longer drop down to `~1.5–1.8 V`.
   - Verified `/dev/input/event0` (`pm8xxx_vib_ffmemless`) `FF_RUMBLE` ioctl on live hardware.

3. **Gates & Artifact SHA-256**:
   - `./build.sh check`: `[OK] 40 patches applied`, `dtbs_check: 12 schema diagnostics from the vendor series`.
   - `scripts/preflight.sh`: `preflight: OK`.
   - `build/out/Image`: `23643ed64c3672254ba5edd12a68983d6ece0c193778308958b17107c8cd0802`
   - `build/out/sm7250-xiaomi-picasso.dtb`: `ab4c3c55116bc55344bf46029bf12db34158108a782d1e11bf1568fada010117`
   - `build/out/boot-linux-auto.img`: `cfc8578624aaab58b984c798180395914909900c558ae5ddb6cd25a634be11d6`

### 34. Experimental `s2idle` Suspend via `hexagonrpcd` `systemd-sleep` Hook (`exp/s2idle-suspend`, 2026-11-21)

Verified on `/dev/disk/by-partlabel/boot` (`Linux picasso 7.3.5-00001-gfdb38e3cdc12-dirty #6 SMP PREEMPT Sat Nov 21 09:45:51 +07 2026 aarch64`):

1. **Root cause of `s2idle` crash/reboot**:
   - Capturing early console via temporary `ramoops@f3e80000` during `pm_test=devices` isolated the crash to `3000000.remoteproc` (ADSP `sensor_process` `frpc_dsp` hosted by `hexagonrpcd`) faulting right on resume (`err_qdi.c:1045:EX:sensor_process:0x1:frpc_dsp:0xd6`) and triggering an ADSP recovery reset.
   - Stopping `hexagonrpcd.service` before entering `s2idle` allowed clean suspend and RTC wakeup (`rtc1`) with zero faults.
   - `userdata` also disables suspend via `/usr/lib/systemd/sleep.conf.d/70-phone-no-sleep.conf` (`AllowSuspend=no`).

2. **Boot-packaged fix (`initramfs/systemd/picasso-hexagonrpcd`, `scripts/mkinitramfs.sh`, `initramfs/init.c`)**:
   - Packaged `initramfs/systemd/picasso-hexagonrpcd` and bind-mounted `/usr/lib/systemd/system-sleep` over tmpfs `/run/picasso-sleep` (preserving `denial-suspend-mode`) to stop `hexagonrpcd.service` on `pre` and restart it (`--no-block`) on `post`.
   - Wrote `/run/systemd/sleep.conf.d/99-picasso-sleep.conf` (`AllowSuspend=yes`, `MemorySleepMode=s2idle`) in tmpfs (zero `userdata` writes).

3. **Live Verification**:
   - `systemctl suspend` with `rtcwake -d rtc1 -m no -s 7`: `suspend_stats/success=1`, `fail=0`, `PM: suspend entry (s2idle)` -> `PM: suspend exit`, `picasso-nvt-vendor` SRAM firmware reloaded (`fw info: ver 0x0a`), `hexagonrpcd` active again, Wi-Fi `5G` (`5240 MHz`) reconnected.
   - `build/out/initrd.cpio.gz`: `39264e4211be364dd2f8d682afd6cbe143213b324f57671e86d2b53bc3543f32`
   - `build/out/boot-linux-auto.img`: `40bd5341254d6e21d2089d2e02f9cce8a2927c3eaf274fb3a744f8f53123a326`

4. **Bluetooth A2DP software volume & high-quality codec enablement (`initramfs/init.c`)**:
   - Added `/run/wireplumber/wireplumber.conf.d/51-picasso-bluez.conf` (`bluez5.enable-hw-volume = false`, `bluez5.enable-sbc-xq = true`, `bluez5.codecs = [ sbc_xq aac aptx aptx_hd ldac sbc ]`) so Bluetooth speakers that fail BlueZ AVRCP `set_volume()` (`-107`) adjust cleanly via PipeWire `softVolumes`.
   - Verified on hardware (`Loa Bose của Trí`, `AC:BF:71:FB:E1:3A`): `softVolumes=[0.274625, 0.274625]` at `65%`.
   - `build/out/initrd.cpio.gz`: `22cf5c57fcdd6f7ef26e16b85a39a60a58c05389ae092e1deebe621934bfc49c`
   - `build/out/boot-linux-auto.img`: `31f0be48ecca8731b7c6d1135b70b75b36ed9d6453442d00bba196a1e7c80035`


5. **Bluetooth AVRCP `/dev/uinput` support (`configs/linux/picasso.config`)**:
   - Enabled `CONFIG_INPUT_UINPUT=y` so `bluetoothd` `avctp.c:init_uinput()` creates `/dev/uinput` and registers the AVRCP virtual input device (`Loa Bose của Trí (AVRCP)` in `/proc/bus/input/devices`) without `No such file or directory`.
   - Verified on live hardware (`boot-linux-auto.img` flashed to `/dev/disk/by-partlabel/boot`): `/dev/uinput` (`10, 223`) present, `Loa Bose của Trí (AVRCP)` registered, zero `init_uinput()` errors. Note: `AC:BF:71:FB:E1:3A` still reports `org.bluez.MediaControl1.Connected=false` and `transport.c:set_volume() -107` while streaming, so `bluez5.enable-hw-volume = false` remains active for PipeWire `softVolumes` control.
   - `build/out/Image`: `a1c0c2b3298ae1ee0ecd5db5df85ec175ee5572bd136d36fd94d94a3f5e0b790`
   - `build/out/initrd.cpio.gz`: `23fced52515b316d526141ddf218dfe2bee157fcc9552101202263426fad9a01`
   - `build/out/boot-linux-auto.img`: `8ecafa6d15feb8205265352b4c64d52a4fc7741426e23e50786297829f636073`
