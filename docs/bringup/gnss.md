# GNSS / GPS bring-up on picasso (SM7250) — QMI LOC over QRTR

Scope: Redmi K30 5G (`xiaomi,picasso`, SM7250/lito), mainline Linux 7.3-rc7, Denial Mobile
(Arch ARM) userland on `<lan-ip>`. Read-only investigation; no kernel build, no writes to
the phone.

## Verdict

**Userland-only.** The GNSS engine is already alive and answering QMI on this boot:

* The modem (MPSS) firmware owns GNSS. There is **no** separate `gps`/`gnss` remoteproc, PAS id,
  firmware file, rail or GPIO on this board — the engine sits inside `modem.mdt` and is reached
  only through QMI.
* Every kernel prerequisite is present and verified live: modem remoteproc `running`, QRTR over
  the GLINK `IPCRTR` channel working (`qrtr-lookup` shows ~60 services), `rmtfs` + `pd-mapper` +
  `tqftpserv` running.
* `QMI_LOC` (service 16, version 2, instance 0, **node 0, port 85**) is announced and answers
  getters: operation mode = **standalone**, i.e. no A-GPS, no network, no SIM involvement.
* What is missing is *only* a userspace QMI LOC client: `qmicli`, `ModemManager`, `geoclue`,
  `gpsd` and `gnss-share` are **all absent** from the installed system (only `qrtr` is installed).
* Desktop caveat: no installed xdg-desktop-portal backend implements
  `org.freedesktop.portal.Location` (`denial.portal` = Settings/Access only, `gtk.portal` has no
  Location), and the Denial/Droidloom Android cell has no GNSS HAL
  (`dmesg`: `Could not find android.hardware.gnss.IGnss/default in the VINTF manifest`). A fix can
  be obtained and served by Geoclue, but apps that go through the portal need that backend, and
  Android apps in the cell need a GNSS HAL — neither exists yet.

Nothing in the kernel, DTS or `configs/linux/picasso.config` has to change to get a fix.

---

## 1. Hardware ground truth

All downstream values from the vendor 4.19 tree proven on this phone
(`~/droidian/kernel/arch/arm64/boot/dts/vendor/qcom/`). Rows marked *on-phone* / *live* come from
the running system instead, because the vendor tree has no GNSS node at all.

| item | downstream `file:line` | value |
|---|---|---|
| GNSS DT node / rail / GPIO / clock | *(none — `grep -rniE 'gnss\|gps' arch/arm64/boot/dts/vendor/` returns 0 hits, whole vendor DTS tree)* | **no AP-visible GNSS hardware**; engine power, XO, L5 antenna path are internal to MPSS |
| GNSS firmware | *(none — on-phone `image/` has `modem.mdt`, `adsp.mdt`, `cdsp*`, `wlanmdsp.mbn`; no `gps.mdt`)* | GNSS is a PD of the modem image |
| GNSS service advertisement | on-phone `image/modemr.jsn` (sr_domain `msm/modem/root_pd`, qmi_instance_id 180) | `{"provider":"gps","service":"gps_service"}` — the modem root PD advertises the GPS service to servreg |
| Modem remoteproc (PAS) | `lito.dtsi:2739-2759` `pil_modem: qcom,mss@4080000` | `reg = <0x4080000 0x100>` (2741), `qcom,pas-id = <4>` (2758), `qcom,smem-id = <421>` (2759), `qcom,ssctl-instance-id = <0x12>` (2757), `qcom,firmware-name = "modem"` (2753) |
| Modem power/clock | `lito.dtsi:2744-2752` | `xo` = `RPMH_CXO_CLK`; `vdd_cx`/`vdd_mss` = RPMh `VDD_CX_LEVEL`/`VDD_MSS_LEVEL` TURBO; proxy timeout 10000 ms (2754) |
| Modem memory | `lito.dtsi:437` `modem_wlan_region@8c000000`; used at `2756` | `memory-region = <&modem_wlan_mem>` |
| IPCC (mailbox) | `lito.dtsi:2110-2117` `ipcc_mproc: qcom,ipcc@408000` | `reg = <0x408000 0x1000>` (2112), aggregate IRQ `GIC_SPI 229 LEVEL_HIGH` (2113), `#interrupt-cells = <3>` (2115), `#mbox-cells = <2>` (2116) |
| GLINK modem edge | `lito.dtsi:2508-2525` `glink_modem: modem` | `qcom,remote-pid = <1>` (2509), `transport = "smem"` (2510), `mboxes = <&ipcc_mproc IPCC_CLIENT_MPSS IPCC_MPROC_SIGNAL_GLINK_QMP>` (2511-2513), `qcom,glink-label = "mpss"` (2519) |
| QRTR channel | `lito.dtsi:2521-2525` `qcom,modem_qrtr` | `qcom,glink-channels = "IPCRTR"` (2522), `qcom,low-latency` (2523), intents `0x800 x5`, `0x2000 x3` |
| SMEM | `lito.dtsi:2496-2500` `smem: qcom,smem` | `memory-region = <&smem_mem>` (2498), `hwlocks = <&tcsr_mutex 3>` (2499); mainline equivalent is the `smem` node in `sm7250.dtsi` |
| QMI LOC addressing | live `qrtr-lookup` (see §5) | service **16**, version **2**, instance **0**, node **0**, port **85** — "Location service (~ PDS v2)" |

