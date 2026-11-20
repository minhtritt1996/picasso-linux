# Cellular data / telephony bringup — QMI over QRTR + IPA/rmnet (SM7250 picasso)

Scope: get from "modem remoteproc running" to "SIM read, network registered, PDP data path up".
Baseline (already verified, not repeated here): `remoteproc0/1` = adsp/modem `running`;
`qrtr-lookup` lists 40 QMI services incl. UIM(11), NAS(3), DMS(2), WDS(1), IPA(49), DPM(47);
`wlan0` up via WCN3990. No `wwan0`/`rmnet*` netdev exists.

Everything below is cited to a real source line. Downstream = `~/droidian/kernel` (vendor 4.19 tree
proven on this phone), mainline = `build/linux` in this repo.

---

## 0. Verdict — kernel vs userland

| Layer | State | Missing pieces |
|---|---|---|
| QMI/QRTR transport | **works** — QRTR over GLINK registered, 40 services visible | nothing |
| QMI userspace | **absent** — no `libqmi`, no `qmicli`, no ModemManager, NM has no modem backend running | install `modemmanager` + `libqmi` (both in Denial `extra`), enable `ModemManager.service` |
| IPA driver | **present in tree, not in config/DT, not loadable** — `CONFIG_QCOM_IPA=m` from arm64 defconfig but `/lib/modules` is empty; no DT node | config `=y`, DTS node, IPA SMP2P entries, (probably) zero driver code |
| rmnet | **present in tree, `=m`, not loadable** (`CONFIG_RMNET=m` from defconfig) | config `=y` |
| IPA firmware (`ipa_fws.mdt`) | **does not exist anywhere on the device** | use modem-loaded GSI (`qcom,gsi-loader = "modem"`), fallback `"skip"` |
| bam-dmux | present, not the sm7250 path | nothing (do not use) |

No new kernel driver is required. This is a DT + Kconfig + distro-package task, with one genuine
hardware uncertainty (which side loads IPA/GSI firmware, §4 R2).

---

## 1. Hardware ground truth

