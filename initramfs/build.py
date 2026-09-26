#!/usr/bin/env python3
"""Pack a static ARM64 BusyBox and init.sh into a gzip newc initramfs."""
import argparse
import gzip
import stat
import struct
from pathlib import Path


def archive(entries):
    data = bytearray()
    for ino, (name, mode, contents, major, minor) in enumerate(entries, 1):
        name = name.encode() + b"\0"
        fields = (ino, mode, 0, 0, 1, 0, len(contents), 0, 0, major, minor, len(name), 0)
        data += b"070701" + "".join(f"{x:08x}" for x in fields).encode() + name
        data += b"\0" * (-len(data) % 4)
        data += contents
        data += b"\0" * (-len(data) % 4)
    data += b"\0" * (-len(data) % 512)
    return bytes(data)


def check_static_arm64(path):
    b = path.read_bytes()
    if b[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", b, 18)[0] != 183:
        raise SystemExit(f"{path}: not a little-endian ARM64 ELF")
    off = struct.unpack_from("<Q", b, 32)[0]
    size, count = struct.unpack_from("<HH", b, 54)
    if any(struct.unpack_from("<I", b, off + i * size)[0] == 3 for i in range(count)):
        raise SystemExit(f"{path}: must be statically linked")
    return b


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--busybox", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--init", type=Path, default=Path(__file__).with_name("init.sh"))
    ap.add_argument("--extra", action="append", default=[], metavar="SRC:DEST",
                    help="add a static ARM64 binary, e.g. modeset:bin/modeset")
    a = ap.parse_args()
    dirs = ("bin", "sbin", "dev", "dev/pts", "proc", "sys", "config", "etc", "root", "tmp")
    entries = [(d, stat.S_IFDIR | 0o755, b"", 0, 0) for d in dirs]
    entries += [
        ("bin/busybox", stat.S_IFREG | 0o755, check_static_arm64(a.busybox), 0, 0),
        ("init", stat.S_IFREG | 0o755, a.init.read_bytes(), 0, 0),
        ("dev/console", stat.S_IFCHR | 0o600, b"", 5, 1),
        ("dev/kmsg", stat.S_IFCHR | 0o600, b"", 1, 11),
        ("dev/null", stat.S_IFCHR | 0o666, b"", 1, 3),
        ("etc/passwd", stat.S_IFREG | 0o644, b"root::0:0:root:/root:/bin/sh\n", 0, 0),
        ("etc/group", stat.S_IFREG | 0o644, b"root:x:0:\n", 0, 0),
    ]
    for spec in a.extra:
        src, dest = spec.split(":", 1)
        entries.append((dest, stat.S_IFREG | 0o755, check_static_arm64(Path(src)), 0, 0))
    entries.append(("TRAILER!!!", 0, b"", 0, 0))
    data = gzip.compress(archive(entries), mtime=0)
    if len(data) > 0x200000:
        raise SystemExit(f"initramfs is {len(data)} bytes; the staging layout allows 2 MiB")
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(data)
    print(f"{a.output}: {len(data)} bytes")


if __name__ == "__main__":
    main()
