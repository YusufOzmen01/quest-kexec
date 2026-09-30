#!/usr/bin/env bash
# Ask stock Android why the SoC went down last time.
#
# The PMIC latches a power-off reason across the reset, so after a failed run
# this distinguishes causes that are otherwise indistinguishable in our logs:
#
#   PS_HOLD   the SoC asked for the reset (kernel restart, watchdog bite, TZ)
#   PMIC_WD   the PMIC's own watchdog expired
#   UVLO      the supply collapsed (battery/charging)
#   TFT/OTST3 thermal
#   GP_FAULT/MBG_FAULT/OVLO  PMIC fault
#
# Run it on stock, after the device has come back up. Read-only.
set -uo pipefail
T=${1:-}

run() { adb shell "su -c '$1'" 2>/dev/null | tr -d '\r'; }

echo "=== PMIC power-on / power-off reasons (latched across the reset) ==="
run 'dmesg | grep -iE "Power-on reason|Power-off reason"' | sed 's/^\[[^]]*\] *//'

echo
echo "=== Android's view of the boot reason ==="
run 'getprop ro.boot.bootreason; getprop sys.boot.reason; getprop ro.boot.alarmboot'

echo
echo "=== PMIC sysfs (same data, unparsed) ==="
run 'for f in /sys/devices/platform/soc/*/*/*/*power-on*/power_o*_reason; do
       [ -r "$f" ] && echo "$f: $(cat $f)"; done'

echo
echo "=== Did anything survive in pstore? (warm reset only) ==="
run 'ls -l /sys/fs/pstore/ 2>&1 | head'

echo
echo "=== Battery / charger state now ==="
run 'dumpsys battery | grep -iE "level|powered|status|voltage|temperature"'

[ -n "$T" ] && { echo; echo "(saved to $T)"; } 
