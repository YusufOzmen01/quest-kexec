#!/usr/bin/env bash
# Build a kexec payload that assembles the custom OS instead of a bare shell.
# Usage: os-prep.sh <captured-dir> <target-Image> <os-dir> <out-dir>
#
# <os-dir> is what tools/os-install.sh produced (maps/ and partitions).
# The maps are baked into the initramfs, so re-run this after reinstalling an
# image: reallocating a pinned file hands out different blocks.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
C=${1:?captured dir} IMG=${2:?target Image} OSDIR=${3:?os dir} OUT=${4:?out dir}
BB=${BUSYBOX:-$HERE/out/busybox}

[ -f "$BB" ] || { echo "no busybox at $BB; run: make initramfs"; exit 1; }
[ -f "$OSDIR/partitions" ] || { echo "no $OSDIR/partitions; run tools/os-install.sh"; exit 1; }
make -C "$HERE/tools/device" >/dev/null

mkdir -p "$(dirname "$OUT")"
RD=$OUT-initramfs.gz
python3 "$HERE/initramfs/build.py" --busybox "$BB" --output "$RD" \
	--os-dir "$OSDIR" --qkx-dm "$HERE/tools/device/qkx_dm"

# A user build ignores androidboot.selinux; qkx_selinux_permissive (our kernel)
# keeps SELinux permissive so the debug shell survives init's policy load.
"$HERE/tools/prep.sh" "$C" "$IMG" "$RD" "$OUT" androidboot.selinux=permissive qkx_selinux_permissive=1 \
	hung_task_panic=1 hung_task_timeout_secs=30 softlockup_panic=1 audit=0 ${QKX_EXTRA_CMDLINE:-}
if [ -f "$OSDIR/qkx-turnkey.json" ]; then
	touch "$OUT/require-stock-calibration"
else
	rm -f "$OUT/require-stock-calibration"
fi
echo "payload with custom OS ready: $OUT"
