#!/usr/bin/env bash
# Install a custom Android OS into pinned files on the headset's userdata.
#
# Usage: tools/os-install.sh <ota-image-dir> [out-dir] [data-size-GiB]
#
# The OS images and a fresh /data image are stored as pinned f2fs files under
# /data/local/tmp/qkx, with their contents written directly to the partition
# blocks f2fs allocated to them. That is what makes them readable from the kexec
# target, which cannot decrypt userdata.
#
# This writes to the userdata partition, but only inside blocks allocated to
# these files. It never formats a partition and never touches the stock OS.
set -euo pipefail

OTA=${1:?usage: os-install.sh <ota-image-dir> [out-dir] [data-size-GiB]}
OUT=${2:-out/os}
DATA_GIB=${3:-16}
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${QKX_WORK_DIR:-/data/local/tmp/qkx}
[[ "$W" =~ ^/data/local/tmp/qkx[-a-zA-Z0-9_]*$ ]] || { echo 'invalid pinned-file work directory'; exit 1; }
# system.img is the root filesystem; the rest mount inside it.
IMAGES=(system system_ext vendor odm product)

say() { printf '\n== %s\n' "$*"; }
sh_() { adb shell "su -c '$*'"; }
# su does not reliably propagate exit status, so test by echoing a marker.
have() { adb exec-out "su -c 'test -f $1 && echo QKX_YES'" | grep -q QKX_YES; }

adb shell 'su -c id' | grep -q 'uid=0' || { echo 'rooted adb (su) required'; exit 1; }
for f in "${IMAGES[@]}"; do
	[ -f "$OTA/$f.img" ] || { echo "missing $OTA/$f.img"; exit 1; }
done

mkdir -p "$OUT/maps"

say 'building device helpers'
make -C "$HERE/tools/device" >/dev/null

say 'pushing helpers'
adb push "$HERE/tools/device/qkx_fsmap" "$HERE/tools/device/qkx_rawcp" \
	"$HERE/tools/device/qkx_dm" "$HERE/tools/device/qkx-install.sh" \
	/data/local/tmp/ >/dev/null
sh_ "mkdir -p $W/bin $W/img $W/maps &&
     cp /data/local/tmp/qkx_fsmap /data/local/tmp/qkx_rawcp /data/local/tmp/qkx_dm $W/bin/ &&
     cp /data/local/tmp/qkx-install.sh $W/ &&
     chmod 755 $W/bin/* $W/qkx-install.sh &&
     chgrp 2000 $W && chmod 0770 $W"

# Record which block device backs each partition name. The target has no vendor
# init to create by-name symlinks, so its initramfs needs this mapping.
say 'recording partition map'
adb exec-out "su -c 'sh $W/qkx-install.sh partitions'" | tr -d '\r' > "$OUT/partitions"
grep -E '^(userdata|persist|metadata) ' "$OUT/partitions" || {
	echo 'partition map looks wrong:'; head -3 "$OUT/partitions"; exit 1; }
adb exec-out 'su -c "getprop ro.boot.slot_suffix"' | tr -d '\r' > "$OUT/slot_suffix"
echo "slot: $(cat "$OUT/slot_suffix")"

install_image() {
	local name=$1 src=$2 bytes
	# stat otherwise reports the symlink length instead of the image size.
	# Resolve it for staging too, so allocation and transfer use one file.
	src=$(readlink -f -- "$src")
	bytes=$(stat -c %s "$src")
	# f2fs allocates in 4 KiB blocks; round up so the whole image fits.
	bytes=$(( (bytes + 4095) / 4096 * 4096 ))

	say "$name: allocating $((bytes / 1024 / 1024)) MiB"
	sh_ "sh $W/qkx-install.sh alloc $name $bytes"

	say "$name: staging"
	adb push "$src" "$W/stage.img" >/dev/null

	say "$name: writing raw blocks"
	sh_ "sh $W/qkx-install.sh write $name $W/stage.img"

	say "$name: verifying"
	local verified
	verified=$(sh_ "sh $W/qkx-install.sh verify $name $W/stage.img" 2>&1)
	printf '%s\n' "$verified"
	grep -q 'qkx_rawcp: verified' <<<"$verified" || { echo "raw verification failed: $name"; exit 1; }
	sh_ "rm -f $W/stage.img"
}

for name in "${IMAGES[@]}"; do
	if have "$W/img/$name.img"; then
		echo "== $name: already installed, skipping (rm $W/img/$name.img to redo)"
		continue
	fi
	install_image "$name" "$OTA/$name.img"
done

if have "$W/img/data.img"; then
	say "data: already installed, skipping (rm $W/img/data.img to redo)"
else
	say "data: building empty ${DATA_GIB} GiB ext4"
	DIMG=$OUT/data.img
	rm -f "$DIMG" "$DIMG.gz"
	truncate -s "${DATA_GIB}G" "$DIMG"
	# Keep to features the 4.19 target kernel understands; newer e2fsprogs
	# defaults (orphan_file, metadata_csum_seed) would make it unmountable.
	mke2fs -q -t ext4 -b 4096 -I 256 -m 0 -L data -E nodiscard \
		-O ^orphan_file,^metadata_csum_seed,^casefold,^fast_commit,^large_dir \
		"$DIMG"
	gzip -1 -c "$DIMG" > "$DIMG.gz"
	ls -lh "$DIMG.gz"

	say "data: allocating ${DATA_GIB} GiB pinned"
	sh_ "sh $W/qkx-install.sh alloc data $((DATA_GIB * 1024 * 1024 * 1024))"

	say 'data: staging compressed image'
	adb push "$DIMG.gz" "$W/stage.img.gz" >/dev/null

	say 'data: writing raw blocks (zero pass then image)'
	sh_ "sh $W/qkx-install.sh writez data $W/stage.img.gz"

	say 'data: verifying'
	verified=$(sh_ "sh $W/qkx-install.sh verify data $W/stage.img.gz" 2>&1)
	printf '%s\n' "$verified"
	grep -q 'qkx_rawcp: verified' <<<"$verified" || { echo 'raw verification failed: data'; exit 1; }
	sh_ "rm -f $W/stage.img.gz"
fi

say 'collecting maps'
for name in "${IMAGES[@]}" data; do
	# Read the map back from the file itself, so a stale copy cannot slip in.
	adb exec-out "su -c 'sh $W/qkx-install.sh map $name'" </dev/null | tr -d '\r' > "$OUT/maps/$name.map"
	[ -s "$OUT/maps/$name.map" ] || { echo "empty map for $name"; exit 1; }
	awk 'NF != 3 { exit 1 } { for (i=1;i<=3;i++) if ($i !~ /^[0-9a-fA-F]+$/) exit 1 }' \
		"$OUT/maps/$name.map" || { echo "invalid map for $name"; head -1 "$OUT/maps/$name.map"; exit 1; }
	printf '%-11s %s extents\n' "$name" "$(wc -l < "$OUT/maps/$name.map")"
done

if [ -f "$OTA/qkx-turnkey.json" ]; then
	cp "$OTA/qkx-turnkey.json" "$OUT/qkx-turnkey.json"
fi
say 'installed'
sh_ "sh $W/qkx-install.sh list"
echo
echo "maps and partition table saved in $OUT"
echo "next: tools/os-prep.sh $OUT <target-Image> out/payload"
