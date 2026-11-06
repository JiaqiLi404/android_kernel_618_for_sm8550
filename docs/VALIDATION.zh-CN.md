# 验证

[English](VALIDATION.en.md) · 上一步：[构建与安装](BUILD.zh-CN.md) · [首页](../README.zh-CN.md)

## 1 · 复核自己的构建

保留完整构建工作区；从项目根目录运行，使用构建时相同的 `BUILD_ROOT` / `TAG`：

```bash
bash scripts/run-limited.sh verify
(cd dist/release && sha256sum -c SHA256SUMS)
```

预期看到 **`BUILD AND PACKAGE VALIDATION PASSED`**，哈希检查全部为 `OK`。自定义 TAG 时，把 `dist/release` 换成相应输出目录。

可选安装器模拟需要 root、loop / mount 和 binfmt_misc 支持，使用临时镜像：

```bash
. scripts/project-env.sh
sudo env BUILD_ROOT="$BUILD_ROOT" TAG="${TAG:-release}" \
  bash scripts/run-limited.sh test-installer
```

预期看到 **`INSTALLER EMULATION PASSED`**。该入口使用固定 QEMU、独立 PID / mount 命名空间，结束后清理临时处理器和 loop 绑定。结果位于 `logs/<tag>/validation.log`、`installer-test.log` 及 `dist/<tag>/{validation,installer-validation}.json`。

可选内核运行检查使用 QEMU virt（四核、2 GiB 客体内存）与构建出的基础 GKI；不模拟手机硬件：

```bash
sudo apt-get install -y qemu-system-arm
bash scripts/run-limited.sh test-runtime
bash scripts/run-limited.sh verify
```

预期看到 **`RUNTIME VALIDATION PASSED`**。结果见 `logs/<tag>/runtime-test.log` 与 `dist/<tag>/runtime-validation.json`；再运行 `verify` 会把已有报告一并纳入 `SHA256SUMS`。

## 2 · 本项目已验证的范围

下表记录最近一次完整运行（bootstrap、构建、`verify`、QEMU 运行测试）的结果。标 † 的一行当前源码没有重跑：这台宿主机的 32 位 ARM 用户态模拟器在安装脚本第一条命令前就卡住。

| 检查 | 结果 |
|:--|:--|
| 独立上游输入重建 | **通过**：15 个仓库、2 个 vendor 引用、6 个分区、54 个模块哈希（[记录](../provenance/input-validation.json)） |
| 源码编译 | **通过**：GKI、249 / 249 SoC 模块、30 / 30 Techpack 组，以及 RPC / EVA、触摸 / THP、CS35L41、ToF 模块 |
| 最终模块与 ABI | **406 个模块、21,230 项内核版本化导入；全部 23,310 项版本化导入与提供该符号的模块或内核逐项核对**；无 CRC 不匹配或 depmod 未解析错误 |
| 内嵌 initramfs 内核 | 18,008 个 vmlinux 导出 CRC 与基础 GKI 一致；内置层与 boot ramdisk 相同（418 项，只有模块层），INITRAMFS_FORCE 关闭 |
| boot / AnyKernel3 一致性 | 内核、压缩 ramdisk、17 项启动参数及顺序一致；ZIP 和必需清单检查通过 |
| 安装器模拟 † | 上一版源码：Horizon / A 槽、recovery / B 槽均通过，另一槽不变。本次未重跑（见上）；安装脚本除启动参数列表外没有改动，打包时会逐项核对该列表与镜像一致 |
| QEMU 内核运行 | tmpfs 属性、可延迟 KFENCE、lazy RCU、8 KiB 跟踪设置通过；1,024 页 zram 数据经两轮重压缩完整读回；24 字节候选缓存随卸载释放；回收开关为默认值；写 "walt" 选中带内置默认值的 schedutilmium |
| TCP 配置 | BBR / CUBIC、fq / fq_codel 内建；默认 CUBIC；官方 mibbr 已在模块加载清单 |
| 设置恢复逻辑 | dash、bash、ARM BusyBox 检查覆盖试用到期、panic 回退、重启生效项，以及普通重启 / 切换内核保留设置 |
| 资源限制 | 实际 cgroup 值确认 12 / 10 / 1 GiB、四核配额 |

