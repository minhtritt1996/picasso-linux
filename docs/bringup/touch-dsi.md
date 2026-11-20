# TDDI touch on picasso: what the vendor display side actually does

Recon of `~/droidian/kernel` (downstream 4.19 that drives this phone) against
`~/droidian/live_dmesg_stream.log` (one real boot). Every claim below cites a
file:line or a dmesg line; unknowns are marked **UNKNOWN** instead of guessed.

## Summary

**The display side never sends the touch controller a command over DSI.** Outside
`mi,is-tddi-flag` (rails/reset gating), a DRM blank notifier the touch driver
consumes, a DSI *read* of the panel lockdown bytes (firmware-variant selection)
and a DSI *read* of the DDIC `0xA1` xy-coordinate register (FOD), no
touch/novatek/TDDI symbol exists in `drivers/gpu/drm/msm` or
`techpack/display/msm`. Everything the controller needs to start scanning arrives
over **SPI** (host SRAM download). The DSI coupling is **ordering + power**: the
whole on-command table must be applied and the panel streaming *before* the touch
MCU's firmware download, or the SRAM writes do not land.

Three findings that changed our picture:

1. **The panel on this phone is `g7a_36_02_0c`**, not `g7a_37_02_0a`
   (`dmesg:61`, `:818`, `:834`). Our mainline driver ships the 37_02_0a table
   (`drivers/gpu/drm/panel/panel-xiaomi-g7a-37-02-0a.c:46-220`) and the board DTS
   binds it (`arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts:419-420`). The
   tables differ in many DDIC pages
   (`dsi-panel-g7a-36-02-0c-dsc-video.dtsi:81-195` vs
   `dsi-panel-g7a-37-02-0a-dsc-video.dtsi:81-251`).
2. **The vendor selects firmware `fw03` for this unit**: live lockdown
   `0x46,0x36,0x32,0x01,0x47,0x7b,0x31,0x00` (`dmesg:1922`) matches
   `novatek,cfg_2` (`tp-vendor=0x46`, `display-maker=0x36` —
   `picasso-sm7250.dtsi:168-172`) → `novatek_nt36672c_g7a_fw03.bin`
   (`dmesg:3448`, `:4689`). The old default was fw01.
3. **The host SRAM download is the live path and only lands while the panel is
   on.** Same boot: at 15.07 s (14 s delayed work, panel off) the download fails
   — reset state `0xFF`, golden CRC `0xFFFFFFFF`, `f2c_disp_off` "Wait F2C_EN = 0"
   fails, fw info `0xFF`, `PID=FFFF` (`dmesg:3447-3453,3463-3465,3580-3610`); at
   22.99 s the identical sequence, run from the DRM unblank notifier 178 µs after
   `DSI_CMD_SET_ON`, succeeds in 171 502 µs and reads `FW type 0x01, fw_ver=10,
   PID=5933` (`dmesg:4686-4703`).

Correction to earlier notes: `0x22D51 = 0xA5` is a red herring. The vendor's
handshake constant is `HANDSHAKING_HOST_READY 0xBB`
(`nt36672c/nt36xxx_ext_proc.c:41`, `:68-86`), written only when leaving test
modes; nothing on the boot/resume path waits for it.

## Panel on-table (36_02_0c), ordering that matters

`qcom,mdss-dsi-on-command` (`dsi-panel-g7a-36-02-0c-dsc-video.dtsi:81-208`),
parsed as `data_type len last vc payload`:

| # | bytes | delay | note |
|---|---|---|---|
| 1 | DDIC page/gamma sweep (`FF 23`, `FF 20`, `FF 26`, `FF 27`, `FF 2A`, `FF 2C`, `FF F0`, `FF E0`, `FF D0`, … `FF 10`) | 0 | `.dtsi:82-187` |
| 2 | `39 .. 03 51 0B 6C` (brightness), `15 .. 02 53 24` | 0 | `.dtsi:186-187` |
| 3 | **`15 .. 02 11 00`** (sleep out) | **70 ms** | `.dtsi:188` |
| 4 | **`15 .. 02 29 00`** (display on) | 0 | `.dtsi:189` |
| 5 | **`FF 27`, `FB 01`, `3F 01`, `43 08`, `40 25`** | 0 | `.dtsi:190-194` — sent *after* display on |
| 6 | `FF 10` | 0 | `.dtsi:195` |

Off table (`.dtsi:196-202`): `FF 27`,`FB 01`,**`3F 00`**,`FF 10`,`28 00`,`10 00`
+ 70 ms. Register `3F` in page `27` is the only on/off-symmetric pair around
display on/off and is the strongest TDDI-gate candidate — **UNKNOWN** whether it
gates the touch AFE.

