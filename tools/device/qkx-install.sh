#!/system/bin/sh
# Device side of the custom-OS installer. Runs under Android as root.
#
# Images live as pinned f2fs files inside the work directory, but their contents
# are written straight to the userdata partition's blocks. Userdata is
# metadata-encrypted, so this is the only form the kexec target can read; from
# Android these files just look like garbage, which is expected.
#
# No partition is formatted and nothing outside these files is written.
set -e

W=/data/local/tmp/qkx
BIN=$W/bin
IMG=$W/img
MAP=$W/maps
DEV=/dev/block/sda7          # userdata; verified 1:1 under dm-default-key

usage() {
	echo "usage: qkx-install.sh alloc  <name> <bytes>"
	echo "       qkx-install.sh write  <name> <staged-file>"
	echo "       qkx-install.sh writez <name> <staged-file.gz>"
	echo "       qkx-install.sh verify <name> <staged-file>"
	echo "       qkx-install.sh map    <name>"
	echo "       qkx-install.sh list"
	echo "       qkx-install.sh partitions"
	exit 1
}

need_map() {
	[ -s "$MAP/$1.map" ] || { echo "qkx-install: no map for $1"; exit 1; }
}

cmd_alloc() {
	name=$1; bytes=$2
	mkdir -p "$IMG" "$MAP"
	# Reallocating would hand out different blocks, invalidating a map that a
	# staged initramfs may already contain.
	if [ -f "$IMG/$name.img" ]; then
		echo "qkx-install: $name already allocated; remove $IMG/$name.img first"
		exit 1
	fi
	"$BIN/qkx_fsmap" alloc "$IMG/$name.img" "$bytes"
	"$BIN/qkx_fsmap" map "$IMG/$name.img" > "$MAP/$name.map"
	echo "qkx-install: $name allocated, $(wc -l < "$MAP/$name.map") extents"
}

cmd_write() {
	name=$1; srcfile=$2
	need_map "$name"
	# Zero first: the blocks hold stale data, and a sparse copy would otherwise
	# leave garbage wherever the image has holes.
	"$BIN/qkx_rawcp" zero "$MAP/$name.map" "$DEV"
	"$BIN/qkx_rawcp" copy "$MAP/$name.map" "$DEV" "$srcfile"
	sync
}

cmd_writez() {
	name=$1; srcgz=$2
	need_map "$name"
	"$BIN/qkx_rawcp" zero "$MAP/$name.map" "$DEV"
	gzip -dc "$srcgz" | "$BIN/qkx_rawcp" copy "$MAP/$name.map" "$DEV" -
	sync
}

cmd_verify() {
	name=$1; srcfile=$2
	need_map "$name"
	# The map must still match the file, or we verified the wrong blocks.
	"$BIN/qkx_fsmap" map "$IMG/$name.img" > "$MAP/$name.map.now"
	if ! cmp "$MAP/$name.map" "$MAP/$name.map.now" >/dev/null 2>&1; then
		echo "qkx-install: $name moved on disk; extents changed"
		exit 1
	fi
	rm -f "$MAP/$name.map.now"
	case "$srcfile" in
	*.gz) gzip -dc "$srcfile" | "$BIN/qkx_rawcp" verify "$MAP/$name.map" "$DEV" - ;;
	*)    "$BIN/qkx_rawcp" verify "$MAP/$name.map" "$DEV" "$srcfile" ;;
	esac
}

cmd_map() {
	need_map "$1"
	"$BIN/qkx_fsmap" map "$IMG/$1.img"
}

cmd_partitions() {
	# "<name> <device node>" for every partition, so the kexec target can find
	# them without a vendor init to create by-name symlinks.
	for l in /dev/block/by-name/*; do
		[ -e "$l" ] || continue
		echo "${l##*/} $(readlink -f "$l")"
	done
}

cmd_list() {
	for f in "$IMG"/*.img; do
		[ -e "$f" ] || continue
		n=${f##*/}; n=${n%.img}
		echo "$n $(stat -c %s "$f") bytes, $(wc -l < "$MAP/$n.map") extents"
	done
}

[ $# -ge 1 ] || usage
op=$1; shift
case "$op" in
alloc)  [ $# -eq 2 ] || usage; cmd_alloc "$@" ;;
write)  [ $# -eq 2 ] || usage; cmd_write "$@" ;;
writez) [ $# -eq 2 ] || usage; cmd_writez "$@" ;;
verify) [ $# -eq 2 ] || usage; cmd_verify "$@" ;;
map)    [ $# -eq 1 ] || usage; cmd_map "$@" ;;
list)   cmd_list ;;
partitions) cmd_partitions ;;
*) usage ;;
esac
