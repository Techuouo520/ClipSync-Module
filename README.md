中文 | [English](docs/en/README.md)

# ClipSync Module

面向 Android ARM64 设备的局域网剪贴板同步模块，支持 KernelSU 和 Magisk 28+。当前版本 `0.1`。

- 支持纯文本、多行内容和 Emoji。
- TCP 同步，支持写入确认与自动重连。
- WebUI 提供设备扫描、手动添加 IP 和暂停/恢复。

## 安装

1. 两台设备安装相同版本的模块 ZIP，并重启。
2. 将两台设备连接到同一 Wi-Fi。
3. 在 KernelSU 打开模块 WebUI，点击扫描；扫描失败时使用 **添加 IP**。
4. 显示已连接后，在一台复制，到另一台粘贴。

请在受信任的局域网使用。模块操作按钮也可以切换同步状态。

## 构建

使用 CMake 和 Ninja 构建 Native 守护进程，`ANDROID_NDK_HOME` 指向 Android NDK 目录。

```bash
git clone https://github.com/Techuouo520/ClipSync-Module.git
cd ClipSync-Module
cmake --preset release
cmake --build --preset release
```

产物为 `build/android/clipsyncd`。

| 预设 | 说明 |
|---|---|
| `debug` | 生成调试符号并启用运行日志 |
| `release` | 优化二进制并剥离符号 |

`scripts/build.ps1` 可生成完整模块包 `build/clipsync-magisk.zip`。已发布的安装包见 [Releases](https://github.com/Techuouo520/ClipSync-Module/releases)。

## 许可证

Copyright (C) 2026 Techuouo520。[AGPL-3.0-only](LICENSE)。

## 哔哩哔哩与赞赏

[Tech悠悠](https://space.bilibili.com/1934537611)

![赞赏码](docs/assets/sponsor-qr.jpg)

[USDT-TRC20](https://tronscan.org/#/address/TRKeWXWc1GL7ARE1sX5rBDcYg9yStFsmnU) · [USDT-Solana](https://solscan.io/account/BrKPdDdF8NY8k1f4A7RfN1inSDsxZ2d1atDHvGLjp23G)
