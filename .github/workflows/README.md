# Gates

`tools/preflight.sh` is the single implementation of gates 1 and 3's static
half; the workflow runs the same script, so a green local preflight means a
green `policy` job.

| Gate | Where | What it proves |
|---|---|---|
| 1 preflight | `ubuntu-latest` | documents present; policy scan (no flash/erase/`dd` to a block device, no machine-specific absolute paths, `upstream/` never tracked); every patch in `patches/linux/series` exists; `bash -n` + `compile()`; `git diff --check` |
| 2 change | pull request | review checklist: kernel changes exist only as patches in `patches/linux/picasso/`, exported from `build/`, `vendor/` untouched, `STATUS.md` records the hardware run |
| 3 CI | `self-hosted, picasso-upstream` | `./build.sh check` applies all 22 patches to a clean upstream and builds the DTB; `linux --clean`, `loader`, `bootimg`, `bootimg-luma` all build |
| 4 hardware | `self-hosted, picasso-phone, ram-boot-only` | one RAM-boot cycle: `oem ramdisk` → `stage` rootfs → `stage` payload → `oem run` → console log. Never a flash |

## Runner requirements

`picasso-upstream`: the three pinned clones under `./upstream/` (linux,
avicii-mainline, hakios-xnu-kernel — see `patches/linux/UPSTREAM`), aarch64 cross
toolchain (`aarch64-linux-gnu-`), gcc for the host, python3, dtc, and
`./.venv-dtschema` or network access for `tools/setup-dtschema.sh`.
`actions/checkout` must run with `clean: false` or it deletes those clones.

`picasso-phone`: the same build tree plus the UEFI binaries the payload needs
(`PICASSO_UEFI_FD`, `PICASSO_BOOTSHIM`, `PICASSO_UEFI_DTB` repository variables)
and the phone on USB, in the RAM loader. One run at a time
(`concurrency: picasso-phone`).

## Trust

Self-hosted jobs never run code from a fork
(`github.event.pull_request.head.repo.full_name == github.repository`). A
job-level `if` may only use the `github` context, which is why the guard is
written out in both jobs instead of living in `env`.
