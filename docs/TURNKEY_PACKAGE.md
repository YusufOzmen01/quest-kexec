# Turnkey Quest Pro alternate Android package

## Split release format

The recommended release uses a device-independent loader ZIP and a separate
ROM ZIP. Extract the loader ZIP, then run:

```sh
./qkx-install-package.sh /path/to/qkx-quest-pro-rom.zip
./qkx-boot.sh
```

The loader contains the kernel, BusyBox, kernel modules, installer, initramfs
and device helpers. The ROM ZIP contains only the five prepared Android images
and turnkey metadata. Neither contains stock calibration or userdata. Existing
alternate `/data` can be retained during ROM replacement; fresh extent maps are
still generated and validated.

Build them with:

```sh
python3 tools/build-rom-package.py IMAGE_DIR rom.zip
python3 tools/build-loader-package.py loader.zip --kernel /path/to/Image
```

## Files

- `qkx-install-package.sh`: interactive installation and package validation.
- `qkx-boot.sh`: boots an installed package without repeating package validation.
- `work/qkx-quest-pro-turnkey-v1.zip`: generated system package.

The package contains the five alternate Android images, known-good kernel #253,
BusyBox, kexec loader, secure-map collector, retained-log helper, and SHA-256
manifest. It contains no userdata and no eye calibration.

## Install

Requirements: Linux host, rooted Quest Pro connected through ADB, Python 3,
`dtc`, `fdtput`, `adb`, e2fsprogs, and the QKX repository/tool binaries.

```sh
cd quest-pro-kexec
./qkx-install-package.sh /path/to/qkx-quest-pro-turnkey-v1.zip
```

The installer asks for alternate `/data` size (4–128 GiB). It verifies every
ZIP member against `manifest.json`, checks the device is `seacliff`, captures
the device-specific runtime DT, and installs images only into pinned files under
`/data/local/tmp/qkx-release`. It reads `modem_a` into another pinned copy; the
partition itself is never written. Existing installed partitions are never
flashed or formatted.

Revalidate a package without installing:

```sh
./qkx-install-package.sh --verify-only /path/to/package.zip
```

## Boot

```sh
./qkx-boot.sh
```

Boot intentionally does not revalidate the ZIP. It uses the verified installed
state, reads the current stock eye calibration into a private temporary
initramfs, snapshots current secure mappings, and performs kexec. The initramfs
mounts the pinned images and launches Android automatically.

On pristine alternate `/data`, the image synchronously creates the vision data
directory, bypasses NUX/provisioning, enables hand tracking and gaze hover,
imports current stock eye calibration, requests high ET/FT fidelity, starts the
Home activity, and leaves CMS, OTA/update engine, DeviceCert/QSEE/SPSS paths
unavailable. Stock calibration and stock userdata are read-only.

## Uninstall from the headset

Reboot into rooted stock Android first, then run:

```sh
./qkx-uninstall.sh
```

The script lists exact QKX-owned userdata paths and requires typing `REMOVE`.
It deletes the current pinned installation, legacy QKX installations, and known
staging files. It refuses to operate from a QKX kernel or while a helper module
is loaded. It never writes, formats, or flashes a partition.

Automation and legacy-retention options:

```sh
./qkx-uninstall.sh --yes
./qkx-uninstall.sh --keep-legacy
```

Host-side packages and `state/` are intentionally retained.

## Build a package

```sh
python3 tools/build-system-package.py \
  ../work/turnkey-images-v4 ../work/qkx-quest-pro-turnkey-v1.zip \
  --kernel ../build/runs/253/Image --repo .
```

Always distribute the ZIP and its separately recorded SHA-256. Do not add image
files, generated state, or calibration material to Git.