Notable absence, deliberately stated because it is the answer to the antenna/clock question: the
vendor DTS has **no** `qcom,gnss`, no `gnss_l5`, no GPS LNA/rail/GPIO and no GNSS clock. There is
nothing for mainline to power up before tracking; the modem firmware does it.

## 2. Current mainline state

### Kernel transport + remoteproc (present, working)

| item | `file:line` | state |
|---|---|---|
| Modem PAS driver | `build/linux/drivers/remoteproc/qcom_q6v5_pas.c:1905` (`qcom,sm7250-mpss-pas` → `mpss_resource_init`), PAS id `4` at `:1039` | `CONFIG_QCOM_Q6V5_PAS=y` (`configs/linux/picasso.config:107`) |
| Modem node | `build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi:2433-2474` | `reg 0x4080000`, wdog `GIC_SPI 266` (2437), `xo` (2446), `cx`/`mss` PDs (2449-2451), `memory-region = <&modem_wlan_mem>` (2453), `qcom,qmp = <&aoss_qmp>` (2455), `glink-edge` with `IPCC_CLIENT_MPSS`/`GLINK_QMP` mbox, `label = "modem"`, `qcom,remote-pid = <1>` (2462-2470) |
| Board enable | `patches/linux/picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch:497-500` | `firmware-name = "qcom/sm7250/xiaomi/picasso/image/modem.mdt"; status = "okay";` |
| QRTR | `build/linux/net/qrtr/smd.c:92-94` matches rpmsg id `"IPCRTR"`; `MODULE_ALIAS("rpmsg:IPCRTR")` at `:109` | `CONFIG_QRTR=y` (`:112`), `CONFIG_QRTR_SMD=y` (`:113`). The channel is created by the *modem* opening `IPCRTR`, so no DT child node is needed — that is why QRTR works with an empty `glink-edge` |
| SMEM / GLINK smem | `configs/linux/picasso.config:83` `CONFIG_QCOM_SMEM=y`, `:111` `CONFIG_RPMSG_QCOM_GLINK_SMEM=y` | working |
| rmtfs mem | `configs/linux/picasso.config:120` `CONFIG_QCOM_RMTFS_MEM=y` | `/dev/qcom_rmtfs_mem*` present at boot (init.c logs `rmtfs_mem1 present`) |
| Kernel PDR/pd-mapper | `CONFIG_QCOM_PD_MAPPER` / `CONFIG_QCOM_PDR_*` **not** in the fragment (`grep` empty) | intentional: the userspace `pd-mapper`/`rmtfs` pair from initramfs is used |
| Host services | `initramfs/init.c:785-800` (starts `/bin/rmtfs`, `/bin/pd-mapper`, `/bin/tqftpserv`), modem start `:864-869`, `tms-pddump-modem-root_pd.jsn` synthesis `:691-694` | live: `ps` shows `rmtfs` (pid 190) and `pd-mapper` (pid 191) |

### GNSS itself (no kernel part, by design)

* `build/linux/drivers/gnss/` contains only `mtk.c`, `sirf.c`, `ubx.c`, `serial.c`, `usb.c`,
  `core.c` — no Qualcomm QMI LOC consumer. Upstream has no `CONFIG_` for QMI GNSS.
