# Multi-board support

This repo currently targets one device, Quest Pro (`seacliff`). Adding Quest 2
(`hollywood`) requires little new code, because most device variance is already
handled: the loader is SoC-generic, and every payload is built from a live
capture of the connected headset rather than from constants in the tree. The
remaining gap is a short list of static per-board facts that have no defined
home. This document specifies that home — one sourceable shell file per board
under `boards/` — and the small set of code changes that go with it.

Device variance falls into three tiers, ordered here by how specific each is:
shared across boards, fixed per board, and variable per boot.

## Tier 0 — SoC-level, shared across boards

Both headsets use the Qualcomm SM8250 (Kona). Quest 2 is XR2 Gen 1; Quest Pro is
XR2+ Gen 1, a binned SM8250. From the loader's perspective they are the same
chip, so the majority of the module needs no per-board handling:

- The entry gate is `of_machine_is_compatible("qcom,kona")` (`module/loader.c:1028`),
  which holds on both.
- The apps watchdog at `0x17c10000`, the SPMI bases, and GIC/RSC are Kona and
  identical.
- The device-tree nodes the loader walks on its critical execute path (kgsl for
  the GPU, dwc3 for USB, the QMP mailbox) are SoC node names, not board node
  names.
- The kexec staging window is fixed in the module: `QKX_LO`/`QKX_HI`/`QKX_ENTRY`/
  `QKX_INITRD`/`QKX_DTB` = `0x90000000`–`0x93400000` (`module/loader.c:33-37`). This
  is a Kona physical-address choice, not a board choice.
- The ARM64 `Image` boot contract (text offset `0x80000` and the flag checks in
  `tools/prepare.py:103`) belongs to the architecture, not the board.
- The reserved-memory and peripheral nodes the Android handoff edits —
  `cont_splash_region@9c000000`, `secure_display_region`, `qseecom@82400000`,
  `vidc@aa00000` — are defined in `kona.dtsi` with the same addresses on both
  boards and no board override, so the install flow treats them as SoC constants.

The loader source already builds unmodified for both boards.

## Tier 1 — board-level static facts

This is the only tier without a defined home. It is a small, fixed set of values
per board:

- the device label and the `ro.product.device` codename it keys off,
- the syncboss SPI node name,
- the first-time-setup APK name,
- the USB gadget manufacturer string,
- the kernel `LOCALVERSION` the module build must match,
- the first-boot DTB-prep node lists (status-disabled and force-no-map) — see
  the note below on why these sit here even though they don't vary on Kona.

These go in one sourceable shell file per board, `boards/<codename>.sh`, written
as `KEY=VALUE` lines so both the bash tools (`source boards/$dev.sh`) and the
Python tools (parsing the same file) read a single source of truth. The schema:

| Key | Meaning |
|---|---|
| `QKX_BOARD` | codename; matches `ro.product.device` |
| `QKX_BOARD_LABEL` | display label used in package manifests and device guards |
| `QKX_USB_MANUFACTURER` | USB gadget manufacturer string set by the initramfs |
| `QKX_SYNCBOSS_SPI` | syncboss SPI node name passed to the loader (`spi0.0` on seacliff, `spi1.0` on hollywood) |
| `QKX_NUX_APK` | first-time-setup APK selected by the turnkey image builder |
| `QKX_KERNEL_LOCALVERSION` | `LOCALVERSION` the module build must match |
| `QKX_DISABLE_NODES` | DTB nodes set `status = disabled` for a first Android boot (seacliff: qseecom; VIDC stays on for Venus) |
| `QKX_NOMAP_NODES` | reserved-memory nodes forced `no-map` for a first Android boot (splash, secure UI) |

Concrete values live in the board files, not here. `boards/seacliff.sh` is
complete; `boards/hollywood.sh` is complete except for its first-time-setup APK
name, still to be identified.

`QKX_DISABLE_NODES` and `QKX_NOMAP_NODES` are a deliberate special case. On Kona
these nodes come from the shared `kona.dtsi`, so seacliff and hollywood carry
identical values, and the install flow still applies them directly rather than
reading the board file. They are kept in the schema as the extension point for a
future non-Kona board (e.g. Quest 3, SM8550), where the nodes and addresses would
genuinely differ; wiring the install flow to read them is left until such a board
exists and can be tested. Anything else that can be derived at runtime stays
derived rather than stored, which keeps the board files minimal.

## Tier 2 — per-boot runtime data

Everything that differs at the physical-memory level is read from the live device
at prep time, not stored in the repo. A payload is built from a *captured
directory* holding the headset's own `runtime.dtb`, `iomem`, and `cmdline`.
`tools/prep.sh` strips the boot-time watchdog bits from the captured cmdline and
passes the three files to `tools/prepare.py`, which assembles the staging layout.
The memory map (`/memory/reg`), the reserved-memory regions, and the device tree
all come from that capture.

The one piece of per-boot variance that cannot live in a static DTB is the set of
physical pages the stock kernel has handed to secure VMs, which the next kernel
must never touch. These assignments change between boots, so they are enumerated
live by `module/ion_secmap.ko` and folded in as a `no-map` reserved-memory node
by `tools/run.sh` (the `/reserved-memory/qkx-ionsec` block near `tools/run.sh:77`).

