# initramfs/bin/

Static aarch64 helpers that `scripts/mkinitramfs.sh` copies into `/bin` of the ramdisk.
The binaries are **not tracked** (see `.gitignore`); build them from source and drop them here:

| Binary | Source | Local changes |
|---|---|---|
| `rmtfs` | `github.com/linux-msm/rmtfs` | udev lookup replaced by the `/sys/class/rmtfs/...` sysfs path |
| `pd-mapper` | `github.com/linux-msm/pd-mapper` | lzma stubbed out (plain `.jsn` files only) |
| `tqftpserv` | `github.com/linux-msm/tqftpserv` | none |

All three need `libqrtr` (`github.com/linux-msm/qrtr`) and are built with
`aarch64-linux-gnu-gcc -static`. Without them the ramdisk still boots, but the modem
(Wi-Fi, Bluetooth firmware services, ADSP sensors) does not come up.
