# Architecture and handoff notes

## Memory layout

| Payload | Physical address |
|---|---:|
| target Image | `0x90080000` |
| initramfs | `0x93000000` |
| DTB | `0x93200000` |
| retained ring | `0x9ba80000`, 64 KiB |

The loader stages segments in ordinary pages, freezes Android userspace,
quiesces GPU, syncboss and DWC3, calls the normal device shutdown path, offlines
secondary CPUs, flushes RPMh, copies the payload through an identity-mapped
ARM64 transition and enters the target at EL1.

## Important inherited-state fixes

The target patch series contains the fixes established during bring-up:

- SMP/GIC and apps-SMMU inherited-state tolerance
- msm_bus/RPMh QoS handling (Quest's nonexistent UFS-card master is skipped)
- display GDSCs, Display CC, SDE RSC and DRM handoff
- DWC3/USB PHY gadget handoff
- AOP QMP link adoption and inherited QDSS vote adoption
- syncboss quiesce through Android's PM callback
- SPMI stale interrupt disable
- CPAS/CCI child-node lookups and camera/CDM diagnostics
- SMB5 probe with unsafe synthetic initial IRQ callbacks skipped
- retained log plus boot watchdog diagnostics

## Runtime tree requirement

Use the runtime tree reconstructed from `/proc/device-tree`, not the original
blob in `/sys/firmware/fdt`. The original boot FDT caused a cold reset in a
validated comparison test. `tools/capture.sh` handles the correct conversion.

## QMP

Never take the AP-to-AOP link down during handoff. Loader-side QMP disconnect
caused an immediate PMIC reset. The target adopts the inherited physical link,
recycles only the logical channel and imports the negotiated mailbox metadata.
The required target command line option is `qkx_qmp_adopt=1`.

## SMB5

Android's normal `device_shutdown()` already calls `smb5_shutdown()`. The target
must not manually call the 11 synthetic initial-status interrupt handlers after
kexec; they reset or lock the PMIC/USB-C path. The patched driver defaults to
skipping those calls. Charging and real plug interrupts remain functional.
