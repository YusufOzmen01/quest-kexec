# Quest Pro kexec

A RAM-only ARM64 kexec loader for Meta Quest Pro.

It boots a patched Linux kernel from rooted Android without flashing a
partition. The included initramfs starts a BusyBox shell over USB Ethernet.

## Current status

Tested on Quest Pro OS build `51503870024400340`.

Working:

- all 8 CPUs
- DRM and both panels
- USB Ethernet
- AOP/QMP handoff
- syncboss
- camera CPAS/CDM
- SPMI and SMB5 charging

The patched kernel is in the `oculus-quest-pro-kernel-master` branch of:

https://github.com/YusufOzmen01/oculus-linux-kernel

The same changes are also included as patch files in `kernel/patches/`.

## Layout

- `module/` — kexec module and log modules
- `initramfs/` — BusyBox USB-network initramfs
- `tools/` — capture, prepare, run and shell scripts
- `kernel/patches/` — Quest Pro target-kernel changes
- `HOW_TO_BUILD.md` — build instructions
- `HOW_TO_USE.md` — boot and shell instructions
- `HOW_TO_PORT.md` — adapting another Quest Pro kernel
- `TROUBLESHOOTING.md` — common failures

## Network shell

The initramfs exposes one ECM USB function:

- headset: `10.42.0.2`
- host: `10.42.0.1`
- shell: `telnet 10.42.0.2`

The target runs DHCP, so NetworkManager can normally configure the host
interface automatically.

## Safety

This is experimental. A bad handoff can reboot or lock the headset.

The included scripts only copy files to `/data/local/tmp`. They do not flash,
format or mount partitions.

Always build `quest_kexec.ko` against the exact Android kernel currently running
on the headset. Never force-load a module with the wrong ABI.

## License

GPL-2.0. See `LICENSE`.
