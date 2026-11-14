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
