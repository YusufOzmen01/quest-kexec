# Meta Quest kexec

An experimental ARM64 kexec implementation for Meta Quest headsets.

It starts a custom Linux kernel from rooted Android without flashing a
partition. The included RAM-only initramfs provides a BusyBox shell over USB
Ethernet.

> **Note:** This project has only been developed and tested on Meta Quest Pro.
> Other Quest headsets require device-specific kernel and loader work.

## What works on Quest Pro

- all 8 CPUs
- DRM and both panels
- USB Ethernet shell
- AOP/QMP handoff
- syncboss
- camera CPAS/CDM
- SPMI and SMB5 charging

The tested kernel changes are available in:

https://github.com/YusufOzmen01/oculus-linux-kernel

Patch copies are also included under `kernel/patches/`.

## Basic build

Install Clang/LLD, an `aarch64-linux-gnu-` toolchain, Python 3, `adb`, `dtc`,
`fdtput`, `curl` and `make`.

Build the kexec modules against the exact Android kernel running on the headset:

```sh
make module KDIR=/path/to/android-kernel-out \
  LOCALVERSION=-g49638c7a8637
```

Build the BusyBox USB-network initramfs:

```sh
make initramfs
```

See [HOW_TO_BUILD.md](HOW_TO_BUILD.md) for preparing the Android and target
kernel trees.

## Basic usage

With rooted Android running:

```sh
# Capture the current runtime device tree and kernel information
tools/capture.sh out/captured

# Prepare the target payload
tools/prep.sh out/captured /path/to/target/Image \
  out/initramfs.gz out/payload

# Push, verify and boot it
tools/run.sh out/payload
```

Connect to the shell after USB Ethernet appears:

```sh
nmcli device connect <interface>
tools/shell.sh
```

Return to Android:

```sh
tools/return-android.sh
```

See [HOW_TO_USE.md](HOW_TO_USE.md) for more information.

## Logs

Read a retained target-kernel log after Android returns:

```sh
tools/read-log.sh
```

Capture Android kernel logs through its current USB gadget:

```sh
tools/load-usb-log.sh 60
```

## Safety

A failed handoff can reboot or lock the headset. The supplied scripts only copy
files to `/data/local/tmp`; they do not flash, format or mount partitions.

Never force-load a module built for a different Android kernel.

## License

GPL-2.0. See `LICENSE`.
