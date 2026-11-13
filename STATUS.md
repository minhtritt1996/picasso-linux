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
- Touch and Wi-Fi are not started.
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
