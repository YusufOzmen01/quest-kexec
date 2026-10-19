# Target-kernel patches

Patches applied to the Oculus 4.19 target kernel (base `d83962780d`) to make it
bootable by the kexec loader. The series is a **common base** that is SoC-generic
(both Quest Pro / `seacliff` and Quest 2 / `hollywood`, SM8250/Kona), plus a small
**per-device override** where one value genuinely differs.

## Layout

```
0001-qkx-kexec-target-patches-...patch   # common base (sets QKX_LOG_PHYS 0x9ba80000)
0002-qkx-...SMB5...patch                 # common
0003-qkx-...SMB5...patch                 # common
hollywood/                               # Quest 2 (hollywood) override
  0004-qkx-hollywood-retained-log-in-ramoops-dump-record-1.patch
seacliff/                                # Quest Pro (seacliff) — no override (see its README)
```

## Apply order

```sh
# 1. common base (both devices)
git am 0001-*.patch 0002-*.patch 0003-*.patch      # or: patch -p1 < each
# 2. the connected device's override, if any
git am hollywood/*.patch                            # Quest 2
#   (seacliff: nothing to apply; the base already targets it)
```

## The only per-device difference: QKX_LOG_PHYS

The base sets the retained-log address to `0x9ba80000` (correct for Quest Pro). On
Quest 2 that address lands inside the ramoops **pmsg** zone, which logd rewrites
continuously, so the log can't survive. The hollywood override moves it to
`0x9ba30000` (inside ramoops dump record 1). See the patch header for the Q2
ramoops map.

| Device | QKX_LOG_PHYS | Override |
|---|---|---|
| seacliff (Quest Pro) | `0x9ba80000` | none (base value) |
| hollywood (Quest 2)  | `0x9ba30000` | `hollywood/0004-*.patch` |

## Coupling: marker_read.ko must match

`read-log.sh` reads the retained log via `kexec/module/marker_read.ko`, whose
`QKX_LOG_PHYS` **must equal** the target kernel's. It is a hardcoded `#define` in
`module/marker_read.c`, currently `0x9ba30000` (Quest 2). For a seacliff target,
edit that define to `0x9ba80000` and rebuild `marker_read.ko`. A mismatch makes
`read-log.sh` silently read the wrong page ("no retained log"). (A cleaner long-
term fix would be to make `QKX_LOG_PHYS` a build-time `-D` override with the Q2
value as the default; not done yet.)
