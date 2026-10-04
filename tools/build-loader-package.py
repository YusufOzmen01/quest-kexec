#!/usr/bin/env python3
"""Build a standalone Quest Pro QKX loader bundle (no ROM or private data)."""
import argparse, hashlib, os, stat, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOP = ('qkx-install-package.sh', 'qkx-boot.sh', 'qkx-uninstall.sh',
       'README.md', 'docs/TURNKEY_PACKAGE.md')
TOOLS = ('capture.sh', 'os-install.sh', 'os-install-modemfw.sh', 'os-prep.sh',
         'prep.sh', 'prepare.py', 'run.sh', 'stock-calibration-initramfs.py')

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(8 << 20), b''):
            h.update(b)
    return h.hexdigest()

def add_tree(items, directory, prefix):
    for p in sorted(directory.rglob('*')):
        if p.is_file() and '__pycache__' not in p.parts and not p.name.endswith(('.o','.ko','.cmd','.mod','.mod.c')):
            items[prefix + '/' + p.relative_to(directory).as_posix()] = p

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--busybox', type=Path, default=ROOT/'out/busybox')
    a = ap.parse_args()
    items = {name: ROOT/name for name in TOP}
    items.update({'tools/'+name: ROOT/'tools'/name for name in TOOLS})
    add_tree(items, ROOT/'initramfs', 'initramfs')
    add_tree(items, ROOT/'tools/device', 'tools/device')
    items.update({
        'runtime/Image': a.kernel.resolve(),
        'runtime/busybox': a.busybox.resolve(),
        'runtime/quest_kexec.ko': ROOT/'module/quest_kexec.ko',
        'runtime/ion_secmap.ko': ROOT/'module/ion_secmap.ko',
        'runtime/marker_read.ko': ROOT/'module/marker_read.ko',
    })
    for arc, path in items.items():
        if not path.is_file(): raise SystemExit(f'missing {arc}: {path}')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    tmp = a.output.with_suffix(a.output.suffix+'.tmp'); tmp.unlink(missing_ok=True)
    with zipfile.ZipFile(tmp, 'w', allowZip64=True) as z:
        for arc, path in sorted(items.items()):
            info = zipfile.ZipInfo('qkx-quest-pro-loader/'+arc)
            info.external_attr = ((path.stat().st_mode & 0xffff) << 16)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, path.read_bytes(), compresslevel=6)
    os.replace(tmp, a.output)
    print(a.output, digest(a.output))

if __name__ == '__main__': main()
