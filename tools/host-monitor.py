#!/usr/bin/env python3
"""Record, from the host, exactly when the device disappears.

The UDP kernel log stops at the moment we lose either the device or the ECM
transport, and those look identical in the log. This samples three independent
signals with timestamps so they can be told apart afterwards:

  usb   every connected gadget's VID:PID, straight from sysfs
  net   ICMP reachability of the rescue/target address
  hdmesg the host's own USB stack messages (disconnect/reset/enumeration)

A device-side reset shows the gadget vanishing from sysfs. Losing only the
network leaves the gadget present while ICMP stops.

Usage: host-monitor.py <out.txt> [seconds] [target-ip]
"""
import os
import re
import subprocess
import sys
import time

SYS = "/sys/bus/usb/devices"


def gadgets():
    """Current set of VID:PID strings, read directly from sysfs."""
    out = set()
    try:
        names = os.listdir(SYS)
    except OSError:
        return out
    for name in names:
        if ":" in name:            # interfaces, not devices
            continue
        try:
            with open(f"{SYS}/{name}/idVendor") as f:
                vid = f.read().strip()
            with open(f"{SYS}/{name}/idProduct") as f:
                pid = f.read().strip()
        except OSError:
            continue
        if vid in ("1d6b", "2833", "18d1"):   # root hubs, Oculus, Google
            out.add(f"{name}={vid}:{pid}")
    return out


def host_usb_log():
    """Host kernel USB lines, best effort; needs no root on most systems."""
    try:
        d = subprocess.run(["dmesg", "--notime", "-l", "info,warn,err"],
                           capture_output=True, text=True, timeout=5)
        return [l for l in d.stdout.splitlines()
                if re.search(r"usb|cdc_ether|ecm", l, re.I)][-3:]
    except Exception:
        return []


def main():
    out_path = sys.argv[1]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
    target = sys.argv[3] if len(sys.argv) > 3 else "10.42.0.2"

    start = time.time()
    prev_usb, prev_net, prev_log = None, None, None
    with open(out_path, "w", buffering=1) as f:
        f.write(f"# host-monitor start={time.strftime('%F %T')} target={target}\n")
        while time.time() - start < limit:
            t = time.time() - start
            usb = gadgets()
            net = subprocess.run(["ping", "-c1", "-W1", target],
                                 capture_output=True).returncode == 0
            if usb != prev_usb:
                f.write(f"{t:8.3f} usb {' '.join(sorted(usb)) or '(none)'}\n")
                prev_usb = usb
            if net != prev_net:
                f.write(f"{t:8.3f} net {'UP' if net else 'DOWN'}\n")
                prev_net = net
            log = host_usb_log()
            if log != prev_log:
                for line in log:
                    f.write(f"{t:8.3f} hdmesg {line}\n")
                prev_log = log
            time.sleep(0.25)
        f.write(f"{time.time() - start:8.3f} monitor-end\n")


if __name__ == "__main__":
    main()