| Item | Downstream source (`file:line`) | Value |
|---|---|---|
| IPA core version | `arch/arm64/boot/dts/vendor/qcom/lito.dtsi:3534` | `qcom,ipa-hw-ver = <18>` = IPAv4.7 (enum `include/uapi/linux/msm_ipa.h:783-784`, `IPA_HW_v4_7 = 18`) |
| IPA register window | `lito.dtsi:3528` (`<0x1e00000 0x84000>`) + `drivers/platform/msm/ipa/ipa_v3/ipahal/ipahal_reg.c:3641-3644` (`ipahal_get_reg_base()` returns `0x00040000`) | IPA regs at **0x1e40000**, 0x8000 long (same layout as upstream sm6350) |
| GSI window | `lito.dtsi:3529` | **0x1e04000**, size **0x23000** |
| IPA local SRAM ("ipa-shared") | `ipahal_reg.c:3284-3286` (`IPA_SW_AREA_RAM_DIRECT_ACCESS_n` offset `0x10000`, inherited by v4.7 via `ipahal_reg.c:3391-3397`) + `ipa.c:3843-3848` (`base + reg_base_ofst + reg_ofst + restricted_bytes`) + `ipa_utils.c:3691-3692` (BADDR read from HW, ×8) | **0x1e50000**, **0x3000** — HW BADDR is 0; footprint of the mainline v4.7 region table is exactly 0x3000 (`drivers/net/ipa/data/ipa_data-v4.7.c`, max offset+size = 0x3000) |
| IPA IRQ | `lito.dtsi:3531-3532` | GIC SPI **311** (level high), GIC SPI **432** (level high) |
| IPA SMMU streams | `lito.dtsi:3610` (AP ctx), `:3620` (WLAN ctx), `:3628` (uC ctx) | AP **0x520**, WLAN 0x521, uC **0x522**; AP `iommu-dma-addr-pool = <0x20000000 0x40000000>` |
| IPA modem IMEM tables | `lito.dtsi:3613` (`additional-mapping = <0x146a9000 0x146a9000 0x2000>`), `:3615` (`ipa-q6-smem-size = <26624>`) | IMEM base **0x146a8000**, size **0x2000** (mainline default: `ipa_data-v4.7.c:361-362`) |
| IPA SMEM item | `drivers/net/ipa/ipa_mem.c:30` (`SMEM_IPA_FILTER_TABLE 497`), `ipa_data-v4.7.c:363` | SMEM host 1, item **497**, **0x9000** bytes |
| IPA firmware PAS | `lito.dtsi:3520` | `qcom,pas-id = <0xf>` = **15**, image `ipa_fws` (`lito.dtsi:3521`) — **image absent on this device**, see §2 |
| IPA SMP2P entries | `lito-smp2p.dtsi:27-37` (inside `smp2p-mpss`, pair `<435>,<428>`) | entry name **"ipa"**, out-state + in-IRQ (2 bits / 2 IRQs) |
| IPA QMI service | `qrtr-lookup` on phone; mainline `drivers/net/ipa/ipa_qmi.c:73` (`IPA_MODEM_SERVICE_SVC_ID 0x31`) | QMI service **49** (0x31), already registered by modem ("IPA control service", node 0 port 27) |
| IPA clock | `build/linux/drivers/clk/qcom/clk-rpmh.c:1023`; `include/dt-bindings/clock/qcom,rpmh.h:21` | `RPMH_IPA_CLK` (id 12) supported for sm7250 |
| IPA ICC paths | `build/linux/include/dt-bindings/interconnect/qcom,sm7250-rpmh.h:25,53,82,102,145` | `MASTER_IPA=4` (aggre2), `SLAVE_IPA_CFG=19` (config_noc), `MASTER_AMPSS_M0=0` (gem_noc), `SLAVE_EBI_CH0=1` (mc_virt), `SLAVE_OCIMEM=9` (system_noc) |
| Modem remoteproc | `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts:463-466` | `qcom,sm7250-mpss-pas`, fw `qcom/sm7250/xiaomi/picasso/image/modem.mdt`, live state `running` |
| Modem SSR name | `build/linux/drivers/remoteproc/qcom_q6v5_pas.c:1352,1364` | `mpss` — matches IPA's `qcom_register_ssr_notifier("mpss")` (`ipa_modem.c:494`) |
| Firmware partition | phone: `/mnt/modem_fw` = `/dev/sde48` (GPT label `modem`), vfat ro, 167 MB, 329 files | contains `modem/adsp/cdsp/npu/wlan` images; **no `ipa_fws.*`** |
| Downstream data path choice | `arch/arm64/configs/vendor/lito_defconfig:538,541` | `CONFIG_IPA3=y`, `CONFIG_RMNET_IPA3=y` → IPA+rmnet, **not** bam-dmux |

Downstream also pins the data netdev name used by the vendor stack: `qcom,rmnet-ipa3`
(`lito.dtsi:3510-3515`, `qcom,ipa-napi-enable`, `qcom,ipa-advertise-sg-support`).

---

## 2. Current mainline state

### 2.1 Drivers

| Driver | Present? | Source |
|---|---|---|
| IPA | yes, v4.7 supported | `build/linux/drivers/net/ipa/ipa_main.c:644-690`; `sm6350-ipa`→`ipa_data_v4_7` at `:660-661`; setup log `:157`, probe log `:893` |
| IPA firmware loader | yes, PAS / `qcom_mdt_load` | `ipa_main.c:586-641` (`ipa_firmware_load`), mode selection `:726-773` (supports `qcom,gsi-loader = self/modem/skip`, legacy `modem-init`) |
| rmnet MAP | yes | `drivers/net/ethernet/qualcomm/rmnet/Kconfig:6` (`menuconfig RMNET`), link kind `"rmnet"` at `rmnet_config.c:401`; vnd type `ARPHRD_RAWIP` (`rmnet_vnd.c:282`) |
| IPA netdev | `rmnet_ipa%d`, created only after the modem IPA-QMI handshake (`ipa_qmi.c:145`) | `ipa_modem.c:26`, `:329-348` |
| bam-dmux | yes but wrong path for lito | `drivers/net/wwan/Kconfig:65` |
| QRTR/GLINK/SMP2P | yes and working | live rpmsg channels `4080000.remoteproc:glink-edge.IPCRTR`, `qrtr-lookup` output §5 |

### 2.2 Kconfig

`configs/linux/picasso.config` currently has **none** of the data-path symbols; only the transport/PAS
stack:

