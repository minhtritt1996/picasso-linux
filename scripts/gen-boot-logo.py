#!/usr/bin/env python3
"""Rasterise an SVG into initramfs/boot_logo.h (run-length encoded, 0xRRGGBB).

usage: gen-boot-logo.py LOGO.svg [OUT.h] [--size WxH] [--bg RRGGBB]

Needs ImageMagick (`convert`) and Pillow. Example (Denial's wordmark):
  gen-boot-logo.py denial-dark.svg
"""
import argparse
import os
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('svg')
    ap.add_argument('out', nargs='?', default=os.path.join(ROOT, 'initramfs', 'boot_logo.h'))
    ap.add_argument('--size', default='588x180')
    ap.add_argument('--bg', default='0c0b10')
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, 'logo.png')
        subprocess.run(['convert', '-background', f'#{args.bg}', '-density', '120', args.svg,
                        '-resize', args.size + '!', '-depth', '8', png], check=True)
        im = Image.open(png).convert('RGB')
    w, h = im.size
    pix = [(r << 16) | (g << 8) | b for r, g, b in im.getdata()]
    runs, cur, cnt = [], pix[0], 1
    for p in pix[1:]:
        if p == cur and cnt < 65535:
            cnt += 1
        else:
            runs.append((cnt, cur))
            cur, cnt = p, 1
    runs.append((cnt, cur))
    with open(args.out, 'w') as f:
        f.write(f'#define BOOT_LOGO_BG 0x{int(args.bg, 16):08x}U\n')
        f.write(f'#define BOOT_LOGO_W {w}\n#define BOOT_LOGO_H {h}\n')
        f.write('static const struct { unsigned short count; unsigned int rgb; } boot_logo_rle[] = {\n')
        for i, (c, v) in enumerate(runs):
            f.write(f'{{{c},0x{v:06x}}},')
            if (i + 1) % 8 == 0:
                f.write('\n')
        f.write('\n};\n')
    print(f'{args.out}: {w}x{h}, {len(runs)} runs', file=sys.stderr)


if __name__ == '__main__':
    main()
