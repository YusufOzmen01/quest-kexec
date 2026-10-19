#!/usr/bin/env bash
# Install a verified QKX system package into pinned userdata files.
# Installed partitions are never written. Usage: ./qkx-install-package.sh [ZIP]
# Revalidation only: ./qkx-install-package.sh --verify-only ZIP
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/tools/board.sh"
VERIFY_ONLY=0
if [ "${1:-}" = --verify-only ]; then VERIFY_ONLY=1; shift; fi
ZIP=${1:-}
if [ -z "$ZIP" ]; then read -r -p 'System package ZIP: ' ZIP; fi
ZIP=$(readlink -f "$ZIP")
[ -f "$ZIP" ] || { echo "Package not found: $ZIP"; exit 1; }

verify_zip() {
 python3 - "$@" <<'PY'
import hashlib,json,sys,zipfile
z=zipfile.ZipFile(sys.argv[1]); names=set(z.namelist())
known=set(sys.argv[2:])
try: m=json.loads(z.read('manifest.json'))
except Exception as e: raise SystemExit('invalid manifest: '+str(e))
fmt=m.get('format'); dev=m.get('device')
if fmt not in ('qkx-system-package-v1','qkx-rom-package-v1'):
 raise SystemExit('unsupported package format')
if dev not in known:
 raise SystemExit('unrecognized device: '+str(dev))
required={'images/'+x+'.img' for x in ('system','system_ext','vendor','odm','product')}
required|={'qkx-turnkey.json'}
if fmt=='qkx-system-package-v1':
 required|={'kernel/Image','runtime/busybox','modules/quest_kexec.ko','modules/ion_secmap.ko','modules/marker_read.ko'}
if not required <= set(m.get('files',{})): raise SystemExit('manifest is incomplete')
for n,s in m['files'].items():
 if n not in names or n.startswith('/') or '..' in n.split('/'): raise SystemExit('unsafe/missing member: '+n)
 h=hashlib.sha256(); size=0
 with z.open(n) as f:
  for b in iter(lambda:f.read(8<<20),b''): h.update(b); size+=len(b)
 if size!=s['size'] or h.hexdigest()!=s['sha256']: raise SystemExit('HASH FAILURE: '+n)
 print('VERIFIED',n)
print('PACKAGE VERIFIED')
PY
}
# Labels of every board this tree knows, for offline package verification.
KNOWN_LABELS=()
for bf in "$HERE"/boards/*.sh; do
 lbl="$(. "$bf"; printf '%s' "${QKX_BOARD_LABEL:-}")"
 [ -n "$lbl" ] && KNOWN_LABELS+=("$lbl")
done
verify_zip "$ZIP" "${KNOWN_LABELS[@]}"
[ "$VERIFY_ONLY" = 0 ] || exit 0
adb shell 'su -c id' </dev/null | grep -q 'uid=0' || { echo 'Rooted adb is required'; exit 1; }
MODEL=$(adb shell getprop ro.product.device </dev/null | tr -d '\r')
qkx_board_load "$MODEL" || { echo "Refusing unsupported device: $MODEL"; exit 1; }
# The package must be built for this headset's board, not merely a known one.
PKG_DEVICE=$(python3 - "$ZIP" <<'PY'
import json,sys,zipfile
print(json.loads(zipfile.ZipFile(sys.argv[1]).read('manifest.json')).get('device',''))
PY
)
[ "$PKG_DEVICE" = "$QKX_BOARD_LABEL" ] ||
 { echo "Package is for '$PKG_DEVICE', not this headset ('$QKX_BOARD_LABEL')"; exit 1; }
read -r -p 'Alternate /data size in GiB [16]: ' DATA_GIB
DATA_GIB=${DATA_GIB:-16}
[[ "$DATA_GIB" =~ ^[0-9]+$ ]] && [ "$DATA_GIB" -ge 4 ] && [ "$DATA_GIB" -le 128 ] || { echo 'Choose 4..128 GiB'; exit 1; }
SERIAL=$(adb get-serialno | tr -cd 'A-Za-z0-9_.-')
STATE="$HERE/state/$SERIAL"; WORK=/data/local/tmp/qkx-release
if adb exec-out "su -c 'test -e $WORK/img/system.img && echo YES'" </dev/null | grep -q YES; then
 read -r -p 'Replace the existing QKX release installation? [y/N] ' ans
 [[ "$ans" = y || "$ans" = Y ]] || exit 1
 read -r -p 'Preserve the existing alternate /data image? [Y/n] ' keep_data
 if [[ "$keep_data" = n || "$keep_data" = N ]]; then
  adb shell "su -c 'rm -rf $WORK'" </dev/null
 else
  # os-install.sh detects the retained data.img, skips rebuilding/transferring
  # it, and emits a fresh extent map along with the replaced OS image maps.
  adb shell "su -c 'rm -f $WORK/img/system.img $WORK/img/system_ext.img \
   $WORK/img/vendor.img $WORK/img/odm.img $WORK/img/product.img \
   $WORK/stage.img $WORK/stage.img.gz'" </dev/null
 fi
fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
python3 - "$ZIP" "$TMP" <<'PY'
import sys,zipfile
with zipfile.ZipFile(sys.argv[1]) as z: z.extractall(sys.argv[2])
PY
# Split ROM packages obtain the device-independent loader binaries from this
# release bundle. Combined legacy packages continue to carry their own copies.
if [ ! -f "$TMP/kernel/Image" ]; then
 for f in runtime/Image runtime/busybox runtime/quest_kexec.ko runtime/ion_secmap.ko runtime/marker_read.ko; do
  [ -f "$HERE/$f" ] || { echo "Loader package is missing $f"; exit 1; }
 done
 mkdir -p "$TMP/kernel" "$TMP/runtime" "$TMP/modules"
 cp "$HERE/runtime/Image" "$TMP/kernel/Image"
 cp "$HERE/runtime/busybox" "$TMP/runtime/busybox"
 cp "$HERE/runtime/quest_kexec.ko" "$TMP/modules/quest_kexec.ko"
 cp "$HERE/runtime/ion_secmap.ko" "$TMP/modules/ion_secmap.ko"
 cp "$HERE/runtime/marker_read.ko" "$TMP/modules/marker_read.ko"
fi
rm -rf "$STATE"; mkdir -p "$STATE/modules" "$STATE/captured" "$STATE/maps"
cp "$TMP"/modules/*.ko "$STATE/modules/"
cp "$TMP/kernel/Image" "$STATE/Image"
cp "$TMP/manifest.json" "$TMP/qkx-turnkey.json" "$STATE/"
"$HERE/tools/capture.sh" "$STATE/captured"
QKX_WORK_DIR="$WORK" "$HERE/tools/os-install.sh" "$TMP/images" "$STATE/maps" "$DATA_GIB"
QKX_WORK_DIR="$WORK" "$HERE/tools/os-install-modemfw.sh" "$STATE/maps"
# Preserve the turnkey marker so initramfs automatically launches Android.
cp "$TMP/qkx-turnkey.json" "$STATE/maps/qkx-turnkey.json"
export BUSYBOX="$TMP/runtime/busybox"
export QKX_EXTRA_CMDLINE='qkx_uart_dma_cleanup=1 qkx_skip_regulator_cleanup=1 deferred_probe_timeout=-1 qkx_stallmon=1 qkx_beat=1 qkx_venus_reload_first=1 qkx_venus_no_pc=1'
"$HERE/tools/os-prep.sh" "$STATE/captured" "$STATE/Image" "$STATE/maps" "$STATE/payload"
fdtput -t s "$STATE/payload/boot.dtb" /soc/qseecom@82400000 status disabled
fdtput -t s "$STATE/payload/boot.dtb" /reserved-memory/cont_splash_region@9c000000 no-map ''
# Runtime DT state can expose the secure-display CMA pool as reusable. The
# verified Android handoff must preserve it, not let Linux allocate from it.
fdtput -d "$STATE/payload/boot.dtb" /reserved-memory/secure_display_region reusable 2>/dev/null || true
fdtput -t s "$STATE/payload/boot.dtb" /reserved-memory/secure_display_region no-map ''
touch "$STATE/payload/require-stock-calibration"
printf '%s\n' "$WORK" > "$STATE/device-work-dir"
printf '%s\n' "$SERIAL" > "$HERE/state/current"
echo "INSTALL VERIFIED AND READY: $STATE"
echo "Boot with: $HERE/qkx-boot.sh"
