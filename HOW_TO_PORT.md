# Porting to another Meta Quest kernel

This implementation has only been tested on Meta Quest Pro. Other Quest models
use different SoCs, device trees, memory maps and hardware handoff states.

## Same headset, newer kernel

1. Get the source matching the new Android release.
2. Build `quest_kexec.ko` against its exact config, local version and
   `Module.symvers`.
3. Apply the patches under `kernel/patches/` to the new target source.
4. Resolve conflicts carefully in the affected drivers.
5. Capture a fresh runtime device tree with `tools/capture.sh`.
6. Test first with the supplied RAM-only initramfs.

Important target areas include:

- ARM64 entry and CPU startup
- GIC and SMMU inherited state
- RPMh and interconnect voting
- display clocks and power domains
- USB controller and PHY handoff
- AOP/QMP mailbox adoption
- SPMI and charger state
- retained logging and watchdog recovery

## Another Quest headset

Do not reuse the Quest Pro physical addresses or device names without checking
them. At minimum, determine:

- target Image, initramfs and DTB staging addresses
- safe retained-log RAM
- target kernel text offset and maximum image size
- CPU, GIC and SMMU setup
- USB controller and UDC name
- watchdog behavior
- platform-device names used by loader shutdown callbacks
- AOP/mailbox protocol used by that SoC

Update `module/loader.c`, `module/transition.S`, `tools/prepare.py` and the target
kernel patches for the new board.

## First test

Keep the first target small:

- static BusyBox initramfs
- no block-device mounts
- USB Ethernet only
- retained kernel log enabled

Verify CPUs, USB and basic device probing before adding a larger userspace.

Do not disconnect the AOP QMP link on Quest Pro. The tested implementation
adopts the inherited link with `qkx_qmp_adopt=1`.
