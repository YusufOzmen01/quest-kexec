# Port a custom Quest Pro kernel

This project is for Quest Pro only. Porting here means adapting another Quest
Pro 4.19 kernel release, not another headset or SoC.

## 1. Start from the matching Meta source

Find the Meta source commit for the headset OS version. Keep a clean copy for
building the Android loader module.

## 2. Apply the target patches

The maintained kernel fork already contains the changes. For another source
revision, apply the patches in order:

```sh
git am /path/to/quest-pro-kexec/kernel/patches/*.patch
```

Resolve conflicts by behavior, not by blindly choosing one side. The important
areas are:

- ARM64 entry and SMP/GIC state
- apps SMMU and msm_bus/RPMh
- display clocks/GDSCs/SDE
- DWC3 and USB PHY
- AOP QMP adoption
- SPMI stale IRQ cleanup
- camera CPAS/CDM
- SMB5 charger handoff
- retained log and boot watchdog

## 3. Keep the required target command line

`tools/prep.sh` adds:

```text
rdinit=/init nokaslr qkx_qmp_adopt=1
initcall_blacklist=virtual_sensor_driver_init
```

Do not use loader-side QMP disconnect. Taking the AP/AOP link down caused a PMIC
reset. The target must adopt the inherited link instead.

## 4. Keep the staging layout

```text
Image       0x90080000
initramfs   0x93000000
DTB         0x93200000
retained log 0x9ba80000 (64 KiB)
```

The Image memory size must be at most `0x2f80000`. Initramfs and DTB must each
fit in 2 MiB.

## 5. Test in small steps

First boot with the supplied RAM-only initramfs. Check:

```sh
getconf _NPROCESSORS_ONLN
ls /dev/dri
ip addr show usb0
dmesg | grep -E 'qkxqmp|QPNP SMB5|cam_hw_cdm_init'
```

Only move to a larger userspace after the basic target is repeatable.