```
configs/linux/picasso.config:58   CONFIG_INTERCONNECT_QCOM_SM7250=y
configs/linux/picasso.config:83   CONFIG_QCOM_SMEM=y
configs/linux/picasso.config:86   CONFIG_QCOM_AOSS_QMP=y
configs/linux/picasso.config:100  CONFIG_QCOM_RPROC_COMMON=y
configs/linux/picasso.config:101  CONFIG_QCOM_MDT_LOADER=y
configs/linux/picasso.config:107  CONFIG_QCOM_Q6V5_PAS=y
configs/linux/picasso.config:111  CONFIG_RPMSG_QCOM_GLINK_SMEM=y
configs/linux/picasso.config:112  CONFIG_QRTR=y
configs/linux/picasso.config:113  CONFIG_QRTR_SMD=y
```

`build.sh` = `defconfig` + `merge_config.sh` of `configs/linux/*.config` (`build.sh:94-101`), and the
arm64 defconfig already sets the data-path symbols **as modules**:

```
build/linux/arch/arm64/configs/defconfig:429   CONFIG_RMNET=m
build/linux/arch/arm64/configs/defconfig:459   CONFIG_QCOM_IPA=m
build/linux/arch/arm64/configs/defconfig:524   CONFIG_QCOM_BAM_DMUX=m
```

This is confirmed by the live kernel (`/proc/config.gz`, §5): `CONFIG_QCOM_IPA=m CONFIG_RMNET=m
CONFIG_QCOM_BAM_DMUX=m`, while `/lib/modules/<uname -r>` does not exist and `lsmod` is empty — i.e.
the data path is compiled but **cannot be loaded**. `QCOM_IPA` also selects `QCOM_MDT_LOADER`,
`QCOM_PAS`, `QCOM_QMI_HELPERS` (`drivers/net/ipa/Kconfig:1-17`) and depends on `QCOM_RPROC_COMMON`,
`QCOM_AOSS_QMP`, `INTERCONNECT`.

### 2.3 Device tree

* No IPA node anywhere: `sm7250.dtsi` has only the ICC provider `ipa_virt`
  (`arch/arm64/boot/dts/qcom/sm7250.dtsi:1620`); the board DTS has no `ipa`/IPA reference.
* The DTB on the phone has no `ipa*` node (`find /proc/device-tree -name '*ipa*'` → empty), and
  `/proc/device-tree/smp2p-mpss/` contains only `master-kernel`, `slave-kernel` — **no `ipa` entry**.
  Note `smp2p-mpss` (`sm7250.dtsi:486`) has **no label**, so the two IPA SMP2P children cannot be
  added from the board DTS (see §3a).
* Reserved memory for a hypothetical AP-side IPA firmware load already exists and is currently
  unused: `ipa_fw_mem: ipa-fw@8bc00000` (0x10000) and `ipa_gsi_mem: ipa-gsi@8bc10000` (0x5400),
  board DTS `:150-158` (introduced/moved by `patches/linux/picasso/0001-…:76-84`).
* `apps_smmu` (`sm7250.dtsi:2022`, `qcom,sm7250-smmu-500`) probes: live
  `/sys/class/iommu/smmu.0x0000000015000000` exists, so SIDs 0x520/0x522 can be attached.
* `rpmhcc` (`sm7250.dtsi:1531`), `aoss_qmp` (`:2184`), `aggre2_noc`/`system_noc`/`config_noc`/`gem_noc`/
  `mc_virt` (`:1607-1675`) all exist for the IPA node below.

### 2.4 Live probe result

`dmesg | grep -icE 'ipa|rmnet'` = **0**; no `/sys/bus/platform/devices/1e40000.*`; no IPA driver in
`/sys/bus/platform/drivers`. IPA cannot probe because there is no DT node (and no loadable module).

### 2.5 IPA firmware reality check

The AP-side loader needs `ipa_fws.mdt` + `.b00…` (default path `ipa_fws.mdt`,
`ipa_main.c:71`). It does not exist on this device:

* `/mnt/modem_fw/image/` (329 files, the whole vendor firmware dir): the only `*ipa*` matches are
  `mlipay.*` and `saipan/`. `mlipay` is **not** IPA: its `.b03` contains the strings `Mlipay`,
  `IFAA_AESEncrypt`, `qsee_RsaKeyGenerate` (Xiaomi Pay / IFAA TEE applet), and the stock image
  manifest in `NON-HLOS.bin` lists it as `<image name="mlipay.mbn"> <info>"Mlipay"</info>`.
