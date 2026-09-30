#!/usr/bin/env bash
# Pin a read-only copy of the modem_a partition (holds the ADSP/CDSP firmware
# images our pre-Android ADSP boot needs) the same way tools/os-install.sh pins
# system/vendor/odm/product: allocate a pinned file on userdata, write it with
# raw-block extent copies, verify, and record its extent map.
#
# The partition itself is only ever read (dd if=<partition>); nothing is
# written back to it. The target later mounts the pinned copy through
# dm-linear, exactly like the OS images, and never touches the raw partition.
#
# Usage: tools/os-install-modemfw.sh [out-dir]
set -euo pipefail

OUT=${1:-out/os}
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=/data/local/tmp/qkx
NAME=modemfw

say() { printf '\n== %s\n' "$*"; }
sh_() { adb shell "su -c '$*'"; }
have() { adb exec-out "su -c 'test -f $1 && echo QKX_YES'" | grep -q QKX_YES; }

[ -s "$OUT/partitions" ] || { echo "missing $OUT/partitions; run tools/os-install.sh first"; exit 1; }
PART_DEV=$(awk '$1 == "modem_a" { print $2 }' "$OUT/partitions")
[ -n "$PART_DEV" ] || { echo "modem_a not found in $OUT/partitions"; exit 1; }

adb shell 'su -c id' | grep -q 'uid=0' || { echo 'rooted adb (su) required'; exit 1; }
mkdir -p "$OUT/maps"

if have "$W/img/$NAME.img"; then
	echo "== $NAME: already installed, skipping (rm $W/img/$NAME.img to redo)"
else
	say "$NAME: reading size of $PART_DEV"
	BASE=${PART_DEV##*/}
	BYTES=$(adb exec-out "su -c 'cat /sys/class/block/$BASE/size'" | tr -d '\r')
	BYTES=$((BYTES * 512))
	echo "$PART_DEV is $((BYTES / 1024 / 1024)) MiB"

	say "$NAME: dumping $PART_DEV (read-only) to device scratch space"
	sh_ "dd if=$PART_DEV of=$W/modemfw-src.img bs=1M"

	say "$NAME: allocating pinned file"
	BYTES=$(( (BYTES + 4095) / 4096 * 4096 ))
	sh_ "sh $W/qkx-install.sh alloc $NAME $BYTES"

	say "$NAME: writing raw blocks from the dump"
	sh_ "sh $W/qkx-install.sh write $NAME $W/modemfw-src.img"

	say "$NAME: verifying"
	sh_ "sh $W/qkx-install.sh verify $NAME $W/modemfw-src.img"
	sh_ "rm -f $W/modemfw-src.img"
fi

say 'collecting map'
adb exec-out "su -c 'sh $W/qkx-install.sh map $NAME'" | tr -d '\r' > "$OUT/maps/$NAME.map"
[ -s "$OUT/maps/$NAME.map" ] || { echo "empty map for $NAME"; exit 1; }
printf '%-11s %s extents\n' "$NAME" "$(wc -l < "$OUT/maps/$NAME.map")"

echo
echo "modemfw pinned and mapped; rerun tools/os-prep.sh to bundle the new map"
