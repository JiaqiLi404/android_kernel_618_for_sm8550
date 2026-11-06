# Validation

[简体中文](VALIDATION.zh-CN.md) · Previous: [Build and install](BUILD.en.md) · [Home](../README.md)

## 1 · Check your build

Keep the complete build workspace. Run from the project root with the same `BUILD_ROOT`/`TAG` used for building:

```bash
bash scripts/run-limited.sh verify
(cd dist/release && sha256sum -c SHA256SUMS)
```

Expect **`BUILD AND PACKAGE VALIDATION PASSED`** and `OK` for every checksum. With a custom TAG, replace `dist/release` with the corresponding output directory.

Optional installer emulation requires root, loop/mount and binfmt_misc support and uses temporary images:

```bash
. scripts/project-env.sh
sudo env BUILD_ROOT="$BUILD_ROOT" TAG="${TAG:-release}" \
  bash scripts/run-limited.sh test-installer
```

Expect **`INSTALLER EMULATION PASSED`**. The entry uses pinned QEMU and private PID/mount namespaces, then removes temporary handlers and loop bindings. Results are in `logs/<tag>/validation.log`, `installer-test.log` and `dist/<tag>/{validation,installer-validation}.json`.

Optional kernel runtime checks use QEMU virt (four CPUs, 2 GiB guest RAM) and the built base GKI; they do not emulate phone hardware:

```bash
sudo apt-get install -y qemu-system-arm
bash scripts/run-limited.sh test-runtime
bash scripts/run-limited.sh verify
```

Expect **`RUNTIME VALIDATION PASSED`**. See `logs/<tag>/runtime-test.log` and `dist/<tag>/runtime-validation.json`; rerunning `verify` includes all available reports in `SHA256SUMS`.

## 2 · Verified project scope

The table records the last complete run (bootstrap, build, `verify` and the QEMU runtime test). The row marked † was not re-run for the current tree: the host's 32-bit ARM user-mode emulator hangs before the installer's first command.

| Check | Result |
|:--|:--|
| Independent upstream reconstruction | **Passed**: 15 repositories, two vendor refs, six partitions and 54 module hashes ([record](../provenance/input-validation.json)) |
| Source compilation | **Passed**: GKI, 249/249 SoC modules, 30/30 Techpack groups, RPC/EVA, touch/THP, CS35L41 and the ToF module |
| Final modules and ABI | **404 modules, 21,107 versioned kernel imports**; no CRC mismatches or unresolved depmod errors |
| Embedded-initramfs kernel | 18,008 vmlinux export CRCs match the base GKI; the built-in layer equals the boot ramdisk (416 entries, module layer only) and INITRAMFS_FORCE is off |
| boot/AnyKernel3 consistency | Kernel, compressed ramdisk and all 17 command-line entries/order match; ZIP and required-list checks pass |
| Installer emulation † | Previous tree: Horizon/slot A and recovery/slot B pass; inactive slot unchanged. Not re-run (see above); the installer script itself is unchanged apart from its command-line list, which the packer checks against the image |
| Kernel runtime in QEMU | tmpfs xattrs, deferrable KFENCE, lazy RCU and the 8 KiB trace setting pass; 1,024 zram pages survive two recompression rounds; the 24-byte candidate cache is released on unload |
| TCP configuration | BBR/CUBIC and fq/fq_codel built in; CUBIC default; official mibbr in module load list |
| Setting recovery logic | dash, bash and ARM BusyBox checks cover trial expiry, panic rollback, boot-only trials and preserved choices across normal reboot/kernel changes |
| Resource limits | Effective cgroup values confirm 12/10/1 GiB and four CPU cores |

Environment: Ubuntu 26.04.1 x86_64/WSL2 with pinned Clang r584948c and Rust 1.91.1.p3. Kernel: `6.18.32-4k-gff4a6c3bb0f4-dirty`; `-dirty` identifies applied port changes. Vendor sources still emit compatibility warnings. Unmatched first-stage entries are listed in `dist/<tag>/first-stage-missing.txt`.

<a id="device"></a>

## 3 · Phone checks

After booting as described in [BUILD](BUILD.en.md#install-on-the-phone):

```bash
adb shell uname -a
adb shell cat /sys/block/zram0/comp_algorithm
adb shell cat /sys/block/zram0/recomp_algorithm
adb shell cat /sys/block/zram0/disksize
adb shell cat /proc/sys/net/ipv4/tcp_available_congestion_control
adb shell cat /proc/sys/net/ipv4/tcp_congestion_control
adb shell cat /sys/module/k618_probe/parameters/in_recovery
```

Defaults should be **6.18.32, LZ4 primary compression, ZSTD recompression and 16 GiB zram**, and `in_recovery` reads `N`; in a recovery such as TWRP it reads `Y` and the recovery's own interface starts. Check touch/gestures, display, AOD exit by fingerprint, calls/mobile data, Wi-Fi, audio, every camera, fingerprint/face unlock, and unplugged screen-off suspend.

<a id="scope"></a>

Source compilation, static checks, QEMU kernel checks and installer emulation on synthetic partitions pass. Real hardware modes, ROM compatibility, thermals, standby and power use still require phone testing; the 13 / 13 Ultra adaptations have not been run on those phones. The 128 MiB emulation fixtures do not establish the phone's physical partition size.
