# Quest Pro full 12 GiB RAM — rescue-only validation

## Result

Kernel #253, payload `quest-pro-kexec/out/ram12-253`, registers the full stock
physical memory map and passes repeated rescue-only memory tests with normal
GPU/display kernel probing enabled. No alternate Android was launched during
these experiments. Installed Android boots were used only as the kexec loader
and recovery environment. No installed partitions or userdata images changed.

Image: `build/runs/253/Image`
SHA-256: `5ded7a62459b5cd7d57f79a06e7bb4c6e798df8ead98d4a8c14fe0b187d266a8`.
Current target: full-RAM Android with Venus and CMS/updater disabled,
`out/ospayload-253-ram12-noupdater`, tag `ram253noupdater`.
User confirmed visible UI on the earlier combined Venus/full-RAM boot.
The newest CMS/updater-removal boot completed initialization and a Venus
encode test; final visibility awaits confirmation. Image replacements require
fresh maps: prior payload maps are now stale. Fresh low6 rollback:
`out/ospayload-249-audio-noupdater`. See the working-setup document.
The archive/run identifier is 253; this Image's emitted kernel build counter
is #250.

The physical RAM population is 12 GiB, with normal firmware/address-map holes,
secure VM reservations, kernel metadata and splash reservations unavailable to
the ordinary allocator. Final repeat MemTotal: 11,798,644 KiB (~11.25 GiB).
This is NOT a claim that all 12 GiB can be freely allocated: protected/firmware
memory must remain excluded. Dynamic secure reservations vary between boots.

### Confirmed tests

- `ram12-253owned`: read scan completed through physical `0x37ff00000` to
  exclusive end `0x380000000`; 11,337 MiB free-page reads, 233 MiB reserved,
  540 MiB unmapped, 176 MiB busy skipped. Pattern test allocated and verified
  all 11,008 requested MiB, zero mismatches, freed all pages.
- Fresh boot `ram12-253repeat`: read scan completed again; 11,346 MiB reads,
  233 MiB reserved, 532 MiB unmapped, 175 MiB busy skipped. Another 11,008 MiB
  allocation/write/read test passed with zero mismatches.
- On that repeat boot, the strengthened pattern test filled ALL allocated
  blocks before verifying ANY block, with physical-address-dependent data
  and cache clean/invalidate between writes and reads. All 11,008 MiB passed
  again with zero mismatches. This also checks aliases between allocated banks.
- Earlier graphics-isolated #249 boots passed two full scans plus 10,240 and
  11,008 MiB pattern tests; these are superseded by the #253 tests with graphics
  probing enabled.

This is substantial allocation/read/write validation, not exhaustive testing
of all bit patterns or long-duration thermal/retention behavior. Android with
this full map has deliberately NOT been tested. Existing baseline Android payloads and image maps were left alone during
rescue validation. Subsequent Android tests are recorded below; Venus/full-RAM
coexistence is not yet verified.

## Subsequent Android test (user authorized Android after rescue validation)

- Venus-enabled `out/ospayload-253-ram12`, tag `android253ram12`: gadget gone
  at 40 s, no completed Android boot established. Retained log shows CPU0
  soft lockup waiting in cross-CPU synchronization during BPF setup, with
  another CPU not responding. This is not proof of a DDR fault or a proven
  Venus root cause.
- `out/ospayload-253-ram12-novidc`, tag `android253ram12nv`: same full memory
  map and splash guard, VIDC disabled, alive at 60 s, sys.boot_completed=1.
  MemTotal 11,786,344 KiB (~11.24 GiB), MemAvailable 9,050,508 KiB at initial
  check. Bluetooth ON, ALSA audio card registered. Proximity and ET/FT restored,
  user calibration true/both_temporal verified. No GPU timeout or soft-lockup
  messages in the subsequent check. Visual rendering awaits user confirmation.
- No image replacements or installed partition changes for these tests.

## Full-RAM Android success and restored Venus

