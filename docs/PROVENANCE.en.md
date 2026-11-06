# Sources, changes and dependency acquisition

[简体中文](PROVENANCE.zh-CN.md) · [Home](../README.md) · Next: [Build](BUILD.en.md)

This page identifies the inputs, our adaptations and the steps to obtain everything needed. Full URLs, revisions and partition hashes are pinned in [input-sources.json](../provenance/input-sources.json); the links below identify fixed upstream versions.

<a id="acquire"></a>

## 1 · Prepare the host and acquire all dependencies

Use **Ubuntu 26.04 x86_64 / WSL2** with systemd and cgroup v2, and at least **80 GiB free Linux disk space**. The build workspace must use a native Linux filesystem; project and workspace paths must contain no spaces. Allow host memory for the capped 12 GiB process group and other applications.

In a Linux shell, from the project root:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential git python3 rsync make bc bison flex \
  libssl-dev libelf-dev libncurses-dev zlib1g-dev \
  cpio lz4 zstd kmod zip unzip file binutils \
  systemd util-linux pkg-config erofs-utils python3-zstandard
findmnt -t cgroup2
bash scripts/run-limited.sh bootstrap
bash scripts/run-limited.sh bootstrap --check
```

`bootstrap` fetches pinned sources and toolchains, applies the SoC patch, extracts official OTA partitions and verifies inputs. After **`Ready for bash scripts/run-limited.sh build`** and **`INPUT VALIDATION PASSED`**, continue to [BUILD](BUILD.en.md). The whole process enforces **12 GiB memory max / 10 GiB high / 1 GiB swap max / four CPU cores** and stops if limits are not effective.

The default workspace is created beneath the current Linux user's home and printed by the script. To customize it, first set `export BUILD_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/xiaomi13-k618"` and retain that variable for preparation, building and validation. Start with an empty directory; interrupted downloads can resume by rerunning `bootstrap` in the same workspace.

### How the inputs are assembled

1. Fetch the **15 source/toolchain repositories** and **two vendor refs** below.
2. Apply [soc-sm8550.patch](../patches/soc-sm8550.patch) to the OnePlus SoC foundation, verify the resulting source tree and assemble the vendor sources. The patch affects **228 files**; origins and tree hashes are in [soc-baseline.json](../provenance/soc-baseline.json).
3. Extract the **six partitions** from the two official OTAs below, unpack ramdisks/EROFS, and verify the [individual hashes](../provenance/binary-modules.tsv) of **54 selected Xiaomi modules**.

This route produces the complete workspace directly. The three `assets/*.tar.zst` files are optional offline caches of these inputs. If already available, use `bash scripts/run-limited.sh prepare` instead of `bootstrap`; it verifies [SHA256SUMS](../assets/SHA256SUMS) before extraction and refuses a cache whose SoC tree or ToF source predates the current inputs.

<a id="drivers"></a>

## 2 · Driver and tool origins, and our adaptations

