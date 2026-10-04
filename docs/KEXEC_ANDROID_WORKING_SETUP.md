# Quest Pro kexec → Android: Working Setup & Fix Log

Current kernel archive/run: #253 (emitted build counter #250), SHA-256
`5ded7a62459b5cd7d57f79a06e7bb4c6e798df8ead98d4a8c14fe0b187d266a8`.
Current payload: `quest-pro-kexec/out/ospayload-253-ram12-noupdater`.
Latest boot tag: `ram253noupdater`. Full RAM + H.264 hardware codecs validated;
UI confirmed on the preceding full-RAM/Venus boot. This new boot removes CMS
and OTA entrypoints; final visibility awaits user confirmation.
Fresh-map low6 rollback: `out/ospayload-249-audio-noupdater`.
Older payload maps are stale after the system/system_ext image replacements.

> This file documents the complete recipe that got alternate Android to
> **visually render**, tracking active, and first-time setup usable without
> controllers, plus the open items (Bluetooth, Wi-Fi). It is permanent
> documentation; keep it updated when things change.

---

## 0. CURRENT KNOWN-GOOD CONFIGURATION (visually confirmed: NUX visible)

Confirmed by the user on 2026-10-02 after a long regression hunt.

| Piece | Value |
|---|---|
| Kernel | #243, `build/runs/243/Image`, SHA-256 `4c2d6df5e97e362d...` (Wi-Fi-capable; includes `qkx_stall.c` exports, commit `095de251d8`) |
| Payload | `quest-pro-kexec/out/ospayload-dock` (built with `prep out/captured-sensors-on 243 ...`, `CAP=out/captured-low6`) |
| vendor.img | `work/no-usb-images/vendor.img` = `work/vendor-current-backup.img` (rebuilt `bcmdhd.ko`, NO `vrmountlock` rc) |
| odm.img | `work/no-usb-images/odm.img` = stock odm **minus** `/etc/vintf/manifest/charging_accessory_service.xml` (backup of the stock one: `work/odm-pre-dockfix.img`) |
| userdata | original 16 GiB `data.img` (an 8 GiB `cleandata.img` test image is also on the device and can be deleted) |
| Launch script | `initramfs/os/qkx-launch-android`: re-runs itself detached (`QKX_DETACHED`), two-run splash (`qkx-splash 1`, 1 s pause, then `qkx-splash 180`), optional `QKX_SWITCH_DELAY` |

### The actual root cause of "Android runs, panel shows nothing / headset thinks it is not worn"