Initial full-RAM Android rendering was blank and the next reboot soft-locked,
even with VIDC disabled; low6 rollback restored visible UI. The expanded
read-only stock snapshot in `module/ion_secmap.c` + `module/smmu_secmap.h`
then added master-side secure SMMU page tables and cached secure table pools.
Their private stock layouts are corroborated through public VMID/TTBR attrs,
and table traversal is serialized with the driver's assignment/table locks.
Only table pages/pools are read; mapped secure payload pages are not read.

The stock driver shares these pages with HLOS READ|WRITE but no EXEC. Reusing
them as ordinary executable/JIT RAM after kexec is unsafe, despite passing
read/write-only tests. The former ION/KGSL-buffer snapshot omitted these table
pages. The first expanded snapshot found 30 table pages across nine domains:
GPU, crypto, CVP, display, video and FastRPC. Current loader reserves their
ranges along with the existing secure ION ranges; no giveback/SCM reassignment.
This omission is strongly implicated in the Android-specific stalls; not every
historical CPU/GPU fault has been individually attributed.

- `ram253sharedtables` (VIDC disabled): Android boot complete, user confirmed
  visible UI. Live Android test allocated 4096 MiB, filled all blocks before
  verifying, zero mismatches, all pages freed. Boot completion, Bluetooth ON,
  audio card and stock-imported calibration remained intact afterward.
- `ram253sharedvenus` and fresh `ram253sharedvenusrepeat`: VIDC enabled with
  qkx_venus_reload_first=1 and qkx_venus_no_pc=1. Both boots completed Android
  initialization and each passed three 1080p H.264 hardware encode/decode
  cycles (300 frames each, EOS=true); detached script STATUS=PASS.
- Downloaded output `work/venus-test/encoded-ram12-253.mp4`: host ffprobe
  H.264/1920x1080/300 frames/10 seconds; FFmpeg full decode exit 0.
- Latest MemTotal 11,774,064 KiB; no GPU timeout, stalled-SMMU or soft-lockup
  messages in the final checks. CVP firmware still reported a separate NOC
  error during boot; this is not a Venus test failure and remains open.
- Visibility on the final combined Venus-enabled boot still needs user
  confirmation. Long-duration/other-codec/secure-DRM tests remain open.
