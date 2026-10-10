#!/usr/bin/env bash
# Board-descriptor resolution for the kexec tools.
#
# Source it, then:
#   name=$(qkx_board_detect "$OVERRIDE") || exit 1   # --board value, or ""
#   qkx_board_load "$name" || exit 1                 # sources boards/<name>.sh
# and the QKX_* board variables are then set in the caller's shell.
#
# Run directly to resolve and print a board (handy for testing without a device):
#   tools/board.sh seacliff
#   tools/board.sh                 # detects the connected headset over adb
#
# Only reads files in boards/ and (for detection) queries the device read-only.

QKX_BOARDS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/boards

# Resolve the board codename. Precedence: explicit argument, then $QKX_BOARD,
# then the connected device's ro.product.device. Prints the name, or returns
# nonzero (and prints nothing) if it can't be determined.
qkx_board_detect() {
	if [ -n "${1:-}" ]; then printf '%s\n' "$1"; return 0; fi
	if [ -n "${QKX_BOARD:-}" ]; then printf '%s\n' "$QKX_BOARD"; return 0; fi
	local d
	d=$(adb ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"} shell getprop ro.product.device 2>/dev/null | tr -d '\r')
	[ -n "$d" ] && { printf '%s\n' "$d"; return 0; }
	return 1
}

# Source boards/<name>.sh into the current shell and sanity-check it. The keys
# checked here are the ones every board must set; tools that need the optional
# DTB values (splash, disable list, NUX apk) check those themselves.
qkx_board_load() {
	local name=${1:-} f
	[ -n "$name" ] || { echo "board: no board specified (pass --board, set QKX_BOARD, or connect a device)" >&2; return 1; }
	f="$QKX_BOARDS_DIR/$name.sh"
	[ -f "$f" ] || { echo "board: unknown board '$name' (no $f)" >&2; return 1; }
	# shellcheck disable=SC1090
	. "$f"
	local k
	for k in QKX_BOARD QKX_BOARD_LABEL QKX_SYNCBOSS_SPI QKX_KERNEL_LOCALVERSION; do
		[ -n "${!k:-}" ] || { echo "board: $f is missing $k" >&2; return 1; }
	done
	[ "$QKX_BOARD" = "$name" ] ||
		{ echo "board: $f sets QKX_BOARD=$QKX_BOARD but is named $name" >&2; return 1; }
	return 0
}

# Direct invocation: resolve, load, and print the board's variables.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	set -euo pipefail
	name=$(qkx_board_detect "${1:-}") ||
		{ echo "board: could not determine the board (no argument and no device)" >&2; exit 1; }
	qkx_board_load "$name"
	echo "board: $QKX_BOARD ($QKX_BOARD_LABEL)"
	for k in QKX_USB_MANUFACTURER QKX_SYNCBOSS_SPI QKX_NUX_APK QKX_KERNEL_LOCALVERSION \
		QKX_DISABLE_NODES QKX_NOMAP_NODES; do
		printf '  %s=%s\n' "$k" "${!k:-}"
	done
fi