Reset/rails: `reset-sequence = <1 5>, <0 1>, <1 10>` with `lp11-init`
(`.dtsi:41,43`), so the TDDI reset (PM8150L GPIO3) is pulsed by the kernel in
`dsi_panel_prepare()` (`dsi_panel.c:4465-4478`) rather than at power-on.

## Ordering as coded and measured

1. `dsi_bridge_enable()` → `MI_DRM_EARLY_EVENT_BLANK` before mode set
   (`dsi_drm.c:213-214`).
2. `dsi_panel_prepare()` → TDDI reset pulse (`dsi_panel.c:4474`).
3. `dsi_display_enable()` → `dsi_panel_tx_cmd_set(DSI_CMD_SET_ON)`
   (`dsi_panel.c:4791`) — the whole table above.
4. **Then** `MI_DRM_EVENT_BLANK` (`dsi_drm.c:250`) → `nvt_drm_notifier_callback`
   (`nt36xxx.c:3227-3232`) → `nvt_ts_resume()`: release touch reset
   (`nt36xxx.c:3148-3151`, comment "display reset(RESX) and mipi dsi cmds first"),
   `nvt_update_firmware()`, `nvt_check_fw_reset_state(REK)`, `nvt_irq_enable(true)`
   (`:3146-3166`).
5. Off: `EARLY_EVENT_BLANK/POWERDOWN` → SPI `0x50 <- 0x11` (deep sleep,
   `nt36xxx.c:3067-3075`) before `DSI_CMD_SET_OFF`.

Readiness gate in the vendor driver: `EVENT_BUF_ADDR + 0x60` in `0xA0..0xAF`
(`nt36xxx.c:627-665`, states `nt36xxx.h:231-237`); REK (`>= 0xA1`) is enough for
the vendor — it does not wait for `0xA3`.

## What is explicitly unknown

1. **UNKNOWN** which DDIC register (if any) enables the touch AFE; candidate
   page `27h` reg `3F`.
2. **UNKNOWN** whether the chip needs the DSI video stream or merely DDIC power.
3. **UNKNOWN** what `nvt_f2c_disp_off()` (`fw_update.c:821-901`) expects; it
   failed both times it ran.
4. **UNKNOWN** why the vendor uses `IRQ_TYPE_EDGE_RISING` (`nt36xxx.h:68`) while
   its DT flag is `0x2001`; on our port the line idles low, so level-low storms
   and edge-rising is the matching trigger.
5. **UNKNOWN** what `spi-xiaomi-tp` arbitration coordinates with
   (`nt36xxx.c:2446-2470`).
6. `fw01` vs `fw03` was the cheapest untested difference — since fixed (fw03).

## Implementation plan (as executed / remaining)

1. Bind the right panel variant; keep the `FF 27 / 3F 01` tail **after** `0x29`
   and do not re-send `0x11`/`0x29` afterwards.
2. Default the touch firmware to `novatek_nt36672c_g7a_fw03.bin` (DT
   `firmware-name`). **Done.**
3. Drive the upload from a panel event (notifier after display on) instead of a
   fixed sleep; cancel it on panel off.
4. Probe should only reset, read the trim id and register; the upload happens on
   the first panel-on event.
5. After upload require `event_buf + 0x60` in `0xA0..0xAF` (log the byte), then
   read the fw info at `event_buf + 0x78` and fail loudly when `info[0] + info[1]
   != 0xff` (a good boot reads `fw_ver 0x0a`, `PID 0x5933`).
6. Use `IRQF_TRIGGER_RISING` for the touch IRQ and keep polling as the fallback.
   **Done** (edge rising).
7. Suspend: `0x50 <- 0x11` and disable the IRQ; never touch the panel rails.
8. Verification gates, all visible in dmesg: panel-on event before "SRAM upload
   attempt"; reset state `0xa1..0xaf`; fw info sane; report counters incrementing
   with a finger down; otherwise dump `0x22d00` and compare with the vendor's own
   read of the same region (`nt36xxx.c:1429-1471`).

Current state of this port (2026-11-16): vendor-core driver
`drivers/input/touchscreen/picasso-nvt-vendor.c` downloads fw03, ILM CRC matches
(`0x547123e5`), reads `ver 0x0a 16x36 type 0x01 PID 0x5933`, reaches ReK
(`0xA1`) and reads `0xFD`-filled reports — i.e. the controller runs but has not
reached normal run yet; the panel table for 36_02_0c is the next variable.
