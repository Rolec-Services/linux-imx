#!/usr/bin/env python3

# The script is now an asyncio-based continuous NCI frame monitor in nxpnfc.py.
#
# What it now does:
#
# 1 .Opens /dev/nxpnfc in non-blocking mode.
# 2. Optionally powers on the NFCC at startup.
# 3. Registers an async reader and continuously prints incoming frames with timestamp, frame counter, and length.
# 4. Handles Ctrl+C / SIGTERM cleanly.
# 5. Optionally powers off on exit.
# 6. Still supports optional one-time startup TX command via --cmd.
#
# Usage examples:
#
# 1. Continuous read only:
#   python3 nxpnfc.py
#
# 2. Send one command once, then keep listening:
#   python3 nxpnfc.py --cmd "20 00 01 00"
#
# 3. Keep power state untouched:
#   python3 nxpnfc.py --no-power-on --no-power-off
 
import asyncio
import argparse
import ctypes
import fcntl
import errno
import os
import signal
import sys
from datetime import datetime


# Linux ioctl encoding (asm-generic/ioctl.h)
_IOC_NRBITS = 8
_IOC_TYPEBITS = 8
_IOC_SIZEBITS = 14
_IOC_DIRBITS = 2

_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS

_IOC_WRITE = 1


def _IOC(direction, ioc_type, nr, size):
    return (
        (direction << _IOC_DIRSHIFT)
        | (ioc_type << _IOC_TYPESHIFT)
        | (nr << _IOC_NRSHIFT)
        | (size << _IOC_SIZESHIFT)
    )

def _IOW(ioc_type, nr, ctype):
    return _IOC(_IOC_WRITE, ioc_type, nr, ctypes.sizeof(ctype))

NFC_MAGIC = 0xE9
NFC_SET_PWR = _IOW(NFC_MAGIC, 0x01, ctypes.c_uint32)

NFC_POWER_OFF = 0
NFC_POWER_ON = 1

def hex_to_bytes(hex_string):
    cleaned = hex_string.replace(",", " ").strip()
    if not cleaned:
        raise ValueError("empty command")
    return bytes(int(x, 16) for x in cleaned.split())

def ts():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


async def run_reader(args):
    fd = None
    stop_event = asyncio.Event()
    frame_count = 0

    def on_signal(*_):
        stop_event.set()

    def on_readable():
        nonlocal frame_count
        try:
            rx = os.read(fd, args.read_len)
        except BlockingIOError:
            return
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                return
            print(f"[{ts()}] Read error: {e}", file=sys.stderr)
            stop_event.set()
            return

        if not rx:
            print(f"[{ts()}] EOF from device, stopping.")
            stop_event.set()
            return

        frame_count += 1
        print(f"[{ts()}] RX[{frame_count}] len={len(rx)}: {rx.hex(' ')}")

    loop = asyncio.get_running_loop()

    try:
        fd = os.open(args.dev, os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK)
        print(f"[{ts()}] Opened {args.dev}")

        if not args.no_power_on:
            fcntl.ioctl(fd, NFC_SET_PWR, NFC_POWER_ON)
            print(f"[{ts()}] NFC power on")

        if args.cmd:
            tx = hex_to_bytes(args.cmd)
            written = os.write(fd, tx)
            print(f"[{ts()}] TX len={written}: {tx.hex(' ')}")

        loop.add_reader(fd, on_readable)

        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, on_signal)
            except NotImplementedError:
                pass

        print(f"[{ts()}] Listening for incoming NCI frames. Press Ctrl+C to stop.")
        await stop_event.wait()
        return 0

    except PermissionError:
        print("Permission denied. Check udev/group for /dev/nxpnfc.", file=sys.stderr)
        return 13
    except FileNotFoundError:
        print(f"Device not found: {args.dev}", file=sys.stderr)
        return 2
    except ValueError as e:
        print(f"Invalid --cmd: {e}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 0
    except OSError as e:
        print(f"OS error: {e}", file=sys.stderr)
        return 1
    finally:
        if fd is not None:
            try:
                loop.remove_reader(fd)
            except Exception:
                pass
            if not args.no_power_off:
                try:
                    fcntl.ioctl(fd, NFC_SET_PWR, NFC_POWER_OFF)
                    print(f"[{ts()}] NFC power off")
                except OSError:
                    pass
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description="nxpnfc test tool")
    parser.add_argument("--dev", default="/dev/nxpnfc", help="device node")
    parser.add_argument(
        "--cmd",
        default="",
        help="optional NCI command bytes in hex to send once at startup",
    )
    parser.add_argument(
        "--read-len",
        type=int,
        default=260,
        help="max response bytes to read",
    )
    parser.add_argument("--no-power-on", action="store_true", help="do not send NFC_POWER_ON")
    parser.add_argument("--no-power-off", action="store_true", help="do not send NFC_POWER_OFF on exit")
    args = parser.parse_args()
    return asyncio.run(run_reader(args))


if __name__ == "__main__":
    sys.exit(main())