* Live: no `/dev/gnss*`, no `/sys/class/gnss`, no `/sys/kernel/debug/qcom*` — consistent with "no
  kernel driver".
* Upstream `drivers/soc/qcom/` splits the pieces that matter: `pdr_interface.c` +
  `qcom_pd_mapper.c` (PD map, disabled here), while the *location* protocol itself is left to
  userspace over `AF_QIPCRTR`. Nothing to port.

### Live probe of the LOC service (raw QMI over `AF_QIPCRTR`)

Getter-only requests (no session start), QMI 7-byte header `type/txn/msg/len` + TLVs, sent to
node 0 port 85:

| request | msg id | response | indication (data carrier) |
|---|---|---|---|
| Get Operation Mode | `0x004B` | `02 … 02 04 00000000` (result ok) | `10 04 04000000` → **op mode 4 = standalone** (3 consecutive samples) |
| Get NMEA Types | `0x003F` | result ok | `10 04 00000200` → mask **0x00020000**, i.e. **no GGA/RMC/GSV/GSA/VTG bit set** (libqmi 1.38 knows bits 0..6 only, `qmi-enums-loc.h:480-490`) |
| Get Engine Lock | `0x003B` | result ok | `12 08 0300000000000000` → vendor TLV 0x12, first u32 = 3 (libqmi expects TLV 0x10/`guint32`, so `qmicli --loc-get-engine-lock` will report "missing") |

Two consequences that shape the plan below:

1. **`--loc-follow-nmea` alone will print nothing on this firmware** — the default NMEA mask has no
   GGA bit. NMEA types must be set explicitly (`--loc-set-nmea-types=gga|gsa|gsv`) before tracking.
   ModemManager does exactly this itself (`src/mm-shared-qmi.c:5341`, `:5366-5372`).
2. This LOC firmware (PDS v2, service version 2) answers getters with a *response* that carries only
   the operation result, plus a separate *indication* (type `0x04`) with the same message id that
   carries the data. libqmi handles this (qmicli waits on the indication), but hand-rolled clients
   must read both.

## 3. Change plan

### (a) DTS — **no change required, and none should be added**

There is no GNSS node in the vendor tree and no consumer in mainline; adding one would be
invented hardware. The two nodes that carry the transport are already enabled and must stay that
way, so the "change" is an empty diff. Both fragments below are valid DTS as written and already
exist in the tree (shown so the no-op is verifiable):

```dts
/* sm7250-xiaomi-picasso.dts — already present, keep as-is */
&mpss {
	firmware-name = "qcom/sm7250/xiaomi/picasso/image/modem.mdt";
	status = "okay";
};
```

```dts
/* sm7250.dtsi:2462-2470 — child of mpss:remoteproc@4080000, already present */
glink-edge {
	interrupts-extended = <&ipcc IPCC_CLIENT_MPSS
				     IPCC_MPROC_SIGNAL_GLINK_QMP
				     IRQ_TYPE_EDGE_RISING>;
	mboxes = <&ipcc IPCC_CLIENT_MPSS
			IPCC_MPROC_SIGNAL_GLINK_QMP>;
	label = "modem";
	qcom,remote-pid = <1>;
};
```

If a future board variant is found where GNSS does *not* come up, the first thing to check is not a
DTS node but whether the `IPCRTR` rpmsg channel probed (`dmesg | grep -i ipcrtr`) and whether
`qrtr-lookup` shows service 16.

### (b) `configs/linux/picasso.config` — **no change required**

Every needed symbol is already `=y` at the lines quoted in §2. Verification (no build):

```sh
grep -E 'CONFIG_(QRTR|QRTR_SMD|QCOM_Q6V5_PAS|QCOM_SMEM|RPMSG_QCOM_GLINK_SMEM|QCOM_RMTFS_MEM)=y' \
     configs/linux/picasso.config
```

Expected: the six lines `83, 107, 111, 112, 113, 120`. `CONFIG_QRTR_TUN` (m on the live kernel) is
*not* needed for QMI; it is only a tunnel endpoint.

### (c) Kernel patch — **none**

