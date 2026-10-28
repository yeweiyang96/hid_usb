# Windows 本地隔离构建环境

工具安装在本仓库 `.tools/`，不会修改系统 PATH、用户环境变量、VS Code 或其他项目。下载包保留在 `.tools/downloads/`，已加入 Git 忽略。构建脚本只临时修改自己的进程环境，退出时恢复。脚本不会连接、烧录或重启 Pico。

## 首次安装

需要 Windows x64、Windows PowerShell 5.1 或更新版本、Git for Windows，以及 Python 3.9 以上。本机使用 `C:\Python315\python.exe`（Python 3.15）。主机无需全局 CMake、Ninja 或 Arm GCC；pioasm 和 picotool 使用 Raspberry Pi 官方预编译版本，因此本构建不需要编译主机端工具。

在仓库目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup-toolchain.ps1
```

脚本先校验下载包 SHA256，再解包；SDK 固定到完整提交，并将全部子模块固定到该 SDK 记录的提交。网络中断后可再次运行；已完成的下载和安装会复用。若解包中断，脚本会报告具体的未完成工具目录，需要先将该目录移开再重试，不会递归删除未知目录。

版本与官方来源锁在 [`scripts/toolchain.lock.json`](../scripts/toolchain.lock.json)：

| 组件 | 固定版本 | 官方来源 |
| --- | --- | --- |
| Pico SDK | 2.3.1，`079c6f39023649b154152db30f1d781e884879bc` | [Raspberry Pi SDK](https://github.com/raspberrypi/pico-sdk/releases/tag/2.3.1) |
| Arm GNU GCC | 15.2.Rel1，Windows x64 arm-none-eabi | [Arm 官方 SHA256](https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-arm-none-eabi.zip.sha256asc) |
| CMake | 4.3.4，Windows x64 | [Kitware 发布](https://github.com/Kitware/CMake/releases/tag/v4.3.4) |
| Ninja | 1.13.2，Windows x64 | [Ninja 发布](https://github.com/ninja-build/ninja/releases/tag/v1.13.2) |
| pioasm、picotool | 2.3.1 | [Raspberry Pi 工具发布](https://github.com/raspberrypi/pico-sdk-tools/releases/tag/v2.3.1-0) |

Arm 归档摘要来自官方 `.sha256asc` 文件，其余二进制归档摘要来自各官方 GitHub Release API 的 `digest` 字段。未替换工具原有许可证文件。`.tools/sdk-submodules.lock.txt` 保存实际安装的子模块清单。

## 无凭据编译验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -CompileOnly -PythonExecutable C:\Python315\python.exe
```

`-CompileOnly` 明确设置 `HID_COMPILE_ONLY=ON`，即便本地已有 `wifi_config.h`，也不会把其中凭据编入此模式。产物只放 `build-compile-only/`，同时写出 `NOT_FOR_DEPLOYMENT.txt`。该模式验证交叉编译、链接和 UF2 生成，**不能连接实际 Wi-Fi，不得作为可部署固件刷入**。

## 使用本地 Wi-Fi 配置构建

```powershell
Copy-Item wifi_config.h.example wifi_config.h
# 用本机编辑器填写 WIFI_SSID 和 WIFI_PASSWORD。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -PythonExecutable C:\Python315\python.exe
```

不要将密码发送到聊天或写在命令行。`wifi_config.h` 已忽略，脚本只检查本文件，不读取其他位置的凭据，也不会输出凭据内容。默认构建拒绝缺失、空白和 `CHANGE_ME` 配置；产物放在 `build-verified/`。该目录与无凭据模式完全分离，避免误取验证固件。

每次成功构建都会输出 UF2 的 SHA256，并在构建目录保存 `build-manifest.json`：包含构建模式、SDK 完整提交、工具锁文件摘要、Python 和 GCC 版本、UF2 摘要。含实际凭据的产物仍属于私密本地文件，不应提交到仓库。编译成功仅证明源码通过指定工具链，不代表板上无线、USB 或游戏动作已经验收。

## 故障排查

- 工具不存在：先运行 `setup-toolchain.ps1`；无需手动修改系统 PATH。
- 缓存归档校验失败：检查错误中指定的下载文件，移开后重试；脚本不执行该归档。
- SDK 提交不符：为避免覆盖本地更改，脚本会中止。先移开 `.tools/pico-sdk`，再运行 setup。
- Python 未找到：传 `-PythonExecutable` 的完整路径。
- 构建失败：保留完整输出和构建目录 `CMakeCache.txt`。不要为排错上传 `wifi_config.h` 或含凭据的固件。
- 需要控制并行度：使用 `-Jobs 4`，默认 8。

SDK 与编译工具升级时应同时更新锁文件并重新验证；不自动跟随 latest。脚本不删除既有构建目录，避免误删；切换工具链时应自行选择新的干净构建目录或将旧目录移开。
