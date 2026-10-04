#!/usr/bin/env python3
"""Read current stock eye calibration and add a private, volatile boot handoff.
No stock settings are written. Never put the resulting archive in source control.
"""
import argparse
import gzip
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path)
    ap.add_argument('output', type=Path)
    a = ap.parse_args()
    p = subprocess.run(['adb', 'exec-out', 'su', '-c',
        '/system_ext/bin/oculussetting --get eye_tracking_calibration'],
        capture_output=True, timeout=30)
    if p.returncode:
        raise SystemExit('Cannot read stock eye calibration; refusing launch')
    text = p.stdout.decode('utf-8')
    try:
        start = text.index('{')
        obj, end = json.JSONDecoder().raw_decode(text[start:])
        if not isinstance(obj, dict) or not obj.get('UserSpecificGazeParams'):
            raise ValueError('Missing stock gaze parameters')
        calibration = text[start:start + end].encode('utf-8')
    except (ValueError, UnicodeError):
        raise SystemExit('Invalid/missing stock eye calibration; refusing launch')
    spec = importlib.util.spec_from_file_location('qkx_archive',
        Path(__file__).resolve().parents[1] / 'initramfs/build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    extension = module.archive([
        ('qkx-stock-eye-calibration.json', stat.S_IFREG | 0o600, calibration, 0, 0),
        ('TRAILER!!!', 0, b'', 0, 0)])
    data = a.source.read_bytes() + gzip.compress(extension, mtime=0)
    if len(data) > 0x200000:
        raise SystemExit('Calibration handoff exceeds initramfs staging capacity')
    fd = os.open(a.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)
    os.chmod(a.output, 0o600)
    print('Current stock calibration captured (contents not logged)')


if __name__ == '__main__':
    main()
