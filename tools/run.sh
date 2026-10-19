#!/usr/bin/env bash
# Push a prepared payload plus the loader, verify hashes, and execute the kexec.
# Usage: run.sh <prepared-dir> [extra loader params...]
# Needs adb with a working `su`. Files go only to /data/local/tmp.
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
P=$1; shift
T=/data/local/tmp
MOD=${QKX_LOADER_MODULE:-$HERE/module/quest_kexec.ko}
MARKER=${QKX_MARKER_MODULE:-$HERE/module/marker_read.ko}
. "$HERE/tools/board.sh"
BOARD=$(qkx_board_detect "${QKX_BOARD:-}") ||
	{ echo "run.sh: could not determine board (set QKX_BOARD or connect a device)"; exit 1; }
qkx_board_load "$BOARD" || exit 1
PARAMS="execute=1 preserve_watchdog=1 watchdog_recovery=0 core_hang_control=2 \
flush_rpmh=1 suspend_syncboss=1 disconnect_qmp=0 phase_delay_ms=300 \
syncboss_spi=$QKX_SYNCBOSS_SPI $*"

for f in "$MOD" "$MARKER" "$P/Image" "$P/initramfs" "$P/boot.dtb"; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
done
INITRD=$P/initramfs
PRIVATE_INITRD=
if [ -f "$P/require-stock-calibration" ] || [ "${QKX_INHERIT_CALIBRATION:-0}" = 1 ]; then
	PRIVATE_INITRD=$(mktemp)
	trap 'rm -f "$PRIVATE_INITRD"' EXIT
	python3 "$HERE/tools/stock-calibration-initramfs.py" "$INITRD" "$PRIVATE_INITRD" || exit 1
	INITRD=$PRIVATE_INITRD
fi
# Stock hyp-assigns secure ION pages to other VMs; they stay locked after
# kexec and any access from the next kernel hangs the CPU. Collect them as
# late as possible and hand them over as one no-map reserved-memory node.
DTB=$P/boot.dtb
SECMAP=${QKX_SECMAP_MODULE:-$HERE/module/ion_secmap.ko}
if [ -f "$SECMAP" ]; then
	adb push "$SECMAP" $T/ion_secmap.ko >/dev/null
	# Preserve Android's live hardware state by default. Stopping services
	# drops hardware/firmware votes and prevents this target from starting.
	if [ "${QKX_STOP_ANDROID:-0}" = 1 ]; then
		adb shell "su -c 'stop; for s in cameraserver virtual_camera bootanim \
			vendor.oculus.hardware.composer-service vendor.qti.hardware.display.composer \
			vendor.qti.hardware.display.allocator; do stop \$s; done; sleep 8; \
			echo 3 > /proc/sys/vm/drop_caches; sleep 2'"
	fi
	if [ "${GIVEBACK:-0}" = 1 ]; then
		# Return the pages to HLOS with the stock unassign calls; only
		# what the hypervisor refuses still needs reserving.
		adb push "$HERE/module/qkx_giveback.ko" $T/qkx_giveback.ko >/dev/null
		adb shell "su -c 'insmod $T/qkx_giveback.ko; dmesg'" | tac |
			sed '/giveback: begin/q' > "$P/giveback.txt"
		grep -o 'giveback: returned.*' "$P/giveback.txt" ||
			{ echo "giveback did not run"; exit 1; }
		grep -o 'giveback: kept 0x[0-9a-f]*-0x[0-9a-f]*' "$P/giveback.txt" |
			sed 's/giveback: kept/ionsec:/' > "$P/ionsec.txt"
	else
		adb shell "su -c 'insmod $T/ion_secmap.ko; dmesg'" | tac | sed '/ionsec: begin/q' |
			grep -o 'ionsec: 0x[0-9a-f]*-0x[0-9a-f]*' > "$P/ionsec.txt"
		[ -s "$P/ionsec.txt" ] || { echo "ion_secmap returned no ranges"; exit 1; }
	fi
