# 来源、改动与依赖获取

[English](PROVENANCE.en.md) · [首页](../README.zh-CN.md) · 下一步：[构建](BUILD.zh-CN.md)

本页说明依赖从哪里来、我们如何适配，以及怎样一次取得完整输入。完整 URL、提交号、分区哈希见 [input-sources.json](../provenance/input-sources.json)；下表链接均指向固定上游版本。

<a id="acquire"></a>

## 1 · 准备宿主并取得全部依赖

使用 **Ubuntu 26.04 x86_64 / WSL2**，开启 systemd 与 cgroup v2，准备至少 **80 GiB Linux 可用磁盘空间**。构建工作区须在原生 Linux 文件系统中，项目和工作区路径均不能含空格。宿主需为受限的 12 GiB 进程组及其他应用留出内存。

在项目根目录的 Linux shell 中安装依赖：

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

`bootstrap` 自动下载固定源码和工具链、应用 SoC 补丁、从官方 OTA 提取分区并校验。看到 **`Ready for bash scripts/run-limited.sh build`** 和 **`INPUT VALIDATION PASSED`** 后，依赖即已就绪，继续 [BUILD](BUILD.zh-CN.md)。全过程强制 **12 GiB 内存上限 / 10 GiB 高水位 / 1 GiB 交换上限 / 四核 CPU**；限制不生效时入口会退出。

工作区默认创建在当前 Linux 用户目录下，脚本会打印位置。自定义时，先设置 `export BUILD_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/xiaomi13-k618"`，后续准备、构建和验证保持同一变量。使用空目录；中断后可在同一工作区重跑 `bootstrap`。

### 输入如何组装

1. 获取下表的 **15 个源码 / 工具链仓库**和 **2 个 vendor 引用**。
2. 在一加 SoC 基础上应用 [soc-sm8550.patch](../patches/soc-sm8550.patch)，核对目标源码树，再组装 vendor 源码。补丁涉及 **228 个文件**，来源与树哈希见 [soc-baseline.json](../provenance/soc-baseline.json)。
3. 从两个官方 OTA 提取下表的 **6 个分区**，解包 ramdisk / EROFS，并核对 **54 个选用小米模块**的[单项哈希](../provenance/binary-modules.tsv)。

这条路线直接生成完整工作区。三个 `assets/*.tar.zst` 是这些输入的可选离线缓存；已持有缓存时，可用 `bash scripts/run-limited.sh prepare` 代替 `bootstrap`，按 [SHA256SUMS](../assets/SHA256SUMS) 校验解包；SoC 源码树或 ToF 源码早于当前输入的缓存会被拒绝。

<a id="drivers"></a>

## 2 · 驱动和工具从哪里来，我们改了什么

