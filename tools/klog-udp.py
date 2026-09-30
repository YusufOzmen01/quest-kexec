#!/usr/bin/env python3
"""Receive the target's /dev/kmsg UDP stream (see qkx-switch) into a file.

Usage: klog-udp.py <output-file> [port]

Records are "prio,seq,usec,flags;text". A restarted reader on the target
replays the whole backlog, so records whose sequence number was already seen
are dropped. Every write is flushed: the target may die at any moment.
"""
import socket
import sys

out = open(sys.argv[1], "a", buffering=1, errors="replace")
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5140
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", port))
seen = set()
partial = ""
while True:
    data, _ = sock.recvfrom(65536)
    partial += data.decode("utf-8", "replace")
    *lines, partial = partial.split("\n")
    for line in lines:
        head = line.split(";", 1)[0].split(",")
        if len(head) >= 2 and head[1].isdigit():
            seq = int(head[1])
            if seq in seen:
                continue
            seen.add(seq)
        out.write(line + "\n")