| Input source | Use in this project | Adaptation / implementation |
|:--|:--|:--|
| [ACK 6.18.32 / Android 17](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d) | GKI core and modules | [6.18 API/config and root integration](../kernel-6.18/k618_gki.sh) |
| [OnePlus SM8850 kernel](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8850/tree/fc30e54174d254ff7f33622a9278e4435f6718d2) + [MiCode fuxi-t-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/7fad87dc178d09489b74a714e4a525d56f546855) | SM8550 clocks, TLMM, interconnect, UFS/ICE, USB, device definitions, DSP RPC and the 13 Ultra's PM8008 camera PMIC | [SoC baseline patch](../patches/soc-sm8550.patch), [device adaptation](../kernel-6.18/k618_kalama.py), [RPC](../kernel-6.18/k618_rpc515.sh), [PM8008 import](../kernel-6.18/k618_fuxi_mods.py) |
| [OnePlus SM8850 modules](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850/tree/5ab2a689ff87d7d28c511f1762cf41c1b90d965a) | Display, camera, GPU, audio, WLAN, NFC and data paths; corresponding vendor/qcom, oplus, nxp and st directories | [Component selection](../kernel-6.18/k618_tp_get.sh), [6.18/vendor ABI adaptation](../kernel-6.18/k618_tp_fix.py) |
| [OnePlus SM8550 modules](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8550/tree/f3cc5d4de0e01dae3a935755367cc43f43c09761) | video-driver and eva-kernel for this device’s video/EVA interfaces | [Source integration and compatibility](../kernel-6.18/k618_tp_fix.py) |
| [MiCode zeus-s-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/7d10d738b2c4299f3b8549c61a01ece39f19c788) | drivers/input/touchscreen/fts_spi and xiaomi | [Touch, gesture and THP integration](../kernel-6.18/k618_touch.sh), [13 / 13 Ultra pins and THP](../kernel-6.18/k618_touch_devices.py) |
| [MiCode ishtar-t-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/16c625d2e63c5c38370f00fdff5e44be217ff6d5) | drivers/input/stmvl53l5: the 13 Ultra's VL53L5 ToF sensor | [Build](../kernel-6.18/k618_tof.sh), [6.18 interface adaptation](../kernel-6.18/k618_tof_fix.py) |
| [ACK initramfs.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/init/initramfs.c) | Built-in initramfs | The kernel carries only the module layer and unpacks it after the bootloader's ramdisk (normal boot or recovery/TWRP); [unpack order](../kernel-6.18/k618_ports.py), [layer](../kernel-6.18/k618_irbuild.sh), [image](../kernel-6.18/k618_bootimg.sh) |
| [ACK CS35L41 / Cirrus DSP](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d) | CS35L41 / Cirrus DSP | [Device amplifier interface adaptation](../kernel-6.18/k618_cs35l41_fix.py) |
| [KernelSU](https://github.com/tiann/KernelSU/tree/0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d) + [SUSFS](https://gitlab.com/simonpunk/susfs4ksu/-/tree/5e0b2b925865000ca7dce993ecf1d281ee312887) | Built-in root and SUSFS v2.3.0 | [6.18 integration](../kernel-6.18/k618_gki.sh) |
| [LZ4](https://github.com/lz4/lz4/tree/83376b665fe6742171407350bb51e963215d6088) / [mkbootimg](https://android.googlesource.com/platform/system/tools/mkbootimg/+/f5c284e8998dd8a085cf027bb518b6c2b84cf744) / [AnyKernel3](https://github.com/osm0sis/AnyKernel3/tree/020dfeccf9d7e962a48400fc94d3e451df92eead) | Compression library, boot-image tools and installer | [Kernel LZ4 wrapper](../tools/kernel-patches/lz4_upgrade.py), [boot packaging](../kernel-6.18/k618_timg.sh), [installer script](../tools/ak3/anykernel-618.sh) |

**Xiaomi 13 / 13 Ultra:** compared with those phones' official firmware (fuxi OS3.0.309.0.WMCCNXM, ishtar OS3.0.308.0.WMACNXM; modules, device trees and the stock display and camera modules), the tree adds THP selection from the device tree and the 13 Ultra's second touch interrupt pin. The [display interface](../kernel-6.18/k618_mi_disp.c) sends the FOD/AOD commands on every panel and adds the 13 Ultra M1 panel's DSC rate-control values, DBI-BWG / skip-source sets by brightness band and temperature, 26h/2Fh/BAh rewrites and local-HBM calibration from SMEM, and the 13 M3 panel's DC dimming and HBM (options `msm_drm.m1_dbi`, `m1_gamma`, `m1_lhbm_cal`, `m1_dsc`, `build_id`, `dc_skip`). [k618_tp_fix.py](../kernel-6.18/k618_tp_fix.py) adds the 13 Ultra's `qcom,csiphy-v2.1.2-m1` camera PHY table and OIS (BU24721, BU24532, RUMBA-S4H, SEM1217S); OIS chip firmware is rewritten only with `camera.k618_ois_flash_update=1`. PM8008, ToF and the fuel-gauge charger nodes complete it ([k618_mi_charger.py](../kernel-6.18/k618_mi_charger.py)). These firmware packages are references, not build inputs; none of this has run on those phones yet.

**AOD exit on all three panels:** AOD_TO_NORMAL follows the stock driver; after a fingerprint-driven exit below 120 Hz the 13 Pro (M2) and 13 Ultra (M1) panels get their timing switch again, and M1 after every nolp.

**The 13 Pro's second panel and per-unit panel data:** the 13 Pro's device tree has two panels, 38 0c 0a and 42 02 0b. For the 42 02 0b (`mi,local-hbm-alpha-ctrl-aa-area`) the display interface sets the 87h alpha of every local-HBM command from the stock table by brightness, writes 51h around the fingerprint area and turns dimming back on afterwards (`msm_drm.lhbm_alpha`). On every panel the name comes from `msm_drm.dsi_display0`, the white point from `msm_drm.oled_wp` / `sec_oled_wp` or the panel, and the build id is read from the panel (DAh; on the 38 0c a failed read keeps 0xA0); max-brightness-clone, the panel-on dimming delay and local HBM come from the device tree. The 13 Ultra adds peak HDR (feature 27, `msm_drm.m1_peak`) and the OIS park-lens thread (packet 6), which is stopped before power-down so closing the camera cannot hang.

**Panel events:** the 42 02 0b is in none of the device tree's touch, `charge-screen` or `thermal-screen` panel lists, so [touch](../kernel-6.18/k618_touch_devices.py), the [charger](../kernel-6.18/k618_mi_charger.py) and [k618_mi_thermal](../kernel-6.18/k618_mi_thermal.c) take the registered panel when none of their list is; the charger also retries every 5 s until msm_drm is up.

**Device-tree audit:** [k618_dtvariants.py](../kernel-6.18/k618_dtvariants.py) merges each phone's base tree and overlay as the bootloader does and lists every node with the driver that binds it. Every hardware part of the three phones has a driver here; the only function with two parts is the 13 Pro's panel. None of this has run on a 13, a 13 Ultra or a 42 02 0b unit yet.

**Pinned tools:** [Clang r584948c](https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+/4a7d0a5e443c27d8f68fbd46abfab464e1355a69), [Rust 1.91.1.p3](https://android.googlesource.com/platform/prebuilts/rust-toolchain/linux-x86/+/a04f70c5179f23eff11c58cb480acee4afb1334b), [clang-tools / bindgen](https://android.googlesource.com/platform/prebuilts/clang-tools/+/4155cb466dcd9b53c6f873829c53aa6f905449dd), [kernel-build-tools / pahole](https://android.googlesource.com/kernel/prebuilts/build-tools/+/78af2daf5d9d36fc6e1ddb28ee0d7b6516709eb4), [build-tools](https://android.googlesource.com/platform/prebuilts/build-tools/+/e25d4821f50af3f6e0b87d7751dfd27037445fe5). `bootstrap` selects the required Linux directories and downloads [QEMU 8.2.2 static](https://archive.ubuntu.com/ubuntu/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_amd64.deb) for optional installer emulation. Do not substitute an arbitrary system Clang. build-tools also includes the `common/bison` data required by Bison.


<a id="firmware"></a>

## 3 · Binary inputs from official firmware

| Fixed official full-OTA download | Extracted partitions | Use |
|:--|:--|:--|
| [nuwa OS3.0.311.0.WMBCNXM](https://bigota.d.miui.com/OS3.0.311.0.WMBCNXM/nuwa-ota_full-OS3.0.311.0.WMBCNXM-user-16.0-0113895944.zip) | `init_boot`, `vendor_boot`, `vendor_dlkm` | Generic/vendor ramdisks, device modules and load lists |
| [madrid OS4.0.24.0.XEOCNXM](https://bigota.d.miui.com/OS4.0.24.0.XEOCNXM/madrid-ota_full-OS4.0.24.0.XEOCNXM-user-17.0-81ece9ec12.zip) | `vendor_boot`, `vendor_dlkm`, `system_dlkm` | Selected Xiaomi 6.18 modules and partition contents |


The entry reads the required payload data with HTTP Range, verifies operation and final partition hashes, then extracts files with mkbootimg/lz4/cpio/fsck.erofs. If Range is unavailable, download the full ZIP above into `downloads/` beneath `BUILD_ROOT`, retain its original filename and rerun `bootstrap`.

The nuwa boot inputs are byte-identical to these official OS3.0.311.0 images; **the runtime target remains the HyperOS port named on the home page**. Closed modules remain identified firmware binaries. The project implements compatible [thermal](../kernel-6.18/k618_mi_thermal.c), [reclaim](../kernel-6.18/k618_perf_helper.c), [memory-info](../kernel-6.18/k618_mi_memory.c), [display](../kernel-6.18/k618_mi_disp.c) and other interfaces without claiming complete private functionality. Public sources retain upstream headers and licenses.

<a id="improvements"></a>

## 4 · Memory, power and setting recovery

The table distinguishes origin from project implementation; full attribution and implementation locations are in [optimizations.json](../provenance/optimizations.json). The main entry is [k618_ports.py](../kernel-6.18/k618_ports.py).

| Change | Source links | Purpose and adaptation |
|:--|:--|:--|
| **PSCI reschedule check** | [ACK cpuidle-psci.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/cpuidle/cpuidle-psci.c) | Recheck pending work after idle bookkeeping in both CPU and power-domain entry paths, avoiding unnecessary CPU PM/PSCI transitions; preserve s2idle semantics; [implementation](../kernel-6.18/k618_ports.py) |
| **EROFS worker priority** | [ACK EROFS zdata.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/fs/erofs/zdata.c) | Reduce decompression-worker queueing on file reads by raising FIFO priority from 1 to 3, adjustable at runtime; [implementation](../kernel-6.18/k618_ports.py) |
| **Binder debug filtering** | [ACK binder.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/android/binder.c) | Compile out default-disabled debug categories to remove calls from Binder transactions and lock paths; retain default error output; [implementation](../kernel-6.18/k618_ports.py) |
| **High-priority loop workers** | [OnePlus workqueue policy](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850/blob/5ab2a689ff87d7d28c511f1762cf41c1b90d965a/vendor/oplus/kernel/storage/storage_feature_in_module/common/wq_dynamic_priority/oplus_wq_dynamic_priority.c) · [Linux v5.12 loop.c](https://github.com/torvalds/linux/blob/v5.12/drivers/block/loop.c) | Give loop workers used by APEX and other reads higher priority to reduce queueing behind ordinary background work; retain a switch; [implementation](../kernel-6.18/k618_ports.py) |
| **Fence-info queries** | [ACK sync_file.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/dma-buf/sync_file.c) | Build fence records on the stack, remove query-array allocation and omit debug names by default to reduce repeated per-frame work; [implementation](../kernel-6.18/k618_ports.py) |
| **Reclaim throttle exemptions** | [OnePlus SM8850 zram_opt.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850/blob/5ab2a689ff87d7d28c511f1762cf41c1b90d965a/vendor/oplus/kernel/mm/zram_opt/zram_opt.c) | Let RT/negative-nice tasks bypass direct-reclaim throttling under memory pressure; expose a switch and counter; [implementation](../kernel-6.18/k618_ports.py) |
| **Reap large killed processes** | [OnePlus SM8550 zram_opt.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8550/blob/f3cc5d4de0e01dae3a935755367cc43f43c09761/vendor/oplus/kernel/mm/zram_opt/zram_opt.c) | Send eligible SIGKILL victims with more than 32 MiB of anonymous memory plus swap to the OOM reaper sooner; make the threshold tunable; [implementation](../kernel-6.18/k618_ports.py) |
| **Remove redundant NOHZ count** | [6b67c8a72e56](https://github.com/torvalds/linux/commit/6b67c8a72e56041f91f70ae5995bdb769761869a) · [94e70734b4d0](https://github.com/torvalds/linux/commit/94e70734b4d034b9df795bd1ad3452ea96e742ca) · [5d86d542f68f](https://github.com/torvalds/linux/commit/5d86d542f68fda7ef6d543ac631b741db734101a) | Use the existing idle-CPU mask instead of a global atomic count, reducing shared-cacheline contention on idle entry/exit; [implementation](../kernel-6.18/k618_ports.py) |
| **Idle-time accounting** | [4b603f1551a7](https://github.com/torvalds/linux/commit/4b603f1551a73e2868b9e7a14b3938c23275cefb) · [c6dcd97c8be7](https://github.com/torvalds/linux/commit/c6dcd97c8be75f052a1ca52cf79b03e7292962f1) | Update statistics on idle exit, covering migration/fork paths and preventing busy time from being counted as idle in balancing decisions; [implementation](../kernel-6.18/k618_ports.py) |
| **Stop reference scans early** | Huawei Kirin 9000 kernel open-source release, `mm/stop_page_referencing.c` | Stop reverse-map walks once the page-activity decision is settled; adapt to folios/classic LRU while retaining ABI; [implementation](../kernel-6.18/k618_ports.py) |
| **Protect highly shared folios** | [MiCode mi_rmap_efficiency.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/e29c0fd34c6e8f91d30046a85d1afac3fe032a7e/drivers/staging/mi_rmap_efficiency/mi_rmap_efficiency.c) | Treat pages with at least 32 mappings as active by default, reducing repeated walks and reclaim of shared pages; retain a pressure cap; [implementation](../kernel-6.18/k618_ports.py) |
| **LZ4: upstream 1.10.1** | [LZ4 pinned lib/](https://github.com/lz4/lz4/tree/83376b665fe6742171407350bb51e963215d6088/lib) · [Linux 1.8.3 decoder baseline](https://github.com/torvalds/linux/commit/2209fda323e2fd2a2d0885595fd5097717f8d2aa) | Update the 1.8.3-based decoder to upstream 1.10.1 C; disable arm64 64-byte wild copies and retain the 32-byte path for EROFS in-place safety, with module ABI adaptation; [implementation](../tools/kernel-patches/lz4_upgrade.py) |
| **Wakelock/alarm hold limits** | This project's policy on [ACK wakelock.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/power/wakelock.c) · [ACK alarmtimer.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/time/alarmtimer.c) | Default untimed wakelocks to 500 ms and shorten alarm hold to 5 ms past firing, reducing unnecessary awake time; expose reversible parameters; [implementation](../tools/kernel-patches/wake_holds.py) |
| **Wakelock blocker** | This project's implementation on [ACK wakeup.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/base/power/wakeup.c) | Filter wake holds through configurable name lists in `/sys/devices/virtual/misc/wakelock_blocker`, with RCU management and prefix prefiltering; clear the lists to restore behavior; [implementation](../tools/kernel-patches/wakelock_blocker.py) |
| **Automatic tiered zram** | [ACK zram driver](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/block/zram/zram_drv.c) · [ACK zram documentation](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/Documentation/admin-guide/blockdev/zram.rst) | Use LZ4 on the hot path and ZSTD for cold pages; default to 16 GiB with 600-second awake-time recompression and freezer/suspend cooperation; [implementation](../kernel-6.18/k618_ports.py) |
| **Deferrable KFENCE sampling** | [Linux KFENCE](https://docs.kernel.org/dev-tools/kfence.html) · [ACK Kconfig](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/lib/Kconfig.kfence) | Enable CONFIG_KFENCE_DEFERRABLE to avoid waking an idle CPU solely for sampling while retaining detection; [implementation](../kernel-6.18/k618_gki.sh) |
| **Initial tracing-buffer size** | [ACK tracing](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/trace/trace.c) · [Kernel parameters](https://docs.kernel.org/admin-guide/kernel-parameters.html) | Start at 8 KiB per CPU to limit automatic buffer growth; Perfetto can still resize buffers; [implementation](../kernel-6.18/k618_timg.sh) |
| **Lazy RCU callback batching** | [ACK RCU](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/rcu/tree.c) · [ACK NOCB](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/rcu/tree_nocb.h) | Explicitly enable offloading and lazy batching to reduce light-load wakeups; retain upstream pressure and urgent flushing; [implementation](../kernel-6.18/k618_timg.sh) |
| **tmpfs extended attributes** | [ACK filesystem Kconfig](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/fs/Kconfig) · [ACK shmem](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/mm/shmem.c) | Enable CONFIG_TMPFS_XATTR for temporary-filesystem compatibility; make no performance claim; [implementation](../kernel-6.18/k618_gki.sh) |
| **Dedicated zram candidate slab** | [ACK zram](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/block/zram/zram_drv.c) · [ACK slab API](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/include/linux/slab.h) | Use a small-object cache for recompression/writeback candidates; fall back to kmalloc if cache creation fails and release it on teardown; [implementation](../kernel-6.18/k618_memory.py) |
| **Budgeted metadata-cache retention** | [OnePlus fixed source](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/49cec3dbd158aa0b7e8f1e681a0d933c9e5dd76e/drivers/md/dm-bufio.c) · [ACK dm-bufio](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/md/dm-bufio.c) | Retain caches only above 10% available RAM and within a budget capped at 2% of RAM; restore ordinary reclaim under pressure and allow disabling; [implementation](../kernel-6.18/k618_memory.py) |
| **Tuning-setting trial rollback** | [KernelSU module lifecycle](https://kernelsu.org/guide/module.html) · [AOSP boot reasons](https://source.android.com/docs/core/architecture/bootloader/boot-reason) | Trial edits for 120 seconds; boot-only choices start next boot. Roll back only confirmed same-kernel panic/watchdog boots inside the window; preserve choices across normal reboots and kernel changes; [implementation](../module/nuwa-618/trial-guard.sh) |


The links identify public code, policies and upstream mechanisms; runtime parameters and 6.18 adaptations are described separately in the implementation column. Automatic zram orchestration adds policy to upstream mechanisms. These changes do not establish measured performance or power gains on this device.


**Existing vendor capabilities:** selected `mi_reclaim` provides watermark boosting, `mi_mtmr` provides VIP page-cache protection, `perf_actuator` / `os_cpu_qi` provide `/dev/xr_fbt`, and `mibbr` provides per-application TCP support. All come from the official madrid OTA above; see the [module identities](../provenance/binary-modules.tsv). Reuse these implementations without stacking a second policy.

**TCP:** [BBR](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/ipv4/tcp_bbr.c) and [CUBIC](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/ipv4/tcp_cubic.c) are built in, alongside [fq](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/sched/sch_fq.c) and [fq_codel](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/sched/sch_fq_codel.c) queue disciplines. The global default is **CUBIC**; the selected official `mibbr.ko` supplies vendor application policy. Compiled support does not mean every connection uses BBR.

**Selection:** enable the upstream mechanisms, add the dedicated slab, budgeted retention and setting trials above. Preserve the cubic global default and existing BBR/fq_codel/mibbr capabilities, along with linear-map protection. Defer extra residency, a direct-reclaim swappiness cap and additional F2FS changes not reproduced on this kernel.

Trial rollback belongs to the optional tuning module. Boot-only settings start their trial next boot; only a confirmed same-kernel panic/watchdog boot inside the window restores the previous value. It does not replace a boot-image backup.

With dependencies ready, continue to [BUILD: build and install](BUILD.en.md).
