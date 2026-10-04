#!/usr/bin/env bash
# Remove QKX-created files from the headset's userdata. Never touches partitions.
# Usage: ./qkx-uninstall.sh [--yes] [--keep-legacy]
set -euo pipefail
YES=0; LEGACY=1
while [ $# -gt 0 ]; do
 case "$1" in
  --yes|-y) YES=1 ;;
  --keep-legacy) LEGACY=0 ;;
  --help|-h)
   echo "usage: $0 [--yes] [--keep-legacy]"
   echo '  --yes          do not prompt'
   echo '  --keep-legacy  retain old /data/local/tmp/qkx and qkx-turnkey-v1 installs'
   exit 0 ;;
  *) echo "unknown argument: $1"; exit 2 ;;
 esac
 shift
done
adb shell 'su -c id' 2>/dev/null | grep -q 'uid=0' || {
 echo 'Rooted stock Android adb is required.'; exit 1; }
KERNEL=$(adb shell uname -r | tr -d '\r')
case "$KERNEL" in
 *qkx*) echo "Refusing to uninstall while a QKX kernel is running ($KERNEL). Reboot stock Android first."; exit 1 ;;
esac
MODEL=$(adb shell getprop ro.product.device | tr -d '\r')
[ "$MODEL" = seacliff ] || { echo "Refusing unexpected device: $MODEL"; exit 1; }
# Exact project-owned paths only. Do not use a broad qkx* glob.
PATHS=(
 /data/local/tmp/qkx-release
 /data/local/tmp/qkx.ko
 /data/local/tmp/qkx_marker.ko
 /data/local/tmp/ion_secmap.ko
 /data/local/tmp/qkx_giveback.ko
 /data/local/tmp/qkx-Image
 /data/local/tmp/qkx-initramfs
 /data/local/tmp/qkx-boot.dtb
 /data/local/tmp/qkx-install.sh
 /data/local/tmp/qkx_fsmap
 /data/local/tmp/qkx_rawcp
 /data/local/tmp/qkx_dm
 /data/local/tmp/qkx-inspect-data.map
 /data/local/tmp/cvp-alt-firmware-sha.txt
)
if [ "$LEGACY" = 1 ]; then
 PATHS+=(/data/local/tmp/qkx /data/local/tmp/qkx-turnkey-v1)
fi
printf -v REMOTE_LIST " '%q'" "${PATHS[@]}"
echo 'QKX-owned headset paths found:'
FOUND=$(adb shell "su -c 'for p in$REMOTE_LIST; do if [ -e \"\$p\" ]; then du -sh \"\$p\" 2>/dev/null || ls -ld \"\$p\"; fi; done'" | tr -d '\r')
if [ -z "$FOUND" ]; then
 echo '  (none; headset is already clean)'
 exit 0
fi
printf '%s\n' "$FOUND" | sed 's/^/  /'
echo
echo 'This only deletes files on userdata; no installed partition is flashed, formatted, or modified.'
if [ "$YES" != 1 ]; then
 read -r -p 'Delete these QKX files? Type REMOVE to continue: ' answer
 [ "$answer" = REMOVE ] || { echo 'Cancelled.'; exit 1; }
fi
# A loaded loader should not normally exist after a successful or failed run.
# Refuse rather than changing live kernel/module state in an uninstaller.
LOADED=$(adb shell "su -c 'grep -E \"^(quest_kexec|marker_read|ion_secmap) \" /proc/modules || true'" | tr -d '\r')
if [ -n "$LOADED" ]; then
 echo 'A QKX helper module is still loaded; reboot stock Android and rerun:'
 printf '%s\n' "$LOADED"
 exit 1
fi
adb shell "su -c 'set -e; for p in$REMOTE_LIST; do rm -rf -- \"\$p\"; done; sync; for p in$REMOTE_LIST; do [ ! -e \"\$p\" ] || { echo FAILED:\$p; exit 1; }; done; echo QKX_HEADSET_FILES_REMOVED'" | tr -d '\r' | grep -q '^QKX_HEADSET_FILES_REMOVED$'
echo 'QKX headset files removed successfully.'
echo 'Host-side state and package ZIPs were intentionally retained.'
