#!/usr/bin/env bash
# After the device is back in Android, print the log the target kernel kept
# in RAM across a warm reset, plus any Android pstore panic record.
set -uo pipefail
adb shell 'su -c "insmod /data/local/tmp/qkx_marker.ko; rmmod marker_read; dmesg"' |
	awk '/qkx_marker_read:/{f=1} f'
adb shell 'su -c "cat /sys/fs/pstore/dmesg-ramoops-0 2>/dev/null"' | grep -a 'quest_kexec\|Kernel panic' | tail -5
