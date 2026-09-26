#!/usr/bin/env bash
# Build a payload directory from the captured inputs and a target Image.
# Usage: prep.sh <captured-dir> <Image> <initramfs> <out-dir> [extra cmdline...]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
C=$1 IMG=$2 RD=$3 OUT=$4; shift 4
# Android's cmdline minus the boot-time watchdog bits, plus what the target needs.
BASE=$(sed -e 's/softdog.soft_panic=1//' "$C/cmdline")
python3 "$HERE/prepare.py" --live-dtb "$C/runtime.dtb" --iomem "$C/iomem" \
	--kernel-image "$IMG" --initramfs "$RD" --output "$OUT" \
	--cmdline "$BASE rdinit=/init nokaslr printk.devkmsg=on qkx_qmp_adopt=1 initcall_blacklist=virtual_sensor_driver_init $*" \
	> "$OUT.manifest.log"
echo "prepared $OUT"
