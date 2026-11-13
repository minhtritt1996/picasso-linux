#!/usr/bin/env python3
"""
Pack the Linux boot image for the Mu-Silicium boot chain.

Outer image (Android boot v1, booted with `fastboot boot`):
    gzip(BootShim.bin + SILICIUM_UEFI.fd + XNUP) + UEFI device tree

XNUP payload at 0xF0000000 (the entry named "loader.efi" is started by
Mu-Silicium's BootManager):
    loader.efi   linux.efi (patched xnuloader)
    Image        arm64 EFI-stub kernel
    initrd       cpio.gz ramdisk (always RAM, replaceable)
    dtb          mainline picasso DTB with /chosen/linux,initrd-* filled in

usage:
  build-bootimg.py -o boot-linux.img \
      --loader loader.efi --image Image --initrd initrd.cpio.gz \
      --dtb sm7250-xiaomi-picasso.dtb \
      --uefi-fd SILICIUM_UEFI.fd --bootshim BootShim.bin --uefi-dtb picasso.dtb \
      [--cmdline "console=tty0 ..."] [--kernel-offset 0x8000]
"""
import argparse
import gzip
import os
import struct
import subprocess
import sys
import tempfile

PAYLOAD_BASE = 0xF0000000
PAYLOAD_MAX = 0x04000000
PAYLOAD_RESERVED = 0x100000          # last MiB: panic log, never part of the XNUP
ENTRY = struct.Struct('<16sII')      # name[16], size, reserved
MAGIC = 0x50554E58                   # "XNUP"


def die(msg):
    print(f'build-bootimg.py: {msg}', file=sys.stderr)
    sys.exit(1)


def align16(n):
    return (n + 15) & ~15


def layout(sizes, base=PAYLOAD_BASE):
    """Address of every entry's data for an ordered list of (name, size)."""
    off = 8
    addrs = {}
    for name, size in sizes:
        addrs[name] = base + off + ENTRY.size
        off += ENTRY.size + align16(size)
    return addrs, off + 24


def build_payload(items):
    body = b''
    for name, data in items:
        if len(name.encode()) > 15:
            die(f'entry name too long: {name}')
        body += ENTRY.pack(name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)
    body += b'\0' * 24
    return struct.pack('<II', MAGIC, len(body)) + body


def fdtput(dtb, path, prop, cells):
    cmd = ['fdtput', '-t', 'x', dtb, path, prop] + [f'0x{c:x}' for c in cells]
    subprocess.run(cmd, check=True)


