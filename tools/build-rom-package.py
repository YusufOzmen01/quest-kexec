#!/usr/bin/env python3
"""Build a ROM-only ZIP consumed by the standalone QKX loader package."""
import argparse, hashlib, json, os, zipfile
from pathlib import Path
from board import load_board

FILES = {f'images/{n}.img': f'{n}.img' for n in
         ('system', 'system_ext', 'vendor', 'odm', 'product')}

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('images', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--board', default='seacliff')
    a = p.parse_args()
    board = load_board(a.board)
    sources = {arc: (a.images / name).resolve() for arc, name in FILES.items()}
    sources['qkx-turnkey.json'] = (a.images / 'qkx-turnkey.json').resolve()
    for arc, path in sources.items():
        if not path.is_file():
            raise SystemExit(f'missing {arc}: {path}')
    manifest = {'format': 'qkx-rom-package-v1', 'device': board['QKX_BOARD_LABEL'],
                'calibration': 'read-current-stock-at-each-boot', 'files': {}}
    for arc, path in sources.items():
        manifest['files'][arc] = {'size': path.stat().st_size,
                                  'sha256': digest(path)}
        print('HASHED', arc)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    tmp = a.output.with_suffix(a.output.suffix + '.tmp')
    tmp.unlink(missing_ok=True)
    with zipfile.ZipFile(tmp, 'w', allowZip64=True) as z:
        z.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n',
                   compress_type=zipfile.ZIP_DEFLATED)
        for arc, path in sources.items():
            z.write(path, arc, compress_type=zipfile.ZIP_DEFLATED, compresslevel=3)
    os.replace(tmp, a.output)
    print(a.output, digest(a.output))

if __name__ == '__main__':
    main()
