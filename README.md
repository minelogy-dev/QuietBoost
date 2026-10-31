# QuietBoost Native

[![Windows build and release](https://github.com/minelogy-dev/QuietBoost/actions/workflows/windows.yml/badge.svg)](https://github.com/minelogy-dev/QuietBoost/actions/workflows/windows.yml)

[下载 Release](https://github.com/minelogy-dev/QuietBoost/releases) · [构建与测试记录](https://github.com/minelogy-dev/QuietBoost/actions)

由 [minelogy-dev](https://github.com/minelogy-dev) 维护，OpenAI Codex 作为 AI 开发协作者参与实现、测试和文档整理。协作署名见 [AUTHORS.md](AUTHORS.md)。

Windows 10/11 x64 的原生音频增益工具。项目采用 C++20、Win32 和 WASAPI，使用 MSVC 静态运行库 `/MT`。发布物只有 **QuietBoost.exe**，不需要 .NET、NAudio、另行安装的 VC++ 运行库或脚本。

## 不使用终端进行开发

本项目提供 `QuietBoost.sln`，直接在 **Visual Studio Community 2022** 中打开。

1. 双击 **QuietBoost.sln**。
2. 顶部选择 **Release / x64**。
3. 菜单选择 **生成 → 生成解决方案**。
4. 程序生成在 **out/x64/Release/QuietBoost.exe**。部署时只复制这个 EXE；PDB 是可选调试符号。
5. 选择 **调试 → 开始执行（不调试）** 即可运行默认的 QuietBoost 项目。

Visual Studio 中的 **Git 更改** 窗口可以查看差异、提交、创建分支和推送。项目的提交作者使用 `minelogy-dev` 的 GitHub 隐私邮箱；由 Codex 协助完成的提交通过 `Co-authored-by` 记录 AI 协作。

解决方案还包含 **QuietBoostTests**。将它设为启动项目，再选择“开始执行（不调试）”，会显示图形化测试结果，并在输出目录保存 `test-results.txt`。也提供 `CMakeLists.txt` 和 `CMakePresets.json`，可使用 Visual Studio 的“打开文件夹”和 CMake 集成。

所需组件列于 `.vsconfig`：C++ 桌面开发工作负载、Windows SDK、MSVC v143 和 CMake 支持。VS 如果提示组件缺失，可直接在图形化安装器中补齐。

## 单 EXE 使用

在 Releases 页面下载 **QuietBoost.exe**；预览版会明确标记 Pre-release。同页提供 `SHA256SUMS.txt` 校验文件。主程序目前尚未签名，Windows 可能显示未知发布者提示。

直接运行 **QuietBoost.exe**。所有依赖的第三方虚拟驱动安装文件都嵌入资源中。

1. 首次没有虚拟设备时，点击 **安装虚拟设备**。
2. 工具将内置的 VB-CABLE 原始文件释放到自己的用户数据目录，逐一检查 SHA-256，并通过 Windows 验证官方安装程序签名。
3. Windows 会请求管理员授权。请在官方安装窗口点击 **Install Driver**，完成后按其要求重启电脑。
4. 再次启动工具，选用 **CABLE Input** 和实际扬声器或耳机。设备就绪时自动建立转发并切换普通播放的默认输出。

**单 EXE 指分发和运行主程序只需要一个文件。** 虚拟音频设备属于 Windows 驱动组件，首次使用仍须安装到系统；安装后的驱动文件由 Windows 管理。程序会生成配置、日志和异常恢复记录，这些是运行数据，不是部署依赖。

VB-CABLE 为 VB-Audio / Vincent Burel 提供的 donationware。程序内的 **VB-CABLE 许可 / 捐助** 按钮可打开其官网。专业用途请遵循其许可条件；本项目没有将第三方驱动声称为自研驱动。

## 音频行为

链路：应用 → 虚拟播放设备 → WASAPI 回环接收 → 增益与联动限幅 → 实际输出。

- 增益默认 **100%**，界面默认上限 **2000%**，表示振幅 20 倍、约 +26 dB。配置可提高上限，修改上限不自动提高当前增益。
- 不写入 Windows 的系统音量、应用音量或静音设置。普通播放默认设备在运行期间切换；通信默认设备保持原样。
- 在原始音频信号上检测有效电平，持续约五秒静音后平滑回到 100%。独立音频线程负责计时，界面确认框不会暂停保护。
- 复位后不会自动恢复此前高增益，需要用户重新调高。
- 关闭自动恢复时，需要第二层对话框并勾选说明。状态仅存在本次进程内，重新启动后默认开启，不写入配置。
- 限幅器始终开启；30ms 增益平滑和左右联动的采样峰值限幅用于控制过载。它不是真峰值限幅器，不能保证物理声压或无限增加响度。
- 虚拟设备支持 1～16 声道 PCM16/24/32 或 float32，按标准声道掩码混合为立体声。实际输出使用 WASAPI 共享模式及 Windows 格式转换。
- 缓冲有界，设备时钟漂移造成积压时优先丢弃过期音频，避免延迟无限增长。

关闭窗口会收起到托盘；点击 **正式退出** 才结束程序。退出时先恢复增益，再恢复设备选择，最后停止转发。运行期间用户手动切换设备后，工具保留新的选择。原设备断开时尝试其他实际输出设备。

## 悬浮窗与侧边条

启动时默认显示置顶的 **悬浮控制**，可直接调整增益、恢复 100% 或打开主窗口。

- 按住顶部“拖动移动”区域拖动；点击 **侧边条**，会收窄并贴到当前显示器最近的左侧或右侧。
- 侧边条可拖动到另一侧或另一块显示器，松开后重新贴边；竖向滑块向上增大增益，点击 **展开** 回到悬浮窗。
- 点击 **×** 只隐藏悬浮控制；通过主窗口的 **显示 / 隐藏悬浮窗** 或托盘右键菜单重新打开。
- 隐藏或最小化主窗口后，悬浮控制仍可使用。正式退出时一同关闭。
- 悬浮窗、侧边条与主窗口共用当前增益，静音自动恢复和快捷键复位会同步到所有界面。未开始音频转发时，增益滑块禁用。

位置、形态和显示状态保存在工具自己的 `%LOCALAPPDATA%/QuietBoostNative/window.ini`。显示器断开或工作区变化后会将窗口移回可见区域。此文件只保存窗口布局，当前增益和关闭自动恢复的状态仍不持久化。

同一个 EXE 以隐藏守护进程模式运行，在主进程异常终止后尝试恢复。主程序和守护进程同时被终止、断电等情况不能保证即时恢复，下次启动会再次读取恢复记录。某些绑定固定设备的播放器不能跟随默认设备切换，需在播放器内改为系统默认或重新开始播放。

## 配置

点击程序中的 **配置** 按钮即可用记事本打开配置，无需终端。修改后正式退出并重新启动生效。文件位置：

`%LOCALAPPDATA%/QuietBoostNative/config.ini`

```ini
[QuietBoost]
MaxGainPercent=2000
SilenceTimeoutMs=5000
SilenceThresholdDb=-90
BufferMs=60
AutoStart=1
VirtualDeviceId=
OutputDeviceId=
```

`MaxGainPercent` 允许 100～1000000；`SilenceTimeoutMs` 允许 1000～600000；`SilenceThresholdDb` 允许 -120～-30 dBFS；`BufferMs` 允许 20～500 毫秒。缓冲参数不等于端到端延迟。

关闭自动恢复和当前增益均没有配置项。`recovery.ini` 只保存设备恢复信息；恢复成功后删除。

## 工程结构

- `src/core.hpp`：增益状态机、DSP、缓冲和可测试的设备恢复逻辑。
- `src/audio.cpp`：原生 WASAPI 接收、输出和音频线程。
- `src/platform.cpp`：设备枚举与切换、原子配置写入、守护进程和恢复记录。
- `src/app.cpp`：原生 Win32 界面与托盘，不通过终端实现产品操作。
- `src/floating.cpp`：可拖动的置顶悬浮窗与左右贴边侧边条。
- `src/installer.cpp`：资源释放、完整性和签名检查、图形驱动安装入口。
- `resources/`：界面资源、版本信息、应用 manifest 和驱动资源映射。
- `third_party/vbcable/`：原始官方驱动文件，逐文件校验值见 `src/driver_resources.hpp`。
- `tests/core_tests.cpp`：可从 Visual Studio 直接运行的图形测试程序。

“静态”指程序自身 C/C++ 运行库静态链接；Win32、COM、WASAPI 等功能仍调用 Windows 自带的系统 DLL。程序没有自研、测试签名或绕过签名检查的驱动。

## GitHub Actions 与发布（无需终端）

推送 main 或创建 Pull Request 后，Actions 会在 Windows Server 2022 / Visual Studio 2022 上构建 Debug 和 Release，运行核心检查。Release 还会在空目录独立运行驱动验证命令，检查内置文件哈希、官方安装程序签名及 EXE 的系统 DLL 依赖。CI 不安装驱动、不接入音频、不切换输出设备。

- 手动构建：仓库 **Actions → Windows build and release → Run workflow**。
- 下载构建：打开成功运行记录，在 **Artifacts** 中下载 `QuietBoost-windows-x64`；ZIP 中只有 EXE 与校验文件。测试和签名验证报告单独保留。
- 发布版本：先更新 `CMakeLists.txt`、`resources/app.rc`、`resources/app.manifest` 中的版本，以及 `docs/RELEASE_NOTES.md`。在 GitHub Desktop 提交并推送；打开 **History**，右键目标提交选择 **Create Tag**，填写 `v2.1.0-preview.1` 这类版本，再点击 **Push origin** 推送标签。

`v*` 标签会触发全部检查；Debug 和 Release 均通过后才创建 GitHub Release，附带原始 `QuietBoost.exe` 与 `SHA256SUMS.txt`。含 `-preview.1` 等后缀的标签自动标记为预览版；版本主体必须与 CMake 版本一致。发布只使用该次任务的临时 `GITHUB_TOKEN`，无需个人访问令牌。已发布的版本不会被工作流覆盖。

## 第三方来源

VB-CABLE 属于独立的第三方组件，保留其原作者、原始文件及许可说明；项目维护者和 AI 协作者署名不改变这些信息。主项目尚未指定开源许可证。

- [VB-CABLE 官方下载与许可入口](https://vb-audio.com/Cable/)
- [VB-Audio 分发说明](https://vb-audio.com/Services/licensing.htm)
- 原始包：`https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip`
- 原始包 SHA-256：`B950E39F01AF1D04EA623C8F6D8EB9B6EA5C477C637295FABF20631C85116BFB`
- 原始许可文本保留在 `third_party/vbcable/readme.txt`，安装时一并释放。

不需要为主程序安装 WDK；工程使用已签名的第三方虚拟驱动，不重新编译内核驱动。详细验证结果见 `VALIDATION.md`。
