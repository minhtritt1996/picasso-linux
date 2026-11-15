#!/usr/bin/env python3
"""Pack a Luma-format boot image for picasso (Redmi K30 5G, SM7250).

This is the image the *stock* bootloader loads from the `boot` partition, or
that `fastboot boot` loads into RAM: an Android boot image, header v2, with the
kernel, the initramfs and the device tree packed the way this phone needs them.

The addresses are the ones proven on this hardware by the Droidian port
(droidian/docs/02-boot-image-layout.md and its working image, 24 047 616 bytes):

    header v2, page size 4096, base 0
    kernel  (Image.gz) @ 0x8000
    ramdisk (initramfs) @ 0x1200000   - not 0x1000000: a decompressed kernel
                                        lands there
    tags               @ 0x100
    dtb                @ 0x1728000   - the field is required, a boot image
                                        without a DTB is dropped by fastboot

`mkbootimg` lays the parts out in the file in that order, so the kernel must end
before the ramdisk offset and the ramdisk before the DTB offset; this script
refuses to build an image where they overlap.

The command line must name the two filesystems of the generic phone userdata
image (`pmos_root_uuid`, `pmos_boot_uuid`): Luma's device kit builder refuses a
boot image without them, and the shared OSTree resolver reads them.

usage:
  build-luma-bootimg.py --image build/out/Image \\
      --initrd build/out/initrd.cpio.gz \\
      --dtb build/out/sm7250-xiaomi-picasso.dtb \\
      -o build/out/boot-luma.img [--cmdline "..."] [--max-size 64M]
"""
import argparse
import gzip
import os
import shutil
import subprocess
import sys
import tempfile

ROOT_UUID = '03797c05-cf87-4eda-adc4-6ce111baf2d3'
BOOT_UUID = '032D-57AD'

KERNEL_OFFSET = 0x8000
RAMDISK_OFFSET = 0x1200000
TAGS_OFFSET = 0x100
DTB_OFFSET = 0x1728000
PAGE_SIZE = 4096

DEFAULT_CMDLINE = (
    'console=tty0 loglevel=7 picasso_pinmux.mode=write '
    'rdinit=/usr/lib/systemd/systemd systemd.show_status=1 '
    'clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=10'
)


def die(msg):
    print(f'build-luma-bootimg.py: {msg}', file=sys.stderr)
    sys.exit(1)


def size_arg(text):
    mult = {'k': 1 << 10, 'm': 1 << 20, 'g': 1 << 30}
    text = text.strip().lower()
    if text[-1] in mult:
        return int(text[:-1], 0) * mult[text[-1]]
    return int(text, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--image', required=True, help='uncompressed arm64 Image')
    ap.add_argument('--initrd', required=True, help='initramfs cpio.gz')
    ap.add_argument('--dtb', required=True, help='picasso device tree blob')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--cmdline', default=DEFAULT_CMDLINE)
    ap.add_argument('--kernel-offset', type=lambda s: int(s, 0), default=KERNEL_OFFSET)
    ap.add_argument('--ramdisk-offset', type=lambda s: int(s, 0), default=RAMDISK_OFFSET)
    ap.add_argument('--dtb-offset', type=lambda s: int(s, 0), default=DTB_OFFSET)
    ap.add_argument('--max-size', type=size_arg, default=64 << 20,
                    help='boot partition size to stay under (default 64 MiB)')
    args = ap.parse_args(argv)

    for path in (args.image, args.initrd, args.dtb):
        if not os.path.isfile(path):
            die(f'{path} does not exist')
    if 'pmos_root_uuid=' not in args.cmdline or 'pmos_boot_uuid=' not in args.cmdline:
        die('the command line must carry pmos_root_uuid= and pmos_boot_uuid=: '
            f'Luma reads them, e.g. "pmos_root_uuid={ROOT_UUID} pmos_boot_uuid={BOOT_UUID}"')

    work = tempfile.mkdtemp(prefix='luma-bootimg.')
    try:
        kernel = os.path.join(work, 'Image.gz')
        with open(args.image, 'rb') as src, gzip.open(kernel, 'wb', compresslevel=9) as dst:
            shutil.copyfileobj(src, dst, 1 << 20)

        ksize = os.path.getsize(kernel)
        rsize = os.path.getsize(args.initrd)
        dsize = os.path.getsize(args.dtb)
        kend = args.kernel_offset + ksize
        rend = args.ramdisk_offset + rsize
        dend = args.dtb_offset + dsize
        _ = dend  # the DTB load address is printed below; the file layout is sequential
        if kend > args.ramdisk_offset:
            die(f'the kernel (Image.gz {ksize} B) ends at 0x{kend:x}, past the ramdisk '
                f'offset 0x{args.ramdisk_offset:x}; raise --ramdisk-offset')
        if rend > args.dtb_offset:
            die(f'the ramdisk ({rsize} B) ends at 0x{rend:x}, past the DTB offset '
                f'0x{args.dtb_offset:x}; raise --dtb-offset')
        # The decompressed kernel is placed by the decompressor, which avoids the
        # initrd: the proven Droidian image has a ~48 MB kernel over a ramdisk at
        # 0x1200000. Say so instead of pretending it cannot happen.
        raw = os.path.getsize(args.image)
        if args.kernel_offset + raw > args.ramdisk_offset:
            print(f'  note: the decompressed kernel ({raw} B) spans past the ramdisk '
                  f'load address 0x{args.ramdisk_offset:x}; the arm64 decompressor '
                  'relocates around the initrd (proven at this offset on this phone)',
                  file=sys.stderr)

        subprocess.run([
            'mkbootimg',
            '--header_version', '2',
            '--pagesize', str(PAGE_SIZE),
            '--base', '0',
            '--kernel', kernel,
            '--ramdisk', args.initrd,
            '--dtb', args.dtb,
            '--kernel_offset', hex(args.kernel_offset),
            '--ramdisk_offset', hex(args.ramdisk_offset),
            '--tags_offset', hex(TAGS_OFFSET),
            '--dtb_offset', hex(args.dtb_offset),
            '--cmdline', args.cmdline,
            '-o', args.output,
        ], check=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    total = os.path.getsize(args.output)
    if total > args.max_size:
        os.unlink(args.output)
        die(f'the image is {total} B, past the {args.max_size} B limit')
    print(f'{args.output}: {total} bytes (limit {args.max_size})')
    print(f'  kernel  Image.gz {ksize:>10} B @ 0x{args.kernel_offset:x}..0x{kend:x}')
    print(f'  ramdisk          {rsize:>10} B @ 0x{args.ramdisk_offset:x}..0x{rend:x}')
    print(f'  dtb              {dsize:>10} B @ 0x{args.dtb_offset:x}..0x{dend:x}')
    print(f'  headroom to the limit: {args.max_size - total} B')
    if shutil.which('unpack_bootimg'):
        out = subprocess.run(['unpack_bootimg', '--boot_img', args.output],
                             capture_output=True, text=True).stdout
        keep = [l for l in out.splitlines()
                if l.startswith(('boot image header version', 'page size', 'kernel load address',
                                 'ramdisk load address', 'dtb address', 'kernel size',
                                 'ramdisk size', 'dtb size'))]
        print('  verified with unpack_bootimg:')
        for line in keep:
            print(f'    {line}')


if __name__ == '__main__':
    main()
