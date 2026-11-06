# 小米 13 Pro · Linux 6.18

**将 Android 17 / ACK 6.18.32 与小米 13 Pro 的 SM8550 硬件、vendor 接口和配套模块整合成可从上游重建的内核项目。**

[English](README.md)

[下载内核包](https://github.com/JiaqiLi404/android_kernel_618_for_sm8550/releases/tag/v6.18.32-nuwa.5) · 预发布，真机验证待完成。

目标设备：**小米 13 Pro / nuwa**（主要目标；38 0c 0a 与 42 02 0b 两种面板都已处理）。已包含 **小米 13 / fuxi** 与 **小米 13 Ultra / ishtar** 的驱动适配（触摸，13 按其原厂的指纹路径规则；屏幕面板调校与 DC 调光、peak HDR、相机 PHY、OIS 与镜头归位、PM8008 相机电源、ToF 测距、充电节点）；这些适配和 42 02 0b 面板都尚未在相应机器上运行过。配套环境：**NikoHyper / HyperOS 4，OS4.0.24.0.XEOCNXM**。其他 ROM / vendor 组合需单独适配。安装前请先看[使用须知](docs/BUILD.zh-CN.md#使用须知)：root 管理器与开机黑屏。

## 我们做了什么

| 工作 | 对使用者的价值 | 来源与实现 |
|:--|:--|:--|
| **6.18 内核移植到 SM8550** | 将较新内核与设备需要的时钟、UFS、USB、互联和电源驱动接起来 | 一加驱动基础 + MiCode 设备定义；[驱动来源](docs/PROVENANCE.zh-CN.md#drivers) |
| **补齐设备与多媒体支持** | 适配显示 / 合成器、相机、视频、DSP、触摸 / 手势及音频接口，含 13 / 13 Ultra 的差异 | 组合 SM8850、SM8550 和小米公开源码，处理 6.18 API / ABI 差异；[来源与适配](docs/PROVENANCE.zh-CN.md#drivers) |
| **匹配内核、模块和引导镜像** | 统一生成模块 payload（406 个模块）、模块层 ramdisk、boot.img 与 AnyKernel3 安装包 | 官方 OTA 中的 54 个小米模块 + 源码重编模块；[固件来源](docs/PROVENANCE.zh-CN.md#firmware) |
| **内存、调度与功耗改动** | 提供回收、LZ4 1.10.1 / ZSTD 分层压缩、调频器、能耗感知选核、厂商提频限制、空闲路径与唤醒控制 | 逐项列出上游来源和本项目适配；[29 项改动](docs/PROVENANCE.zh-CN.md#improvements) |
| **可重建与可检查** | 固定依赖版本、自动获取输入、检查 ABI 和安装包；WSL 构建限制内存为 12 GiB | [依赖获取](docs/PROVENANCE.zh-CN.md#acquire) → [构建](docs/BUILD.zh-CN.md) → [验证](docs/VALIDATION.zh-CN.md) |

项目的核心是将这些来源组合并适配到同一套设备接口与模块 ABI。性能、功耗及全部硬件功能仍需在目标手机验证，详见[验证范围](docs/VALIDATION.zh-CN.md#scope)。

## 关键改进的原理

**沿用设备自己的启动盘。** 内核只内置本项目的模块层（`/lib/modules`、加载清单和批量加载器），并在 bootloader 交来的 ramdisk 之后解包。设备自己的 vendor_boot / init_boot ramdisk，或 recovery / TWRP 的 ramdisk 照常运行，只有同名模块文件和清单被替换。[来源与实现](docs/PROVENANCE.zh-CN.md#drivers)

**LZ4：上游 1.10.1。** 用上游 C 实现更新内核原有的 1.8.3 解压代码，覆盖 EROFS 文件读取与 zram 等调用路径。arm64 的 64 字节超前拷贝会超过 EROFS 原地解压预留的余量，这里保留 32 字节路径，并处理内核接口与模块 ABI 兼容。[来源与实现](docs/PROVENANCE.zh-CN.md#improvements)

**有待执行任务，就跳过本次空闲。** 核选好空闲状态后，在关中断的记账期间仍可能有任务被其他核唤醒。浅空闲和电源域空闲两条入口都在 CPU PM / PSCI 调用前重查调度，减少刚进入低功耗状态又被唤回的开销；保留 s2idle 原有语义。[来源与实现](docs/PROVENANCE.zh-CN.md#improvements)

**调频与选核。** vendor 开机脚本给每个 CPU 策略写入的 "walt" 选中 schedutilmium——本项目内置于内核的调频器：升频、降频分开限速，用 CPU 实际忙闲代替调度器的估计，频率上限随负载走，触摸后短暂保底。选核仍由内核自己的能耗感知调度（EAS）按设备树的能量表完成，和高通内核一样；`k618_place` 围绕画帧线程加了四条可单独开关的规则，另有可选的实测能量表。调优模块让 vendor 性能 HAL 在触摸和大合成时不再抬高中核、超大核的最低频率，并关闭 MIUI 的 SchedBoostService。[来源与实现](docs/PROVENANCE.zh-CN.md#improvements)

其他改进包括回收时顺带检查邻页、kswapd 放中核、kcompressd，以及可延迟 KFENCE、lazy RCU、紧凑的冷页候选表、有预算的哈希缓存和调优设置试用保护；tmpfs 扩展属性用于兼容性。来源、默认值与取舍见[全部 28 项改动](docs/PROVENANCE.zh-CN.md#improvements)。

## 按顺序使用

| 步骤 | 文档 | 完成后得到 |
|:--|:--|:--|
| 1 | [PROVENANCE：来源与依赖获取](docs/PROVENANCE.zh-CN.md) | 全部固定源码、工具链、固件分区和所需文件 |
| 2 | [BUILD：构建与安装](docs/BUILD.zh-CN.md) | boot.img、AnyKernel3 ZIP、可选 KernelSU 调优模块 |
| 3 | [VALIDATION：验证](docs/VALIDATION.zh-CN.md) | 输入、ABI、打包与可选安装器模拟结果 |

首次使用先完成依赖文档中的宿主准备，然后获取源码并构建：

```bash
git clone https://github.com/JiaqiLi404/android_kernel_618_for_sm8550.git xiaomi13-k618
cd xiaomi13-k618
bash scripts/run-limited.sh bootstrap
bash scripts/run-limited.sh build
bash scripts/run-limited.sh verify
```

## 目录用途

| 类别 | 目录 / 文件 |
|:--|:--|
| **首次上传 GitHub** | 本页、`docs/`、`kernel-6.18/`、`module/`、`patches/`、`profiles/`、`provenance/`、`scripts/`、`tools/`、`assets/SHA256SUMS` 和根目录 Git 配置文件 |
| **下载的依赖** | Linux `BUILD_ROOT` 内的源码、工具链和固件；`assets/*.tar.zst` / `*.deb` 是可选缓存，不上传 Git |
| **中间产物与日志** | `BUILD_ROOT` 内的编译 / 暂存目录，以及 `logs/`；已排除 Git |
| **最终输出** | `dist/release/`：boot、安装包、调优模块、验证报告及校验和；按需发布到 Release |
