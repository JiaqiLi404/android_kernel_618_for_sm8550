# Xiaomi 13 Pro · Linux 6.18

**An Android 17 / ACK 6.18.32 port combining the Xiaomi 13 Pro's SM8550 hardware support, vendor interfaces and matching modules, with inputs reconstructed from upstream.**

[简体中文](README.zh-CN.md)

[Download kernel packages](https://github.com/JiaqiLi404/android_kernel_618_for_sm8550/releases/tag/v6.18.32-nuwa.5) · Pre-release; physical-device validation is pending.

Device: **Xiaomi 13 Pro / nuwa** (main target; both of its panel variants, 38 0c 0a and 42 02 0b, are handled). Driver adaptations for the **Xiaomi 13 / fuxi** and **Xiaomi 13 Ultra / ishtar** are included (touch, with the 13's own fingerprint-path rules; panel tuning and DC dimming, peak HDR, camera PHY, OIS and park lens, PM8008 camera PMIC, ToF sensor, charger nodes); they and the 42 02 0b panel have not been run on such units yet. Software target: **NikoHyper / HyperOS 4, OS4.0.24.0.XEOCNXM**. Other ROM/vendor combinations require separate adaptation. Before installing, read the [usage notes](docs/BUILD.en.md#usage-notes) on the root manager and the boot screen.

## What we added

| Work | Value to users | Sources and implementation |
|:--|:--|:--|
| **Port 6.18 to SM8550** | Connect the newer kernel to the device's clock, UFS, USB, interconnect and power drivers | OnePlus driver foundation + MiCode device definitions; [driver origins](docs/PROVENANCE.en.md#drivers) |
| **Integrate device and multimedia support** | Adapt display/composer, camera, video, DSP, touch/gestures and audio interfaces, including the 13 / 13 Ultra variants | Combine SM8850, SM8550 and Xiaomi sources and handle 6.18 API/ABI differences; [sources and adaptations](docs/PROVENANCE.en.md#drivers) |
| **Match kernel, modules and boot images** | Produce a consistent module payload (406 modules), module-layer ramdisk, boot.img and AnyKernel3 package | 54 Xiaomi modules from official OTAs + rebuilt source modules; [firmware origins](docs/PROVENANCE.en.md#firmware) |
| **Memory, scheduling and power changes** | Provide reclaim, LZ4 1.10.1/ZSTD recompression, a cpufreq governor, energy-aware task placement, vendor boost limits, idle-path and wake controls | Each upstream origin and project adaptation is identified; [29 changes](docs/PROVENANCE.en.md#improvements) |
| **Make builds reproducible and inspectable** | Pin and fetch dependencies, check ABI and packaging, and cap WSL build memory at 12 GiB | [Acquire inputs](docs/PROVENANCE.en.md#acquire) → [Build](docs/BUILD.en.md) → [Validate](docs/VALIDATION.en.md) |

The main contribution is integrating these sources against one device interface and module ABI. Performance, power use and complete hardware behavior still require target-phone testing; see [validation scope](docs/VALIDATION.en.md#scope).

## How the key changes work

**Boot with the device's own ramdisk.** The kernel carries only the project's module layer (`/lib/modules`, its load lists and the batch loader) and unpacks it after the ramdisk the bootloader passes. The device's vendor_boot/init_boot ramdisk, or a recovery/TWRP ramdisk, stays in charge; only same-named module files and lists are replaced. [Sources and implementation](docs/PROVENANCE.en.md#drivers)

**LZ4: upstream 1.10.1.** Update the kernel's 1.8.3-based decoder to upstream C, serving EROFS file reads, zram and other callers. Arm64 64-byte wild copies can overrun the margin reserved for EROFS in-place decompression, so this port retains the 32-byte path and adapts kernel interfaces and module ABI. [Sources and implementation](docs/PROVENANCE.en.md#improvements)

**Skip idle when work is pending.** Another CPU can wake a task while this CPU performs bookkeeping with interrupts disabled after selecting an idle state. Both CPU-idle and power-domain entry paths recheck scheduling before CPU PM/PSCI calls, reducing entry followed immediately by wakeup; s2idle retains its original semantics. [Sources and implementation](docs/PROVENANCE.en.md#improvements)

**Frequency and placement.** The "walt" that the vendor's boot script writes into every CPU policy selects schedutilmium, the project's governor built into the kernel: separate up/down rate limits, the CPU's busy time in place of the scheduler's estimate, a ceiling that follows the load and a short floor after a touch. The kernel's own energy-aware scheduling (EAS) keeps placing tasks with the device tree's energy model, as Qualcomm's kernel does; `k618_place` adds four switchable rules around the threads that draw frames and an optional energy model derived from measured power. The tuning module keeps the vendor's perf HAL from raising the middle and prime clusters' minimum frequency on touches and large compositions, and turns MIUI's SchedBoostService off. [Sources and implementation](docs/PROVENANCE.en.md#improvements)

Other improvements include neighbour-page ageing, kswapd on the middle cores and kcompressd in reclaim, deferrable KFENCE, lazy RCU, compact cold-page records, budgeted hash-cache retention and setting trials; tmpfs xattrs add compatibility. See [all 28 changes](docs/PROVENANCE.en.md#improvements) for sources, defaults and selection decisions.

## Follow these steps

| Step | Guide | Result |
|:--|:--|:--|
| 1 | [PROVENANCE: sources and dependencies](docs/PROVENANCE.en.md) | All pinned sources, toolchains, firmware partitions and required files |
| 2 | [BUILD: build and install](docs/BUILD.en.md) | boot.img, AnyKernel3 ZIP and optional KernelSU tuning module |
| 3 | [VALIDATION: checks](docs/VALIDATION.en.md) | Input, ABI, package and optional installer-emulation results |

Complete the host setup in the dependency guide first, then clone and build:

```bash
git clone https://github.com/JiaqiLi404/android_kernel_618_for_sm8550.git xiaomi13-k618
cd xiaomi13-k618
bash scripts/run-limited.sh bootstrap
bash scripts/run-limited.sh build
bash scripts/run-limited.sh verify
```

## Directory roles

| Category | Directories / files |
|:--|:--|
| **Initial GitHub upload** | This page, `docs/`, `kernel-6.18/`, `module/`, `patches/`, `profiles/`, `provenance/`, `scripts/`, `tools/`, `assets/SHA256SUMS` and root Git configuration files |
| **Downloaded dependencies** | Sources, toolchains and firmware inside the Linux `BUILD_ROOT`; `assets/*.tar.zst` / `*.deb` are optional caches excluded from Git |
| **Intermediate files and logs** | Compilation/staging directories inside `BUILD_ROOT`, plus `logs/`; excluded from Git |
| **Final outputs** | `dist/release/`: boot image, installers, tuning module, reports and checksums; publish as Release assets when needed |
