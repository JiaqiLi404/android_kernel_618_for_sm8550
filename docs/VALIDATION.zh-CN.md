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

| 检查 | 结果 |
|:--|:--|
| 独立上游输入重建 | **通过**：14 个仓库、2 个 vendor 引用、6 个分区、54 个模块哈希；[记录](../provenance/input-validation.json) |
| 源码编译 | **通过**：GKI、247 / 247 SoC 模块、30 / 30 Techpack 组，以及 RPC / EVA、触摸 / THP、CS35L41 |
| 最终模块与 ABI | **401 个模块、20,969 项内核版本化导入**；无 CRC 不匹配或 depmod 未解析错误 |
| 内嵌 initramfs 内核 | 18,008 个 vmlinux 导出 CRC 与基础 GKI 一致 |
| boot / AnyKernel3 一致性 | 内核、压缩 ramdisk、17 项启动参数及顺序一致；ZIP 和必需清单检查通过 |
| 安装器模拟 | Horizon / A 槽、recovery / B 槽均通过；另一槽不变 |
| QEMU 内核运行 | tmpfs 属性、可延迟 KFENCE、lazy RCU、8 KiB 跟踪设置通过；1,024 页 zram 数据经两轮重压缩完整读回；24 字节候选缓存随卸载释放 |
| TCP 配置 | BBR / CUBIC、fq / fq_codel 内建；默认 CUBIC；官方 mibbr 已在模块加载清单 |
| 设置恢复逻辑 | dash、bash、ARM BusyBox 检查覆盖试用到期、panic 回退、重启生效项，以及普通重启 / 切换内核保留设置 |
| 资源限制 | 实际 cgroup 值确认 12 / 10 / 1 GiB、四核配额 |

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
```

默认预期为 **6.18.32、LZ4 主压缩、ZSTD 重压缩、16 GiB zram**。还需检查触摸 / 手势、显示、通话 / 移动数据、Wi-Fi、音频、各摄像头、指纹 / 人脸，以及拔线熄屏后的系统挂起。

<a id="scope"></a>

源码编译、静态检查、QEMU 内核检查与合成分区上的安装器模拟已通过；真实硬件模式、ROM 兼容、温控、待机及功耗仍需真机验证。模拟使用的 128 MiB 测试镜像不代表手机实际分区容量。
