# Wi-Fi hotspot (AP mode) on `ath10k_snoc` (WCN3990/3991)

Goal: `wlan0`'s phy beacons an AP (`picasso-ap`), hands out DHCP leases, and NATs clients
out to the internet, **without losing the STA uplink that carries the SSH session**.

Everything below is read from this repo, the downstream 4.19 tree proven on the phone
(`~/droidian/kernel/`), or the live phone (7.3.0-rc7, `<lan-ip>`). Line numbers are
exact for the tree state at the time of writing. Claims that are model knowledge rather
than observation are tagged `[INFERENCE]`.

---

## 1. Hardware ground truth

Downstream paths are relative to `$HOME` on the analysis host (`~/droidian/kernel/...`).

| Item | Downstream source | Value |
|---|---|---|
| WLAN controller base (SNOC membase) | `arch/arm64/boot/dts/vendor/qcom/lito.dtsi:3635` | `0x18800000`, size `0x800000` |
| WLAN IPA SMMU window (vendor only) | `lito.dtsi:3636` | `0xb0000000`, size `0x10000`, `reg-names = "membase", "smmu_iova_ipa"` (`:3637`) |
| SMMU stream | `lito.dtsi:3638` | `iommus = <&apps_smmu 0xC0 0x1>` (SID 0xc0, CB 1) |
| CE interrupts | `lito.dtsi:3639`–`:3650` | SPI 132 (CE0), 81 (CE1), 415 (CE2), 416 (CE3), 417 (CE4), 86 (CE5), 94 (CE6), 96 (CE7), 82 (CE8), 119 (CE9), 120 (CE10), 87 (CE11), all `IRQ_TYPE_LEVEL_HIGH`, GIC SPI |
| MSA / firmware memory | `lito.dtsi:3651` (`pil_wlan_fw_mem`) | reserved `no-map` window; mainline maps it as `memory-region = <&wlan_fw_mem>` = `0x8ba00000` +`0x200000` (`patches/linux/picasso/0002-arm64-dts-qcom-add-sm7250-xiaomi-picasso.patch:179-181`) |
| `qcom,msa-fixed-perm` | `lito.dtsi:3633`-node semantics; mainline `arch/arm64/boot/dts/qcom/sm7250.dtsi:2491` | MSA fixed permission (no per-boot MSA remap) |
| Regulator `vdd-cx-mx` (0.8 V core) | `lito.dtsi:3655` (`L1A`), config `:3660` `<0 0>` | mainline: `vreg_l1a_0p75` (`patch 0002:503`) |
| Regulator `vdd-1.8-xo` | `lito.dtsi:3656` (`L14A`) | mainline: `vreg_l14a_1p8` (`patch 0002:504`) |
| Regulator `vdd-1.3-rfa` | `lito.dtsi:3657` (`L2C`) | mainline: `vreg_l2c_1p3` (`patch 0002:505`) |
| Regulator `vdd-3.3-ch0` | `lito.dtsi:3659` (`L10C`), 3.0–3.312 V (`:3662`) | mainline: `vreg_l10c_3p3` (`patch 0002:506`) |
| Regulator `vdd-3.3-ch1` | `lito.dtsi:3658` (`L11C`), 3.0–3.312 V (`:3661`) | mainline: `vreg_l11c_3p3` (`patch 0002:507`) |
| Crash signalling | `lito.dtsi:3663-3667` | `smp2p_wlan_1_in` force-fatal-error / early-crash-ind |
| CLK / enable GPIOs | none in either tree | WCN3990 on this board has **no** `WL_EN` GPIO and no per-chip clock property; power is purely PMIC rails above (verified: mainline `sm7250.dtsi:2473-2493` node has no `clocks`/`*-gpios`) |
| Radio firmware (running image) | `/mnt/modem_fw/image/wlanmdsp.mbn` (3871128 B, modem partition) | downloaded by the **modem's WLAN PD** over `tqftpserv`; driver attaches "non-bmi", i.e. firmware already running (`tools/init.c:739-740`) |
| Firmware descriptor (driver side) | `tools/mkinitramfs.sh:58-61` | `/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin`, 60-byte TLV container (`QCA-ATH10K` magic, API 5, features `wowlan,mgmt-tx-by-reference,non-bmi`) — contains **no** FW payload |
| Board data (BDF) | `tools/init.c:771-773`, fallback `:774-776` | `board.bin` → `image/bdf_g7a.bin` (26328 B, md5 `ed0582a2…`); fallback `image/bdwlan.bin` (26328 B, md5 `3a62dd18…`) — the two files differ |
| WLAN MAC address | live `dmesg`: `ath10k_snoc 18800000.wifi: invalid MAC address; choosing random` | no provisioning path in either tree; `wlan0` currently `22:94:86:3b:e1:1c` (locally administered, re-randomised every boot) |
| AP peer capacity | live `dmesg`: `htt-ver 3.81 … max-sta 32` | 32 peers in AP mode |

### 1.1 Regulatory domain (this is firmware/BDF ground truth, not a userspace setting)

