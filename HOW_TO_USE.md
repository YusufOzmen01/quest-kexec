# Usage guide

> These steps are tested on Meta Quest Pro with rooted Android.

## Prepare

Build the modules, target `Image` and initramfs first. See
[HOW_TO_BUILD.md](HOW_TO_BUILD.md).

Boot Android, enable ADB and confirm root:

```sh
adb shell 'su -c id'
```

Capture the current device information:

```sh
tools/capture.sh out/captured
```

This is read-only. It captures the runtime device tree, iomem, command line and
kernel configuration.

## Create the payload

```sh
tools/prep.sh out/captured /path/to/target/Image \
  out/initramfs.gz out/payload
```

## Boot

```sh
tools/run.sh out/payload
```

The script copies the module, Image, initramfs and DTB to `/data/local/tmp`,
runs `sync`, verifies their hashes and starts kexec.

## Connect to BusyBox

The initramfs exposes USB Ethernet with:

- headset: `10.42.0.2`
- host: `10.42.0.1`

Find the new interface:

```sh
ip link
```

NetworkManager can normally obtain the address from the headset:

```sh
nmcli device connect <interface>
tools/shell.sh
```

If DHCP does not work:

```sh
sudo ip addr add 10.42.0.1/24 dev <interface>
tools/shell.sh
```

Run one command without an interactive shell:

```sh
python3 tools/qkx_sh.py 'uname -a'
```

## Return to Android

```sh
tools/return-android.sh
```

## Read logs

After Android returns:

```sh
tools/read-log.sh
```

To capture Android kernel logs over its existing USB gadget before a run:

```sh
tools/load-usb-log.sh 60
```

`usb_log.ko` intentionally remains installed until Android reboots.