This tier already generalizes. The Quest 2's 6 GiB map, its reserved regions, and
its secure carveouts appear in a hollywood capture the same way seacliff's do.
Nothing here belongs in a board file.

## Board selection

The tools read `getprop ro.product.device` from the connected headset — it returns
`seacliff` or `hollywood` — and `source boards/$device.sh`. A `--board` flag
overrides the probe for offline work. An unknown device has no board file, which
is the error path.

Selection keys on `ro.product.device` rather than the device-tree `compatible`
string because both headsets report `qcom,kona` at the SoC level (the premise of
Tier 0) and cannot be distinguished that way. The property is the one identifier
that reliably differs by name.

This also replaces the device checks already in the tree: the
`[ "$MODEL" = seacliff ]` guards in `qkx-install-package.sh:42` and
`qkx-uninstall.sh:26`, plus the hardcoded `'Quest Pro (seacliff)'` string in the
package-verify Python at `qkx-install-package.sh:21`, collapse into a single
lookup against `boards/`.

## Code changes beyond relocation

Most of the work moves constants into a board file. Two sites need real code
changes.

**Syncboss SPI node becomes a `module_param`.** The loader hardcodes
`find_dev(spi_bus, NULL, "spi0.0")` at `module/loader.c:676` and names `spi0.0`
again in the `MODULE_PARM_DESC` at `module/loader.c:88`. The loader already
exposes a long list of module params — `image`, `initrd`, `flush_rpmh`,
`suspend_syncboss`, and others (`module/loader.c:67-89`) — so the syncboss node
name joins them as a `charp` param defaulted from `QKX_SYNCBOSS_SPI`, and one
loader source builds for both boards. The risk is low: the syncboss lookup fires
only under `suspend_syncboss`, which is off by default and sits on the optional
PM-suspend path.

**The RAM-end bound is derived rather than hardcoded.** Two sites assume
`0x380000000` — 12 GiB, Quest Pro's population:

- `module/smmu_secmap.h:29`, in `secmap_ram()`: `pa >= 0x80000000ULL && pa < 0x380000000ULL`.
- `tools/tests/ram_scan2/ram_scan2.c:15`: `end_pfn = PFN_DOWN(0x380000000ULL)`.

Both should come from the kernel's own `memblock` / `max_pfn` instead of a
literal. This is the actual end of RAM for any board, and it is the one constant
that would be incorrect on a 6 GiB Quest 2 rather than merely misplaced. For
seacliff this is a genuine behavior change — the single point in the refactor
that is not a pure constant relocation — and it is accepted because deriving the
bound is correct independent of how many boards the tree supports.

## Hardcode inventory

Every site that encodes a board fact today, and where it moves:

| Site | Currently | Handling |
|---|---|---|
| `module/loader.c:676` (desc at `:88`) | syncboss node `spi0.0` | `module_param`, defaulted from `QKX_SYNCBOSS_SPI` |
| `module/smmu_secmap.h:29` | RAM end `0x380000000` | derived from `memblock`/`max_pfn` (behavior change for seacliff) |
| `tools/tests/ram_scan2/ram_scan2.c:15` | RAM end `0x380000000` | derived from `memblock`/`max_pfn` |
| `qkx-install-package.sh:42`, `:21` | `[ "$MODEL" = seacliff ]`, `'Quest Pro (seacliff)'` | board probe + `boards/` lookup + `QKX_BOARD_LABEL` |
| `qkx-uninstall.sh:26` | `[ "$MODEL" = seacliff ]` | board probe + `boards/` lookup |
| `tools/build-system-package.py:34`, `tools/build-rom-package.py:26` | manifest `device` = `"Quest Pro (seacliff)"` | `QKX_BOARD_LABEL` |
| `tools/build-turnkey-images.py:19` | `FirstTimeNuxSeacliff.apk` | `QKX_NUX_APK` |
| `initramfs/init.sh:33`, `initramfs/ramtest-init.sh:33` | USB manufacturer `quest-pro-kexec` | `QKX_USB_MANUFACTURER` |
| `qkx-install-package.sh` prep-time DT edits | qseecom `disabled`, splash + secure-display `no-map` | SoC-fixed on Kona; left as constants, mirrored into `QKX_DISABLE_NODES` / `QKX_NOMAP_NODES` as the extension point for a future non-Kona board (the `/memory` reg is Tier 2 — from capture) |
| `module/loader.c:2`, `:1136`; `tools/prepare.py:2`, `:104` | "Quest Pro" wording | generalize text (cosmetic) |
| `tools/disable-alt-updaters.py` | hardcoded author path `ROOT=Path('/home/yusuf/...')` | not a board fact — made a required `images` argument |

The last two rows carry no board semantics. The "Quest Pro" strings are cosmetic
and generalize. The `/home/yusuf/...` path in `disable-alt-updaters.py` was one
contributor's local working directory, not a board fact; it is now passed as a
required `images` argument.