`VrPowerManagerService.prepareService()` blocks forever in
`DockstateNotifier$DockstateSensor.<init>` -> `ServiceManager.waitForDeclaredService("vendor.meta.hardware.dock.IDock/default")`
(proved with `debuggerd -j <system_server>`: thread `VrPowerManagerService`
parked in `waitForServiceNative`). The odm VINTF fragment
`/odm/etc/vintf/manifest/charging_accessory_service.xml` DECLARES that AIDL HAL,
but `charging_accessory_service` never starts on this kernel/DT
(`init: Could not find 'aidl/vendor.meta.hardware.dock.IDock/default'` every second).
Because the wait never returns, `prepareService()` never reaches
`setInitialState()`, `setMountNotifierEnabled`, the hotplug notifier or
`registerBroadcastReceivers()`. Consequences seen for days:
`HEADSET_UNMOUNTED` forever, no `prox_close` receiver, tracking in Standby,
VR lockscreen and crash dialogs covering the view, shell content never drawn
(only the compositor's own IPD overlay showed).

Fix: delete that one VINTF fragment from the odm image copy so the framework
treats the dock as "not declared" and skips the wait:
```
cp work/no-usb-images/odm.img work/odm-pre-dockfix.img
debugfs -w -R 'rm /etc/vintf/manifest/charging_accessory_service.xml' work/no-usb-images/odm.img
e2fsck -fn work/no-usb-images/odm.img
# reinstall through the verified pipeline (odm only):
adb shell 'su -c "rm -f /data/local/tmp/qkx/img/odm.img"'
cd quest-pro-kexec && tools/os-install.sh /home/yusuf/kexectest/work/no-usb-images out/captured-sensors-on 16
```
After this the `Waited one second for ...dock.IDock` spam is gone.

This also explains why the earlier `vrmountlock` workaround "worked": it fed a
mounted state to a half-initialised service. Whether `vrmountlock` is still
needed with the dock fix is UNTESTED (the test command was aborted). Do not
assume it is required, and do not assume it is harmless.

### Things that were red herrings (do not chase again)
- `crtc-1` (writeback, id 185) owning eye planes 57/79: stock has exactly the
  same layout (verified with `drmtest/drmdump` on stock).
- `wb:2 kickoff timed out` bursts (8 per boot is normal; hundreds means a reset loop).
- `msm_drm` IRQ delivery: healthy on both stock and alternate.
- Kernel #242 vs #243, payload, vendor image, userdata (clean data made no difference).
- Proximity sensor settings; `hotplugd` MUST keep running (the sensors HAL blocks on it).
- Kernel #244 (KGSL stuck-fault re-wake) was a real regression for the settings app freeze test; its source change was reverted (`git checkout drivers/gpu/msm/kgsl_iommu.c`). The earlier KGSL fault freeze in the Wi-Fi settings page is therefore still open.

### Other useful discoveries from this session
- Panel only lights if `qkx-splash` runs twice (once for 1 s, then for real). Run inline it gets cut by the 4 s telnet timeout, so the launch script now re-runs itself detached. The two-run splash is enabled again in the script (verify the panel lights without a manual sleep/wake).
- `com.oculus.os.cm` keeps recreating a `SYSTEM_ERROR` crash dialog; dismiss with `input keyevent KEYCODE_TAB` then `KEYCODE_ENTER` (inside system_server's namespace). `com.oculus.os.vrlockscreen` can also take focus; `wm dismiss-keyguard`.
- Never tear down surfaceflinger/composers live: system_server restarts and VrPower disappears.
- `trackingservice` needs `/data/misc/vision/insideout` (`/vision` symlink); a clean `/data` lacks it. It can come up relocalizing in 3DoF; `stop/start trackingservice` recovers 6DoF.
- `tel` in `work/cycle.sh` now returns as soon as the script finishes.
- Device may reboot by itself occasionally; just `wait_root` and relaunch.
- The `vendor.img` rebuild recipe for modules is in section 5 (Wi-Fi).

### Uncommitted at the time of writing
`quest-pro-kexec/initramfs/os/qkx-launch-android`; `work/` image copies are not in git.

## 0a. Confirmed kernel #245, Bluetooth, ET/FT and calibrated gaze

Current tested payload: `quest-pro-kexec/out/ospayload-245-bt`.
Kernel: `build/runs/245/Image`, SHA-256
`2c446ad12d0dee4d674b7a89f29bd2a0e5500ff389ebbcbf5064d8a087564262`.
Bluetooth controller initialization reports ON (pairing not yet verified).
The UART fix and GPU recovery changes are described in the Bluetooth section.
The user confirmed gaze control works after importing stock calibration.

Run alternate Android commands inside system_server's mount/root namespace:
```
SS=$(pidof system_server)
N="nsenter -t $SS -m -r/proc/$SS/root"
$N /system/bin/am broadcast -a com.oculus.vrpowermanager.prox_close
$N /data/local/tmp/faceeye-request 5
```
`prox_close` works now that the dock initialization hang is fixed; it sets
virtual proximity CLOSE and HEADSET_MOUNTED, without vrmountlock.
`work/faceeye-request.cpp` / `work/faceeye-request` issue the installed
TrackingFidelityService requestFeaturesWithFidelities transaction. Verified
feature IDs: ORTHOFIT=2, FACE=3, EYE=4; HIGH=5, OFF=0. The tool requests
FACE/EYE HIGH and ORTHOFIT OFF; argument `0` turns those requests off.
This is a live diagnostic request, not an autostart configuration.
Settings enablement alone did not request camera fidelity. ET/FT and consent
preferences were already true, but both requested fidelities were OFF.
After the request, mux is socialAvatarsHigh_0, ET runs at 60–72 FPS,
and gaze/blendshape publish timestamps advance. CDSP is ONLINE; successful
Hexagon RPC/model execution was observed. No DSP bring-up change was needed.

Preferences use the newer tool for the active user:
```
$N /system_ext/bin/oculuspreferences --getc et_enabled ft_enabled et_consented ft_consented
$N /system_ext/bin/oculuspreferences --setc gaze_input_enabled 1 debug_eye_gaze_emit_hover_enabled true
```
`gaze_input_enabled` is an INTEGER (previously -1), not a boolean;
`debug_eye_gaze_emit_hover_enabled` is a boolean (previously false).

### Import the existing stock user eye calibration (no new calibration)
Stock was inspected read-only: `oculussetting --get eye_tracking_calibration`
and `oculuspreferences --getc eye_tracking_calibration`, plus dumpsys tracking.
It reported user_calibrated=true, both eyes calibrated, version=2,
pipeline_name=both_temporal. Alternate initially had empty parameters and
user_calibrated=false, despite valid gaze samples.
Exact stock JSON backup: `work/stock-eye-calibration.json`.
Host diagnostic captures: `/tmp/stock-tracking-calibration.txt`,
`/tmp/stock-eye-calibration-setting.txt`.
Transfer JSON to alternate `/data/local/tmp/stock-eye-calibration.json`, then:
```
$N /system_ext/bin/oculuspreferences --getc eye_tracking_calibration > /data/local/tmp/eye-calibration-pre-stock.txt
$N /system_ext/bin/oculussetting --get eye_tracking_calibration > /data/local/tmp/eye-calibration-legacy-pre-stock.txt
CAL=$(cat /data/local/tmp/stock-eye-calibration.json)
$N /system_ext/bin/oculuspreferences --setc eye_tracking_calibration "$CAL"
$N /system_ext/bin/oculussetting --set eye_tracking_calibration "$CAL"
stop trackingservice
start trackingservice
# Once trackingservice is available, restore the live fidelity request:
$N /data/local/tmp/faceeye-request 5
```
Both stores were updated; changing either live did not immediately reload
tracking's parameters. Restarting trackingservice (NOT system_server or the
display stack) loaded the calibration. Verified user_calibrated=true,
is_calibrated=true for both eyes, both_temporal, valid interaction gaze,
and advancing ET/FT output. User confirmed gaze hover/control works.
No writes to stock userdata or installed partitions; calibration is stored
in alternate data.img. Do not publish the user's calibration JSON in git.

## 0b. Everything that is disabled / removed / stopped (keep this list current)

Images (host copies under `work/no-usb-images/`, installed via the verified pipeline; installed partitions are never touched):

| What | How | Why | Backup / restore |
|---|---|---|---|
| Dock HAL declaration `vendor.meta.hardware.dock.IDock/default` | removed `/odm/etc/vintf/manifest/charging_accessory_service.xml` from odm.img | declared HAL whose service never starts; `VrPowerManagerService.prepareService()` blocked forever on it (root cause of the blank view) | `work/odm-pre-dockfix.img` |
| DeviceCert HAL declaration `vendor.oculus.hardware.devicecert@1.0::IDeviceCert/default` | removed `/odm/etc/vintf/manifest/vendor.oculus.hardware.devicecert@1.0-service.xml` from odm.img | no init rc defines it, so init logged `Could not find ... for ctl.interface_start` every second; DeviceCert/QSEE is unavailable anyway | `work/odm-pre-devicecert.img` (stock + dock fix only) |
| Stock `bcmdhd.ko` | replaced by module rebuilt for kernel #243 | stock module rejected (`module_layout`) | `work/vendor-pre-bcmdhd-rebuild.img` |

Kernel / device tree / payload:

| What | How | Why |
|---|---|---|
| QSEE/SPSS (`/soc/qcom,qseecom@82400000`) | disabled in dtb by `prep` (`fdtput`) | live giveback/removal panicked (`spss` error-ready timeout) |
| Venus video codecs (`/soc/qcom,vidc@aa00000`) | `DISABLE_NODES` | hardware codecs unsupported after kexec (known limitation) |
| CDSP, Venus, NPU | `shutdown_cdsp=1 shutdown_subsys=venus,npu` loader args | clean state before kexec |

Android userdata (alternate `data.img`, per-user package state, set with `pm disable-user --user 0`):

| What | Why | Re-enable |
|---|---|---|
| `com.oculus.os.cm` | crash-loops (DeviceCert unavailable) and keeps spawning a dimming `SYSTEM_ERROR` dialog | `pm enable com.oculus.os.cm` |
| `com.oculus.os.vrlockscreen` | VR lockscreen took focus over the shell | `pm enable com.oculus.os.vrlockscreen` |
| crash dialogs suppressed via `settings put global show_first_crash_dialog 0`, `show_restart_in_crash_dialog 0`, `secure anr_show_background 0` | black bar from the cm crash dialog | set back to 1 |

Runtime-only changes that are NOT persistent (lost on every boot): `stop hotplugd` (do NOT do this: the sensors HAL blocks on it), `stop/start trackingservice`, `am force-stop ...`. The `vrmountlock` autostart service was removed from vendor.img; the native binary is only at `work/vrmountlock`.

Must stay enabled: `trackingservice`, `hotplugd`, the sensors HAL, `vendor.bluetooth-1-0`.

## 1. Standard launch procedure

```bash
cd /home/yusuf/kexectest
source work/cycle.sh
export CAP=out/captured-low6 DISABLE_NODES='/soc/qcom,vidc@aa00000'
prep out/captured-sensors-on <kernel#> out/ospayload-252fpoll
rescue out/ospayload-252fpoll <tag> shutdown_cdsp=1 shutdown_subsys=venus,npu
launch <tag> 60
```

- Always build with `CAP=out/captured-low6` (`captured-firstbank` caused a
  624 MiB OOM regression).
- `DISABLE_NODES` for `vidc` keeps hardware video codecs (Venus) off.
- Return to stock: `echo r > /proc/qkx_bootdone` (from rescue shell).
- Root shell after kexec: telnet to `10.42.0.2:23` (host `10.42.0.1`,
  ECM on usb0). Android-side commands often need `nsenter` (see §3).

## 2. Fixes that were required to reach visible rendering

In order of the boot chain; all of these are **required** today:

1. **Memory map / capture**: pinned, verified partition copies installed via
   `qkx-install.sh` + `qkx_rawcp` (`qkx_rawcp: verified`), never raw
   partitions. `captured-low6` region set.
2. **ADSP**: `qkx-adsp-hold.ko` (`subsystem_get/put("adsp")`) restarts ADSP
   after kexec; `vendor.pd_mapper`, `vendor.per_mgr` enabled; modem firmware
   served from pinned copy `/dev/qkx/modemfw` (dm-linear, mounted ro VFAT)
   instead of raw `/dev/sde4`. Audio PDR + `adsprpcd` come up.
3. **Display/DRM**: initramfs `qkx-splash` runs before `switch_root`, giving
   Android a valid DSI connector/encoder/CRTC before composer start (avoids
   DRM-master races).
4. **USB VINTF**: stale ODM USB VINTF fragment removed from the pinned ODM
   image (fixed system_server USB boot-phase deadlock).
5. **IAD/IPD node**: ODM property
   `ro.iad.sysfs_nodes=/sys/bus/iio/devices/iio:device0/in_voltage_ipd_sensor_input`
   (target enumerates the IPD ADC as device0, stock as device1).
6. **GPU faults**: apps-SMMU context-bank fault IRQs are not delivered after
   kexec. Workaround = gated fast fault poller (`kgsl_iommu_qkx_fault_poll()`
   in `drivers/gpu/msm/kgsl_iommu.c`, task started in
   `kgsl_iommu_probe()`), polls every 1–1.5 ms only while
   `kgsl_state_is_awake()`; hands faults to the existing threaded handler via
   `kgsl_iommu_latch_missed_fault()` + `irq_wake_thread()`.
   Zero `gpu timeout` / `SMMU is stalled` since. VrApi 90/90 FPS.
7. **Head-mounted state (THE black-screen key fix)**: after kexec
   `VrPowerManagerService` never registers its `IPowerstate` HIDL callback
   (`prepareService()` aborts at `MountEventNotifier.startListening()`), so
   `vrpowermanager` stays `HEADSET_UNMOUNTED`, tracking stays Standby, no
   active pose, and TimeWarp blanks the final frame even though the panel
   pipeline is perfect (MISR constant, capture all-zero).
   **Fix**: `work/vrmountlock.cpp` → native binary `work/vrmountlock` that
   connects to AIDL `oculus.internal.power.IVrPowerManager/default`, calls
   transaction 3 `acquirePowerStateLock` with a real binder token and
   lock type 1 (MOUNTED). Deploy on target:
   ```sh
   setsid /data/local/tmp/vrmountlock >/data/local/tmp/vrmountlock-native.log 2>&1 &
   ```
   Result: `State: HEADSET_MOUNTED`, tracking `RUNNING`, camera 30 Hz,
   `Frame: v8, 6DoF, active: true`, and **visible rendering** (user-verified).
8. **Crash-dialog black bar**: `com.oculus.os.cm` crash-loops because
   `vendor.oculus.hardware.devicecert@1.0::IDeviceCert` is unavailable
   (QSEE/SPSS remain disabled). Its `SYSTEM_ERROR` dialog rendered as a black
   bar over the compositor. Mitigation: `pm disable-user --user 0 com.oculus.os.cm`
   (persists in alternate userdata across reboots of the alternate OS) and
   `settings put global show_first_crash_dialog 0`.
9. **KGSL diagnostics** (kept for future work):
   `CONFIG_DEBUG_FS=y`, `CONFIG_QCOM_KGSL_ENTRY_METADATA=y`, direct
   allocation lookup in `kgsl_iommu_qkx_dump_faults()` via
   `_get_pagetable_from_contextidr()` + `kgsl_sharedmem_find()`.

## 3. Android-side command quirks (telnet rescue shell)

The rescue shell shares the kernel but NOT Android's mount namespace:

- Android binaries: `nsenter -t $(pidof system_server) -m -r/proc/<pid>/root /system/bin/...`
  (e.g. `svc bluetooth enable`, `oculussetting`).
- `svc`/`oculussetting` are not on rescue PATH — always full path + nsenter.
- App data: access via `/proc/<app-pid>/root/data/user/0/<pkg>/...` while the
  app runs; after `am force-stop` use `/proc/$(pidof system_server)/root/...`.
- uiautomator/screencap: run inside SurfaceFlinger's namespace
  (`nsenter -t $(pidof surfaceflinger) -m ...`).

## 4. First-time setup (NUX) without controllers

The NUX flow state lives in
`/data/user/0/com.oculus.firsttimenux/databases/RKStorage` (SQLite,
table `catalystLocalStorage`):

- `first_time_nux_flow_type` = `V3_PRIMARY_USER`
- `first_time_nux_history` = JSON array of
  `{"isCheckpoint":bool,"isCompleted":bool,"url":"/first_time_nux/V3_PRIMARY_USER/<STEP>"}`
- The app resumes at the **last entry with `isCheckpoint:true`**.

Observed V3 flow order (steps seen): `HMD_BATTERY → CONTROLLER_CHECK →
LANGUAGE → STAY_SEATED → CONTROLLERS_POWER_ON → CONTROLLER_PAIRING_LEFT →
CONTROLLER_PAIRING_RIGHT → …` (bundle strings indicate later steps include
`HEADSET_FIT`, guardian, wifi, account).

**Procedure to skip controller pairing** (only that):

1. Pull DB: `cp /proc/$(pidof com.oculus.firsttimenux)/root/.../RKStorage
   /data/local/tmp/` and `nc` it to host.
2. On host, with sqlite3: set last history entry
   `{"isCheckpoint":true,"isCompleted":false,
   "url":"/first_time_nux/V3_PRIMARY_USER/CONTROLLER_PAIRING_RIGHT"}`
   (verified working: app resumed on right-controller page).
   Then set the entry to `HEADSET_FIT` to skip pairing entirely
   (user-confirmed working).
3. Push back:
   ```sh
   SS=$(pidof system_server); DB=/proc/$SS/root/data/user/0/com.oculus.firsttimenux/databases/RKStorage
   am force-stop com.oculus.firsttimenux
   cat /data/local/tmp/RKStorage.edited > $DB
   chown 10076:10076 $DB; rm -f ${DB}-wal ${DB}-shm
   am start -n com.oculus.firsttimenux/.FirstTimeNuxActivity
   ```
4. Original DB backup kept at `/data/local/tmp/RKStorage.controller-backup`.

**Hand tracking** (lets you click things without controllers):

```sh
SS=$(pidof system_server)
nsenter -t $SS -m -r/proc/$SS/root /system_ext/bin/oculussetting --set hand_tracking_opt_in 1
```

Verified: `hand_tracking_opt_in=1`, hand pipeline RUNNING, valid hand poses,
pinch interaction usable.

## 5. Open items

### Bluetooth — controller initialization FIXED (kernel #245)

Current payload: `quest-pro-kexec/out/ospayload-245-bt`, kernel
`build/runs/245/Image`, SHA-256
`2c446ad12d0dee4d674b7a89f29bd2a0e5500ff389ebbcbf5064d8a087564262`.

The SyncBoss RX handoff workaround in
`drivers/tty/serial/msm_geni_serial.c` forced `UART_MANUAL_RFR_EN`
on every UART when `qkx_uart_dma_cleanup` was enabled. Restricting that
workaround to `uport->line == 14` leaves Bluetooth ttyHS0 using its normal
hardware flow control. Kernel #245 reports Bluetooth `enabled: true`,
`state: ON`, with UART RX/TX completions advancing and no HAL init timeout.
Pairing/discovery has not yet been verified. FIFO mode did not solve the
problem and was restored to SE_DMA before this kernel test.

#245 also restores the GPU fault-thread re-wake behavior (10 ms retry while
FSR remains set and faults are outstanding) and guards the fault-count
 decrement against underflow. Both changes are currently uncommitted.
