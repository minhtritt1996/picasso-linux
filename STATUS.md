# Status

## M0 — build from upstream (in progress)

Target: `./build.sh linux` produces `Image` + `sm7250-xiaomi-picasso.dtb` from
`upstream/` + `patches/linux/series`, and `./build.sh check` applies the series to a clean tree.

| Item | State |
|---|---|
| upstream/linux @ v7.3-rc7 | pinned `ec8bf82` |
| upstream/avicii-mainline | pinned `7194447` |
| vendor series 01..14 | imported unmodified |
| picasso DTS (reserved-memory split + board) | pending first build |
| kernel config | `configs/linux/{base,picasso}.config` |
| boot vehicle (`linux.efi`) | not started (M1) |

## Verified on hardware

Nothing yet. Every entry here must name the artifact hash and the boot command.