* The stock image set (`~/droidian/downloads/picasso_images_V12.5.7.0.RGICNXM_.../images`) has no
  `ipa_fws` partition image either, and `strings NON-HLOS.bin | grep -i ipa_fws` is empty.
* `saipan/qdsp6m.qdb` is a WLAN/WCN calibration DB, not IPA firmware.

Therefore the AP cannot self-load IPA firmware with the files available; the modem (or XBL) must own
GSI/IPA bring-up, which is exactly what `qcom,gsi-loader = "modem"` (modem signals GSI ready over
SMP2P) and `"skip"` (nobody needs to load) exist for (`ipa_main.c:726-773`).

---

## 3. Change plan

### (a) DTS

**A1 — `build/linux/arch/arm64/boot/dts/qcom/sm7250.dtsi`, inside `smp2p-mpss { … }`** (insert after
the `modem_smp2p_in` child: after its closing brace at line 508 and before `smp2p-mpss`'s closing
brace at line 509). This cannot live in
the board DTS: `smp2p-mpss` has no label, and a second `qcom,smp2p` node on the same SMEM pair would
fail because the IPCC mbox is exclusive.

```dts
		ipa_smp2p_out: ipa-ap-to-modem {
			qcom,entry-name = "ipa";
			#qcom,smem-state-cells = <1>;
		};

		ipa_smp2p_in: ipa-modem-to-ap {
			qcom,entry-name = "ipa";
			interrupt-controller;
			#interrupt-cells = <2>;
		};
```

(Same shape as upstream `sdm845.dtsi:1159-1168` and `kodiak.dtsi:813-822`.)

**A2 — `build/linux/arch/arm64/boot/dts/qcom/sm7250-xiaomi-picasso.dts`, paste-ready node.** No extra
`#include` is needed: `sm7250.dtsi` already includes `arm-gic.h`, `qcom,rpmh.h`,
`qcom,sm7250-rpmh.h` and `qcom,icc.h`, and the board includes the dtsi. `&soc`, `&apps_smmu`,
`&rpmhcc`, `&aoss_qmp`, `&aggre2_noc`, `&system_noc`, `&config_noc`, `&gem_noc`, `&mc_virt` are all
existing labels.

```dts
/*
 * IPA v4.7: AP<->modem IP data path.  There is no ipa_fws image on this
 * device; the modem performs GSI firmware load and signals completion over
 * the "ipa" SMP2P entry, so the AP uses qcom,gsi-loader = "modem".
 * Register block: 0x1e00000 + IPAv4.7 reg-base offset 0x40000 (vendor
 * ipahal_get_reg_base()); local SRAM at +0x10000; GSI at 0x1e04000.
 */
&soc {
	ipa: ipa@1e40000 {
		compatible = "qcom,sm7250-ipa", "qcom,sm6350-ipa";

		reg = <0x0 0x01e40000 0x0 0x8000>,
		      <0x0 0x01e50000 0x0 0x3000>,
		      <0x0 0x01e04000 0x0 0x23000>;
		reg-names = "ipa-reg", "ipa-shared", "gsi";

		interrupts-extended = <&intc GIC_SPI 311 IRQ_TYPE_LEVEL_HIGH>,
				      <&intc GIC_SPI 432 IRQ_TYPE_LEVEL_HIGH>,
				      <&ipa_smp2p_in 0 IRQ_TYPE_EDGE_RISING>,
				      <&ipa_smp2p_in 1 IRQ_TYPE_EDGE_RISING>;
		interrupt-names = "ipa", "gsi",
				  "ipa-clock-query", "ipa-setup-ready";

		iommus = <&apps_smmu 0x520 0x0>,
			 <&apps_smmu 0x522 0x0>;

		clocks = <&rpmhcc RPMH_IPA_CLK>;
		clock-names = "core";

		interconnects = <&aggre2_noc MASTER_IPA QCOM_ICC_TAG_ALWAYS
				 &mc_virt SLAVE_EBI_CH0 QCOM_ICC_TAG_ALWAYS>,
				<&aggre2_noc MASTER_IPA QCOM_ICC_TAG_ALWAYS
				 &system_noc SLAVE_OCIMEM QCOM_ICC_TAG_ALWAYS>,
				<&gem_noc MASTER_AMPSS_M0 QCOM_ICC_TAG_ACTIVE_ONLY
				 &config_noc SLAVE_IPA_CFG QCOM_ICC_TAG_ACTIVE_ONLY>;
		interconnect-names = "memory", "imem", "config";

		qcom,qmp = <&aoss_qmp>;
		qcom,smem-states = <&ipa_smp2p_out 0>,
				   <&ipa_smp2p_out 1>;
		qcom,smem-state-names = "ipa-clock-enabled-valid",
					"ipa-clock-enabled";
		qcom,gsi-loader = "modem";

		status = "okay";
	};
};
```

