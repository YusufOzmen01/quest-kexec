# Quest-KeXec

A kernel module for Meta Quest headsets that allows you to load a custom kernel without unlocking the bootloader of the headset. Based on [kexec-at-home](https://github.com/provide-salad/kexec-at-home)

> **Note:** This project has only been developed and tested on Meta Quest Pro.
> Other Quest headsets require device specific kernel and loader work.
> **ROOT IS REQUIRED**

A modified kernel for this kexec for the Quest Pro is available [from the repository here](https://github.com/YusufOzmen01/oculus-linux-kernel)
The changes done to the kernel are in `kernel/patches/` as patches. 

## Basic build

Install Clang/LLD, an `aarch64-linux-gnu-` toolchain, Python 3, `adb`, `dtc`, `fdtput`, `curl` and `make`.

Build the kexec modules against the exact Android kernel running on the headset:

```sh
make module KDIR=/path/to/android-kernel-out LOCALVERSION=-g49638c7a8637
```

To build a basic initramfs that has busybox and USB ethernet (to get shell), run:

```sh
make initramfs
```

See [HOW_TO_BUILD.md](HOW_TO_BUILD.md) for preparing the Android and target
kernel trees.

## Basic usage

```sh
# Capture the current runtime device tree and kernel information
tools/capture.sh out/captured

# Prepare the target payload
tools/prep.sh out/captured /path/to/target/Image out/initramfs.gz out/payload

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

Note: AI has been used heavily on this project

## License

GPL-2.0. See `LICENSE`.
