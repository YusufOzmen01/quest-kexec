# Build guide

> This project currently has tested settings only for Meta Quest Pro.

Two kernel trees are used:

1. The **Android kernel tree** builds `quest_kexec.ko` for the kernel currently
   running on the headset.
2. The **target kernel tree** builds the custom kernel entered by kexec.

## Requirements

Install:

- Clang and LLD
- `aarch64-linux-gnu-` GCC/binutils
- `adb`
- Python 3
- `dtc` and `fdtput`
- `curl`, `make` and standard build tools

## Android kernel and loader module

Check the running version:

```sh
adb shell uname -r
```

Use matching source and save the device config:

```sh
mkdir -p android-out
adb exec-out 'su -c "zcat /proc/config.gz"' > android-out/.config
```

For the tested Quest Pro release, the source commit is:

```text
fa2e480a85c6adbcc73dfc4f0ab0728781e14584
```

Build the kernel and modules so that `Module.symvers` contains the correct
symbol CRCs:

```sh
SRC=/path/to/android-kernel-source
OUT=$PWD/android-out
LOCAL=-g49638c7a8637

$SRC/scripts/config --file "$OUT/.config" --disable LOCALVERSION_AUTO

make -C "$SRC" O="$OUT" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION="$LOCAL" olddefconfig

make -C "$SRC" O="$OUT" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION="$LOCAL" KCFLAGS=-I$SRC/drivers/pinctrl \
  -j"$(nproc)" Image modules
```

Build the kexec and log modules:

```sh
make module KDIR="$OUT" LOCALVERSION="$LOCAL"
```

Verify the result:

```sh
modinfo -F vermagic module/quest_kexec.ko
adb shell uname -r
```

The release strings must match.

## Target kernel

For the tested Quest Pro kernel:

```sh
git clone https://github.com/YusufOzmen01/oculus-linux-kernel
cd oculus-linux-kernel
git checkout oculus-quest-pro-kernel-master
```

Build a raw ARM64 `Image` using the headset config. Required options include:

```text
CONFIG_LOCALVERSION_AUTO=n
CONFIG_ION_POOL_AUTO_REFILL=n
CONFIG_USB_CONFIGFS_ECM=y
CONFIG_DEVTMPFS=y
CONFIG_PRINTK_TIME=y
CONFIG_LOG_BUF_SHIFT=20
```

For another Quest model or kernel release, see
[HOW_TO_PORT.md](HOW_TO_PORT.md).

## Initramfs

From this repository:

```sh
make initramfs
```

This downloads and verifies BusyBox 1.36.1, cross-builds it statically for
ARM64 and writes:

```text
out/initramfs.gz
```