Notes:
* For the **first, zero-code-change experiment**, use `compatible = "qcom,sm6350-ipa";` only. The
  binding (`Documentation/devicetree/bindings/net/qcom,ipa.yaml:47-58`) accepts it and
  `ipa_main.c:660` maps it to `ipa_data_v4_7`; this avoids the binding/driver patch until bringup
  proves out. The two-string form above needs patch (c2).
* `qcom,gsi-loader = "modem"` deliberately has no `memory-region`; the binding only requires it for
  `"self"` (`qcom,ipa.yaml:200-221`). `ipa_fw_mem`/`ipa_gsi_mem` stay unused reserved memory.
* No `sram = <&ipa_modem_tables>`: sm7250 has no IMEM syscon node, and `ipa_mem.c:662-678` falls back
  to the v4.7 constants (imem 0x146a8000/0x2000), which are the same addresses downstream hard-codes.
* IRQ trigger types are the downstream values (`lito.dtsi:3531-3532`). Upstream's other IPA nodes use
  `GIC_SPI 311 … EDGE_RISING`; see risk R4.

### (b) `configs/linux/picasso.config` — append exactly

```
# Cellular data path: IPA v4.7 + rmnet MAP.  Built in, not modules: the
# initramfs carries no /lib/modules, so defconfig's "=m" is not loadable.
CONFIG_QCOM_IPA=y
CONFIG_RMNET=y
CONFIG_QCOM_SMP2P=y
CONFIG_QCOM_SMEM_STATE=y
CONFIG_QCOM_PAS=y
CONFIG_QCOM_QMI_HELPERS=y
```

Already satisfied and therefore not repeated: `CONFIG_QCOM_SMEM=y` (:83), `CONFIG_QCOM_AOSS_QMP=y`
(:86), `CONFIG_QCOM_RPROC_COMMON=y` (:100), `CONFIG_QCOM_MDT_LOADER=y` (:101), `CONFIG_QCOM_Q6V5_PAS=y`
(:107), `CONFIG_INTERCONNECT_QCOM_SM7250=y` (:58, pulls `CONFIG_INTERCONNECT=y`, live config confirms).
`CONFIG_QCOM_BAM_DMUX=m` stays as an unused module from defconfig; `=n` would only shrink the build.

### (c) Proposed patches

1. `patches/linux/picasso/0016-arm64-dts-qcom-sm7250-add-ipa.patch` — adds the two `smp2p-mpss`
   children (A1) and the `ipa@1e40000` node (A2) to `sm7250.dtsi` (SoC node, `status = "disabled"`),
   plus `&ipa { status = "okay"; }` in `sm7250-xiaomi-picasso.dts`. Add both files to
   `patches/linux/series` after `picasso/0015-…`. ~70 lines, no C code.
2. `patches/linux/picasso/0017-net-ipa-add-sm7250-compatible.patch` — one line in
   `Documentation/devicetree/bindings/net/qcom,ipa.yaml` (`qcom,sm7250-ipa` added to the enum) and one
   match entry in `drivers/net/ipa/ipa_main.c`:
   `{ .compatible = "qcom,sm7250-ipa", .data = &ipa_data_v4_7 }`. ~6 lines. One paragraph: *"SM7250
   carries the same IPA v4.7 core as SM6350 (vendor `qcom,ipa-hw-ver = <18>`), so it can reuse
   `ipa_data_v4_7`; the new compatible documents the SoC instead of pretending to be SM6350."*
