#!/usr/bin/env python3
"""Run a shell command on the kexec target via its ECM telnetd (10.42.0.2:23).

Usage: qkx_sh.py 'COMMAND' [timeout_seconds]  (env QKX_HOST overrides 10.42.0.2)
"""
import os
import socket
import sys
import time


def main():
    cmd = sys.argv[1]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 15
    marker = "QKX%dDONE" % os.getpid()
    s = socket.create_connection((os.environ.get("QKX_HOST", "10.42.0.2"), 23), timeout=5)
    s.settimeout(0.2)
    # Split the marker in the command so only real output contains it whole.
    s.sendall(('%s; echo QKX%d""DONE\n' % (cmd, os.getpid())).encode())
    buf = b""
    end = time.time() + limit
    while time.time() < end and marker.encode() not in buf:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            pass
    s.close()
    out = buf.replace(b"\xff\xfb\x01", b"").replace(b"\xff\xfb\x03", b"")
    sys.stdout.write(out.decode(errors="replace").split(marker)[0])


if __name__ == "__main__":
    main()
