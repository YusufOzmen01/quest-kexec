# Build

There are two kernel builds:

- **Android kernel:** used to compile `quest_kexec.ko`
- **Target kernel:** the patched kernel booted by kexec

## Requirements

Install:

- Clang and LLD
- `aarch64-linux-gnu-` GCC/binutils
- `adb`, Python 3, `dtc`, `fdtput`, `curl`, `make`

## 1. Build the Android kernel tree

The loader must match the kernel reported by:

```sh
adb shell uname -r
```

For OS build `51503870024400340`, use Meta kernel commit:

```text
fa2e480a85c6adbcc73dfc4f0ab0728781e14584
```

Save the running config:

```sh
mkdir -p android-out
adb exec-out 'su -c "zcat /proc/config.gz"' > android-out/.config
```

Disable the automatic git suffix, then build `Image modules`. A full modules
build is needed for a correct `Module.symvers`.

```sh
SRC=/path/to/android-kernel-source
OUT=$PWD/android-out
$SRC/scripts/config --file "$OUT/.config" --disable LOCALVERSION_AUTO

make -C "$SRC" O="$OUT" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION=-g49638c7a8637 olddefconfig

make -C "$SRC" O="$OUT" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION=-g49638c7a8637 KCFLAGS=-I$SRC/drivers/pinctrl \
  -j"$(nproc)" Image modules
```

## 2. Build the kexec and log modules

```sh
make module KDIR="$OUT" LOCALVERSION=-g49638c7a8637
modinfo -F vermagic module/quest_kexec.ko
```

The vermagic must start with the output of `adb shell uname -r`.

## 3. Build the target kernel

Use the patched kernel fork:

```sh
git clone https://github.com/YusufOzmen01/oculus-linux-kernel
cd oculus-linux-kernel
git checkout oculus-quest-pro-kernel-master
```

Build it using the Quest Pro config. The target needs at least:

```text
CONFIG_LOCALVERSION_AUTO=n
CONFIG_ION_POOL_AUTO_REFILL=n
CONFIG_USB_CONFIGFS_ECM=y
CONFIG_DEVTMPFS=y
CONFIG_PRINTK_TIME=y
CONFIG_LOG_BUF_SHIFT=20
```

The result must be a raw ARM64 `Image`.

## 4. Build the initramfs

```sh
make initramfs
```

This downloads and verifies BusyBox 1.36.1, builds it statically for ARM64 and
creates `out/initramfs.gz`.
