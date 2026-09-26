#!/usr/bin/env bash
# Push a prepared payload plus the loader, verify hashes, and execute the kexec.
# Usage: run.sh <prepared-dir> [extra loader params...]
# Needs adb with a working `su`. Files go only to /data/local/tmp.
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
P=$1; shift
T=/data/local/tmp
MOD=$HERE/module/quest_kexec.ko
PARAMS="execute=1 preserve_watchdog=1 watchdog_recovery=0 core_hang_control=2 \
flush_rpmh=1 suspend_syncboss=1 disconnect_qmp=0 phase_delay_ms=300 $*"

for f in "$MOD" "$HERE/module/marker_read.ko" "$P/Image" "$P/initramfs" "$P/boot.dtb"; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
done
adb push "$MOD" $T/qkx.ko >/dev/null
adb push "$HERE/module/marker_read.ko" $T/qkx_marker.ko >/dev/null
adb push "$P/Image" $T/qkx-Image >/dev/null
adb push "$P/initramfs" $T/qkx-initramfs >/dev/null
adb push "$P/boot.dtb" $T/qkx-boot.dtb >/dev/null
adb shell 'su -c "sync; sync"'
for pair in "$MOD:qkx.ko" "$P/Image:qkx-Image" "$P/initramfs:qkx-initramfs" "$P/boot.dtb:qkx-boot.dtb"; do
	l=${pair%%:*}; r=${pair##*:}
	[ "$(md5sum < "$l" | cut -d' ' -f1)" = "$(adb shell "su -c 'md5sum $T/$r'" | awk '{print $1}')" ] ||
		{ echo "hash mismatch: $r"; exit 1; }
done
# Invalidate the previous retained log so a reset can't reuse it.
adb shell "su -c 'insmod $T/qkx_marker.ko clear=1; rmmod marker_read'" >/dev/null 2>&1
echo "verified; executing: $PARAMS"
adb shell "su -c 'insmod $T/qkx.ko image=$T/qkx-Image initrd=$T/qkx-initramfs dtb=$T/qkx-boot.dtb $PARAMS'"
sleep 2
if adb shell 'su -c true' >/dev/null 2>&1; then
	echo "loader returned without jumping:"
	adb shell 'su -c "dmesg | grep quest_kexec | tail -5"'
	adb shell 'su -c "rmmod quest_kexec"' >/dev/null 2>&1
	exit 1
fi
echo "jumped. Expect USB gadget 1d6b:0105; then: telnet 10.42.0.2"