ET/FT live requests were restored with `work/faceeye-request.cpp`:
FACE=3, EYE=4 at HIGH=5; ORTHOFIT=2 restored to OFF=0. Gaze and
blendshape timestamps advance. No ET/FT boot-default changes were made.

Earlier investigation notes (superseded where stated above):
- Framework OK; vendor HAL `android.hardware.bluetooth@1.0-service.seacliff`
  (pid ~1209) fails to get the controller to answer.
- Sequence per attempt (dmesg): `[BT] Bluetooth Power Off` →
  `Power On (1121)` → bluesleep LPM regs → 3 s silence →
  `HCI HAL init failed ... BT controller not responding`.
- Facts gathered:
  - rfkill0 `bcm4361 Bluetooth`, driver `bcm4361_bluetooth`, DT
    `vendor/bt_driver` (`brcm,btdriver`, bt-reset-gpio 0x8a active-low?).
  - UART = GENI serial `/dev/ttyHS0` (of_node `qcom,qup_uart@998000`),
    owned by uid 1002 between attempts; HAL closes it after failure.
  - FW present: `/vendor/firmware/bcmdhd/BCM4389*.hcd` (B0/C0/C1) —
    note: rfkill says bcm4361, HCD says 4389 (combo chip naming).
  - HAL thread blocked in binder when idle; only 2 threads → vendor impl is
    synchronous & single-shot per attempt.
  - `servicemanager: Could not find android.hardware.bluetooth.IBluetoothHci/default
    in the VINTF manifest` messages each cycle (VINTF entry may be missing in
    pinned vendor manifest — to verify; may or may not be fatal).
