# Use

## 1. Capture the running device state

Boot rooted Android and connect ADB:

```sh
tools/capture.sh out/captured
```

This reads the runtime device tree, iomem, command line and kernel config. It
does not modify the device.

Use the runtime tree from `/proc/device-tree`. The original
`/sys/firmware/fdt` blob is not safe for this handoff.

## 2. Prepare the payload

```sh
tools/prep.sh out/captured /path/to/target/Image \
  out/initramfs.gz out/payload
```

## 3. Boot it

```sh
tools/run.sh out/payload
```

The script freshly pushes and verifies the module, Image, initramfs and DTB.
All device files are placed under `/data/local/tmp`.

## 4. Connect to the shell

Find the new Ethernet interface with `ip link`. Then:

```sh
nmcli device connect <interface>
tools/shell.sh
```

If DHCP is unavailable:

```sh
sudo ip addr add 10.42.0.1/24 dev <interface>
tools/shell.sh
```

Run one command without telnet:

```sh
python3 tools/qkx_sh.py 'uname -a'
```

## 5. Return to Android

```sh
tools/return-android.sh
```

## Logs

After Android returns:

```sh
tools/read-log.sh
```

This loads `marker_read.ko` and prints the target's retained RAM log.

To capture Android kernel messages over the existing USB gadget before kexec:

```sh
tools/load-usb-log.sh 60
```

`usb_log.ko` hooks the active gadget's EP0 callback and intentionally cannot be
unloaded safely. Reboot Android to remove it.
