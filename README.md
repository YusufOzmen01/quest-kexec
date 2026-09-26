# Quest Pro kexec

Experimental ARM64 kexec loader and RAM-only Linux environment for Meta Quest Pro
(Seacliff/Kona, Linux 4.19). It hands execution from the rooted Android kernel to
a patched Quest kernel entirely from RAM. No partition is flashed or mounted.

The included initramfs exposes a BusyBox shell over USB Ethernet (ECM):

- target: `10.42.0.2`
- host: `10.42.0.1` (offered by target DHCP)
- shell: `telnet 10.42.0.2`
- no USB serial functions and no block-device mounts

## Status

Validated on Quest Pro OS build `51503870024400340`, Android kernel
`4.19.325-cip128-st12-g49638c7a8637`, Meta source commit
`fa2e480a85c6adbcc73dfc4f0ab0728781e14584`.

Working in the RAM-only target: 8 CPUs, DRM/panels, USB ECM, AOP QMP handoff,
syncboss, camera CPAS/CDM, SPMI and SMB5 charging. The target kernel patches are
also maintained on the `qkx-kexec` branch of
[`YusufOzmen01/oculus-linux-kernel`](https://github.com/YusufOzmen01/oculus-linux-kernel).
Patch files are mirrored under `kernel/patches/` for review and portability.

## Safety

This is research software. A failed handoff can freeze or reboot the headset.
Keep the stock boot image available and use only on a rooted device you can
recover. The supplied scripts only write temporary files under
`/data/local/tmp`; they do not flash, format or mount partitions.

Do **not** use a loader module built for a different running Android kernel.
Module ABI mismatches can crash Android during an irreversible shutdown.

## Host requirements

Linux host with:

- `adb`, root access through `adb shell 'su -c ...'`
- Clang/LLD and an `aarch64-linux-gnu-` toolchain
- Python 3, `dtc`, `fdtput`, `curl`, `make`, `cpio`, `gzip`
- NetworkManager or another way to configure the ECM host interface

## Quick start

See [docs/build.md](docs/build.md) for complete kernel/module build steps.
Assuming the patched target `Image` and matching modules already exist:

```sh
# Build static BusyBox and the ECM-only initramfs
make initramfs

# Build loader against the *running Android kernel's* completed build tree
make module KDIR=/path/to/android-kernel-out \
  LOCALVERSION=-g49638c7a8637

# Capture runtime DT, iomem, cmdline, config (read-only on device)
tools/capture.sh out/captured

# Prepare a payload using the patched target kernel
tools/prep.sh out/captured /path/to/target/Image \
  out/initramfs.gz out/payload

# Freshly pushes and verifies every file before jumping
tools/run.sh out/payload
```

After USB re-enumerates, NetworkManager should obtain `10.42.0.1` from the
target. If not:

```sh
nmcli device connect <ECM-interface>
# or: sudo ip addr add 10.42.0.1/24 dev <ECM-interface>
telnet 10.42.0.2
```

Return to Android while the target is running:

```sh
python3 tools/qkx_sh.py 'echo r > /proc/qkx_bootdone'
```

After Android returns, retrieve a retained target log:

```sh
tools/read-log.sh
```

## Repository layout

- `module/` — kexec loader, ARM64 transition code, retained-log reader
- `initramfs/` — ECM-only PID 1 and reproducible BusyBox/initramfs builders
- `tools/` — device capture, payload preparation, run and shell helpers
- `kernel/patches/` — patches required by the target kernel
- `docs/` — detailed build, usage and architecture notes

## License

GPL-2.0. See `LICENSE`. BusyBox is downloaded from upstream and built locally;
its binary and source are not committed.