3. Only if §4 R3 fires: `0018-net-ipa-add-sm7250-endpoint-data.patch` — a `ipa_data-v4.7-lito.c`
   variant or edits to `ipa_data-v4.7.c` (endpoint ids/channel ids/resource groups for lito).
   Size estimate: **100–250 lines** of data table, no logic. There is no missing driver: the IPA
   stack (GSI, endpoints, tables, QMI, PM, SMMU) exists upstream and is shared with sm6350.

### (d) Verification ladder (exact commands, expected proof)

Run everything as root over the USB/wifi ssh. `cd` is irrelevant; commands are host- or phone-side as
marked. Do not change phone state beyond what each step says.

1. **DT + config landed in the boot image**
   ```
   phone# dmesg | grep -i ipa
   ipa 1e40000.ipa: IPA driver initialized
   ```
   (no node/DT match ⇒ `grep -c` = 0 and no `1e40000.ipa` in `/sys/bus/platform/devices`).
2. **GSI firmware owner resolved (R2)** — after the modem is `running`:
   ```
   phone# dmesg | grep -iE 'ipa .*setup completed|ipa_setup|limiting IPA memory'
   ipa 1e40000.ipa: IPA driver setup completed successfully
   ```
   Failure signatures:
   * nothing after "IPA driver initialized" ⇒ modem never sent `ipa-setup-ready`; switch
     `qcom,gsi-loader = "skip"` and retest.
   * `region <n> ends beyond memory limit (0x…3000)` ⇒ wrong `ipa-shared` window (R1).
   * `GSI command … timed out` ⇒ endpoint/FW mismatch (R3).
3. **Modem IPA-QMI handshake + netdev**
   ```
   phone# dmesg | grep -E 'received modem (starting|running) event|error .*starting modem'
   phone# ip -br link | grep rmnet
   rmnet_ipa0          DOWN           <mac>
   ```
   `rmnet_ipa0` is created by `ipa_qmi_ready()` (`ipa_qmi.c:145`), so it proving up also proves the
   kernel QMI path to modem service 49 works.
4. **SIM read (needs userland)**
   ```
   phone# pacman -S --needed modemmanager libqmi    # extra: modemmanager 1.24.2-2, libqmi 1.38.0-2
   phone# systemctl enable --now ModemManager.service
   phone# qmicli -d qrtr://0 --uim-get-card-status
   [/dev/…] Successfully got card status
   Primary SIM card:
     Card state: 'present'
     …
       ICCID: '8986…'
   ```
   and identities/registration:
   ```
   phone# qmicli -d qrtr://0 --dms-get-ids
   phone# qmicli -d qrtr://0 --nas-get-serving-system
   Registration state: 'registered'
   ```
