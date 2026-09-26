# Usage and troubleshooting

## Normal run

1. Charge the headset and boot rooted Android.
2. Build loader for that exact Android kernel.
3. Build patched target kernel and initramfs.
4. Run `tools/capture.sh out/captured` after every OS/device-tree change.
5. Prepare and execute as shown in the README.

Each execution freshly pushes Image, initramfs and DTB, runs `sync` twice and
verifies MD5 hashes before inserting the loader.

## Target shell

The initramfs advertises one USB ECM function (VID:PID `1d6b:0105`) with fixed
MAC addresses. The interface name differs from old ACM+ECM payloads because ECM
is now interface zero. Prefer finding it by MAC or `ip link` rather than hard
coding its name.

`udhcpd` offers only `10.42.0.1/24`; it advertises no gateway or DNS, so the host
should not route Internet traffic through the headset.

## Reset classification

- Retained marker exists: target reached far enough to initialize the ring and
  returned via a DRAM-preserving warm reset.
- Marker is zero and pstore contains `quest_kexec: native CPU shutdown failed`:
  loader panic inside Android.
- Marker is zero with no pstore evidence: PMIC hard/cold reset or very early
  failure; do not attribute it to the last target probe without a checkpoint.

`tools/read-log.sh` reads both retained-ring and pstore evidence after Android
returns. A stale marker is cleared before every run.

## Common failures

- **Module won't load / disagrees about symbol version:** rebuild against the
  exact running Android source/config/release; never force-load it.
- **Gadget exists but target is unreachable:** ask NetworkManager to connect or
  manually assign `10.42.0.1/24` to the new ECM interface.
- **No gadget and Android returns:** retrieve retained logs immediately.
- **Hard lockup:** force reboot may erase all evidence. Avoid repeating an
  unchanged payload; add a checkpoint or narrow the boundary first.
