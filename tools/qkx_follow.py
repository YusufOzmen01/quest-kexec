#!/usr/bin/env python3
"""Stream the target's kernel log to the host over telnet.

Everything is written to a host file as it arrives, so the log survives a
target hang or reset. Usage: qkx_follow.py <output-file> [grep-pattern]
"""
import os
import socket
import sys
import time

out = open(sys.argv[1], "ab", buffering=0)
pattern = sys.argv[2] if len(sys.argv) > 2 else ""
cmd = "qkx-klogwatch" + (f" -g '{pattern}'" if pattern else "") + "\n"
s = socket.create_connection((os.environ.get("QKX_HOST", "10.42.0.2"), 23), timeout=10)
time.sleep(0.5)
s.sendall(b"stty -echo 2>/dev/null\n" + cmd.encode())
s.settimeout(None)
while True:
    try:
        data = s.recv(65536)
    except OSError:
        break
    if not data:
        break
    out.write(data)
out.write(b"\n[qkx_follow: connection closed]\n")
