#!/usr/bin/env python3
import argparse
import ctypes
import fcntl
import os
import select
import sys


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


def main():
    parser = argparse.ArgumentParser(description="nxpnfc test tool")
    parser.add_argument("--dev", default="/dev/nxpnfc", help="device node")
    parser.add_argument(
        "--cmd",
        default="20 00 01 00",
        help="NCI command bytes in hex (default: Core Reset)",
    )
    parser.add_argument(
        "--read-len",
        type=int,
        default=260,
        help="max response bytes to read",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="read timeout seconds",
    )
    args = parser.parse_args()

    try:
        tx = hex_to_bytes(args.cmd)
    except ValueError as e:
        print(f"Invalid --cmd: {e}", file=sys.stderr)
        return 2

    fd = None
    try:
        fd = os.open(args.dev, os.O_RDWR | os.O_CLOEXEC)
        print(f"Opened {args.dev}")

        # Power ON NFCC
        fcntl.ioctl(fd, NFC_SET_PWR, NFC_POWER_ON)
        print("NFC power on")

        # Send NCI command
        written = os.write(fd, tx)
        print(f"Wrote {written} bytes: {tx.hex(' ')}")

        # Wait for response (driver read is blocking)
        rlist, _, _ = select.select([fd], [], [], args.timeout)
        if not rlist:
            print(f"Timeout waiting for response ({args.timeout}s)")
            return 1

        rx = os.read(fd, args.read_len)
        print(f"Read {len(rx)} bytes: {rx.hex(' ')}")

        return 0

    except PermissionError:
        print("Permission denied. Check udev/group for /dev/nxpnfc.", file=sys.stderr)
        return 13
    except FileNotFoundError:
        print(f"Device not found: {args.dev}", file=sys.stderr)
        return 2
    except OSError as e:
        print(f"OS error: {e}", file=sys.stderr)
        return 1
    finally:
        if fd is not None:
            try:
                fcntl.ioctl(fd, NFC_SET_PWR, NFC_POWER_OFF)
                print("NFC power off")
            except OSError:
                pass
            os.close(fd)


if __name__ == "__main__":
    sys.exit(main())