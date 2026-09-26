#!/bin/busybox sh
# RAM-only init: USB Ethernet (ECM) gadget with a BusyBox telnet shell.
# The headset is 10.42.0.2; udhcpd hands the host 10.42.0.1.
# Nothing here mounts a block device.
export PATH=/bin:/sbin
export HOME=/root
export TERM=vt100
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /config /tmp
mount -t devpts devpts /dev/pts
mount -t configfs configfs /config
mount -t tmpfs tmpfs /tmp
exec >/dev/kmsg 2>&1

log() { echo "qkx-init: $*"; }
# /proc/qkx_bootdone comes from the target kernel patches: 'd' disarms the
# boot watchdog, 'k' is a keepalive, 'r' forces a log-preserving warm reset.
bootdone() { [ -w /proc/qkx_bootdone ] && echo "$1" > /proc/qkx_bootdone; }

log "reached $(uname -r)"

g=/config/usb_gadget/qkx
mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" "$g/functions/ecm.usb0"
echo 0x1d6b > "$g/idVendor"
echo 0x0105 > "$g/idProduct"
echo QKX-RAM > "$g/strings/0x409/serialnumber"
echo 'quest-pro-kexec' > "$g/strings/0x409/manufacturer"
echo 'RAM-only USB Ethernet shell' > "$g/strings/0x409/product"
echo 'ECM' > "$g/configs/c.1/strings/0x409/configuration"
echo 250 > "$g/configs/c.1/MaxPower"
# Fixed MACs keep the host interface name stable across boots.
echo 02:51:4b:58:00:02 > "$g/functions/ecm.usb0/dev_addr"
echo 02:51:4b:58:00:01 > "$g/functions/ecm.usb0/host_addr"
ln -s "$g/functions/ecm.usb0" "$g/configs/c.1/ecm.usb0"

udc=
for _ in 1 2 3 4 5 6 7 8 9 10; do
	for c in /sys/class/udc/*; do
		[ -e "$c" ] || continue
		echo "${c##*/}" > "$g/UDC" && udc=$c && break 2
	done
	log "waiting for UDC"
	bootdone k
	sleep 1
done
if [ -z "$udc" ]; then
	log "no UDC; forcing warm reset so the log survives"
	bootdone r
	while :; do sleep 60; done
fi
log "bound ${udc##*/}"
# Keep the DWC3 glue device out of runtime suspend.
echo on > "$udc/device/../power/control" 2>/dev/null

ifconfig lo 127.0.0.1 up
ifconfig usb0 10.42.0.2 netmask 255.255.255.0 up
cat > /tmp/udhcpd.conf <<EOC
interface usb0
start 10.42.0.1
end 10.42.0.1
max_leases 1
lease_file /tmp/udhcpd.leases
option subnet 255.255.255.0
option lease 86400
EOC
touch /tmp/udhcpd.leases
udhcpd /tmp/udhcpd.conf
telnetd -l /bin/sh -p 23

bootdone d
log "ready: telnet 10.42.0.2"
while :; do sleep 3600; done
