#!/usr/bin/env python3
"""Restore the original node order of a captured runtime device tree.

capture.sh rebuilds the runtime tree (with the bootloader's overlays applied)
from a copy of /proc/device-tree using `dtc -I fs`. dtc reads that copy in the
host filesystem's directory order, which is effectively a hash, so every
node's children end up shuffled. Order is not cosmetic on this kernel:

  * /cpus order sets the logical CPU numbers (CPU1 became the prime core),
    which breaks Android's cluster assumptions, thermal cooling-device names
    and CPU affinities;
  * drivers such as kgsl search forward from their own node for siblings.

This keeps the runtime tree's content and reorders children and properties
to follow the boot FDT (/sys/firmware/fdt). Nodes or properties that exist
only in the runtime tree keep their relative order after the known ones.

Usage: dt-reorder.py <runtime.dtb> <boot.fdt> <out.dtb>
"""
import struct
import sys

FDT_MAGIC = 0xd00dfeed
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name):
        self.name = name
        self.props = []      # [(name, bytes)]
        self.children = []   # [Node]


def parse(blob):
    (magic, totalsize, off_struct, off_strings, off_rsvmap, version,
     last_comp, boot_cpuid, size_strings, size_struct) = struct.unpack_from('>10I', blob, 0)
    if magic != FDT_MAGIC:
        raise SystemExit('not an FDT')
    strings = blob[off_strings:off_strings + size_strings]

    def string_at(off):
        return strings[off:strings.index(b'\0', off)].decode()

    rsv = []
    p = off_rsvmap
    while True:
        addr, size = struct.unpack_from('>QQ', blob, p)
        p += 16
        if addr == 0 and size == 0:
            break
        rsv.append((addr, size))

    p = off_struct
    stack = []
    root = None
    while True:
        tok, = struct.unpack_from('>I', blob, p)
        p += 4
        if tok == BEGIN_NODE:
            end = blob.index(b'\0', p)
            node = Node(blob[p:end].decode())
            p = (end + 1 + 3) & ~3
            if stack:
                stack[-1].children.append(node)
            else:
                root = node
            stack.append(node)
        elif tok == END_NODE:
            stack.pop()
        elif tok == PROP:
            length, nameoff = struct.unpack_from('>II', blob, p)
            p += 8
            stack[-1].props.append((string_at(nameoff), blob[p:p + length]))
            p = (p + length + 3) & ~3
        elif tok == NOP:
            continue
        elif tok == END:
            break
        else:
            raise SystemExit(f'bad token {tok} at {p - 4}')
    return root, rsv, boot_cpuid, last_comp


def reorder(node, ref):
    """Order node's props/children like ref's; recurse into matching children."""
    if ref is None:
        for c in node.children:
            reorder(c, None)
        return
    prop_rank = {n: i for i, (n, _) in enumerate(ref.props)}
    node.props.sort(key=lambda pv: prop_rank.get(pv[0], len(prop_rank)))
    child_rank = {c.name: i for i, c in enumerate(ref.children)}
    ref_by_name = {c.name: c for c in ref.children}
    # sort() is stable, so unknown children keep their relative order.
    node.children.sort(key=lambda c: child_rank.get(c.name, len(child_rank)))
    for c in node.children:
        reorder(c, ref_by_name.get(c.name))


def serialize(root, rsv, boot_cpuid, last_comp):
    strings = bytearray()
    string_off = {}

    def sref(name):
        if name not in string_off:
            string_off[name] = len(strings)
            strings.extend(name.encode() + b'\0')
        return string_off[name]

    st = bytearray()

    def pad():
        while len(st) % 4:
            st.append(0)

    def emit(node):
        st.extend(struct.pack('>I', BEGIN_NODE))
        st.extend(node.name.encode() + b'\0')
        pad()
        for name, val in node.props:
            st.extend(struct.pack('>III', PROP, len(val), sref(name)))
            st.extend(val)
            pad()
        for c in node.children:
            emit(c)
        st.extend(struct.pack('>I', END_NODE))

    emit(root)
    st.extend(struct.pack('>I', END))

    rsvmap = b''.join(struct.pack('>QQ', a, s) for a, s in rsv) + struct.pack('>QQ', 0, 0)
    off_rsvmap = 40
    off_rsvmap = (off_rsvmap + 7) & ~7
    off_struct = off_rsvmap + len(rsvmap)
    off_strings = off_struct + len(st)
    totalsize = off_strings + len(strings)
    hdr = struct.pack('>10I', FDT_MAGIC, totalsize, off_struct, off_strings, off_rsvmap,
                      17, last_comp, boot_cpuid, len(strings), len(st))
    out = bytearray(hdr)
    out.extend(b'\0' * (off_rsvmap - len(out)))
    out.extend(rsvmap)
    out.extend(st)
    out.extend(strings)
    return bytes(out)


def count(node):
    return 1 + sum(count(c) for c in node.children)


def main():
    runtime, rsv, boot_cpuid, last_comp = parse(open(sys.argv[1], 'rb').read())
    boot, _, _, _ = parse(open(sys.argv[2], 'rb').read())
    before = count(runtime)
    reorder(runtime, boot)
    assert count(runtime) == before
    open(sys.argv[3], 'wb').write(serialize(runtime, rsv, boot_cpuid, last_comp))
    cpus = next((c for c in runtime.children if c.name == 'cpus'), None)
    if cpus:
        print('cpus:', ' '.join(c.name for c in cpus.children))
    print(f'reordered {before} nodes')


if __name__ == '__main__':
    main()