| 输入来源 | 用于本项目 | 适配 / 实现 |
|:--|:--|:--|
| [ACK 6.18.32 / Android 17](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d) | GKI 核心、内核模块 | [6.18 API / 配置与 root 集成](../kernel-6.18/k618_gki.sh) |
| [OnePlus SM8850 kernel](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8850/tree/fc30e54174d254ff7f33622a9278e4435f6718d2) + [MiCode fuxi-t-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/7fad87dc178d09489b74a714e4a525d56f546855) | SM8550 时钟、TLMM、互联、UFS / ICE、USB、设备定义、DSP RPC 及 13 Ultra 的 PM8008 相机电源 | [SoC 基线补丁](../patches/soc-sm8550.patch)、[设备适配](../kernel-6.18/k618_kalama.py)、[RPC](../kernel-6.18/k618_rpc515.sh)、[PM8008 导入](../kernel-6.18/k618_fuxi_mods.py) |
| [OnePlus SM8850 modules](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850/tree/5ab2a689ff87d7d28c511f1762cf41c1b90d965a) | 显示、相机、GPU、音频、WLAN、NFC 和数据通路；保留对应 vendor/qcom、oplus、nxp、st 源码目录 | [组件选取](../kernel-6.18/k618_tp_get.sh)、[6.18 / vendor ABI 适配](../kernel-6.18/k618_tp_fix.py) |
| [OnePlus SM8550 modules](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8550/tree/f3cc5d4de0e01dae3a935755367cc43f43c09761) | video-driver 与 eva-kernel，匹配本设备的视频 / EVA 接口 | [源码组合与兼容处理](../kernel-6.18/k618_tp_fix.py) |
| [MiCode zeus-s-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/7d10d738b2c4299f3b8549c61a01ece39f19c788) | drivers/input/touchscreen/fts_spi 与 xiaomi | [触摸、手势与 THP 集成](../kernel-6.18/k618_touch.sh)、[13 / 13 Ultra 引脚与 THP](../kernel-6.18/k618_touch_devices.py) |
| [MiCode ishtar-t-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/16c625d2e63c5c38370f00fdff5e44be217ff6d5) | drivers/input/stmvl53l5：13 Ultra 的 VL53L5 ToF 测距 | [编译](../kernel-6.18/k618_tof.sh)、[6.18 接口适配](../kernel-6.18/k618_tof_fix.py) |
| [ACK initramfs.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/init/initramfs.c) | 内置 initramfs | 内核只内置模块层，并在 bootloader 交来的 ramdisk 之后解包（正常启动或 recovery / TWRP）；[解包顺序](../kernel-6.18/k618_ports.py)、[模块层](../kernel-6.18/k618_irbuild.sh)、[镜像](../kernel-6.18/k618_bootimg.sh) |
| [ACK CS35L41 / Cirrus DSP](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d) | CS35L41 / Cirrus DSP | [设备功放接口适配](../kernel-6.18/k618_cs35l41_fix.py) |
| [KernelSU](https://github.com/tiann/KernelSU/tree/0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d) + [SUSFS](https://gitlab.com/simonpunk/susfs4ksu/-/tree/5e0b2b925865000ca7dce993ecf1d281ee312887) | 内建 root 与 SUSFS v2.3.0 | [6.18 集成](../kernel-6.18/k618_gki.sh) |
| [LZ4](https://github.com/lz4/lz4/tree/83376b665fe6742171407350bb51e963215d6088) / [mkbootimg](https://android.googlesource.com/platform/system/tools/mkbootimg/+/f5c284e8998dd8a085cf027bb518b6c2b84cf744) / [AnyKernel3](https://github.com/osm0sis/AnyKernel3/tree/020dfeccf9d7e962a48400fc94d3e451df92eead) | 压缩库、boot 镜像工具、安装器 | [内核 LZ4 封装](../tools/kernel-patches/lz4_upgrade.py)、[boot 打包](../kernel-6.18/k618_timg.sh)、[安装脚本](../tools/ak3/anykernel-618.sh) |

**小米 13 / 13 Ultra：** 对照两台手机的官方固件（fuxi OS3.0.309.0.WMCCNXM、ishtar OS3.0.308.0.WMACNXM，比较模块、设备树及原厂显示和相机模块），本树按设备树决定 THP，并支持 13 Ultra 的第二个触摸中断脚。[显示接口](../kernel-6.18/k618_mi_disp.c)对所有面板发送指纹高亮 / AOD 命令，并加入 13 Ultra M1 面板的 DSC 码率控制参数、按亮度段与温度切换的 DBI-BWG / skip-source 命令、26h / 2Fh / BAh 改写、来自 SMEM 的局部高亮校准，以及 13 M3 面板的 DC 调光与 HBM（参数 `msm_drm.m1_dbi`、`m1_gamma`、`m1_lhbm_cal`、`m1_dsc`、`m1_build_id`、`dc_skip`）。[k618_tp_fix.py](../kernel-6.18/k618_tp_fix.py) 加入 13 Ultra 的 `qcom,csiphy-v2.1.2-m1` 相机 PHY 表和 OIS（BU24721、BU24532、RUMBA-S4H、SEM1217S）；只有 `camera.k618_ois_flash_update=1` 时才改写 OIS 芯片固件。另有 PM8008、ToF 及电量计充电节点（[k618_mi_charger.py](../kernel-6.18/k618_mi_charger.py)）。这些固件包只作对照，不是构建输入；以上均未在这两台手机上运行过。

**三种面板的 AOD 退出：** AOD_TO_NORMAL 与原厂驱动一致；低于 120 Hz 时由指纹退出 AOD 后，13 Pro（M2）和 13 Ultra（M1）面板会重发时序切换命令，M1 在每次 nolp 后也会重发。

**固定工具链：** [Clang r584948c](https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+/4a7d0a5e443c27d8f68fbd46abfab464e1355a69), [Rust 1.91.1.p3](https://android.googlesource.com/platform/prebuilts/rust-toolchain/linux-x86/+/a04f70c5179f23eff11c58cb480acee4afb1334b), [clang-tools / bindgen](https://android.googlesource.com/platform/prebuilts/clang-tools/+/4155cb466dcd9b53c6f873829c53aa6f905449dd), [kernel-build-tools / pahole](https://android.googlesource.com/kernel/prebuilts/build-tools/+/78af2daf5d9d36fc6e1ddb28ee0d7b6516709eb4), [build-tools](https://android.googlesource.com/platform/prebuilts/build-tools/+/e25d4821f50af3f6e0b87d7751dfd27037445fe5)。`bootstrap` 选取所需 Linux 目录，并下载用于可选安装器模拟的 [QEMU 8.2.2 static](https://archive.ubuntu.com/ubuntu/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_amd64.deb)。编译不使用任意系统 Clang 替代这些输入；build-tools 同时获取 Bison 所需的 `common/bison` 共享数据。


<a id="firmware"></a>

## 3 · 官方固件中的二进制输入

| 固定官方完整 OTA 下载 | 提取的分区 | 用途 |
|:--|:--|:--|
| [nuwa OS3.0.311.0.WMBCNXM](https://bigota.d.miui.com/OS3.0.311.0.WMBCNXM/nuwa-ota_full-OS3.0.311.0.WMBCNXM-user-16.0-0113895944.zip) | `init_boot`, `vendor_boot`, `vendor_dlkm` | 通用 / vendor ramdisk、设备模块与加载清单 |
| [madrid OS4.0.24.0.XEOCNXM](https://bigota.d.miui.com/OS4.0.24.0.XEOCNXM/madrid-ota_full-OS4.0.24.0.XEOCNXM-user-17.0-81ece9ec12.zip) | `vendor_boot`, `vendor_dlkm`, `system_dlkm` | 选用的小米 6.18 模块及对应分区内容 |


入口按 HTTP Range 下载所需 payload 数据，核对操作与最终分区哈希，再用 mkbootimg / lz4 / cpio / fsck.erofs 提取所需文件。若服务器不支持 Range，可从上表下载完整 ZIP，保持原文件名放进 `BUILD_ROOT` 下的 `downloads/`，再重跑 `bootstrap`。

nuwa 的引导输入与这里的官方 OS3.0.311.0 镜像逐字节相同；**运行目标仍是首页指定的 HyperOS 移植环境**。闭源模块按固件二进制保留；项目另据这些接口实现[温控](../kernel-6.18/k618_mi_thermal.c)、[回收控制](../kernel-6.18/k618_perf_helper.c)、[内存信息](../kernel-6.18/k618_mi_memory.c)、[显示](../kernel-6.18/k618_mi_disp.c)等兼容接口，未覆盖全部私有功能。公开源码保留上游文件头与许可证。

<a id="improvements"></a>

## 4 · 内存、功耗与使用保护

下表区分来源与本项目实现；完整归属和实现位置见 [optimizations.json](../provenance/optimizations.json)。核心入口为 [k618_ports.py](../kernel-6.18/k618_ports.py)。

| 改动 | 来源链接 | 作用与适配 |
|:--|:--|:--|
| **PSCI 进空闲前重查调度** | [ACK cpuidle-psci.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/cpuidle/cpuidle-psci.c) | 空闲记账期间若有新任务唤醒，浅空闲和电源域空闲两条入口都重查调度，避免白做 CPU PM / PSCI 进出；保留 s2idle 语义；[实现](../kernel-6.18/k618_ports.py) |
| **EROFS 解压线程优先级** | [ACK EROFS zdata.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/fs/erofs/zdata.c) | 文件读取等待解压时，让解压线程少排队：FIFO 优先级由 1 提到 3，可运行时调整；[实现](../kernel-6.18/k618_ports.py) |
| **Binder 调试类别编译过滤** | [ACK binder.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/android/binder.c) | 将默认关闭的调试类别在编译时裁去，减少 Binder 事务与加锁路径的无用调用；保留默认错误输出；[实现](../kernel-6.18/k618_ports.py) |
| **loop 高优先级工作线程** | [OnePlus workqueue policy](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850/blob/5ab2a689ff87d7d28c511f1762cf41c1b90d965a/vendor/oplus/kernel/storage/storage_feature_in_module/common/wq_dynamic_priority/oplus_wq_dynamic_priority.c) · [Linux v5.12 loop.c](https://github.com/torvalds/linux/blob/v5.12/drivers/block/loop.c) | APEX 等 loop 文件读取经过的工作队列使用高优先级，减少与普通后台任务排队；保留开关；[实现](../kernel-6.18/k618_ports.py) |
| **fence 信息查询** | [ACK sync_file.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/dma-buf/sync_file.c) | 逐条在栈上生成 fence 信息，省掉查询数组分配，默认省略调试名称，减少每帧重复工作；[实现](../kernel-6.18/k618_ports.py) |
| **直接回收节流豁免** | [OnePlus SM8850 zram_opt.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850/blob/5ab2a689ff87d7d28c511f1762cf41c1b90d965a/vendor/oplus/kernel/mm/zram_opt/zram_opt.c) | 内存紧张时让实时 / 负 nice 任务绕过直接回收的节流等待，避免关键线程跟着停顿；可关闭并计数；[实现](../kernel-6.18/k618_ports.py) |
| **及时回收被杀大进程** | [OnePlus SM8550 zram_opt.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8550/blob/f3cc5d4de0e01dae3a935755367cc43f43c09761/vendor/oplus/kernel/mm/zram_opt/zram_opt.c) | 被 SIGKILL 的进程占用匿名内存加 swap 超过默认 32 MiB 时，提前交给 OOM 回收器释放；阈值可调；[实现](../kernel-6.18/k618_ports.py) |
| **去掉冗余 NOHZ CPU 计数** | [6b67c8a72e56](https://github.com/torvalds/linux/commit/6b67c8a72e56041f91f70ae5995bdb769761869a) · [94e70734b4d0](https://github.com/torvalds/linux/commit/94e70734b4d034b9df795bd1ad3452ea96e742ca) · [5d86d542f68f](https://github.com/torvalds/linux/commit/5d86d542f68fda7ef6d543ac631b741db734101a) | 用现有空闲 CPU 掩码代替全局原子计数，减少各核进出空闲时对同一缓存行的争用；[实现](../kernel-6.18/k618_ports.py) |
| **空闲时间统计** | [4b603f1551a7](https://github.com/torvalds/linux/commit/4b603f1551a73e2868b9e7a14b3938c23275cefb) · [c6dcd97c8be7](https://github.com/torvalds/linux/commit/c6dcd97c8be75f052a1ca52cf79b03e7292962f1) | 在离开空闲时更新统计，覆盖迁移 / fork 等路径，避免把忙碌时间误算为空闲并影响均衡判断；[实现](../kernel-6.18/k618_ports.py) |
| **提前停止反向映射扫描** | 华为麒麟 9000 内核开源发布，`mm/stop_page_referencing.c` | 页的活跃性结论已确定时提前结束反向映射扫描，减少回收遍历；适配 folio / 经典 LRU 并保留 ABI；[实现](../kernel-6.18/k618_ports.py) |
| **保留高共享映射页** | [MiCode mi_rmap_efficiency.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/e29c0fd34c6e8f91d30046a85d1afac3fe032a7e/drivers/staging/mi_rmap_efficiency/mi_rmap_efficiency.c) | 默认将映射计数至少 32 的共享页视为活跃，减少反复遍历和回收公共页；保留压力上限；[实现](../kernel-6.18/k618_ports.py) |
| **LZ4：上游 1.10.1** | [LZ4 pinned lib/](https://github.com/lz4/lz4/tree/83376b665fe6742171407350bb51e963215d6088/lib) · [Linux 1.8.3 decoder baseline](https://github.com/torvalds/linux/commit/2209fda323e2fd2a2d0885595fd5097717f8d2aa) | 以 1.10.1 C 实现更新原有 1.8.3 解压代码；禁用 arm64 的 64 字节超前拷贝，保留 32 字节路径以保护 EROFS 原地解压，并适配模块 ABI；[实现](../tools/kernel-patches/lz4_upgrade.py) |
| **唤醒锁 / 闹钟保持时间** | 本项目策略，作用于 [ACK wakelock.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/power/wakelock.c) · [ACK alarmtimer.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/time/alarmtimer.c) | 无时限唤醒锁默认 500 ms 到期，闹钟的额外保持缩至响后 5 ms，减少不必要的清醒时间；参数可恢复；[实现](../tools/kernel-patches/wake_holds.py) |
| **唤醒锁拦截** | 本项目实现，基于 [ACK wakeup.c](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/base/power/wakeup.c) | 按可调名称名单（`/sys/devices/virtual/misc/wakelock_blocker`）过滤唤醒保持，RCU 管理名单并预筛前缀，减少热点查表；支持清空名单恢复；[实现](../tools/kernel-patches/wakelock_blocker.py) |
| **zram 自动分层压缩** | [ACK zram driver](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/block/zram/zram_drv.c) · [ACK zram documentation](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/Documentation/admin-guide/blockdev/zram.rst) | LZ4 处理热路径，冷页转用 ZSTD；默认 16 GiB，清醒时每 600 秒重压缩，并配合 freezer / 挂起；[实现](../kernel-6.18/k618_ports.py) |
| **KFENCE 可延迟采样** | [Linux KFENCE](https://docs.kernel.org/dev-tools/kfence.html) · [ACK Kconfig](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/lib/Kconfig.kfence) | 采样定时器不再专门叫醒空闲核；启用 CONFIG_KFENCE_DEFERRABLE，保留检测能力；[实现](../kernel-6.18/k618_gki.sh) |
| **跟踪缓冲初始大小** | [ACK tracing](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/trace/trace.c) · [Kernel parameters](https://docs.kernel.org/admin-guide/kernel-parameters.html) | 初始每核 8 KiB，减少自动扩张的内存占用；Perfetto 仍可按需扩容；[实现](../kernel-6.18/k618_timg.sh) |
| **lazy RCU 回调批处理** | [ACK RCU](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/rcu/tree.c) · [ACK NOCB](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/kernel/rcu/tree_nocb.h) | 显式启用回调卸载与 lazy 批处理，减少轻负载唤醒；沿用上游的内存压力与紧急刷新路径；[实现](../kernel-6.18/k618_timg.sh) |
| **tmpfs 扩展属性** | [ACK filesystem Kconfig](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/fs/Kconfig) · [ACK shmem](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/mm/shmem.c) | 启用 CONFIG_TMPFS_XATTR，补齐临时文件系统的属性兼容性；不作为性能提升宣称；[实现](../kernel-6.18/k618_gki.sh) |
| **冷页候选表专用 slab** | [ACK zram](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/block/zram/zram_drv.c) · [ACK slab API](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/include/linux/slab.h) | 用专用小对象缓存保存重压缩 / 写回候选，减少临时分配浪费；创建失败回退 kmalloc，退出时完整释放；[实现](../kernel-6.18/k618_memory.py) |
| **有预算的系统哈希缓存保留** | [OnePlus fixed source](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8550/blob/49cec3dbd158aa0b7e8f1e681a0d933c9e5dd76e/drivers/md/dm-bufio.c) · [ACK dm-bufio](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/drivers/md/dm-bufio.c) | 可用内存超过 10% 且缓存未超预算时少回收；预算最多为总内存 2%，低内存时照常回收，可关闭；[实现](../kernel-6.18/k618_memory.py) |
| **调优设置试用保护** | [KernelSU module lifecycle](https://kernelsu.org/guide/module.html) · [AOSP boot reasons](https://source.android.com/docs/core/architecture/bootloader/boot-reason) | 修改后试用 120 秒；重启生效项从下次启动计时。仅确认同内核、窗口内 panic / watchdog 才回退，普通重启与内核切换保留设置；[实现](../module/nuwa-618/trial-guard.sh) |


以上链接用于核对公开源码、策略和上游机制；运行时参数与 6.18 适配在实现列单独说明。zram 自动策略是在上游机制上增加的调度逻辑。这些改动不构成本设备性能 / 功耗提升的实测证明。


**已有厂商能力：** 本项目已选用 `mi_reclaim` 的水位提升、`mi_mtmr` 的 VIP 页缓存、`perf_actuator` / `os_cpu_qi` 的 `/dev/xr_fbt`，以及 `mibbr` 的按应用 TCP 能力，均来自上表 madrid 官方 OTA，单项身份见[模块清单](../provenance/binary-modules.tsv)。不再叠加一套同类实现。

**TCP：** 已内建 [BBR](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/ipv4/tcp_bbr.c)、[CUBIC](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/ipv4/tcp_cubic.c) 及 [fq](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/sched/sch_fq.c) / [fq_codel](https://android.googlesource.com/kernel/common/+/ff4a6c3bb0f471a9dd186293a47b62c83829b97d/net/sched/sch_fq_codel.c) 队列。全局默认 **CUBIC**，并选用官方 `mibbr.ko` 的应用策略；内建 BBR 不表示所有连接都使用 BBR。

**选用原则：** 采用上表的上游机制启用、专用 slab、有预算缓存与设置试用保护。保留 TCP 全局 cubic 默认及已有 BBR / fq_codel / mibbr 能力；保持线性映射权限策略。额外常驻保护、直接回收 swappiness 上限和未在当前内核复现的 F2FS 补充修改暂不加入。

试用保护属于可选调优模块：需重启的设置从下次启动计时，能确认同内核、窗口内 panic / watchdog 时才恢复修改前的值；不代替 boot 镜像备份。

依赖就绪后进入 [BUILD：构建与安装](BUILD.zh-CN.md)。