No `patches/linux/picasso/` addition. Rationale in one paragraph: the entire GNSS data path is
firmware → GLINK → QRTR → `AF_QIPCRTR`, and all of those are upstream and already working on this
boot; QMI LOC has no kernel-side component in Linux (unlike ModemManager's older `PDS` path), so a
kernel patch would have nothing to do. The only work item is packaging a QMI LOC client plus a
desktop plumbing unit, which belongs in the userland image (initramfs/rootfs), not in the kernel
patch series. If a unit is desired for persistence, it is a systemd unit on the phone (§"Make it
persistent"), not a patch in this repo.

### (d) Runtime verification (the commands that count)

Package availability was checked read-only on the phone (`pacman -Si`): `libqmi 1.38.0-2`,
`modemmanager 1.24.2-2` (depends on `libqmi` + `libqrtr-glib`), `geoclue 2.8.2-2`,
`gpsd 3.27.5-1`. Installing them is a write to the phone and therefore coordinator-owned; none of
the commands below were run.

Path A — minimal proof with libqmi (no daemons):

```sh
# 1. confirm the engine and its (useless) default NMEA mask
qmicli -d qrtr://0 --loc-get-operation-mode      # QRTR URI form: src/qmicli/qmicli.c:81
qmicli -d qrtr://0 --loc-get-nmea-types
# 2. enable the traces we need (flags are '|'-separated: src/qmicli/qmicli-helpers.c:31-74)
qmicli -d qrtr://0 --loc-set-nmea-types=gga|rmc|gsv|gsa
# 3. start a periodic session (session id 1)
qmicli -d qrtr://0 --loc-start
# 4. stream NMEA (one action per qmicli invocation: qmicli-loc.c:1754-1770 returns after each)
qmicli -d qrtr://0 --loc-follow-nmea
```

Exact expected output that proves success:

```
[/dev/…] Successfully retrieved operation mode: standalone
[/dev/…] Successfully retrieved NMEA types:             # empty: mask 0x00020000, no bit libqmi knows
Successfully set NMEA types
[/dev/…] Successfully started location tracking (session id 1)
$GPGGA,092750.000,5321.6802,N,00630.3372,W,1,8,1.03,61.7,M,55.2,M,,*76
$GPRMC,092750.000,A,5321.6802,N,00630.3372,W,0.02,31.66,280511,,,A*43
```

**The pass criterion is the 6th field of `$GPGGA` (fix quality) > 0** (`1` = GPS fix, `2` = DGPS,
`3` = PPS; `0` = no fix). `$GPRMC` field 2 must be `A`. A cold start outdoors can take 30-90 s;
`--loc-follow-nmea` prints nothing until the first trace.

Fallbacks if NMEA never appears (see §4 for which one is likely):

```sh
qmicli -d qrtr://0 --loc-get-position-report     # decoded lat/lon instead of NMEA
qmicli -d qrtr://0 --loc-get-gnss-sv-info        # are satellites visible at all?
qmicli -d qrtr://0 --loc-set-engine-lock=none    # only if a start is rejected as "engine locked"
```

Path B — ModemManager (single long-lived client; this is the robust path and the one that makes the
fix available to Geoclue). MM 1.24 talks QMI directly to the QRTR node
(`src/mm-port-qmi.c:2528` `qmi_device_new_from_node`, inside `#if defined WITH_QRTR`; plugin
`src/plugins/qcom-soc/mm-plugin-qcom-soc.c:72-83` accepts subsystem `qrtr` and requires a QMI
port). `WITH_QRTR` is MM's meson `qrtr` option, default **true** (`meson_options.txt:25`), and the
Arch package depends on `libqrtr-glib`, so the path is compiled in. It needs no `cdc-wdm`, no
`qmi_wwan`, and no `/dev/qrtr0` sysfs bus (mainline QRTR has none; node discovery is done by
libqrtr-glib, which is why MM works on upstream kernels).

```sh
systemctl start ModemManager
mmcli -L                                              # -> /org/freedesktop/ModemManager1/Modem/0
mmcli -m 0 --location-status                          # capabilities include gps-raw,gps-nmea
mmcli -m 0 --location-enable-gps-nmea
mmcli -m 0 --location-get                             # -> "gps | nmea: $GPGGA,…"
```

Expected `--location-status` excerpt (field names from `cli/mmcli-modem-location.c:306-312`; the
`gps-*` capability names come from `mm_modem_location_source_build_string_from_mask`):

```
  ----------------------------
  Location | capabilities: 3g,agps,assistance-data,gps-raw,gps-nmea
           | enabled:      gps-nmea
           | signals:      yes
           | gps refresh rate: 0 seconds
```

`mmcli -m 0 --location-get` prints the traces multiline under `gps | nmea:`
(`cli/mmcli-modem-location.c:566`), i.e. `$GPGGA,…` / `$GPRMC,…` lines that must contain a fix
quality > 0.

Geoclue (this is the "feed the desktop" step). `[modem-gps] enable=true` is already the shipped
default (`data/geoclue.conf.in:62-65`), nothing to edit:

```sh
systemctl start geoclue
gdbus call --system --dest org.freedesktop.GeoClue2 \
  --object-path /org/freedesktop/GeoClue2/Manager \
  --method org.freedesktop.GeoClue2.Manager.GetClient       # -> (objectpath '/org/freedesktop/GeoClue2/Client/1',)
gdbus call --system --dest org.freedesktop.GeoClue2 \
  --object-path /org/freedesktop/GeoClue2/Client/1 \
  --method org.freedesktop.DBus.Properties.Set \
  org.freedesktop.GeoClue2.Client DesktopId "'gnss-test'"
gdbus call --system --dest org.freedesktop.GeoClue2 \
  --object-path /org/freedesktop/GeoClue2/Client/1 \
  --method org.freedesktop.GeoClue2.Client.Start
busctl --system get-property org.freedesktop.GeoClue2 \
  /org/freedesktop/GeoClue2/Client/1 org.freedesktop.GeoClue2.Client Location
```

Success = a `Location` object path, then `Latitude`/`Longitude`/`Accuracy` non-zero on that object
(`busctl --system get-property … /org/freedesktop/GeoClue2/Location/1 … Latitude`). Geoclue's
`modem-gps` source drives MM's `Location.Enable` with GPS-NMEA itself
(`src/mm-iface-modem-location.c:137-141` maps `MM_MODEM_LOCATION_SOURCE_GPS_NMEA`), so no manual
`mmcli --location-enable-*` is needed once Geoclue is running.

Path C — gnss-share (only if ModemManager is unwanted): it is a Go daemon with drivers
`modemmanager`, `stm` (`internal/gnss/`) and publishes NMEA on a socket; Geoclue consumes it via
`[network-nmea]` (`data/geoclue.conf.in:36-47`, `nmea-socket=/var/run/gps-share.sock`). It still
needs ModemManager for the `mm` driver, so it is *additional* plumbing, not a replacement.

### Make it persistent

Nothing new is needed for the firmware side — `initramfs/init.c:785-800` already starts
`rmtfs`/`pd-mapper`/`tqftpserv` and `:864-869` starts the modem on every boot (verified live).
Persistence is three steps on the phone (the first two are enough for the Geoclue path):

```sh
systemctl enable --now ModemManager
systemctl enable --now geoclue
# only if Path A is used instead of MM: a supervisor for the split qmicli pair
cat >/etc/systemd/system/gnss-nmea.service <<'EOF'
[Unit]
Description=QMI LOC NMEA stream (picasso modem)
After=ModemManager.service

[Service]
# --loc-start is a one-shot; the session lives in the modem, the follower registers NMEA events
ExecStartPre=-/usr/bin/qmicli -d qrtr://0 --loc-set-nmea-types=gga|rmc|gsv|gsa
ExecStartPre=-/usr/bin/qmicli -d qrtr://0 --loc-start
ExecStart=/usr/bin/qmicli -d qrtr://0 --loc-follow-nmea
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
systemctl enable --now gnss-nmea.service
```

The `ExecStartPre` pair relies on the LOC session surviving the starting client's exit — that is the
one behavioural assumption in this plan that was **not** verified (§4, risk 1). If it does not hold,
use ModemManager (Path B), which keeps one client for start+follow.

## 4. Unknowns and risks, each with its cheapest experiment

1. **Does a mobile-initiated LOC session survive the starting qmicli process exiting?** That is the
   only assumption behind the split Path A recipe (and the persistence unit). *Cheapest*:
   `qmicli -d qrtr://0 --loc-start`, then in a second shell
   `qmicli -d qrtr://0 --loc-follow-nmea`; if no `$GP` line within 90 s, re-read
   `qmicli -d qrtr://0 --loc-get-operation-mode` and switch to Path B (MM keeps one client for
   start + follow).
2. **Does the engine accept MI sessions at all given lock value 3?** *Cheapest*: the `--loc-start`
   response — `Successfully started location tracking` vs `could not start location tracking:
   engine locked`. If locked: `--loc-set-engine-lock=none` and retry once.
3. **Antenna / real sky view.** No AP-side antenna control exists, so this is the only hardware
   unknown. *Cheapest*: outdoors, `qmicli -d qrtr://0 --loc-get-gnss-sv-info` after a start; expect
   SVs with `CN0 > 20`. All-zero SV list after 2 min outdoors = RF path problem (would then need a
   vendor-kernel comparison, e.g. the Droidian boot on the same phone).
4. **The firmware's default NMEA mask is `0x00020000` (bit 17, unknown to libqmi 1.38).** It may be
   a vendor-only trace that eats engine bandwidth or an encoding libqmi misreads.
   *Cheapest*: after `--loc-set-nmea-types=gga`, run `--loc-get-nmea-types`; expect bit 0
   (`…01`) set, and confirm `--loc-follow-nmea` emits `$GPGGA`.
5. **Modem watchdog / SSR during a long session.** A GNSS session keeps the modem busy; if the PD
   watchdog fires (`dog_hal_common.c:180: DOG detects stalled initialization` class of failure) the
   session dies mid-fix. *Cheapest*: `dmesg | grep -iE 'DOG|SSR|subsys'` after a 5-minute session;
   `remoteproc1` state must stay `running`.
6. **ModemManager may refuse to enable the modem without a SIM** (gnss-share's README requires
   MM ≥ 1.23.2 exactly for the "modem without SIM" case). *Cheapest*: `mmcli -m 0` and
   `mmcli -m 0 --location-status`; if the modem is `disabled`/`failed`, `mmcli -m 0 --enable` and
   read `mmcli -m 0 --location-status` again.
7. **No desktop consumer for the fix.** `denial.portal` implements Settings/Access only,
   `gtk.portal` has no Location interface, and the Droidloom cell has no
   `android.hardware.gnss.IGnss` HAL. *Cheapest*: prove the fix with the Geoclue D-Bus client in
   Path B (geoclue itself is the consumer); adding a Location portal backend or a GNSS HAL is a
   separate work item.
8. **`--loc-get-engine-lock` / `--loc-get-nmea-types` TLV mismatch with this LOC firmware.**
   libqmi 1.38 may print "missing" or an empty list for values the firmware does send. *Cheapest*: the raw
   `AF_QIPCRTR` probe in §5 confirms the wire truth; never gate the fix on those getters.

## 5. Live observations (phone `<lan-ip>`)

Environment: `Linux picasso 7.3.0-rc7-00001-ge1927d6cbd57-dirty #199 SMP PREEMPT aarch64`,
`Arch Linux ARM`.

```
# qrtr-lookup | grep -E 'Location|Modem embedded|Service registry'
       15       1       0    0    17 Test service
       21       1       0    0    20 Modem embedded file system service
       16       2       0    0    85 Location service (~ PDS v2)
       64       1       1    1 16386 Service registry locator service
```

```
# pacman -Qq | grep -iE 'qmi|gps|geo|modem|qrtr'
qrtr
# command -v qmicli mmcli gpsd geoclue gnss-share
(all MISSING; python3 = /usr/bin/python3)
```

```
# remotes / host services
cat /sys/class/remoteproc/remoteproc*/state   -> running, running
cat /sys/class/remoteproc/remoteproc*/firmware-> qcom/sm7250/xiaomi/picasso/image/adsp.mdt
                                               qcom/sm7250/xiaomi/picasso/image/modem.mdt
ps -o pid,ppid,args -p 190,191                -> 190 1 rmtfs -v -o /run/rmtfs
                                                 191 1 pd-mapper
ls /dev/gnss* /sys/class/gnss                -> No such file or directory (both)
ls /sys/bus/qrtr                             -> No such file or directory  (mainline QRTR has no sysfs bus)
```

```
# dmesg | grep -iE 'gnss|gps|q6v5|remoteproc'      (relevant hits only)
[ 10.49] q6asm-dai open: comp 3000000.remoteproc:glink-edge:apr:service@7:dais …
[ 22.25] servicemanager: Caller(pid=426,…) Could not find
         android.hardware.gnss.IGnss/default in the VINTF manifest. No alternative instances declared.
```

```
# raw QMI getters over AF_QIPCRTR (node 0, port 85), no session started
opmode 020a004b00070002040000000000        # response: result ok
opmode 0401004b000e000104000000000010040004000000   # indication: status 0, op mode 4 = standalone
nmea   020b003f00070002040000000000
nmea   0402003f000e000104000000000010040000000200   # NMEA mask 0x00020000 (no GGA)
lock   020c003b00070002040000000000
lock   0403003b001200010400000000001208000300000000000000  # vendor TLV 0x12, u32 3
```

Probe used (read-only getters; run without creating a file on the phone, e.g.
`ssh root@<ip> 'python3 -' <<'EOF' … EOF`):

```python
import ctypes, struct, socket as sk
libc = ctypes.CDLL("libc.so.6", use_errno=True)
class SA(ctypes.Structure):
    _fields_ = [("family", ctypes.c_ushort), ("node", ctypes.c_uint32), ("port", ctypes.c_uint32)]
fd = libc.socket(42, 2, 0)                                  # AF_QIPCRTR, SOCK_DGRAM
libc.bind(fd, ctypes.byref(SA(42, 1, 0)), ctypes.sizeof(SA))  # node 1 = AP, kernel picks port
libc.connect(fd, ctypes.byref(SA(42, 0, 85)), ctypes.sizeof(SA))  # node 0 = modem, LOC port
s = sk.socket(42, 2, fileno=fd); s.settimeout(2)
s.send(struct.pack("<BHHH", 0, 1, 0x4B, 0))   # Get Operation Mode; 7-byte QMI header
print(s.recv(4096).hex()); print(s.recv(4096).hex())
```

```
# portal / desktop consumer state
cat /usr/share/xdg-desktop-portal/portals/denial.portal
  Interfaces=org.freedesktop.impl.portal.Settings;org.freedesktop.impl.portal.Access;   # no Location
cat /usr/share/xdg-desktop-portal/portals/gtk.portal
  Interfaces=…FileChooser;AppChooser;Print;Notification;Inhibit;Access;Account;Email;
             DynamicLauncher;Lockdown;Settings                                            # no Location
```

Package facts (read-only `pacman -Si` on the phone): `libqmi 1.38.0-2`, `modemmanager 1.24.2-2`
(`Depends On: … libqmi libqrtr-glib …`), `geoclue 2.8.2-2`, `gpsd 3.27.5-1`; nothing GNSS-related is
installed.

## 8. Update (2026-11-17, after libqmi was installed): the client lifetime matters

- `--loc-start` **succeeds** when the LOC client is kept across the command
  (`qmicli -d qrtr://0 --loc-noop --client-no-release-cid` prints `CID: '1'`, then every later
  invocation carries `--client-cid=1 --client-no-release-cid`). With a fresh client per invocation
  (the naive `--loc-start`) the modem answers `QMI protocol error (46): 'GeneralError'` - the earlier
  "engine refuses to start" reading was wrong, it is a per-client session state issue.
- Reusing that CID from a *different process* fails later with `Unknown client 1 for service loc`
  (the LOC service drops the client when the owning process exits), so set-nmea-types + start must be
  issued while the client is alive; qmicli refuses `--loc-start --loc-follow-nmea` in one call
  ("too many LOC actions requested").
- `--loc-follow-nmea` and `--loc-follow-position-report` on their own run cleanly (no error) but emit
  **nothing** for 130 s (`0` lines), and `--loc-get-position-report --loc-timeout=40` on the stale CID
  also fails with `Unknown client`. So the remaining gap is the engine producing traces/fixes, not the
  QMI plumbing: either the firmware only emits NMEA on a first fix (no sky view indoors) or the GNSS
  RF/engine needs an assist step (`--loc-inject-time` on the live client, XTRA data).
- Next experiment: allocate a client, inject time, set NMEA types, start, and *stay in the same
  process* while following (a small libqmi program, not qmicli), then repeat outdoors.
