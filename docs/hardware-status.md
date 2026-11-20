# Hardware status

Current state of mainline Linux on the Redmi K30 5G (`picasso`, SM7250). Evidence for every
line (boot number, cmdline, artifact hash, logs) is in [changelog/boot-history.md](changelog/boot-history.md);
per-subsystem research is in [bringup/](bringup/).

Last verified boot: `#222`, 2026-11-20, RAM payload flashed to `boot` (auto-boot image).

## Working

| Area | State |
|---|---|
| Boot | `boot-linux-auto.img`: BootShim → Mu-Silicium → XnuLoader → Linux EFI stub, no PC needed |
| CPU / RAM | 8 cores, ~5.3 GiB, `schedutil`, `UCLAMP`, `HZ=1000`, zram swap |
| Display | DPU 6.1 + DSI, `1080x2400 @ 120 Hz`, WLED backlight, Denial renders on `card1` |
| GPU | Adreno 620 (freedreno), no faults |
| Touch | Novatek NT36672C over SPI, firmware from the initramfs |
| UFS / storage | all LUNs; Denial rootfs on the `linux` partition |
| Wi-Fi / Bluetooth | WCN3990 `wlan0`, WCN3991 `hci0`, RFCOMM enabled |
| Audio | TFA9874 speaker amp, WCD938x, PipeWire + UCM2 |
| Modem / ADSP | `remoteproc` running with rmtfs / pd-mapper / tqftpserv; sensors via `hexagonrpcd` |
| Camera | OV8856, IMX686, S5K3T2 (front rotation 90°) through CAMSS |
| Battery | PM7250B charger + QGauge, IR-compensated and filtered state of charge |
| Video decode | Venus H.264 (`h264_v4l2m2m`) |
| Others | buttons, NFC (NXP NCI), GNSS, haptics, USB ECM + ACM, SSH (USB, Wi-Fi, Tailscale) |

## Known limits

- **Display cutout**: Denial has no cutout / safe-area setting, so status-bar icons sit under the
  dual front camera. The only knobs are `system_bar=top|bottom|hidden` in `outputs.conf`.
- **Denial input**: the on-screen keyboard has no Shift key and a touch long-press is not turned
  into a right click, so terminals have no paste menu. kitty is configured with
  `copy_on_select yes` and `Ctrl+V` / `Alt+V` paste.
- **CDSP** remoteproc does not start (`-22`); nothing uses it yet.
- **Battery** percentage is an open-circuit-voltage estimate, not a coulomb counter.
- Harmless boot-time log noise: DSI PLL reparent warning, early firmware requests before the
  vendor partitions are mounted, SoundWire port-count mismatches.

## Flashing

`fastboot boot build/out/boot-linux-auto.img` tests an image from RAM. Writing the `boot`
partition (`/dev/disk/by-partlabel/boot`) is allowed only for a verified image and only when asked
for; see `AGENTS.md`.
