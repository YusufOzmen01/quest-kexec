#!/usr/bin/env python3
"""Create alternate-only turnkey images from the known-working image set.
Source images remain untouched. Calibration/user data are never included.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
NAMES = ('system', 'system_ext', 'vendor', 'odm', 'product')
REMOVE = {
 'system': ['/system/etc/init/update_engine.rc', '/system/bin/update_engine',
            '/system/bin/update_engine_client', '/system/bin/update_verifier', '/system/bin/postinstall'],
 'system_ext': ['/etc/init/update_engine_gold.rc', '/priv-app/OSUpdater/OSUpdater.apk',
                '/priv-app/NuxOta/NuxOta.apk', '/priv-app/CMSHeadset/CMSHeadset.apk',
                '/priv-app/FirstTimeNuxSeacliff/FirstTimeNuxSeacliff.apk'],
}


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def fsck(image, *opts):
    p = subprocess.run(['e2fsck', *opts, str(image)], capture_output=True, text=True)
    if p.returncode not in ((0,) if '-n' in opts or '-fn' in opts else (0, 1)):
        raise RuntimeError(p.stdout + p.stderr)


def debug(image, command, write=False):
    p = subprocess.run(['debugfs', *(['-w'] if write else []), '-R', command, str(image)],
                       capture_output=True, text=True)
    result = p.stdout + p.stderr
    if p.returncode or any(x in result.lower() for x in
                          ['could not allocate', 'filesystem full', 'no space']):
        raise RuntimeError(result)
    return result


def exists(image, path):
    return 'File not found' not in debug(image, 'stat ' + path)


def put(image, source, dest, tmp, mode=0o644):
    if exists(image, dest):
        debug(image, 'rm ' + dest, True)
    debug(image, f'write {source} {dest}', True)
    debug(image, f'set_inode_field {dest} mode {(0o100000 | mode):07o}', True)
    debug(image, f'set_inode_field {dest} uid 0', True)
    debug(image, f'set_inode_field {dest} gid 0', True)
    debug(image, f'ea_set -f {tmp / "label"} {dest} security.selinux', True)
    verify = tmp / 'verify'
    verify.unlink(missing_ok=True)
    debug(image, f'dump {dest} {verify}')
    if verify.read_bytes() != source.read_bytes():
        raise RuntimeError('Image byte verification failed: ' + dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--faceeye-request', type=Path, required=True)
    a = ap.parse_args()
    if a.output.exists():
        raise SystemExit('Refusing to overwrite output directory')
    for n in NAMES:
        if not (a.source / (n + '.img')).resolve().is_file():
            raise SystemExit('Missing source image: ' + n)
    if not a.faceeye_request.is_file():
        raise SystemExit('Build faceeye-request.cpp for Android ARM64 first')
    a.output.mkdir(parents=True)
    manifest = {'version': 1, 'stock_calibration_required': True,
                'source_images': {}, 'quarantined': [], 'images': {}}
    with tempfile.TemporaryDirectory(prefix='qkx-turnkey-') as temp:
        tmp = Path(temp)
        (tmp / 'label').write_bytes(b'u:object_r:system_file:s0\0')
        for n in NAMES:
            src = (a.source / (n + '.img')).resolve()
            image = a.output / (n + '.img')
            run('cp', '--reflink=auto', '--sparse=always', str(src), str(image))
            manifest['source_images'][n] = str(src)
            if n in REMOVE or n == 'product':
                with image.open('r+b') as f:
                    f.truncate(image.stat().st_size + (64 << 20))
                fsck(image, '-fy')
                run('resize2fs', str(image))
                fsck(image, '-fy', '-E', 'unshare_blocks')
                if n in REMOVE:
                    if not exists(image, '/qkx-disabled'):
                        debug(image, 'mkdir /qkx-disabled', True)
                    for path in REMOVE[n]:
                        if not exists(image, path):
                            continue  # Already quarantined by the baseline.
                        original = tmp / 'original'
                        original.unlink(missing_ok=True)
                        debug(image, f'dump {path} {original}')
                        dest = '/qkx-disabled/' + path.replace('/', '_') + '.disabled'
                        put(image, original, dest, tmp, 0o600)
                        debug(image, 'rm ' + path, True)
                        if exists(image, path):
                            raise RuntimeError('Quarantine failed: ' + path)
                        manifest['quarantined'].append(n + ':' + path)
                if n == 'system':
                    put(image, HERE / 'turnkey/qkx-defaults', '/system/bin/qkx-defaults', tmp, 0o755)
                    put(image, HERE / 'turnkey/qkx-defaults.rc', '/system/etc/init/qkx-defaults.rc', tmp)
                    put(image, HERE / 'turnkey/qkx-start-keystore', '/system/bin/qkx-start-keystore', tmp, 0o755)
                    put(image, HERE / 'turnkey/qkx-early-keystore.rc', '/system/etc/init/qkx-early-keystore.rc', tmp)
                    put(image, a.faceeye_request, '/system/bin/qkx-faceeye-request', tmp, 0o755)
                elif n == 'product':
                    put(image, HERE / 'turnkey/QkxFrameworkOverlay.apk',
                        '/overlay/QkxFrameworkOverlay.apk', tmp)
            fsck(image, '-fn')
            manifest['images'][n] = hashlib.file_digest(image.open('rb'), 'sha256').hexdigest()
            print('VERIFIED/CLEAN', image)
    (a.output / 'qkx-turnkey.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
