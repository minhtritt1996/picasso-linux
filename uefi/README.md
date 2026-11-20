# uefi/

Inputs for `./build.sh bootimg` / `./build.sh bootimg-auto` (the Mu-Silicium UEFI chain):

| File | In git | What it is |
|---|---|---|
| `BootShim.bin` | yes (176 B) | copies the UEFI FD to `0x9F100000` and the XNUP payload to `0xF0000000` |
| `SILICIUM_UEFI.fd` | no | HakiOS Mu-Silicium firmware volume (2 MiB). It must be the build that contains `XnuLoaderMain`, i.e. the one the `hakios-picasso` tree produces, not a plain UniXNU build |
| `picasso.dtb` | no | device tree handed to the UEFI build (taken from the stock firmware / HakiOS build tree) |

Place the two ignored files here, or point `UEFI_FD` / `UEFI_DTB` at them. Vendor UEFI drivers
are never stored in this repository.