- `iw reg get` → global `country 00: DFS-UNSET`, **phy#1 `country 99: DFS-UNSET`** with
  rules `(2402-2472 @40)`, `(5140-5360 @80) PASSIVE-SCAN`, `(5715-5860 @80) PASSIVE-SCAN`.
- `country 99` is the **ath world regdomain** the BDF/OTP selects: `ath/regd.c:68,78,89,99,109`
  define `alpha2 = "99"` regdoms, applied with `wiphy_apply_custom_regulatory()`
  (`drivers/net/wireless/ath/regd.c:662`) and flagged `REGULATORY_CUSTOM_REG` (`:644`).
- Consequence: the phy is world-roaming (`net/wireless/reg.c:2126-2141`), so
  - userspace `iw reg set <XX>` / hostapd `country_code=` is **ignored** for this wiphy
    (`reg.c:2135-2140`: "driver uses its own custom regulatory domain"), and
  - cfg80211's *beacon hint* mechanism clears `NO_IR` on any channel where a beacon was
    received (`reg.c:2160-2180`, `handle_reg_beacon()`), which is exactly why the live
    `iw phy` shows **5765 MHz without `no IR`** while 5745/5785/5805/5825/5845 all carry it.
- DFS: `dmesg` line `kconfig debug 1 debugfs 0 tracing 0 dfs 0` (`ath10k/debug.c:60` prints
  `CONFIG_ATH10K_DFS_CERTIFIED`) proves the driver has **no CAC / radar detector**. The
  Kconfig symbol additionally depends on `CFG80211_CERTIFICATION_ONUS`
  (`ath10k/Kconfig:93-95`), which is not set. Therefore the 5260–5330 MHz channels
  (`no IR, radar detection`) **cannot** host an AP: `cfg80211_reg_can_beacon()` rejects a
  radar chandef when no `radar_detect_widths` are advertised (`net/wireless/chan.c:1739-1785`,
  called from `nl80211.c:12372` in `start_ap`). 5500–5720 MHz are `disabled` outright by the
  applied rules.

---

## 2. Current mainline state

### 2.1 Driver, config, DTS — present

| Check | Evidence |
|---|---|
| `ath10k` + `ath10k_snoc` built in | `configs/linux/picasso.config:103-105`; live `/proc/config.gz`: `CONFIG_ATH10K=y`, `CONFIG_ATH10K_SNOC=y` |
| cfg80211/mac80211 in | `picasso.config:97-98`; live `=y` |
| DTS node | `build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi:2473-2493` (`qcom,wcn3990-wifi`, 12 CE IRQs, `qcom,msa-fixed-perm`) |
| Board supplies + firmware name | `patches/linux/picasso/0002-…picasso.patch:502-509` (`&wifi { … firmware-name = "picasso"; }`) |
| Modem PD that hosts the WLAN firmware | `&mpss { firmware-name = "…/modem.mdt"; }` (`patch 0002:498-500`), WLAN fw mem `modem_wlan_mem: 0x8c000000+0xf800000` (`patch 0002:214-216`) |
| Probe result (live) | `ath10k_snoc 18800000.wifi: wcn3990 hw1.0 target 0x00000008 …`, `htt-ver 3.81`, `hwcrypto 1`, `wlan0` UP, DHCP lease `<lan-ip>/22` |
| `wlan0` link | ch 153 (5765 MHz), 40 MHz, centre1 5755 MHz, txpower 30 dBm, SSID `<ssid>` |
| Harmless warning | `ath10k_snoc … chan info: invalid frequency 0 (idx 41 out of bounds)` (`ath10k/wmi.c:2738`) — one bogus entry in the firmware's channel list; the band tables are otherwise complete |

### 2.2 Interface capabilities (live `iw phy` / `iw list`)

- Modes: `IBSS, managed, AP, monitor, P2P-client, P2P-GO, P2P-device`. **No `AP/VLAN`, no
  mesh, no 4-address** → per-station VLANs and WDS/bridged-AP-4addr are off the table; routed
  NAT is the only sane topology.
- `start_ap`, `new_station`, `set_bss` are in `Supported commands`;
  `Driver supports full state transitions for AP/GO clients`, `Driver supports a userspace MPM`,
  `Maximum associated stations in AP mode: 32`.
- `valid interface combinations`:
  1. `#{managed}<=2, #{AP,P2P-client,P2P-GO}<=2, #{P2P-device}<=1, total<=4, #channels<=1`
  2. `#{managed}<=2, #{P2P-client}<=2, #{AP,P2P-GO}<=1, #{P2P-device}<=1, total<=4, #channels<=2`
  3. `#{managed}<=1, #{IBSS}<=1, total<=2, #channels<=1`

  These are `ath10k_tlv_qcs_if_comb`, selected because the firmware advertises
  `WMI_SERVICE_ADAPTIVE_OCS` (`ath10k/mac.c:10237-10248`, combos at `:9766-9778`, limits at
  `:9712-9736`). The 2-channel variant is the one that permits AP+STA on **different**
  channels; upstream itself marks these combos provisional: *"FIXME: This is not thoroughly
  tested. These combinations may over- or underestimate hw/fw capabilities."*
  (`mac.c:9748-9749`). `ath10k` does implement chanctx ops (`mac.c:9508-9512`), so MCC is not
  structurally impossible — it is just unproven on this firmware.
