#!/usr/bin/env python3
"""Talk to the picasso USB-ACM console of the RAM-booted mainline kernel.

The kernel's init opens a CDC-ACM gadget on /dev/ttyGS0, which the host sees as
/dev/ttyACM0. Everything here is read/echo only; `--reboot` asks the running
kernel to reboot, which lands back in the UEFI loader on this phone.

Usage:
  console_cmd.py "dmesg | tail"       # send one command, print the reply
  console_cmd.py "cmd" 20             # ... waiting 20 s for output
  console_cmd.py --reboot             # reboot the RAM kernel (back to the loader)
  console_cmd.py --capture            # stream the console to $OUT for $CAPTURE s
"""
import os
import select
import sys
import time
import termios

DEV = os.environ.get("CONSOLE_DEV", "/dev/ttyACM0")


def open_tty(dev=DEV):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    os.set_blocking(fd, False)
    return fd


def write_retry(fd, data, timeout=10.0):
    end = time.time() + timeout
    view = memoryview(data)
    while view and time.time() < end:
        try:
            n = os.write(fd, view)
            view = view[n:]
        except BlockingIOError:
            select.select([], [fd], [], 0.2)
        except OSError:
            break
    return not view


def drain(fd, seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 16384)
        except (BlockingIOError, OSError):
            break
        if not chunk:
            break
        out += chunk
    return out.decode(errors="replace")


def capture(fd, seconds, path):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    written = 0
    with open(path, "wb") as log:
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.3)
            if not r:
                continue
            try:
                chunk = os.read(fd, 16384)
            except (BlockingIOError, OSError):
                break
            if not chunk:
                break
            log.write(chunk)
            log.flush()
            written += len(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
    return written


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    try:
        fd = open_tty()
    except FileNotFoundError:
        print(f"no console at {DEV}")
        return 1
    drain(fd, 0.3)

    if args[0] == "--reboot":
        ok = write_retry(fd, b"\n") and write_retry(fd, b"reboot\n")
        print("reboot sent" if ok else "reboot NOT sent")
        return 0 if ok else 1

    if args[0] == "--capture":
        seconds = float(os.environ.get("CAPTURE", "30"))
        path = os.environ.get("OUT", "build/logs/console.log")
        n = capture(fd, seconds, path)
        print(f"\ncaptured {n} bytes to {path}", file=sys.stderr)
        return 0

    wait = float(args[1]) if len(args) > 1 else 8.0
    write_retry(fd, b"\n")
    write_retry(fd, args[0].encode() + b"\n")
    print(drain(fd, wait))
    return 0


if __name__ == "__main__":
    sys.exit(main())
