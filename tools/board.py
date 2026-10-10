#!/usr/bin/env python3
"""Read a boards/<name>.sh descriptor by sourcing it through tools/board.sh, so
the Python tools and the shell tools agree on one source of truth."""
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
KEYS = ('QKX_BOARD', 'QKX_BOARD_LABEL', 'QKX_USB_MANUFACTURER', 'QKX_SYNCBOSS_SPI',
        'QKX_SPLASH_NODE', 'QKX_SPLASH_BASE', 'QKX_SPLASH_SIZE', 'QKX_DISABLE_NODES',
        'QKX_NUX_APK', 'QKX_KERNEL_LOCALVERSION')


def load_board(name):
    """Return {key: value} for boards/<name>.sh. Raises SystemExit if unknown."""
    emit = '; '.join(f'printf "%s\\0" "${{{k}:-}}"' for k in KEYS)
    script = f'. "$1"; qkx_board_load "$2" || exit 1; {emit}'
    r = subprocess.run(['bash', '-c', script, 'bash', str(REPO / 'tools/board.sh'), name],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(r.stderr.strip() or f'unknown board {name!r}')
    return dict(zip(KEYS, r.stdout.split('\0')))


if __name__ == '__main__':
    import sys
    b = load_board(sys.argv[1] if len(sys.argv) > 1 else 'seacliff')
    for k in KEYS:
        print(f'{k}={b[k]}')