- The low-IOMMU-table-allocation diagnostic (#254 archive) did not resolve the
  stalls, was removed from source, and is not part of this working recipe.

Before reproduction, rebuild/use the expanded stock-compatible ion_secmap
module as `module/ion_secmap.ko`. It is essential to the Android recipe.
Historical rescue tests below preceded that snapshot enhancement.

## Changes needed

1. Restore full stock `/memory/reg` rather than the conservative low-6-GiB map.
   Use the values in `out/captured/runtime.dtb`, while retaining the otherwise
   working `out/captured-low6` device tree setup:
   ```
   0 80000000 0 39900000
   2 0        1 80000000
   0 c0000000 1 40000000
   ```
2. Mark `/reserved-memory/cont_splash_region@9c000000` `no-map` in the target
   DTB. This retains the 35 MiB inherited splash carveout and prevents the
   ordinary CPU linear map/allocator from treating it as general RAM.
3. In `oculus-linux-kernel/techpack/display/msm/sde/sde_kms.c`, guard
   `_sde_kms_release_splash_buffer()` against freeing unmapped PFNs. The driver
   formerly called `free_reserved_page()` unconditionally. #253 checks the
   whole range with `pfn_valid()` before releasing anything, logs
   `preserving unmapped splash memory 0x9c000000+0x2300000`, and retains it.
4. Continue the existing read-only stock secure-ION snapshot/no-map handoff in
   `tools/run.sh`; do not use GIVEBACK or arbitrary hypervisor reassignment.

The final payload does NOT disable GPU/display. QSEE and VIDC remain disabled
for these RAM experiments. No Venus flags or Android initramfs are necessary.
Normal cpuidle remains enabled; `cpuidle.off=1` was only an unsuccessful
intermediate diagnostic, not the fix.

Do NOT mark the DFPS-data region no-map with the unmodified PLL driver:
`drivers/clk/qcom/mdss/mdss-pll-util.c` releases it via its own bootmem helper.
That experiment caused an early `new_slab` page fault at the DFPS physical
range. Restoring its original mapping removed that separate experimental bug.

## The old RAM scanner was unsafe for this purpose

`work/ram_scan2/ram_scan2.c` previously read every valid PFN, including reserved
carveouts and pages owned by drivers. A valid PFN/linear mapping does not mean
HLOS may read its contents: secure GPU/ION pages can remain inaccessible.
Raw scans stalled near splash RAM or upper driver-owned pages while rescue
could otherwise remain alive. These failures alone are not proof of bad DDR.

The scanner now:
- accepts an exclusive `end_pfn` (default physical end `0x380000000`);
- skips invalid/unmapped PFNs, PageReserved pages, and pages with a nonzero
  compound-aware page_count;
- reports actual read/reserved/unmapped/busy totals.

The read-only free-page walk is diagnostic, not an ownership guarantee against
concurrent allocation. The stronger test is `work/ram_owned/ram_owned.c`:
- obtains 1 MiB blocks ONLY through Linux `alloc_pages`, keeping all blocks
  allocated simultaneously;
- uses GFP_HIGHUSER_MOVABLE to permit movable/CMA allocation;
- writes every 64-bit word with a physical-address-dependent pattern;
- flushes through the standard ARM64 cache API, then verifies all blocks;
- frees every allocation before returning.
No arbitrary physical pages, device registers, or firmware-owned memory are
written by this test. Both modules deliberately return `-EAGAIN` after their
completed test so they can be inserted repeatedly without retaining a module.
Thus insmod's `Resource temporarily unavailable` is expected ONLY when the
corresponding `done`/`DONE ... mismatches=0` kernel message is present.

## Reproduce (never launch Android)

From `quest-pro-kexec`:
```
source /home/yusuf/kexectest/work/cycle.sh
rescue out/ram12-253 RAM_LOG_TAG shutdown_cdsp=1 shutdown_subsys=venus,npu
# Do NOT call launch or qkx-launch-android.
```
Standalone initramfs: `out/ram12-rescue-stream-initramfs.gz`, built from
`work/ram12-init.sh` with `initramfs/build.py` and `out/busybox`, no --os-dir.
It contains no Android image maps/launch scripts. It streams /dev/kmsg over
UDP to 10.42.0.1:5140 and pins its diagnostic parent to CPU0.

To rebuild the payload (stock capture retained; no installed partition writes):
```
tools/prep.sh out/captured-low6 /home/yusuf/kexectest/build/runs/253/Image \
  out/ram12-rescue-stream-initramfs.gz out/NEW_RAM_PAYLOAD \
  'qkx_uart_dma_cleanup=1 qkx_skip_regulator_cleanup=1 deferred_probe_timeout=-1 qkx_stallmon=1 qkx_beat=1'
fdtput -t x out/NEW_RAM_PAYLOAD/boot.dtb /memory reg \
  $(fdtget -t x out/captured/runtime.dtb /memory reg)
fdtput -t s out/NEW_RAM_PAYLOAD/boot.dtb \
  /reserved-memory/cont_splash_region@9c000000 no-map ''
fdtput -t s out/NEW_RAM_PAYLOAD/boot.dtb /soc/qseecom@82400000 status disabled
fdtput -t s out/NEW_RAM_PAYLOAD/boot.dtb /soc/qcom,vidc@aa00000 status disabled
```

Build each external test module against `build/diagnostic-kernel` using the
normal diagnostic clang/arm64 flags and M=work/ram_scan2 or M=work/ram_owned.
Transfer through a short-lived host HTTP server; load from rescue /tmp:
```
taskset 10 insmod /tmp/ram_scan2.ko
taskset 20 insmod /tmp/ram_owned.ko mib=11008
dmesg | grep -E 'ramscan2: done|ramowned: (allocation complete|DONE)'
```
CPU pinning keeps log/network readers off a CPU potentially stalled by a
read probe. Successful scan normally takes ~82 seconds; pattern test takes
~3–9 seconds. Logs: `quest-pro-kexec/logs/ram12-253owned-udp.txt` and
`ram12-253repeat-udp.txt`. Kernel/test changes are uncommitted.