- **Conservative choice: same channel as the STA uplink (153), 20 MHz.** No MCC, combo #1.

### 2.3 Netfilter / forwarding / NAT — sufficient for routed NAT, with two gaps

| Item | `picasso.config` | running kernel (`/proc/config.gz`) |
|---|---|---|
| `CONFIG_NF_CONNTRACK` / `NF_NAT` | `:255` / `:256` | `=y` |
| `CONFIG_NF_NAT_MASQUERADE` | `:257` | `=y` |
| `CONFIG_NF_TABLES`, `_INET`, `_IPV4` | `:258-260` | `=y` |
| `CONFIG_NF_TABLES_IPV6` | not pinned (implied by `NF_TABLES_INET`) | `=y` |
| `CONFIG_NFT_NAT`, `NFT_MASQ`, `NFT_CHAIN_NAT` | `:262-264` | `=y` |
| `CONFIG_NETFILTER_NETLINK` | `:254` | `=y` |
| `CONFIG_IP_NF_*` + `IP_NF_TARGET_MASQUERADE` | `:291-297` | `=y` |
| `CONFIG_IPV6` + `IP6_NF_*` | `:281`, `:298-303` | `=y` |
| `CONFIG_NET_SCHED` | not pinned | `=y` |
| `CONFIG_BRIDGE` | `:266` | `=y` |
| **`CONFIG_BRIDGE_NETFILTER`** | **absent** | **`=m`** ← module, no `.ko` exists in the RAM initramfs (`/lib/modules` is empty) → **unusable** |
| `CONFIG_NF_TABLES_NETDEV` | absent | not set → no `nft … dev` (netdev family) |

Live state: `net.ipv4.ip_forward = 1` (set at runtime by something in the Denial/Droidloom
stack; **no** `/etc/sysctl.d` or `/usr/lib/sysctl.d` entry was found, so it is not guaranteed
across boots), `net.ipv6.conf.all.forwarding = 0`, `rp_filter` `all=0`, `wlan0=2` (loose).
Existing ruleset is only Droidloom's and matches `dlh1000` exclusively, `policy accept`:

```
table inet droidloom_u1000 { chain input/forward … iifname "dlh1000" … }
table ip droidloom_u1000_nat { chain postrouting { … ip saddr 10.177.0.2 masquerade } }
```

`iptables -t nat -S` is empty. So `nft` has room and nothing collides with a new
`table ip hotspot`.

### 2.4 Userland inventory on the phone

| Tool | State | Consequence |
|---|---|---|
| `wpa_supplicant` 2:2.12-2 | present, AP support compiled in (strings: `AP mode active - skip EAPOL…`, `Invalid pbss value(%d) for AP mode`) | AP backend available without hostapd |
| `NetworkManager` 1.58.1-3 | active; `WIFI-PROPERTIES.AP: yes` | AP via wpa_supplicant; **no `hostapd`, no `nm-ap-manager`** (binary contains zero `hostapd` strings; `/usr/lib/NetworkManager/` has no AP helper) |
| `hostapd` | **missing** (`extra/hostapd 2.12-3` is in the synced repos) | must be installed for the canonical path |
| `dnsmasq` | **missing** (`extra/dnsmasq 2.93-1` in repos; it is an *optional* dep of networkmanager, "connection sharing") | `ipv4.method shared` **cannot work**: NM's sharing path spawns `/usr/bin/dnsmasq` (`strings`: `%s…: ip:shared…: could not start dnsmasq`) |
| `dhcpcd`, `udhcpd`, `busybox`, `dhcpd`, `kea-dhcp4`, `odhcpd` | all missing | no DHCP server at all except NM's dnsmasq path or `systemd-networkd` |
| `systemd-networkd` | active + enabled; no `/etc/systemd/network/` files; **already runs a DHCP server on `usb0`** (`/usr/lib/systemd/network/10-phone-usb.network` `DHCPServer=yes`, live listener `0.0.0.0%usb0:67`) | `DHCPServer=yes` for `ap0` is proven infrastructure on this image |
| `systemd-resolved` 262 | active; stub on `127.0.0.53` + `127.0.0.54` only; uplink DNS `10.14.2.2`; `DNSStubListenerExtra=` supported | dnsmasq can forward to `127.0.0.53`; `127.0.0.1:53` is free, and resolved can be made to answer on `192.168.50.1` |
| `nftables` 1:1.1.7, `iptables`, `iw` 6.17, `nmcli` 1.58 | present | tooling for the plan |
| `/var/lib/misc` | exists, `root:root 0755` | dnsmasq lease file location |
| Root filesystem | `/dev/sda36 ext4 rw` (5.3 G free), PID 1 = `systemd` | packages/units can be persisted |

