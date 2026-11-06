# 构建与安装

[English](BUILD.en.md) · 上一步：[来源与依赖](PROVENANCE.zh-CN.md) · 下一步：[验证](VALIDATION.zh-CN.md)

## 1 · 生成内核与安装包

先完成 [PROVENANCE 的宿主准备和 bootstrap](PROVENANCE.zh-CN.md#acquire)。以下命令均在项目根目录的 Linux shell 中运行；若自定义过 `BUILD_ROOT`，保持同一变量。

```bash
bash scripts/run-limited.sh build
```

入口依次编译 GKI、SoC、Techpack 和设备专用模块，生成匹配 ramdisk / boot.img / ZIP，并自动执行静态校验。全程强制 **12 GiB 内存上限、10 GiB 高水位、1 GiB 交换上限、四核 CPU**，默认四任务；需要时用 `JOBS=2 bash scripts/run-limited.sh build` 降低并发。

| 输出 | 用途 |
|:--|:--|
| `dist/release/boot.img` | 内置模块层的内核，ramdisk 为同一模块层，可用于 fastboot |
| `dist/release/nuwa-kernel-6.18-AnyKernel3.zip` | 兼容刷写工具 / recovery 的内核安装包 |
| `dist/release/nuwa-618-v5.zip` | 可选 KernelSU 调优模块，控制内核设置 |
| `dist/release/validation.json`、`SHA256SUMS` | 校验结果与产物哈希 |
| `logs/release/build.log` | 构建日志；失败时从首个错误排查 |

看到 `BUILD AND PACKAGE VALIDATION PASSED` 后，按 [VALIDATION](VALIDATION.zh-CN.md)复核，再进行安装。构建使用已经准备好的输入，不再联网。`TAG=my-build` 可更换输出目录名；后续验证也使用相同 TAG。

## 2 · 核对手机并备份

主要目标为 **小米 13 Pro / nuwa** 和首页指定的配套 ROM。13 / fuxi 与 13 Ultra / ishtar 的适配尚未在这两台手机上运行，测试前在它们上面只使用临时引导。手机需解锁 bootloader；宿主安装 Android platform-tools，手机开启 USB 调试。AnyKernel3 的设备检查已关闭，安装前自行核对：

```bash
adb shell getprop ro.product.device
adb shell getprop ro.build.version.incremental
adb shell getprop ro.boot.slot_suffix
```

记录槽位，保存当前可用 ROM 的 boot 镜像。已有 root 时可读取活动槽：

```bash
mkdir -p backup
adb exec-out 'su -c "cat /dev/block/by-name/boot$(getprop ro.boot.slot_suffix)"' \
  > backup/current-boot.img
python3 -c 'assert open("backup/current-boot.img", "rb").read(8) == b"ANDROID!"'
sha256sum backup/current-boot.img > backup/current-boot.img.sha256
```

没有 root 时，从当前安装的同一份 ROM 包取得匹配 boot。确认能进入 fastboot，并先停用面向其他内核 ABI 的调优模块。

<a id="安装到手机"></a>

## 3 · 临时引导与安装

设备支持临时引导时，先运行：

```bash
adb reboot bootloader
fastboot getvar product
fastboot getvar current-slot
fastboot boot dist/release/boot.img
```

临时引导不刷写 boot；系统仍会写入用户数据，此内核的诊断日志器也会写 rawdump。按 [真机检查清单](VALIDATION.zh-CN.md#device)验证后，选择一种永久安装方法：

| 方法 | 操作 |
|:--|:--|
| Horizon Kernel Flasher / 兼容 recovery | 安装 `dist/release/nuwa-kernel-6.18-AnyKernel3.zip` |
| Bootloader fastboot | 确认当前槽位后，执行 `fastboot flash boot dist/release/boot.img`，再 `fastboot reboot` |

内核、ramdisk 与模块必须配套；保留 AnyKernel3 ZIP 中的替换 ramdisk。vendor_boot / init_boot 的 ramdisk，或 recovery（例如 TWRP）的 ramdisk 仍会随这个内核运行；只有模块文件和清单来自模块层。可选 `nuwa-618-v5.zip` 在 KernelSU 模块界面单独安装；其“调度”页可逐项开关调频器、选核规则和回收改动（关闭即恢复 6.18 原版行为）。

<a id="使用须知"></a>

### 使用须知

| 项目 | 说明 |
|:--|:--|
| root 管理器 | 内核内置 KernelSU v3.3.1（版本号 32680）与 SUSFS v2.3.0。内置的 KernelSU 只认官方 KernelSU 管理器的签名：KernelSU 分支版本的管理器会显示“未安装”，并提示用 LKM 方式安装。LKM 方式不适用于本内核：KernelSU 已编进内核，LKM 模块是给不带 KernelSU 的内核用的。请使用官方管理器；以前用 LKM 修补过 init_boot 的，最好刷回原厂 init_boot（不刷也能开机，只是每次开机加载旧模块都会失败）。 |
| bootloader 徽标后黑屏 | 正常现象：从 bootloader 徽标到开机动画之间屏幕是黑的。启动参数带 `msm_drm.k618_cont_splash=0`，内核不接管 bootloader 点亮的屏幕，而是重新给面板上电、画出自己的第一帧。这样 `fastboot boot` 也能正常显示：那种启动方式下 bootloader 不留下亮着的屏幕可供接管。 |
| 小米 13 的触摸 | 按触摸固件名（`st_fts_m3`）自动采用 13 原厂在指纹路径上的规则；`dmesg \| grep "fingerprint rules"` 可看到当前手机用的规则。 |

## 4 · 回退与排错

回到 bootloader，确认恢复到备份对应的同一槽位：

```bash
fastboot getvar current-slot
fastboot flash boot backup/current-boot.img
fastboot reboot
```

| 问题 | 处理 |
|:--|:--|
| 下载 / 哈希校验失败 | 保持同一工作区重跑 `bootstrap`；完整 OTA 备用方式见[依赖获取](PROVENANCE.zh-CN.md#firmware) |
| 工作区非空且无项目标记 | 选择新的空 `BUILD_ROOT` |
| cgroup 检查失败 | 启用 systemd / cgroup v2，继续使用受限入口 |
| 编译错误 / 达到内存上限 | 看 `logs/<tag>/build.log` 首个错误；内存不足时保持硬上限并改用 `JOBS=2` |
| 手机无法启动 | 恢复同槽位的匹配备份 boot |
