# Building

There are two kernels involved:

1. **Android kernel** currently running on the headset. It is used only to build
   `quest_kexec.ko`; source, configuration, release suffix and `Module.symvers`
   must match the device exactly.
2. **Target kernel** entered by kexec. It is the Meta Quest Pro kernel with the
   patches in `kernel/patches/` (or branch `qkx-kexec` in the companion fork).

## Android loader ABI tree

For OS build `51503870024400340`, use Meta commit `fa2e480a85...`, copy the
headset's `/proc/config.gz` to the output `.config`, disable
`CONFIG_LOCALVERSION_AUTO`, and build `Image modules` with local version
`-g49638c7a8637`. A full module build is required to produce correct symbol
CRCs in `Module.symvers`.

The vendor audio tree needs its pinctrl include directory in `KCFLAGS`:

```sh
SRC=/path/to/meta-kernel-at-fa2e480a85
OUT=/path/to/android-out
adb exec-out 'su -c "zcat /proc/config.gz"' > "$OUT/.config"
$SRC/scripts/config --file "$OUT/.config" --disable LOCALVERSION_AUTO
make -C "$SRC" O="$OUT" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION=-g49638c7a8637 olddefconfig
make -C "$SRC" O="$OUT" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LOCALVERSION=-g49638c7a8637 KCFLAGS=-I$SRC/drivers/pinctrl -j8 Image modules
make module KDIR="$OUT" LOCALVERSION=-g49638c7a8637
modinfo -F vermagic module/quest_kexec.ko
```

The reported vermagic must begin with the output of `adb shell uname -r`.

## Target kernel

Clone `YusufOzmen01/oculus-linux-kernel`, then check out `qkx-kexec`. Or apply
all patches in order to upstream Quest Pro commit `42dc87a978`:

```sh
git am /path/to/quest-pro-kexec/kernel/patches/*.patch
```

Start from the device config and make these configuration changes:

```text
CONFIG_LOCALVERSION_AUTO=n
CONFIG_ION_POOL_AUTO_REFILL=n
CONFIG_USB_CONFIGFS_ECM=y
CONFIG_DEVTMPFS=y
CONFIG_PRINTK_TIME=y
CONFIG_LOG_BUF_SHIFT=20
```

Build a raw ARM64 `Image`. The repository's staging layout requires text offset
`0x80000`, a relocatable little-endian 4 KiB-page image, maximum memory size
`0x2f80000`, and maximum initramfs/DTB size 2 MiB each.

## BusyBox/initramfs

```sh
make initramfs
```

`initramfs/busybox.sh` downloads BusyBox 1.36.1 from busybox.net, verifies its
published SHA-256, then cross-builds it static. `initramfs/build.py` verifies the
binary is static ARM64 and generates deterministic gzip/newc output.