5. **PDP + data**
   ```
   phone# mmcli -L
   /org/freedesktop/ModemManager1/Modem/0 [Xiaomi] …
   phone# mmcli -m 0 --simple-connect="apn=<apn>"
   successfully connected the modem
   phone# ip -br addr show rmnet_data0
   rmnet_data0  UP  10.x.y.z/… 
   phone# ip route get 1.1.1.1
   1.1.1.1 via … dev rmnet_data0
   phone# ping -I rmnet_data0 -c3 1.1.1.1
   3 packets transmitted, 3 received, 0% packet loss
   ```
   ModemManager instantiates the virtual rmnet netdev for IPA-based Qualcomm SoCs
   (modemmanager.org "WWAN device types", Qualcomm SoCs section: expected network port `rmnet_ipa0`,
   "it is required to instantiate new virtual network interfaces when a connection is being brought
   up"). If MM does not do it, the manual equivalent is:
   ```
   phone# ip link add link rmnet_ipa0 name rmnet_data0 type rmnet mux_id 1
   phone# qmicli -d qrtr://0 --wds-start-network=apn='<apn>' --client-no-release-cid
   phone# ip link set rmnet_data0 up; ip addr add <addr>/<len> dev rmnet_data0
   ```
6. **NetworkManager (Denial's UI path)** — NM's WWAN plugin (built against `libmm-glib`,
   `libnm-device-plugin-wwan.so`) is already installed and loaded (`NMWwanFactory`); after
   ModemManager is up:
   ```
   phone# nmcli device status | grep gsm
   <iface>  gsm  connected  <apn>
   ```

---

## 4. Unknowns / risks

| # | Risk | Why uncertain | Cheapest single experiment |
|---|---|---|---|
| R1 | `ipa-shared` window 0x1e50000/0x3000 | Derived from IPAv4.7 reg-base offset + SW-area offset + BADDR=0; no lito line spells it out | Boot with node; `dmesg \| grep -i 'IPA memory\|ends beyond memory limit'`. A limit error means adjust base/size (try 0x1e40000/0x13000) |
| R2 | Who loads IPA/GSI firmware (`modem` vs `skip` vs `self`) | No `ipa_fws` image exists; XBL/modem images contain no such name string (strings of `NON-HLOS.bin`, `xbl.img`, `abl.img`, `picasso_fv.fv`), yet stock Android must have working IPA | Boot with `"modem"`; if no `IPA driver setup completed` within ~30 s of modem `running`, change the one property to `"skip"` |
| R3 | `ipa_data_v4_7` endpoint/channel map fits lito | Table is upstream's SM6350 tuning; lito's vendor table is not available (vendor only ships ipa4.5 dumps) | Boot; look for `GSI command … timed out`, `error … from ipa_setup()`, or missing `rmnet_ipa0`. If so, diff against `drivers/platform/msm/ipa/ipa_v3` endpoint init and add an sm7250 data variant |
| R4 | IPA IRQ 311 trigger is level (downstream) vs edge (upstream nodes) | Vendor DT and upstream code disagree | `/proc/interrupts` after boot: `ipa` line count 0 while modem is up ⇒ switch 311 to `IRQ_TYPE_EDGE_RISING` |
| R5 | IPA ICC path resolution on sm7250 | Driver's RPMh topology is shared with QUP paths that do work, but `MASTER_IPA→SLAVE_EBI_CH0` is unproven | dmesg for `icc`/`interconnect` errors at IPA probe; if `-ENODEV`, drop the `imem` path (sm8350-style 2-path node) and retest |
| R6 | IPA module vs builtin | Live kernel has `=m` with no modules on disk | Set `=y` (§3b) — no experiment needed, this is deterministic |
| R7 | ModemManager QRTR discovery | MM must be built with QRTR and use the kernel name service; there is no `/sys/bus/qrtr` on this kernel (QRTR here is sockets only) | `mmcli -L` after install; if empty, `qmicli -d qrtr://0 --uim-get-card-status` still proves QMI, and data can be brought up manually (§3d.5) |
| R8 | rmnet mux id expected by the modem | Modem expects Bind Mux Data Port / mux id from the DPM service (47 present) | `ip link add … mux_id 1` + WDS start; if RX is empty, try `mux_id` values 1..8 and inspect `tc -s qdisc`/`ip -s link` counters |
| R9 | AP-side firmware self-load ever needed | Only if both modem and skip fail | Place a matching `ipa_fws.mdt` (PAS 15, from another lito-family vendor package) into `/lib/firmware`, set `qcom,gsi-loader = "self"` + `memory-region = <&ipa_fw_mem>` + `firmware-name`; the reserved regions already exist |
| R10 | SIM/EFS independence from data path | UIM/NAS already advertised, but no userspace QMI client exists to prove a card read | Step §3d.4; this is userland-only and independent of IPA |

---

## 5. Live observations (phone <lan-ip>, kernel `7.3.0-rc7-00001-g…-dirty`, built 2026-11-17 18:45)

All commands read-only. Key excerpts:

```
$ ssh … root@<lan-ip> 'qrtr-lookup | head -30'
  Service Version Instance Node  Port
       21       1        0    0    20 Modem embedded file system service
       49       1        2    0    27 IPA control service
        9       2        0    0    56 Voice service
        3       1        0    0    57 Network Access Service
       11       1        0    0    60 User Identity Module service
        1       1        0    0    61 Wireless Data Service
       47       1        0    0    59 Data Port Mapper service
       2       1        0    0    70 Device Management Service
```

```
$ … 'ip -br addr'
lo     UNKNOWN  127.0.0.1/8 ::1/128
usb0   UP       10.77.71.2/30 fe80::ff:fe00:2/64
dlh1000@if2 UP  10.177.0.1/30 …
wlan0  UP       <lan-ip>/22 …
# no wwan0, no rmnet_ipa0, no rmnet_data0
```

```
$ … 'for r in /sys/class/remoteproc/remoteproc*; do echo "$r: $(cat $r/state) $(cat $r/firmware)"; done'
/sys/class/remoteproc/remoteproc0: running qcom/sm7250/xiaomi/picasso/image/adsp.mdt
/sys/class/remoteproc/remoteproc1: running qcom/sm7250/xiaomi/picasso/image/modem.mdt
```

```
$ … 'dmesg | grep -icE "ipa|rmnet"; dmesg | grep -iE "q6v5|pas" | head'
0
[16.239011] state: adsp = running
[16.239312] state: modem = running      # repeated every 30 s by the project's status probe
```

```
$ … 'ls /sys/bus/platform/devices | grep -i "1e00000\|1e40000"; ls /sys/bus/platform/drivers | grep -i ipa'
   # empty: no IPA device, no IPA driver bound
```

```
$ … 'find /proc/device-tree -name "*ipa*"; ls /proc/device-tree/smp2p-mpss'
smp2p-mpss entries: compatible interrupts-extended master-kernel mboxes name
                    qcom,local-pid qcom,remote-pid qcom,smem slave-kernel
   # no ipa node in the DTB, no "ipa" SMP2P entry
```

```
$ … 'lsmod | wc -l; ls /lib/modules; zcat /proc/config.gz | grep -E "CONFIG_QCOM_IPA|CONFIG_RMNET|CONFIG_QCOM_BAM_DMUX|CONFIG_QCOM_SMP2P|CONFIG_QCOM_SMEM_STATE"'
1                    # header only: no modules loaded
                     # /lib/modules exists but has no <uname -r> subdir / no .ko files
CONFIG_RMNET=m
CONFIG_QCOM_IPA=m
CONFIG_QCOM_BAM_DMUX=m
CONFIG_QCOM_SMP2P=y
CONFIG_QCOM_SMEM_STATE=y
```

```
$ … 'command -v mmcli qmicli qrtr-lookup; pacman -Q | grep -iE "modemmanager|libqmi|ofono"'
/usr/bin/qrtr-lookup
# mmcli: command not found; qmicli: absent; no modemmanager/libqmi/libmbim installed
```

```
$ … 'pacman -Si libqmi modemmanager | grep -E "^(Repository|Name|Version)"'
Repository : extra      Name : libqmi        Version : 1.38.0-2
Repository : extra      Name : modemmanager  Version : 1.24.2-2
   # libqmi "Depends On: … libmbim libqrtr-glib" → QRTR-capable build
```

```
$ … 'cat /proc/mounts | grep -E "modem_fw|dsp_fw"'
/dev/sde48 /mnt/modem_fw vfat ro,…
/dev/sde47 /mnt/dsp_fw  ext4 ro,…
$ … 'ls /mnt/modem_fw/image | grep -iE "ipa|fws"'
mlipay.b00 … mlipay.mdt
saipan
$ … 'ls /dev/disk/by-partlabel/ | grep -i ipa'      # empty
```

```
$ … 'ls /sys/bus/rpmsg/devices | grep 4080000'
4080000.remoteproc:glink-edge.DATA1.-1.-1
4080000.remoteproc:glink-edge.IPCRTR.-1.-1
4080000.remoteproc:glink-edge.glink_ssr.-1.-1
…
# GLINK/QRTR transport to the modem is registered; no /sys/bus/qrtr (this kernel's
# QRTR is sockets-only, which is what libqrtr-glib/libqmi use)
```

```
$ … 'ls /sys/class/iommu; ls /sys/bus/platform/drivers | grep smmu'
smmu.0x0000000003da0000  smmu.0x0000000015000000
arm-smmu  arm-smmu-v3      # apps_smmu (15000000) is bound → IPA SIDs can attach
```

Host-side check of the pulled firmware (`mlipay` is not IPA):

```
$ readelf -h mlipay.mdt | grep -E 'Class|Machine'      # ELF32, ARM (32-bit)
$ strings mlipay.b03 | head -3
Mlipay
IFAA_AESEncrypt
$ strings NON-HLOS.bin | grep -i ipa_fws | wc -l         # 0
$ strings NON-HLOS.bin | grep -oE '<image name="[^"]+"' | grep -i ipa   # none
```
