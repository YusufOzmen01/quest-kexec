#!/usr/bin/env bash
# Capture the device-specific inputs needed by prepare.py (read-only on device).
# Usage: capture.sh <output-dir>
set -euo pipefail
O=$1; mkdir -p "$O"
su() { adb shell "su -c \"$1\""; }
# /sys/firmware/fdt is the original boot FDT. Kexec needs the kernel's
# runtime tree after overlays/fixups; using the boot FDT caused a cold reset.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb exec-out 'su -c "tar -C /proc/device-tree -cf - ."' |
    tar --warning=no-timestamp -C "$tmp" -xf -
dtc -q -I fs -O dtb "$tmp" -o "$O/runtime.dtb"
su 'cat /proc/iomem' > "$O/iomem"
su 'cat /proc/cmdline' > "$O/cmdline"
adb exec-out 'su -c "zcat /proc/config.gz"' > "$O/android.config"
adb shell uname -r > "$O/android-release"
echo "captured into $O: runtime.dtb iomem cmdline android.config android-release"
