#!/usr/bin/env python3
"""Read the Quest's resident EP0 console; no ADB, interface claim or detach.

Uses libusb through ctypes. Output includes host timestamps and drop counters.
"""
import argparse
import ctypes as c
import ctypes.util
import sys
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--output', help='Host log file (defaults to stdout)')
    ap.add_argument('--vid', type=lambda s: int(s, 0), default=0x2833)
    ap.add_argument('--pid', type=lambda s: int(s, 0), default=0x5013)
    a = ap.parse_args()
    if a.output:
        sys.stdout = open(a.output, 'w', buffering=1)
    lib = c.CDLL(ctypes.util.find_library('usb-1.0'))
    lib.libusb_init.argtypes = [c.POINTER(c.c_void_p)]
    lib.libusb_init.restype = c.c_int
    lib.libusb_exit.argtypes = [c.c_void_p]
    lib.libusb_open_device_with_vid_pid.argtypes = [c.c_void_p, c.c_uint16, c.c_uint16]
    lib.libusb_open_device_with_vid_pid.restype = c.c_void_p
    lib.libusb_close.argtypes = [c.c_void_p]
    lib.libusb_control_transfer.argtypes = [c.c_void_p, c.c_uint8, c.c_uint8,
                                          c.c_uint16, c.c_uint16, c.c_void_p,
                                          c.c_uint16, c.c_uint]
    lib.libusb_control_transfer.restype = c.c_int
    context = c.c_void_p()
    if lib.libusb_init(c.byref(context)):
        raise RuntimeError('libusb initialization failed')
    handle = None
    try:
        handle = lib.libusb_open_device_with_vid_pid(context, a.vid, a.pid)
        if not handle:
            raise RuntimeError('Quest USB device unavailable or permission denied')
        buf = c.create_string_buffer(1024)
        deadline = time.monotonic() + a.seconds
        while time.monotonic() < deadline:
            n = lib.libusb_control_transfer(handle, 0xc0, 0x5a, 0x514b,
                                            0x584c, buf, 1024, 1000)
            if n < 0:
                print(f'{time.time():.6f} USB error {n}', flush=True)
                if n == -7:  # Timeout is not proof of a disconnect/reset.
                    continue
                return 1
            if n < 4:
                raise RuntimeError(f'short console header: {n}')
            dropped = int.from_bytes(buf.raw[:4], 'little')
            if n > 4:
                print(f'[{time.time():.6f} dropped={dropped}]', flush=True)
                sys.stdout.write(buf.raw[4:n].decode('utf-8', errors='replace'))
                sys.stdout.flush()
            else:
                time.sleep(0.01)
    finally:
        if handle:
            lib.libusb_close(handle)
        lib.libusb_exit(context)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