### 2.5 Why **not** `nmcli device wifi hotspot` on `wlan0`

Two independent blockers:

1. NM is one-connection-per-device: activating an AP profile on `wlan0` tears down
   `<ssid>`, i.e. the uplink **and the SSH transport** (`<lan-ip>` lives on `wlan0`;
   `10.77.71.2`/`usb0` refuses port 22, so there is no second path in).
2. Even if both were acceptable, `ipv4.method shared` needs `dnsmasq`, which is absent.

Therefore the plan adds a **second vif (`ap0`) on the same phy** and leaves `wlan0` alone.

---

## 3. Change plan

### 3.0 Preconditions (already verified; re-check after any reboot)

```sh
iw dev wlan0 info | grep -E 'ssid|channel'    # expect: ssid <ssid>, channel 153 (5765 MHz)
iw phy | grep -E '^\s+\* (AP|IBSS|managed)$'  # expect AP present
cat /proc/sys/net/ipv4/ip_forward             # expect 1
nft list tables                               # expect only droidloom_* tables
command -v hostapd dnsmasq                    # expect both (after 3.1 step 0)
```

### 3.1 Path A — hostapd + dnsmasq + nft (recommended)

Keeps `wlan0` associated, so SSH and the NAT uplink survive. Run in this order.

```sh
# 0. one-time packages (extra repo; phone has working DNS: archlinux.org resolves)
pacman -S --needed --noconfirm hostapd dnsmasq

# 1. AP vif on the same phy, then hand it away from NetworkManager
iw dev ap0 del 2>/dev/null || true
iw dev wlan0 interface add ap0 type __ap
nmcli device set ap0 managed no
#   persistent equivalent for step 1's last command:
printf '[keyfile]\nunmanaged-devices=interface-name:ap0\n' \
	>/etc/NetworkManager/conf.d/99-ap0-unmanaged.conf

# 2. address, link stays down until hostapd enables it
ip link set ap0 down
ip addr replace 192.168.50.1/24 dev ap0

# 3. hostapd config — 20 MHz on the STA's channel: 40 MHz would need ch149,
#    which is NO_IR (see 1.1) and is rejected by cfg80211's beacon check.
cat >/etc/hostapd/hotspot.conf <<'EOF'
interface=ap0
driver=nl80211
ctrl_interface=/run/hostapd
ssid=picasso-ap
hw_mode=a
channel=153
ieee80211n=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=<password>
auth_algs=1
ignore_broadcast_ssid=0
EOF

hostapd -B -P /run/hostapd-ap0.pid -f /var/log/hostapd-ap0.log /etc/hostapd/hotspot.conf

# 4. DHCP + DNS on ap0 only (resolved keeps 127.0.0.53:53; no port clash)
cat >/etc/dnsmasq.d/ap0.conf <<'EOF'
interface=ap0
except-interface=lo
bind-dynamic
dhcp-range=192.168.50.10,192.168.50.50,255.255.255.0,12h
dhcp-option=option:router,192.168.50.1
dhcp-option=option:dns-server,192.168.50.1
dhcp-authoritative
domain-needed
bogus-priv
EOF
printf 'conf-dir=/etc/dnsmasq.d,*.conf\n' >>/etc/dnsmasq.conf
systemctl enable --now dnsmasq

# 5. forwarding + NAT out of the STA uplink
printf 'net.ipv4.ip_forward=1\n' >/etc/sysctl.d/90-hotspot.conf
sysctl -q -w net.ipv4.ip_forward=1
nft delete table ip hotspot 2>/dev/null || true
nft -f - <<'EOF'
table ip hotspot {
	chain forward {
		type filter hook forward priority filter; policy accept;
		iifname "ap0" oifname "wlan0" counter accept
		iifname "wlan0" oifname "ap0" ct state established,related counter accept
	}
	chain postrouting {
		type nat hook postrouting priority srcnat; policy accept;
		ip saddr 192.168.50.0/24 oifname "wlan0" counter masquerade
	}
}
EOF
```

Notes that matter:
- The guarded `nft delete table ip hotspot` keeps the block idempotent. Do **not** put
  `delete table` inside the `nft -f` heredoc: `nft -f` stops at the first failing command, so
  on the first run the table definition would never be applied.
- The forward chain is added at `priority filter` next to Droidloom's `inet droidloom_u1000`
  chain; both are `policy accept` and Droidloom's rules only match `dlh1000`, so `ap0`
  traffic is unaffected.
- `masquerade` on `wlan0` is the same uplink the phone itself uses
  (`default via <lan-ip> dev wlan0`); no change to the default route is needed.
- If the uplink later roams to 2.4 GHz, the AP **must** move with it (see 4.2).

### 3.2 Path A2 — DHCP without installing dnsmasq

Same as 3.1 minus steps 0/4, replaced by `systemd-networkd`'s built-in DHCP server.
This is not speculative: the Denial image already runs networkd's DHCP server on `usb0`
(`/usr/lib/systemd/network/10-phone-usb.network` → `DHCPServer=yes`; live listener
`0.0.0.0%usb0:67`), and no `/etc/systemd/network/` file exists yet, so `ap0` is free.

