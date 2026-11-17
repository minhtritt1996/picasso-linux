# Fedora 44 aarch64 userspace for picasso (RAM-booted)

The root tree is built on the host (no root) and booted from the loader's
resident ramdisk; nothing here is installed on the phone's UFS except the log
copy on the Android `cache` partition.

## Build the tree

```sh
docker run --rm --platform linux/amd64 -v <luma>:/luma:ro -v /tmp/armroot:/out \
  fedora:44 dnf -y --installroot=/out --releasever=44 --forcearch=aarch64 \
  --use-host-config --setopt=install_weak_deps=False --setopt=tsflags=noscripts \
  install --skip-broken $(grep -vE '^\s*(#|$)' /luma/config/mobile/packages.txt) \
  fedora-release systemd-udev util-linux kmod
```

`tsflags=noscripts` is required (the aarch64 `%post` scriptlets run under qemu
and take minutes each: one `semodule -i` was 4m39s of CPU). Re-add what the
scriptlets would have done, in a root container:

```sh
R=/tmp/armroot
mkdir -p $R/usr/sbin $R/dev $R/proc $R/sys $R/run $R/tmp $R/var/tmp
ln -s usr/sbin $R/sbin
: > $R/etc/machine-id; : > $R/etc/fstab
ln -s dbus-broker.service $R/usr/lib/systemd/system/dbus.service
systemd-sysusers --root=$R
sed -i 's|^root:[^:]*:|root::|' $R/etc/shadow      # empty root password
for u in usbacm ttygs-shell weston fw-setup log-to-ufs.timer; do
  ln -sfn /usr/lib/systemd/system/$u.* $R/etc/systemd/system/multi-user.target.wants/
done
ln -sfn /usr/lib/systemd/system/log-to-ufs.timer $R/etc/systemd/system/timers.target.wants/
install -D -m755 tools/usbacm tools/fw-setup.sh tools/log-to-ufs.sh $R/usr/local/bin/
install -D -m644 userland/fedora/*.service userland/fedora/*.timer $R/usr/lib/systemd/system/
install -D -m644 userland/fedora/pamd/system-auth userland/fedora/pamd/postlogin $R/etc/pam.d/
cp prebuilt/fw-gpu/* $R/lib/firmware/qcom/     # Adreno zap/GMU/SQE
```

Pack and boot:

```sh
docker run --rm --platform linux/amd64 -v /tmp/armroot:/rootfs:ro -v $PWD/build/out:/out \
  fedora:44 bash -c 'dnf install -y cpio gzip >/dev/null; cd /rootfs &&
  find . -print0 | cpio --null -o -H newc --owner=0:0 2>/dev/null | gzip -1 > /out/fedora-rootfs.cpio.gz'
PAYLOAD_NO_INITRD=1 CMDLINE="console=tty0 loglevel=7 selinux=0 \
  rdinit=/usr/lib/systemd/systemd systemd.show_status=1 clk_ignore_unused \
  pd_ignore_unused regulator_ignore_unused" ./build.sh bootimg
fastboot oem ramdisk
fastboot stage build/out/fedora-rootfs.cpio.gz
fastboot stage build/out/payload-linux.bin
fastboot oem run
```

## Units

| unit | what it does |
| --- | --- |
| `usbacm.service` | oneshot: configfs CDC-ACM gadget, bind UDC (Type=oneshot so `serial-getty@`/the shell start after `/dev/ttyGS0` exists; never restart it — a rebind drops the DWC3 link) |
| `ttygs-shell.service` | `bash --login` on `/dev/ttyGS0` (login(1) refuses root there, PAM) |
| `weston.service` | weston on the DRM backend with `PAMName=login` — libseat needs a logind session for the DRM device; `--tty` no longer exists in weston 15 |
| `fw-setup.service` | mount the Android `modem`/`dsp`/`bluetooth` partitions read-only, link `image/*` into `/lib/firmware/qcom/sm7250/xiaomi/picasso/image`, then unbind/bind `qcom_q6v5_pas` and `ath10k_snoc` |
| `log-to-ufs.timer` | every 30 s: journal + dmesg + cmdline to `/picasso-logs/` on the Android `cache` partition (ext4; `userdata` is metadata-encrypted) |

## Verified on the phone

`multi-user.target` in 9.1 s (5.65 kernel + 3.5 userspace), root shell over USB,
`nproc` 8, 5296 MB RAM, ADSP and modem both up after the firmware mounts,
`kmscube` on `/dev/dri/card1` with `renderer: FD620` (freedreno, ~10.7 fps),
weston `active` with `Output 'DSI-1' enabled with head(s) DSI-1` and a logind
session, and the boot log landing on UFS every 30 s.