def patch_dtb(dtb, initrd_start, initrd_end, cmdline):
    subprocess.run(['fdtget', '-t', 'x', dtb, '/chosen'], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if cmdline:
        subprocess.run(['fdtput', '-t', 's', dtb, '/chosen', 'bootargs', cmdline], check=True)
    # /chosen uses the root's #address-cells = 2: two 32-bit cells per value.
    fdtput(dtb, '/chosen', 'linux,initrd-start', [initrd_start >> 32, initrd_start & 0xFFFFFFFF])
    fdtput(dtb, '/chosen', 'linux,initrd-end', [initrd_end >> 32, initrd_end & 0xFFFFFFFF])


def check_pe(path):
    data = open(path, 'rb').read(0x100)
    if len(data) < 0x40 or data[:2] != b'MZ':
        die(f'{path}: not a PE image (no MZ header)')
    lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[lfanew:lfanew + 4] != b'PE\0\0':
        die(f'{path}: not a PE image (no PE signature)')
    machine = struct.unpack_from('<H', open(path, 'rb').read(lfanew + 6), 0)[0] if False else 0
    with open(path, 'rb') as f:
        f.seek(lfanew + 4)
        machine = struct.unpack('<H', f.read(2))[0]
    if machine != 0xAA64:
        die(f'{path}: PE machine 0x{machine:04x}, expected 0xaa64 (AArch64)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--loader', required=True, help='linux.efi')
    ap.add_argument('--image', required=True, help='arm64 Image')
    ap.add_argument('--initrd', required=True, help='initramfs cpio.gz')
    ap.add_argument('--dtb', required=True, help='mainline board DTB')
    ap.add_argument('--uefi-fd', required=True, help='SILICIUM_UEFI.fd')
    ap.add_argument('--bootshim', required=True, help='BootShim.bin')
    ap.add_argument('--uefi-dtb', required=True, help='device tree for the UEFI build')
    ap.add_argument('--cmdline', default='', help='override /chosen/bootargs')
    ap.add_argument('--uefi-ramdisk', default='', help='ramdisk for the outer boot header (default: 5 zero bytes)')
    ap.add_argument('--payload-out', default='', help='also write the XNUP container here (for fastboot stage)')
    ap.add_argument('--no-usb', action='store_true',
                    help='do not add the "usb" entry: the loader boots the kernel immediately instead of '
                         'starting its fastboot server')
    ap.add_argument('--kernel-offset', default='0x8000')
    args = ap.parse_args()

    for path in (args.loader, args.image, args.initrd, args.dtb, args.uefi_fd,
                 args.bootshim, args.uefi_dtb):
        if not os.path.isfile(path):
            die(f'missing input: {path}')
    for tool in ('fdtput', 'mkbootimg'):
        if not subprocess.run(['sh', '-c', f'command -v {tool}'], capture_output=True).returncode == 0:
            die(f'{tool} not found in PATH')

    check_pe(args.loader)
    check_pe(args.image)

    blobs = [(name, open(path, 'rb').read()) for name, path in (
        ('loader.efi', args.loader), ('Image', args.image), ('initrd', args.initrd), ('dtb', args.dtb))]
    if not args.no_usb:
        # "usb" makes the loader stay resident and serve fastboot instead of
        # booting at once: the xnuloader iteration channel.
        blobs.append(('usb', b'1\0'))

    addrs, end = layout([(name, len(data)) for name, data in blobs])
    if end > PAYLOAD_MAX - PAYLOAD_RESERVED:
        die(f'payload would be {end} bytes, limit is {PAYLOAD_MAX - PAYLOAD_RESERVED}')

    with tempfile.TemporaryDirectory() as tmp:
        dtb = os.path.join(tmp, 'board.dtb')
        open(dtb, 'wb').write(dict(blobs)['dtb'])
        initrd_start = addrs['initrd']
        patch_dtb(dtb, initrd_start, initrd_start + len(dict(blobs)['initrd']), args.cmdline)
        patched_dtb = open(dtb, 'rb').read()
        if len(patched_dtb) > len(dict(blobs)['dtb']):
            # dtb is the last entry, so a growth only changes the payload size
            pass
        payload = build_payload([
            ('loader.efi', dict(blobs)['loader.efi']),
            ('Image', dict(blobs)['Image']),
            ('initrd', dict(blobs)['initrd']),
            ('dtb', patched_dtb),
            ('kernel.ver', b'picasso-mainline\0'),
        ] + ([] if args.no_usb else [('usb', b'1\0')]))
        if len(payload) > PAYLOAD_MAX - PAYLOAD_RESERVED:
            die(f'payload is {len(payload)} bytes, limit is {PAYLOAD_MAX - PAYLOAD_RESERVED}')
        if args.payload_out:
            open(args.payload_out, 'wb').write(payload)

        blob = open(args.bootshim, 'rb').read() + open(args.uefi_fd, 'rb').read() + payload
        blob += b'\0' * (-len(blob) % 16)
        kernel = os.path.join(tmp, 'kernel')
        with open(kernel, 'wb') as f:
            f.write(gzip.compress(blob, 6))
            f.write(open(args.uefi_dtb, 'rb').read())

        if args.uefi_ramdisk:
            ramdisk = args.uefi_ramdisk
        else:
            ramdisk = os.path.join(tmp, 'ramdisk')
            open(ramdisk, 'wb').write(b'\0' * 5)

        subprocess.run([
            'mkbootimg', '--header_version', '1',
            '--kernel', kernel, '--ramdisk', ramdisk,
            '--kernel_offset', args.kernel_offset,
            '-o', args.output,
        ], check=True)

    total = os.path.getsize(args.output)
    print(f'{args.output}: {total} bytes')
    print(f'  payload {len(payload)} / {PAYLOAD_MAX - PAYLOAD_RESERVED} bytes (XNUP at 0x{PAYLOAD_BASE:x})')
    for name in ('loader.efi', 'Image', 'initrd', 'dtb'):
        data = dict(blobs)[name] if name != 'dtb' else patched_dtb
        print(f'  {name:<10} {len(data):>10} bytes @ 0x{addrs[name]:x}')
    print(f'  initrd 0x{initrd_start:x}..0x{initrd_start + len(dict(blobs)["initrd"]):x} in /chosen')
    print(f'  mode: {"fastboot listener (usb entry)" if not args.no_usb else "one-shot boot"}')


if __name__ == '__main__':
    main()