```sh
cat >/etc/systemd/network/50-ap0.network <<'EOF'
[Match]
Name=ap0
[Network]
Address=192.168.50.1/24
DHCPServer=yes
ConfigureWithoutCarrier=yes
[DHCPServer]
PoolOffset=10
PoolSize=40
EmitRouter=yes
EmitDNS=yes
DNS=192.168.50.1
EOF

# clients must be able to resolve through the AP address: let resolved answer there too
# (systemd 262 supports this; the stub keeps 127.0.0.53/127.0.0.54)
cat >/etc/systemd/resolved.conf.d/ap0.conf <<'EOF'
[Resolve]
DNSStubListenerExtra=192.168.50.1
EOF
systemctl restart systemd-resolved

systemctl reload-or-restart systemd-networkd
networkctl status ap0        # expect: Address 192.168.50.1/24, "DHCP Server" active
```

Zero-config variant if restarting `systemd-resolved` is unwanted: set `DNS=10.14.2.2`
(the uplink's resolver) and skip the drop-in — clients then bypass the phone for DNS.
There is no lease file on this path; use `networkctl status ap0`,
`journalctl -u systemd-networkd -b | grep -i dhcp` and the nft counters as evidence.

### 3.3 Path B — wpa_supplicant AP mode (zero packages)

Use only if installing packages is vetoed. NM already runs one supplicant with `-u`
(D-Bus); a second instance **without** `-u` and with its own control socket coexists.

```sh
iw dev ap0 del 2>/dev/null || true
iw dev wlan0 interface add ap0 type __ap
nmcli device set ap0 managed no
ip link set ap0 down
ip addr replace 192.168.50.1/24 dev ap0
cat >/etc/wpa_supplicant/ap0.conf <<'EOF'
ctrl_interface=/run/wpa_supplicant-ap0
network={
	ssid="picasso-ap"
	mode=2
	frequency=5765
	key_mgmt=WPA-PSK
	proto=RSN
	pairwise=CCMP
	group=CCMP
	psk="<password>"
}
EOF
wpa_supplicant -B -i ap0 -D nl80211 -c /etc/wpa_supplicant/ap0.conf \
	-P /run/wpa_supplicant-ap0.pid
```
Then 3.2 (networkd DHCP) + 3.1 step 5 (nft). Control/evidence via
`wpa_cli -p /run/wpa_supplicant-ap0 -i ap0 status`.

### 3.4 Path C — NetworkManager one-shot (documented, **not** recommended)

```sh
nmcli device wifi hotspot ifname wlan0 con-name picasso-hotspot \
      ssid picasso-ap password <password>
```
It requires (a) `dnsmasq` installed, (b) giving up the `wlan0` STA connection. With the
current build the first failure is
`ip:shared: could not start dnsmasq: No such file or directory`; and if the uplink needs to
be `wlan0`, the hotspot is pointless anyway. It is only viable if the NAT uplink is `usb0`
and a second SSH path is first arranged.

### 3.5 (a) DTS — **no change required**

The `&wifi` node is already complete and correct for AP mode (supplies + firmware name);
this is the exact text in `patch 0002:502-509`, i.e. already in
`build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`:

```dts
&wifi {
	vdd-0.8-cx-mx-supply = <&vreg_l1a_0p75>;
	vdd-1.8-xo-supply = <&vreg_l14a_1p8>;
	vdd-1.3-rfa-supply = <&vreg_l2c_1p3>;
	vdd-3.3-ch0-supply = <&vreg_l10c_3p3>;
	vdd-3.3-ch1-supply = <&vreg_l11c_3p3>;
	firmware-name = "picasso";
	status = "okay";
};
```

Do **not** add `local-mac-address` to get a stable BSSID: there is no provisioned address in
either tree (the vendor stack feeds the MAC from the persist partition through ICNSS), and a
hard-coded one would be wrong for this unit. Random MAC per boot is acceptable for a hotspot
test; document it for clients.

### 3.6 (b) `configs/linux/picasso.config`

Nothing is missing for routed NAT — `NF_TABLES_INET`, `NFT_MASQ`, `NFT_NAT`,
`NF_NAT_MASQUERADE`, `IP_NF_TARGET_MASQUERADE` are all `=y` and present in the running
kernel. Two pins are worth adding so the fragment stops depending on defaults and on a
module that cannot be loaded at runtime:

```
# bridged-AP support must be built in (there is no module loader in the RAM boot);
# today it is =m via a Kconfig default and therefore dead at runtime
CONFIG_BRIDGE_NETFILTER=y
# pin what NF_TABLES_INET currently selects implicitly
CONFIG_NF_TABLES_IPV6=y
CONFIG_NET_SCHED=y
```

`CONFIG_ATH10K_DFS_CERTIFIED` must stay **unset** (`ath10k/Kconfig:93-95` needs
`CFG80211_CERTIFICATION_ONUS`); enabling it would let the radio initiate on radar channels
without a valid certification story.

### 3.7 (c) Kernel patch

**None.** No code change is needed for AP mode, DHCP or NAT; the capability, the regulatory
relaxation and the netfilter stack are all in place. The only kernel-side defect observed is
cosmetic (`chan info: invalid frequency 0 (idx 41 out of bounds)`, `ath10k/wmi.c:2738`) and
does not gate AP mode. If MAC provisioning is wanted later, it is a separate item
(`patches/linux/picasso/0016-wifi-ath10k-read-mac-from-persist.patch`, out of scope here).

### 3.8 (d) Verification — exact commands and expected output

From the phone:

```sh
iw dev ap0 info
#   Interface ap0 / type AP / channel 153 (5765 MHz), width: 20 MHz

hostapd_cli -i ap0 status        # or: tail -3 /var/log/hostapd-ap0.log
#   state=ENABLED
#   ssid[0]=picasso-ap
#   channel=153

ip -br addr show ap0
#   ap0   UP   192.168.50.1/24

ip route; cat /proc/sys/net/ipv4/ip_forward
#   default via <lan-ip> dev wlan0 ...   (unchanged)
#   1
```

After the client joins:

```sh
iw dev ap0 station dump
#   Station 5c:df:... (on ap0)
#        inactive time:  120 ms
#        signal:         -41 [-41, -42] dBm
#        tx bitrate:     433.3 MBit/s VHT-MCS 9 80MHz ...
#        rx bytes:       nnnnn

cat /var/lib/misc/dnsmasq.leases
#   1763...  5c:df:...  192.168.50.10  <client-hostname>  01:5c:df:...

nft list chain ip hotspot postrouting
#   ip saddr 192.168.50.0/24 oifname "wlan0" counter packets 143 bytes 18732 masquerade
```

On the client:

```sh
nmcli device wifi list | grep picasso-ap              # AP visible
nmcli device wifi connect picasso-ap password <password>
ip -4 addr show dev <wlan>; ip route
#   inet 192.168.50.10/24 ...
#   default via 192.168.50.1 dev <wlan>
ping -c3 192.168.50.1      # 3 packets transmitted, 3 received
ping -c3 1.1.1.1           # NAT path
getent hosts example.com   # DNS through 192.168.50.1
curl -s -o /dev/null -w '%{http_code}\n' https://archlinux.org   # 200/301
```

Success = all four of: `type AP` on `ap0` + `AP-ENABLED`/`state=ENABLED`, a lease line,
non-zero `masquerade` counters, and client `ping 1.1.1.1` + name resolution.

### 3.9 Persistence in Denial **without** touching `tools/init.c`

`/` is ext4 read-write and systemd is PID 1, so ordinary units survive. Nothing in the RAM
boot path needs editing: the vif, hostapd and dnsmasq are all created after userspace is up.

`/usr/local/bin/picasso-hotspot-vif` (the AP channel must follow the uplink, which roams):

```sh
#!/bin/sh
set -eu
CHAN=$(iw dev wlan0 info | sed -n 's/.*channel \([0-9]\+\).*/\1/p')
[ -n "$CHAN" ] || exit 1
[ -e /sys/class/net/ap0 ] || iw dev wlan0 interface add ap0 type __ap
nmcli device set ap0 managed no || true
ip link set ap0 down
if [ "$CHAN" -le 14 ]; then HW=g; else HW=a; fi
sed -e "s/^hw_mode=.*/hw_mode=$HW/" -e "s/^channel=.*/channel=$CHAN/" \
	/etc/hostapd/hotspot.conf.tmpl >/etc/hostapd/hotspot.conf
ip addr replace 192.168.50.1/24 dev ap0
nft delete table ip hotspot 2>/dev/null || true
nft -f /etc/nftables/hotspot.nft
```

`/etc/nftables/hotspot.nft` is the `table ip hotspot { … }` block from 3.1 step 5, verbatim
(no `delete` inside it). `/etc/hostapd/hotspot.conf.tmpl` is the 3.1 step 3 config, verbatim.

Units:

```ini
# /etc/systemd/system/picasso-hotspot-vif.service
[Unit]
Description=picasso AP vif + NAT rules
After=NetworkManager-wait-online.service
Wants=NetworkManager-wait-online.service
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/picasso-hotspot-vif
[Install]
WantedBy=multi-user.target
```

```ini
# /etc/systemd/system/picasso-hotspot-ap.service
[Unit]
Description=picasso hostapd (ap0)
After=picasso-hotspot-vif.service
Requires=picasso-hotspot-vif.service
[Service]
ExecStart=/usr/bin/hostapd /etc/hostapd/hotspot.conf
Restart=always
RestartSec=3
[Install]
WantedBy=multi-user.target
```

```sh
systemctl enable picasso-hotspot-vif.service picasso-hotspot-ap.service dnsmasq.service
```

Ordering caveat: if the uplink is not yet associated when the unit runs, the unit exits 1 and
the AP is not started; `Restart=always` on the AP unit plus a later `systemctl start
picasso-hotspot-vif` (or a `NetworkManager dispatcher.d` script keyed on `up`) fixes that.
The dispatcher script is the cleaner long-term answer and still requires no `init.c` change.

---

## 4. Unknowns and risks

| # | Risk | Evidence / reasoning | Cheapest experiment |
|---|---|---|---|
| 4.1 | Firmware refuses a second (AP) vdev while STA is associated | `iw phy` advertises `vdev create`/`start_ap` and `max-sta 32` from the running firmware, but that is the driver's combos + firmware peer limit, not a proof of concurrent AP+STA | Start hostapd per 3.1 step 3 and read `dmesg | tail -20`: `ath10k_warn … failed to start vdev` / `could not create vdev` = unsupported; `AP-ENABLED` = supported |
| 4.2 | AP must sit on the STA's channel; roaming breaks it | Combo #1 is `#channels<=1`; the 2-channel combo is labelled "not thoroughly tested" (`mac.c:9748-9749`) | Set `channel=6` in `/etc/hostapd/hotspot.conf` (2.4 GHz is IR from the regd rules) and start it while the STA stays on 5765 → expect hostapd `Could not set channel for kernel driver`; `dmesg` then shows the ath10k chanctx attempt |
| 4.3 | ch153 is IR-legal only because of the beacon hint (runtime state, not a fixed property) | `iw phy` shows 5765 without `no IR` while every neighbouring 5 GHz channel has it; `reg.c:2160-2180` clears `NO_IR` on a channel where a beacon was heard, and only for world-roaming wiphys | Run `hostapd -d` (foreground) on ch153 right after a fresh association: it prints the cfg80211 channel flags it gets. If it rejects the channel, re-associate `wlan0` (re-arms the beacon hint) or move the AP to ch1–11 |
| 4.4 | 40 MHz / VHT80 AP rejected | 40 MHz on 153 bonds ch149, which is `NO_IR`; beacon check requires IR on the whole chandef (`chan.c:1739-1785`) | `sed -i 's/^ieee80211n=1/ieee80211n=1\nht_capab=[HT40+]/' /etc/hostapd/hotspot.conf` and restart → expect `Failed to set beacon parameters` / `channel not allowed` |
| 4.5 | DFS channels never work | `dfs 0` in dmesg; `CONFIG_ATH10K_DFS_CERTIFIED` unset and gated on `CFG80211_CERTIFICATION_ONUS` (`ath10k/Kconfig:93-95`) | `hostapd` with `channel=52` → `Channel 52 (5260 MHz) is disabled` / `radar detection required` |
| 4.6 | BDF variant: `bdf_g7a.bin` vs `bdwlan.bin` differ (different md5) | `tools/init.c:771-776` picks `bdf_g7a.bin` first; wrong BDF would show up as missing channels/txpower, not as AP failure | Swap the `board.bin` symlink to `bdwlan.bin`, `rebind` ath10k, diff `iw phy` channel flags |
| 4.7 | Random WLAN MAC every boot ⇒ new BSSID, clients re-scan/cache | `dmesg`: `invalid MAC address; choosing random` | None cheap; treat as accepted for bring-up. Future: read MAC from persist (separate patch) |
| 4.8 | `ip_forward` not persistent | `=1` live, but no sysctl drop-in found on the phone | `sysctl -w` already done by 3.1 step 5; verify `/etc/sysctl.d/90-hotspot.conf` exists after reboot |
| 4.9 | Bridged AP impossible today | `CONFIG_BRIDGE_NETFILTER=m`, `/lib/modules` empty (`modprobe` cannot work) | `modprobe br_netfilter` → `Module not found`; do not design a bridge |
| 4.10 | NM may claim `ap0` on reboot | `nmcli device set … managed no` is runtime-only | Reboot and `nmcli device status | grep ap0` → `unmanaged` proves `conf.d/99-ap0-unmanaged.conf` works |
| 4.11 | hostapd `ieee80211d=1`/`country_code` folklore | Kernel-side it is a no-op here: the wiphy has a custom regd, so `REQ_SET_REG` is ignored (`reg.c:2135-2140`) and any `country_code` produces at most a "Could not set country code" log line `[INFERENCE]` | Add `ieee80211d=1`+`country_code=US` and check the log/`iw reg get`; if nothing changes, drop them |
| 4.12 | Installing packages mutates the Denial rootfs | `pacman -S hostapd dnsmasq` writes to `/dev/sda36` (not a UFS partition flash, but a persistent change) | Coordinator decision; Path B + 3.2 is the zero-install alternative |

---

## 5. Live observations (commands + excerpts)

All via `ssh -i ~/.ssh/picasso_key -o BatchMode=yes root@<lan-ip>`; read-only.

`iw reg get`
```
global
country 00: DFS-UNSET
	(2402 - 2472 @ 40), (6, 20), (N/A)
	(5170 - 5250 @ 80), (6, 20), (N/A), AUTO-BW, PASSIVE-SCAN
	(5250 - 5330 @ 80), (6, 20), (0 ms), DFS, AUTO-BW, PASSIVE-SCAN
	(5490 - 5730 @ 160), (6, 20), (0 ms), DFS, PASSIVE-SCAN
	(5735 - 5835 @ 80), (6, 20), (N/A), PASSIVE-SCAN
	(57240 - 63720 @ 2160), (N/A, 0), (N/A)
phy#1
country 99: DFS-UNSET
	(2402 - 2472 @ 40), (N/A, 20), (N/A)
	(5140 - 5360 @ 80), (N/A, 30), (N/A), PASSIVE-SCAN
	(5715 - 5860 @ 80), (N/A, 30), (N/A), PASSIVE-SCAN
```

`iw phy` (excerpts)
```
	Supported interface modes:
		 * IBSS
		 * managed
		 * AP
		 * monitor
		 * P2P-client
		 * P2P-GO
		 * P2P-device
...
			* 5240.0 MHz [48] (30.0 dBm) (no IR)
			* 5260.0 MHz [52] (30.0 dBm) (no IR, radar detection)
...
			* 5500.0 MHz [100] (disabled)
...
			* 5745.0 MHz [149] (30.0 dBm) (no IR)
			* 5765.0 MHz [153] (30.0 dBm)
			* 5785.0 MHz [157] (30.0 dBm) (no IR)
...
	valid interface combinations:
		 * #{ managed } <= 2, #{ AP, P2P-client, P2P-GO } <= 2, #{ P2P-device } <= 1,
		   total <= 4, #channels <= 1
		 * #{ managed } <= 2, #{ P2P-client } <= 2, #{ AP, P2P-GO } <= 1, #{ P2P-device } <= 1,
		   total <= 4, #channels <= 2
		 * #{ managed } <= 1, #{ IBSS } <= 1,
		   total <= 2, #channels <= 1
...
	Driver supports full state transitions for AP/GO clients
	Driver supports a userspace MPM
	Maximum associated stations in AP mode: 32
```

`iw dev wlan0 info`
```
	ssid <ssid>
	type managed
	channel 153 (5765 MHz), width: 40 MHz, center1: 5755 MHz
	txpower 30.00 dBm
```

`dmesg | grep ath10k`
```
[   12.342102] ath10k_snoc 18800000.wifi: wcn3990 hw1.0 target 0x00000008 chip_id 0x00000000 sub 0000:0000
[   12.342124] ath10k_snoc 18800000.wifi: kconfig debug 1 debugfs 0 tracing 0 dfs 0 testmode 0
[   12.342133] ath10k_snoc 18800000.wifi: firmware ver  api 5 features wowlan,mgmt-tx-by-reference,non-bmi crc32 b3d4b790
[   12.380428] ath10k_snoc 18800000.wifi: htt-ver 3.81 wmi-op 4 htt-op 3 cal file max-sta 32 raw 0 hwcrypto 1
[   12.470313] ath10k_snoc 18800000.wifi: invalid MAC address; choosing random
[   13.926417] ath10k_snoc 18800000.wifi: chan info: invalid frequency 0 (idx 41 out of bounds)
```

`nmcli -f WIFI-PROPERTIES dev show wlan0` (relevant lines)
```
WIFI-PROPERTIES.WPA2:                    yes
WIFI-PROPERTIES.AP:                      yes
WIFI-PROPERTIES.2GHZ:                    yes
WIFI-PROPERTIES.5GHZ:                    yes
WIFI-PROPERTIES.MESH:                    no
```

Tool inventory
```
hostapd          MISSING
dnsmasq          MISSING
nft              /usr/bin/nft
iptables         /usr/bin/iptables
wpa_supplicant   /usr/bin/wpa_supplicant
iw               /usr/bin/iw
nmcli            /usr/bin/nmcli            (1.58.1-3)
dhcpcd/udhcpd/busybox/kea-dhcp4/odhcpd   MISSING
```

`zcat /proc/config.gz | grep -E 'NF_NAT_MASQUERADE|BRIDGE_NETFILTER|NFT_MASQ'`
```
CONFIG_BRIDGE_NETFILTER=m
CONFIG_NF_NAT_MASQUERADE=y
CONFIG_NFT_MASQ=y
```

`nft list ruleset` (excerpt) — only Droidloom tables exist; `iptables -t nat -S` is empty.
`/proc/sys/net/ipv4/ip_forward` = `1`, `net.ipv6.conf.all.forwarding` = `0`,
`rp_filter` `all=0` / `wlan0=2`.

Firmware artifacts
```
/lib/firmware/ath10k/WCN3990/hw1.0/board.bin -> …/image/bdf_g7a.bin   (26328 B)
/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin                     (60 B, TLV container)
/mnt/modem_fw/image/wlanmdsp.mbn                                      (3871128 B)
md5: bdwlan.bin 3a62dd18c9bbda2410b295de36d21233
     bdf_g7a.bin ed0582a29bd514908bb196a3d3c5e3d2
```
