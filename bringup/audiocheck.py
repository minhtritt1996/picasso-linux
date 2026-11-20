#!/usr/bin/env python3
"""Console-side audio probe after the flat-TLMM change.

Reads the flat TLMM registers for the amp reset (gpio58) and the sec MI2S pins
(70/71/72/74), checks the sound card and the amp probe in dmesg, then plays the
tone.  Everything goes through the USB-ACM console of the RAM-booted kernel.
"""
import os
import select
import sys
import termios
import time

DEV = os.environ.get("ACM", "/dev/ttyACM0")


def open_console():
    fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)

    a[2] |= termios.CLOCAL | termios.CREAD
    a[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    os.set_blocking(fd, False)
    return fd


def cmd(fd, line, wait=6.0):
    end = time.time() + 5
    v = memoryview(b"\n" + line.encode() + b"\n")
    while v and time.time() < end:
        try:
            v = v[os.write(fd, v):]
        except BlockingIOError:
            select.select([], [fd], [], 0.2)

    out = b""
    end = time.time() + wait
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if r:
            try:
                d = os.read(fd, 16384)
                if not d:
                    break
                out += d
            except (BlockingIOError, OSError):
                break
    return out.decode(errors="replace")


def main():
    fd = open_console()

    for line in ("mmio 0xf13a000 2", "mmio 0xf146000 1", "mmio 0xf147000 1",
                 "mmio 0xf148000 1", "mmio 0xf14a000 1"):
        sys.stdout.write("## %s\n%s\n" % (line, cmd(fd, line, 5.0)[-300:]))

    sys.stdout.write("## dmesg tfa\n%s\n" % cmd(fd, "dmesg tfa", 6.0)[-700:])
    sys.stdout.write("## dmesg sec\n%s\n"
                     % cmd(fd, "dmesg sec-mi2s", 6.0)[-400:])
    sys.stdout.write("## tone\n%s\n" % cmd(fd, "tone", 10.0)[-400:])


if __name__ == "__main__":
    main()