`verify` 会把启动盘里每个模块的每项版本化导入，与提供该符号的模块或内核逐项核对（[k618_stage_crc.py](../kernel-6.18/k618_stage_crc.py)；生成镜像时也会运行，有一项不匹配就不出镜像），并检查 `mi_power` 与 `k618_place` 在批量加载清单中；QEMU 运行测试增加回收开关（`look_around`、`kswapd_mid`、`reclaim_cpus`、`kcompressd`）和内核测试用 cpufreq 驱动上的 schedutilmium（"walt" 选中它，且带内置默认值）。

验证环境：Ubuntu 26.04.1 x86_64 / WSL2，固定 Clang r584948c 与 Rust 1.91.1.p3。内核为 `6.18.32-4k-gff4a6c3bb0f4-dirty`；`-dirty` 表示已应用移植改动。vendor 源码仍有兼容性警告；第一阶段未匹配条目见 `dist/<tag>/first-stage-missing.txt`。

<a id="device"></a>

## 3 · 真机检查

按 [BUILD](BUILD.zh-CN.md#安装到手机)引导后检查：

```bash
adb shell uname -a
adb shell cat /sys/block/zram0/comp_algorithm
adb shell cat /sys/block/zram0/recomp_algorithm
adb shell cat /sys/block/zram0/disksize
adb shell cat /proc/sys/net/ipv4/tcp_available_congestion_control
adb shell cat /proc/sys/net/ipv4/tcp_congestion_control
adb shell cat /sys/module/k618_probe/parameters/in_recovery
adb shell cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
adb shell cat /proc/sys/kernel/sched_energy_aware
adb shell cat /sys/module/k618_place/parameters/state
adb shell cat /proc/sys/vm/look_around /proc/sys/vm/kswapd_mid /proc/sys/vm/kcompressd
adb shell cat /sys/class/power_debug/temp_aware
adb shell cat /sys/hwconf/hw_mon_info_no_clear
adb shell su -c 'dmesg | grep -E "fingerprint rules|ufscld"'
```

默认预期为 **6.18.32、LZ4 主压缩、ZSTD 重压缩、16 GiB zram**，`in_recovery` 为 `N`；进入 TWRP 等 recovery 时为 `Y`，且 recovery 自己的界面能启动。开机完成后每个策略的调频器都是 `schedutilmium`，`sched_energy_aware` 为 1，选核状态显示 `em measured` 和 3 个能量域，三个回收开关分别为 1、1、2，`temp_aware` 为 10；内核日志写明触摸规则（13 上为 `m3`，其他机型为 `m2`），KIOXIA UFS 上还有 ufscld 的记录。还需检查触摸 / 手势、显示、指纹退出 AOD、通话 / 移动数据、Wi-Fi、音频、各摄像头、指纹 / 人脸，以及拔线熄屏后的系统挂起。42 02 0b 面板还需检查低亮度和息屏时的指纹解锁。每台手机都要检查熄屏指纹解锁第一次就能识别，13 还要检查亮屏和熄屏的指纹解锁；手边有无线充电座时，检查无线充电和反向充电。

<a id="scope"></a>

源码编译、静态检查、QEMU 内核检查与合成分区上的安装器模拟已通过；真实硬件模式、ROM 兼容、温控、待机及功耗仍需真机验证；13 / 13 Ultra 的适配、13 的触摸规则和 42 02 0b 面板的支持尚未在相应机器上运行。调频器、选核规则和回收改动只在 QEMU 中检查过，对功耗与流畅度的影响需要在手机上做 A/B 对比（调优模块里每项都有开关）。模拟使用的 128 MiB 测试镜像不代表手机实际分区容量。
