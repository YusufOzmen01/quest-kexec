#!/usr/bin/env bash
# Ask the target kernel for a DRAM-preserving warm reset back to Android.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
exec python3 "$HERE/qkx_sh.py" 'echo r > /proc/qkx_bootdone' 5
