# Build and install

[简体中文](BUILD.zh-CN.md) · Previous: [Sources and dependencies](PROVENANCE.en.md) · Next: [Validate](VALIDATION.en.md)

## 1 · Generate the kernel and packages

Complete the [host setup and bootstrap in PROVENANCE](PROVENANCE.en.md#acquire) first. Run the commands below in a Linux shell from the project root. Retain the same `BUILD_ROOT` if customized.

```bash
bash scripts/run-limited.sh build
```

The entry compiles GKI, SoC, Techpack and device modules, generates matching ramdisk/boot.img/ZIP packages, and runs static validation. It enforces **12 GiB memory max, 10 GiB high, 1 GiB swap max and four CPU cores**, with four jobs by default. Use `JOBS=2 bash scripts/run-limited.sh build` to reduce concurrency if needed.

| Output | Purpose |
|:--|:--|
| `dist/release/boot.img` | Kernel with the module layer built in, and the same layer as its ramdisk, for fastboot |
| `dist/release/nuwa-kernel-6.18-AnyKernel3.zip` | Kernel installer for a compatible flashing tool/recovery |
| `dist/release/nuwa-618-v3.zip` | Optional KernelSU module to control kernel settings |
| `dist/release/validation.json`, `SHA256SUMS` | Validation result and artifact hashes |
| `logs/release/build.log` | Build log; investigate the first error if a stage fails |

After `BUILD AND PACKAGE VALIDATION PASSED`, follow [VALIDATION](VALIDATION.en.md) before installation. Building uses the prepared inputs without network access. `TAG=my-build` changes the output directory name; retain the same TAG for subsequent checks.

## 2 · Identify the phone and back up boot

The main target is the **Xiaomi 13 Pro / nuwa** with the ROM specified on the home page. The 13 / fuxi and 13 Ultra / ishtar adaptations have not been run on those phones yet; use only temporary boot on them until they are tested. The bootloader must be unlocked. Install Android platform-tools on the host and enable USB debugging. AnyKernel3 device checks are disabled, so check before installation:

```bash
adb shell getprop ro.product.device
adb shell getprop ro.build.version.incremental
adb shell getprop ro.boot.slot_suffix
```

Record the slot and save the working ROM's boot image. With root, read the active slot:

```bash
mkdir -p backup
adb exec-out 'su -c "cat /dev/block/by-name/boot$(getprop ro.boot.slot_suffix)"' \
  > backup/current-boot.img
python3 -c 'assert open("backup/current-boot.img", "rb").read(8) == b"ANDROID!"'
sha256sum backup/current-boot.img > backup/current-boot.img.sha256
```

Without root, obtain a matching boot image from the exact ROM currently installed. Confirm fastboot access and disable tuning modules written for other kernel ABIs.

<a id="install-on-the-phone"></a>

## 3 · Trial boot and installation

If the device supports temporary boot, start with:

```bash
adb reboot bootloader
fastboot getvar product
fastboot getvar current-slot
fastboot boot dist/release/boot.img
```

Temporary boot does not flash boot; Android still writes user data and this kernel's diagnostic logger writes rawdump. Complete the [phone checks](VALIDATION.en.md#device), then choose one installation method:

| Method | Action |
|:--|:--|
| Horizon Kernel Flasher / compatible recovery | Install `dist/release/nuwa-kernel-6.18-AnyKernel3.zip` |
| Bootloader fastboot | Confirm the active slot, run `fastboot flash boot dist/release/boot.img`, then `fastboot reboot` |

Keep the kernel, ramdisk and modules matched; retain the replacement ramdisk inside the AnyKernel3 ZIP. The vendor_boot/init_boot ramdisk, or the recovery ramdisk (for example TWRP), still runs with this kernel; only the module files and lists come from the module layer. Install the optional `nuwa-618-v3.zip` separately through KernelSU's module interface.

## 4 · Rollback and troubleshooting

Return to the bootloader and confirm the slot matches the backup:

```bash
fastboot getvar current-slot
fastboot flash boot backup/current-boot.img
fastboot reboot
```

| Problem | Action |
|:--|:--|
| Download/hash failure | Rerun `bootstrap` in the same workspace; see the [full-OTA fallback](PROVENANCE.en.md#firmware) |
| Nonempty workspace without a project marker | Select a new empty `BUILD_ROOT` |
| cgroup check failure | Enable systemd/cgroup v2 and keep using the limited entry |
| Compile error / memory cap reached | Check the first error in `logs/<tag>/build.log`; keep the hard cap and use `JOBS=2` for memory pressure |
| Phone does not boot | Restore the matching backup boot to the same slot |
