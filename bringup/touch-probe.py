#!/usr/bin/env python3
"""Read a touchscreen evdev node and summarise what it reports.

The picasso panel's touch controller (Novatek NT36672C on GENI SPI SE7) talks to
the kernel fine, but that says nothing about whether real finger reports reach
userspace. This prints what actually arrives, so a port can be judged:

  python3 bringup/touch-probe.py [DEVICE] [SECONDS]

DEVICE defaults to the first /dev/input/event* whose name contains "NT36672",
SECONDS to 20. Output: per-event-type counts, the multitouch slot/position
ranges seen, and the BTN_TOUCH transitions - all you need to tell "no reports"
from "reports with wrong coordinates".
"""
import os
import select
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
BTN_TOUCH = 0x14A
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39
ABS_X, ABS_Y = 0x00, 0x01

# struct input_event on 64-bit: timeval (2x8) + type,code,value (3x2,2,4)
EVENT = struct.Struct('@qqHHi')


def find_device():
    for name in sorted(os.listdir('/dev/input')):
        if not name.startswith('event'):
            continue
        path = os.path.join('/dev/input', name)
        try:
            with open(f'/sys/class/input/{name}/device/name') as f:
                label = f.read().strip()
        except OSError:
            continue
        if 'NT36672' in label or 'Novatek' in label:
            return path, label
    return None, None


def main():
    args = sys.argv[1:]
    dev = None
    secs = 20.0
    for a in args:
        if a.replace('.', '', 1).isdigit():
            secs = float(a)
        else:
            dev = a
    if dev is None:
        dev, label = find_device()
        if dev is None:
            print('no NT36672/Novatek input device found')
            return 1
        print(f'device: {dev} ({label})')
    if not os.path.exists(dev):
        print(f'{dev} does not exist')
        return 1

    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    counts = {}
    slots = set()
    xs, ys = [], []
    touch_states = []
    reports = 0
    end = time.time() + secs
    print(f'reading {dev} for {secs:.0f}s ...')
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            data = os.read(fd, EVENT.size * 256)
        except (BlockingIOError, OSError):
            continue
        for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
            _s, _us, etype, code, value = EVENT.unpack_from(data, off)
            counts[etype] = counts.get(etype, 0) + 1
            if etype == EV_SYN and code == SYN_REPORT:
                reports += 1
            elif etype == EV_KEY and code == BTN_TOUCH:
                touch_states.append(value)
            elif etype == EV_ABS:
                if code == ABS_MT_SLOT:
                    slots.add(value)
                elif code == ABS_MT_POSITION_X:
                    xs.append(value)
                elif code == ABS_MT_POSITION_Y:
                    ys.append(value)
                elif code == ABS_X:
                    xs.append(value)
                elif code == ABS_Y:
                    ys.append(value)

    print(f'reports(SYN_REPORT): {reports}')
    print(f'event counts: {counts}')
    print(f'slots seen: {sorted(slots)[:8]}{" ..." if len(slots) > 8 else ""}')
    if xs:
        print(f'X range: {min(xs)}..{max(xs)} ({len(xs)} samples)')
    if ys:
        print(f'Y range: {min(ys)}..{max(ys)} ({len(ys)} samples)')
    print(f'BTN_TOUCH transitions: {touch_states[:12]}')
    if reports == 0:
        print('RESULT: no touch reports arrived')
        return 2
    print('RESULT: touch reports arrived')
    return 0


if __name__ == '__main__':
    sys.exit(main())
