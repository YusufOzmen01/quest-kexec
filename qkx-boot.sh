#!/usr/bin/env bash
# Boot the already-installed QKX OS. Deliberately does not revalidate package files.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
adb shell 'su -c id' | grep -q 'uid=0' || { echo 'Rooted stock Android adb is required'; exit 1; }
SERIAL=$(adb get-serialno | tr -cd 'A-Za-z0-9_.-')
STATE="$HERE/state/$SERIAL"
if [ ! -d "$STATE" ] && [ -f "$HERE/state/current" ]; then STATE="$HERE/state/$(cat "$HERE/state/current")"; fi
for f in "$STATE/payload/Image" "$STATE/payload/initramfs" "$STATE/payload/boot.dtb" \
 "$STATE/modules/quest_kexec.ko" "$STATE/modules/ion_secmap.ko" "$STATE/modules/marker_read.ko"; do
 [ -f "$f" ] || { echo "Not installed: $f"; exit 1; }
done
export QKX_LOADER_MODULE="$STATE/modules/quest_kexec.ko"
export QKX_SECMAP_MODULE="$STATE/modules/ion_secmap.ko"
export QKX_MARKER_MODULE="$STATE/modules/marker_read.ko"
export QKX_STOP_QSEE=1
# run.sh captures current stock calibration into a private temporary initramfs,
# snapshots secure mappings, and performs the jump. Turnkey initramfs launches
# Android automatically; there is intentionally no rescue-shell launch step.
"$HERE/tools/run.sh" "$STATE/payload" shutdown_adsp=1 shutdown_cdsp=1 \
 shutdown_subsys=venus,npu phase_delay_ms=0
echo 'QKX jump initiated; Android will start automatically.'