- Next steps: trace vendor HAL open of ttyHS0 (strace via nsenter not
  possible for vendor pid; use kernel ftrace on geni serial / check whether
  any bytes are RXed), compare `bt_vendor_rf.xml` UART config with stock,
  verify VINTF manifest entry for IBluetoothHci, check GPIO/regulator states
  (BT_ON/WLAN_REG_ON), and compare against stock boot (BT works on stock).

### Wi-Fi — SOLVED (kernel #243)
Symptom: no `wlan0`; `WifiHAL: Timed out waiting on Driver ready`.
Root cause (two layers):
1. The stock `bcmdhd.ko` in `vendor.img:/lib/modules` is rejected by our
   kernel: `bcmdhd: disagrees about version of symbol module_layout`
   (every stock dlkm is rejected the same way; only built-in code works).
2. Even a correctly built module could not link: our `asm/io.h` /
   `asm/irqflags.h` diagnostics call `qkx_mmio_enter/exit`,
   `qkx_irqmask_note`, `qkx_irqmask_ready` (kernel/sched/qkx_stall.c),
   which were not exported -> modpost "undefined!".
Fix:
- `EXPORT_SYMBOL` those four in `kernel/sched/qkx_stall.c`
  (+ `#include <linux/export.h>`), rebuild Image (#243) and modules:
  ```
  make -C oculus-linux-kernel O=build/diagnostic-kernel ARCH=arm64 CC=clang LD=ld.lld \
    HOSTCC=clang CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
    LOCALVERSION=-qkxdiag KCFLAGS=-I$PWD/oculus-linux-kernel/drivers/pinctrl -j16 Image modules
  mkdir -p build/runs/243 && cp build/diagnostic-kernel/arch/arm64/boot/Image build/runs/243/
  ```
  (driver dir is `drivers/net/wireless/broadcom/brcm4389`, not `brcm4389ar`).
- Strip and swap the module into the pinned vendor image copy:
  ```
  aarch64-linux-gnu-strip --strip-debug -o /tmp/bcmdhd.ko build/diagnostic-kernel/drivers/net/wireless/broadcom/brcm4389/bcmdhd.ko
  cp work/no-usb-images/vendor.img work/vendor-pre-bcmdhd-rebuild.img   # backup
  printf 'u:object_r:vendor_file:s0\0' > /tmp/sel.bin
  debugfs -w -f - work/no-usb-images/vendor.img <<EOF
  rm /lib/modules/bcmdhd.ko
  write /tmp/bcmdhd.ko /lib/modules/bcmdhd.ko
  set_inode_field /lib/modules/bcmdhd.ko uid 0
  set_inode_field /lib/modules/bcmdhd.ko gid 0
  set_inode_field /lib/modules/bcmdhd.ko mode 0100644
  ea_set -f /tmp/sel.bin /lib/modules/bcmdhd.ko security.selinux
  EOF
  e2fsck -fn work/no-usb-images/vendor.img
  source work/cycle.sh; reinstall out/captured-sensors-on vendor
  ```
- Launch with kernel 243:
  `prep out/captured-sensors-on 243 out/ospayload-243wifi`, then
  `rescue ... ; launch ...` as in section 1.
Result: `bcmdhd` loads at boot (init.insmod), BCM4389 enumerates on PCIe
(0000:01:00.0, WL_REG_ON = GPIO 1120, host-wake = GPIO 1224), `wlan0` +
`swlan0` appear, framework "Wifi is enabled", scan returns real APs.
Check: `nsenter ... /system/bin/cmd wifi list-scan-results`.
Note: the other stock dlkms (audio, usb-net, powerstate_mgr, ...) still fail
with the same module_layout error; rebuilt versions of all of them are in
`build/diagnostic-kernel` and can be swapped in the same way if needed.

### CMS and alternate OS updater — persistently disabled

`com.oculus.os.cm` was disabled-user but still crash-looped; OSUpdater was
protected from pm disable-user. Both are now removed from the alternate image
scan paths, together with NuxOta, by quarantining their APKs under the image
root `/qkx-disabled/*.disabled` (root-only mode 0600). They are not scanned as
apps. Quarantine also contains original update_engine RC, gold-core RC,
update_engine, update_engine_client, update_verifier, and postinstall binaries.
Neither normal nor gold update-engine service is registered, and native OTA
entrypoints are absent from their original paths. This disables the alternate
Android OS updater mechanisms; it does NOT modify the stock installation or
assert that unrelated firmware-update code has been audited.

Backups: `work/system-pre-disable-updater-cm.img` and
`work/system_ext-pre-disable-updater-cm.img`. Actual system_ext source remains
`work/no-vision-images/system_ext.img` via the no-usb-images symlink.
Script: `tools/disable-alt-updaters.py` (workspace-specific paths; refuses to
overwrite backups). It expands each host image by 256 MiB, unshares ext4
shared blocks before edits, verifies each quarantined file's SHA-256 against
its original, and requires a clean final e2fsck. Restore by replacing host
copies from the backups, reinstalling those two pinned files, and preparing
fresh payload maps; never write installed partitions.

Install verification: system 1452 MiB, system_ext 1518 MiB, both qkx_rawcp
verified. `tools/os-install.sh` now resolves image symlinks before stat/staging:
its former lstat-sized system_ext allocation was wrong. The failed staging
attempt did not write that image; it was deleted/reallocated before retry.
Fresh prep rebuilt both full-RAM and low6 payload maps.

After reboot: sys.boot_completed=1; all three packages absent from pm's installed
package list; no corresponding processes or update-engine services. Explicit
start attempts for update_engine/update_engine_gold failed, and processes
remained absent after a successful 60-frame Venus H.264 encode. Bluetooth ON,
audio card present, proximity and ET/FT fidelity restored. Controllers remain
intentionally deferred. Current final UI visibility awaits user confirmation.

CVP = Qualcomm Computer Vision Processor, a vision accelerator distinct from
Venus codecs. Its firmware NOC (Network-on-Chip interconnect) error remains
open; confirmed tracking/rendering/video successes do not prove CVP is healthy.

### Full RAM — Android UI confirmed; Venus also passes repeated tests

Kernel #253 and `out/ram12-253` register the full 12 GiB physical RAM map.
Two fresh boots with GPU/display kernel probing enabled passed the corrected
read scan and 11,008 MiB simultaneous allocation/pattern tests, zero errors.
The strengthened fill-all-before-verify alias test also passed. Firmware,
secure VM and kernel reservations leave about 11.25 GiB usable.
Initial RAM validation was rescue-only. Subsequent Android test:
`out/ospayload-253-ram12-novidc`, tag `android253ram12nv`, alive at 60 s,
sys.boot_completed=1, MemTotal=11,786,344 KiB (~11.24 GiB). Bluetooth ON,
audio card present, proximity broadcast and ET/FT fidelity request restored;
user_calibrated=true/both_temporal verified. No GPU timeout/soft-lockup messages
in the subsequent check. Visual rendering awaits user confirmation.
Those initial tests were followed by another failed reboot and a confirmed
low6 rollback. The missing handoff coverage was then addressed: the read-only
stock `ion_secmap` snapshot now also enumerates master-side secure SMMU tables
and their secure pools (GPU, display, CVP, video, crypto and FastRPC domains).
These pages retain HLOS RW, but not EXEC, after stock shares them with a secure
VM; the previous ION-only snapshot omitted them. Reserve them across kexec,
never change their ownership. Initial snapshot: 30 table pages in 9 domains.
With the expanded reservation, full-RAM Android UI was confirmed by the user
(`ram253sharedtables`). A live 4096 MiB allocator/fill-all/verify test passed
with zero mismatches; Bluetooth ON, audio card present and calibration intact.
VIDC was then re-enabled with the established reload-first/no-PC flags:
`ram253sharedvenus` and fresh `ram253sharedvenusrepeat` both completed Android
boot and three 1920x1080, 300-frame H.264 encode/decode cycles apiece, EOS=true.
Host FFmpeg decoded the captured output without errors. Latest MemTotal:
11,774,064 KiB. UI visibility on the final Venus-enabled boot awaits user
confirmation. The unsuccessful low-page-table-allocation diagnostic was
removed; it is not needed by the working configuration.
The previously verified low6 audio/Venus payload remains available. See
`KEXEC_RAM_12GB_RESCUE.md` for the full history and safeguards.

### Audio — audible output confirmed by user on kernel #249

Working combined payload: `out/ospayload-249-audio-unshared`, log tag
`audio249unshared`. Keeps #249's Venus flags and enabled VIDC node.
Matching kernel audio modules replaced the 12 stock `*_dlkm.ko` modules in
host `work/no-usb-images/vendor.img`. Stock modules were rejected with
`module_layout` mismatches, leaving no ALSA cards despite running audio HAL.
Modules must load during boot: late insertion missed the initial DSP/PDR
readiness notification. On the successful boot, APR received Q6 Up and ALSA
registered `kona-cm7120-tdm-snd-card`; Android loaded its audio effects.
**User heard audio and confirmed audible output.** Microphone capture and
headphone/Bluetooth audio routes are not yet verified.

Vendor backup: `work/vendor-pre-audio249.img`. Current host vendor image was
expanded by 64 MiB (368 MiB total) and, critically, ext4 shared blocks were
unshared using `e2fsck -fy -E unshare_blocks` before module replacement.
Stock vendor has the `shared_blocks` feature: direct remove/write replacement
can corrupt other files by freeing shared extents. Earlier experimental audio
copies exhibited binary corruption and were discarded. All 12 module files
in the final image were compared byte-for-byte against host binaries;
`e2fsck -fn` passed, and installation reported `qkx_rawcp: verified 368 MiB`.
Fresh prep rebuilt the pinned-file maps after installation. Do not reuse
pre-install payload maps for the current image files.

Modules staged under `work/audio249`, built with the normal diagnostic kernel
configuration and stripped only of debug data. A diagnostic-only machine-driver
log in `techpack/audio/asoc/kona.c` includes the failing DAI link index/name;
no functional audio-driver patch was necessary. Installed partitions and
stock userdata were not modified. Gaze fidelity and proximity activation
were restored after this boot. Changes remain uncommitted.

### Venus H.264 encode/decode — verified on low6 #249 and full-RAM run #253

Current combined payload: `out/ospayload-253-ram12-venus` (full RAM,
expanded stock secure-table reservation, same Venus reload-first/no-PC flags).
Six 1080p/300-frame encode/decode cycles passed across two fresh boots.
Independent host output: `work/venus-test/encoded-ram12-253.mp4` (FFmpeg exit 0).
Detached completion-driven test: `tools/tests/venus/ram12-check.sh`;
wait for an actual `STATUS=PASS` line, not an echoed shell command.
The historical low6 test payload was `out/ospayload-249-venus`.
Kernel: `build/runs/249/Image`, SHA-256
`a5b35a9a8fa6584e629fe36a77cda526e4d7ff7bfd8baec59a88b52703cfb5a0`.
Build and launch from `quest-pro-kexec` using:
```
source /home/yusuf/kexectest/work/cycle.sh
export CAP=out/captured-low6 DISABLE_NODES=''
prep out/captured-sensors-on 249 out/ospayload-249-venus qkx_venus_reload_first=1 qkx_venus_no_pc=1
rescue out/ospayload-249-venus venus249retry shutdown_cdsp=1 shutdown_subsys=venus,npu
launch venus249retry 60
```
Only this Venus test configuration deliberately enables the VIDC DT node;
QSEE remains disabled. The known #245 payload remains an untouched fallback.

Gated changes in `techpack/video/msm/vidc/hfi_common.c`:
- `qkx_venus_reload_first`: load firmware via existing PIL/SSR before VIDC-side
  power-on; avoid resetting the authenticated core immediately afterward.
  Also disable video LLCC/syscache from boot through the existing driver policy.
- `qkx_venus_no_pc`: disable software power collapse, keep regulator ownership
  in software, and tell firmware not to use inter-frame hardware power collapse.
These are a working conservative configuration, NOT a proven minimal fix.
Expect greater power use while Venus is active; power-collapse and video
syscache optimization remain open. Both kernel flags are required by the
currently tested configuration. Changes are uncommitted.

Validation: `c2.qti.avc.encoder` passed three consecutive 320x240/60-frame
sessions, then a changing-luma 1920x1080/300-frame encode. Each reached EOS.
Downloaded 1080p output: `work/venus-test/encoded-249-1080p.mp4`.
Host ffprobe confirmed H.264, 1920x1080, 10 seconds, 300 decoded frames;
FFmpeg decoded it fully without errors (exit 0). `c2.qti.avc.decoder` also
completed the 60-frame input test on #249. No firmware SFR/NOC error messages
were present in the subsequent kernel-log check. Other codecs, secure DRM,
and long-duration workloads are not yet verified.

Tests: `work/venus-test/VenusEncode.java`, `VenusTest.java` and `sample.mp4`.
Compile with Android SDK android.jar and d8, transfer classes.dex to alternate
`/data/local/tmp/venus-test.dex`. To use app_process from rescue, export
ANDROID_ROOT/DATA/ART_ROOT/I18N_ROOT/TZDATA_ROOT and BOOTCLASSPATH/
DEX2OATBOOTCLASSPATH from system_server's environment, plus CLASSPATH, and
invoke `/system/bin/app_process64` inside its mount/root namespace.

Earlier unsuccessful investigations (superseded by the #249 results above):
Experimental `out/ospayload-245-venus` is a copy of the working #245 payload
with `/soc/qcom,vidc@aa00000` set to okay. It is NOT a working configuration.
`shutdown_subsys=venus,npu` remained enabled. Logs `venus245-udp.txt` show
VIDC IOMMU faults on first codec open, followed by CPU0 stuck in the SMMU IRQ
thread and gadget loss at 35 seconds. Stopping stock
`vendor-qti-media-c2-hal-1-0` and `vendor.media.omx` before kexec did not cure it:
`venus245closed-udp.txt` reproduced faults/stalls and gadget loss at 40 seconds.
Restored the safe `out/ospayload-245-bt` payload (`venusbaseline`, alive at 60 s)
after those tests. #246–248 subsequently achieved only intermittent codec
success; a live syscache-disable toggle alone was not reliable. #249 is the
verified conservative configuration described above. Pointer-valued TTBR
logs use `%pK`, so zeros in those logs do NOT establish a zero page-table base.

### Also open
- `com.oculus.os.cm`/DeviceCert (QSEE/SPSS disabled — restoring them
  panicked with `Timed out waiting for error ready: spss!`).
- Controllers (need BT working, then pairing).
- KGSL poller steady-state perf once BT/Wi-Fi add load.

## 6. Key files

| What | Where |
|---|---|
| Launch helper | `/home/yusuf/kexectest/work/cycle.sh` |
| Mount-lock client source | `/home/yusuf/kexectest/work/vrmountlock.cpp` (+ binary `work/vrmountlock`, target copy `/data/local/tmp/vrmountlock`) |
| Kernel changes | `oculus-linux-kernel` commit `a8c21437c3` ("booting android") |
| Kexec repo changes | `quest-pro-kexec` commit `b7c1422` ("booting android") |
| Modem firmware installer | `quest-pro-kexec/tools/os-install-modemfw.sh` |
| NUX DB backups | `/data/local/tmp/RKStorage.controller-backup`, `/data/local/tmp/RKStorage.after` |
| Stock dumps | `/tmp/stock-tracking.txt`, `/tmp/stock-screen.png` |
