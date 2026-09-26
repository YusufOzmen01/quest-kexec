#!/usr/bin/env bash
# Load the Android-side USB console logger and read it from the host.
# This module intentionally cannot be unloaded; reboot Android to remove it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SECONDS=${1:-60}
adb push "$HERE/module/usb_log.ko" /data/local/tmp/qkx_usb_log.ko >/dev/null
adb shell 'su -c "insmod /data/local/tmp/qkx_usb_log.ko"'
exec python3 "$HERE/tools/usb_log_host.py" --seconds "$SECONDS"
