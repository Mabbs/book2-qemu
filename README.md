# Book2 QEMU 模拟器

在 QEMU 中直接运行 BBK Book 2 学习机的原始固件，包括 Boot0、tinyos、ExtOS 以及 A/B/C/D 各卷中的系统资源和应用。模拟器只提供硬件，不修改固件代码、客机内存或输入事件。

## 主要功能

- **原固件启动**：从 SD 卡镜像加载 Boot0、tinyos 与 ExtOS，显示 800×480 屏幕。
- **完整桌面**：小学首页、中学首页、更多功能、分类导航，以及资源管理器、课程表、系统设置、计算器等应用。
- **鼠标与键盘**：USB HID 鼠标；矩阵键盘支持字母、数字和常用按键。鼠标移入 QEMU 窗口即可输入。
- **声音**：系统提示音和固件自带 MP3 由固件自身解码，经 DMA/AIC 输出；默认使用 Windows DirectSound，也可选 WAV 文件或静音。
- **持久化时间**：RTC 寄存器保存在状态文件中，重启后继续计时，不需要每次重设时间。
- **镜像构建器**：从原始资源离线生成带 A 三副本、B/C/D 卷和完整固件副本的单一镜像，并逐文件校验。
- **运行包打包**：将可执行文件、依赖库、GTK 资源和说明打包为可整体复制的目录。

## 快速开始

从源码构建后直接启动：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\start.ps1 -SystemImage <镜像路径>
```

`build.ps1` 在当前仓库内配置并编译 MSYS2 UCRT64 的 `mipsel-softmmu` 目标，输出 `build\qemu-system-mipsel.exe`。`start.ps1` 会自动找到它。

也可准备好 `dist` 运行包后双击 `start.cmd`。运行包包含 QEMU、DLL、GTK 资源和 `images/book2.img`，不需要另外安装 MSYS2。

首次启动会出现固件自带的时间窗口：设置时间后点击“确定”，之后进入桌面。再次启动时 RTC 状态从文件恢复，直接进入桌面。

## 发布包

CI 生成的三个运行包都是自包含的，目标机器不需要安装 MSYS2、Homebrew 或 GTK：

- **Windows**：解压 `book2-qemu-windows-x86_64.zip`，把系统镜像放到 `dist-book2\images\book2.img`，双击 `start.cmd`。
- **Linux**：解压 `book2-qemu-linux-x86_64.tar.gz`，执行 `./dist-book2/run.sh -M book2,msc0-image=<镜像>`。构建基线为 Ubuntu 22.04（glibc 2.35），更新的发行版可直接运行。
- **macOS**：解压 `book2-qemu-macos-arm64.tar.gz`，执行 `./dist-book2/run.sh ...`。包为 arm64；未签名，首次运行若被 Gatekeeper 拦截，执行 `xattr -dr com.apple.quarantine dist-book2`。

运行包仍使用宿主机的显示服务（X11/Wayland 或 Quartz）、字体和内核，但不再依赖宿主机安装的 GTK/GLib 库。

## 操作

- 桌面底部按钮切换到中学界面；中学页面点击“更多功能”进入分类页。
- 图标需要先单击选中，再双击打开。
- 应用窗口或弹窗需要分别关闭；课程表等应用可能再打开编辑弹窗。
- 键盘输入前先把鼠标移入 QEMU 窗口。
- 首次进入系统设置后可点击音量条听到固件自带的测试音。

## 参数

`start.ps1` 的常用参数：

```powershell
-SystemImage <路径>   # 系统卡镜像；默认查找仓库 images/book2.img
-ExternalSd <路径>    # 可选外置卡（MSC1）
-Audio dsound|wav|none # 音频后端，默认 dsound
-RtcState <路径>      # RTC 状态文件；默认 %LOCALAPPDATA%\book2-runtime\book2-rtc.state
-Display gtk|sdl|none # 显示后端，默认 gtk
```

同一份 RTC 状态文件只允许一个模拟器实例使用。需要多个实例时分别指定不同的 `-RtcState`。

## 构建

需要 MSYS2 UCRT64 环境（bash、gcc、ninja、Python 3）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

`hw/mips` 中已包含 Book2 机器和全部外设模型，Kconfig 与 meson.build 已注册 `book2.c`；无需再向源码树复制或同步文件。

从原始资源生成系统镜像：

```powershell
python .\tools\build_image.py --out Z:\book2-new.img --manifest Z:\book2-new.manifest.json
```

构建器只依赖 Python 标准库。它生成 3 GiB 单镜像，包含 Boot0、tinyos/ExtOS 各三副本、A 三镜像以及 B/C/D 卷，并校验所有文件 SHA256。目标文件和 manifest 必须不存在。

生成独立运行包：

```powershell
python .\tools\package.py --output .\dist-new
```

打包完成后，把系统镜像放入 `dist-new\images\book2.img`，再运行其中的 `start.cmd`。

## 已知范围

- 尚未逐一验证全部学习资源、词典、视频和游戏。
- 外置 SD、麦克风录音/ADC、完整模拟音量衰减、全路由 codec、CPM 时钟和迁移未建模。
- 数字键已验证；部分按键在固件中有独立键码，运算符可以使用屏幕按钮。
- RTC 状态文件损坏或不可写时会报错。强制结束进程可能丢失最近一分钟的进度，关键设置写入会立即保存。