fi
if [ -s "$P/ionsec.txt" ] && [ -f "$SECMAP" ]; then
	DTB=$(mktemp --suffix=.dtb)
	cp "$P/boot.dtb" "$DTB"
	# Early memblock can't grow its region array, so every no-map range
	# costs a split; merge into a few 2 MiB-aligned blocks.
	REG=$(python3 - "$P/ionsec.txt" <<'EOF'
import re, sys
A = 2 << 20
m = []
for a, b in sorted(tuple(int(x, 16) for x in re.findall(r'0x[0-9a-f]+', l))
                   for l in open(sys.argv[1])):
    a &= ~(A - 1)
    b = (b + A - 1) & ~(A - 1)
    if m and a <= m[-1][1]:
        m[-1][1] = max(m[-1][1], b)
    else:
        m.append([a, b])
print(' '.join('0x%x 0x%x 0x%x 0x%x' % (a >> 32, a & 0xffffffff,
               (b - a) >> 32, (b - a) & 0xffffffff) for a, b in m))
EOF
)
	fdtput -c "$DTB" /reserved-memory/qkx-ionsec &&
	fdtput -t x "$DTB" /reserved-memory/qkx-ionsec reg $REG &&
	fdtput -t s "$DTB" /reserved-memory/qkx-ionsec no-map "" ||
		{ echo "failed to add ionsec reservation"; exit 1; }
	echo "reserved $(wc -l < "$P/ionsec.txt") secure ranges as $(($(echo $REG | wc -w) / 4)) blocks"
fi

# The appended calibration archive changes the initrd length. Keep /chosen
# consistent with the verified bytes staged by the loader.
if [ -n "$PRIVATE_INITRD" ]; then
	if [ "$DTB" = "$P/boot.dtb" ]; then
		DTB=$(mktemp --suffix=.dtb)
		cp "$P/boot.dtb" "$DTB"
	fi
	END=$(python3 - "$DTB" "$INITRD" <<'EOF'
import pathlib, subprocess, sys
cells = subprocess.check_output(['fdtget', '-t', 'x', sys.argv[1], '/chosen', 'linux,initrd-start'], text=True).split()
start = 0
for c in cells:
    start = (start << 32) | int(c, 16)
end = start + pathlib.Path(sys.argv[2]).stat().st_size
print(hex(end >> 32), hex(end & 0xffffffff))
EOF
) || exit 1
	fdtput -t x "$DTB" /chosen linux,initrd-end $END || exit 1
fi

adb push "$MOD" $T/qkx.ko >/dev/null
adb push "$MARKER" $T/qkx_marker.ko >/dev/null
adb push "$P/Image" $T/qkx-Image >/dev/null
adb push "$INITRD" $T/qkx-initramfs >/dev/null
adb push "$DTB" $T/qkx-boot.dtb >/dev/null
adb shell 'su -c "sync; sync"'
for pair in "$MOD:qkx.ko" "$P/Image:qkx-Image" "$INITRD:qkx-initramfs" "$DTB:qkx-boot.dtb"; do
	l=${pair%%:*}; r=${pair##*:}
	[ "$(md5sum < "$l" | cut -d' ' -f1)" = "$(adb shell "su -c 'md5sum $T/$r'" | awk '{print $1}')" ] ||
		{ echo "hash mismatch: $r"; exit 1; }
done
# The PMIC latches why the SoC last went down. Read it here, before this run
# overwrites it, so a failed attempt can be attributed afterwards.
adb shell "su -c 'dmesg | grep -iE \"Power-on reason|Power-off reason\"'" 2>/dev/null |
	sed 's/^\[[^]]*\] *//' | tr -d '\r' > "$P/prev-reset-reason.txt"
[ -s "$P/prev-reset-reason.txt" ] &&
	echo "previous shutdown: $(grep -m1 -i 'Power-off reason' "$P/prev-reset-reason.txt" | sed 's/.*Power-off reason: //')"

# QSEE listeners are registered in TrustZone against stock-kernel buffers.
# Freezing their owners preserves those stale registrations across kexec, and
# the target then hangs in __qseecom_scm_call2_locked. Stop only TEE clients;
# do not use Android's broad `stop`, which drops required hardware votes.
if [ "${QKX_STOP_QSEE:-0}" = 1 ]; then
	adb shell "su -c 'for s in vendor.oculus.hardware.attestation-service \
		vendor.oculus.devicecert-hal-1-0 gatekeeperd gatekeeper-1-0 \
		vendor.keymaster-4-1 vendor.spdaemon vendor.qseecomd; do \
		setprop ctl.stop \$s; done; sleep 2; \
		echo qsee_services_stopped; \
		ps -A | grep -E \"qseecomd|spdaemon|keymaster|gatekeeper|attestation|devicecert\" || true'" | tr -d '\r'
fi

# Invalidate the previous retained log so a reset can't reuse it.
adb shell "su -c 'insmod $T/qkx_marker.ko clear=1; rmmod marker_read'" >/dev/null 2>&1
# Assignments made after the ion_secmap snapshot would be missing from the
# reservation; hypwatch (if loaded) shows whether that window saw any.
adb shell "su -c 'cat /proc/qkx_hypwatch 2>/dev/null'" > "$P/hypwatch.txt" &&
	[ -s "$P/hypwatch.txt" ] && echo "hypwatch before jump: $(head -1 "$P/hypwatch.txt")"
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
