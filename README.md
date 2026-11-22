# Book2 QEMU 模拟器

本工程以 `book2-res` 的原始 Boot0、tinyos、ExtOS 和应用资源运行结果为准。模拟器运行原固件，不修改 guest PC、程序、消息队列或桌面状态。

## 直接运行

打开 `dist` 目录，双击 `start.cmd`。完整运行包包含 Windows QEMU、依赖 DLL、GTK 资源和 `images/book2.img`，无需另外安装 MSYS2。镜像约 3 GiB，目录可以整体复制。

首次启动会出现固件自己的“时间”窗口。在里面设置需要的时间，点击底部“确定”。随后进入小学桌面。RTC 状态由模拟器保存，下次启动继续计时并直接进入桌面。

桌面底部可切换到中学界面；中学页面的“更多”进入分类。图标可双击打开。原系统的各个应用窗口和弹窗需要分别关闭；课程表等应用可能另开编辑弹窗。

默认声音使用 Windows DirectSound，键盘和鼠标通过真实 USB HID 接入。QEMU 窗口的菜单/快捷键遵循 QEMU GTK 默认行为。

可选 PowerShell 调用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\dist\start.ps1 -SystemImage .\dist\images\book2.img
```

静音：追加 `-Audio none`。后备状态可指定 `-RtcState C:\book2\rtc.state`；同一状态文件只允许由一个启动入口实例使用。默认后备状态位于 `%LOCALAPPDATA%\book2-runtime\book2-rtc.state`。运行日志各自放在该目录的 `run-*` 子目录。使用其他镜像/独立设备实例时请指定另一份 RTC 状态。

源目录的 `start.ps1` 也会查找构建版或 `dist` 运行包；在当前机器开发时默认可使用 `Z:\book2.img`。Z 是内存盘，正式交付镜像已另存于 `dist/images/book2.img`。

## 声音播放页面卡死（已修复）

播放声音一两秒后界面不响应，根因是 JZ4750 的 DMA bank1（音频通道）完成中断 IRQ15。该固件的音频驱动以**轮询**方式等待 DMA 完成（反复读 DCCSR.TT），从不写 DMAIPR 清除中断；模型原先在 DMA 完成时拉高 IRQ15，造成无人清除的持续电平中断，CPU 陷入中断风暴，主循环和鼠标处理被饿死。

修复：保留正确的 IRQ15 接线与 DMAIPR 状态位（供固件读取），但不再为 bank1 的完成事件拉高 IRQ15。对照验证：修复前播放后光标 0/8 响应，修复后 8/8 响应；音频 PCM 输出仍正常（约 1.87 MB，非零采样存在）。

## 本轮四项问题修复

**1）除鼠标外画面整体发暗。** LCD 模型对两层都套用了全局 alpha 0xA0，而固件是「FG1 不透明底图 + FG0 逐像素 alpha 光标」。全屏底图被按 160/255 混到黑色背景上，所以除光标区外整体压暗。现按 OSDC.ALPHAEN / ALPHAMD 语义分层合成：底图不透明，光标层用像素高字节 alpha。实测桌面中位亮度 93→146，95 分位 160→255。

同时修正了绘制顺序：硬件上 FG1 是全屏主平面、FG0 是叠在上面的光标层。此前先画 FG0 再画不透明的 FG1，会把光标整块盖掉（表现为看不到鼠标指针）。现在先画 FG1、后画 FG0。

**2）键盘输入无效。** 有两层原因。第一层是 QEMU GTK 前端只在控制台持有输入抓取时才转发按键，未抓取时窗口收不到任何键；启动入口现在默认带 `gtk,grab-on-hover=on`，鼠标移入窗口即可打字。第二层是宿主轻点可能短于固件扫描周期而被丢弃，模型现为每次闭合加最短保持时间。

排查过程中的一个教训：QMP `send-key` 会绕过 GTK 前端直接注入，所以只用它测试会**误判键盘已可用**。本轮改用带真实扫描码的 Win32 `SendInput` 验证：`1 2 3` 在计算器显示 `123`（`validation/kbd-final`）。合成按键若不带扫描码，GDK 会得到 keycode 0 并被丢弃，这也是另一种假阳性来源。

**3）运行卡顿。** 实测客户机时间与真实时间严格 1:1（`-display none` 与 `gtk` 均为 1.000×），所以不是模拟变慢，而是固件本身在忙轮询，长期占满一个 CPU 核心。同时原模型每虚拟帧额外强制一次 800×480 全屏重绘（与 UI 刷新重复）。已去除重复重绘、加 RGB565 查找表与帧内容比对（画面不变则不推送显示）。如需进一步降低占用可用 `-Audio none`，或接受该固件的轮询行为。

**4）点击“版本信息”卡死。** 不是缺机器码。固件在该页用私有流程读卡，模型的 R1 短响应第三字没有回填状态高字节，`READY_FOR_DATA`/CURRENT_STATE 校验始终为假并在时钟关闭时无限重试（实测 951 次 CMD13）。修正响应字节顺序后该循环降到 1 次，并补齐 CMD8 数据阶段与 CMD60 卡身份响应。版本页现已正常显示并可用 ESC 退出：版本号 V1.32、客服 400-168-8888、www.eebbk.com、制造商。机器码是由卡 CID 派生的设备码，模型提供稳定可复现的 CID；固件在无授权数据时仍显示空占位，这不是硬件阻塞。

## 已验证

- 原始固件从 Boot0 读取 SD 卡、加载 tinyos/ExtOS、显示 800×480 界面。
- 首次时间确认后，桌面原生控件和命中状态正常初始化；小学、中学、“更多”、分类页可导航。
- 资源管理打开并列出 D 盘原始资源目录；课程表、系统设置、计算器可运行。
- USB 数字键 1、2 在计算器显示为 12；实际鼠标操作证据见 `validation/final`。
- 系统音量条触发原始 `A:\系统\数据\sound.mp3`，固件自行解码并通过 DMA/AIC 输出。捕获 1,811,648 字节 PCM，非零字节 110,814，44.1 kHz / 16 bit / 双声道。提供标准 WAV 格式的采样证据。
- RTC 真实 seconds/HSPR 寄存器保存、退出并重新启动后恢复，第二次直接进入桌面。初始状态不伪造固件初始化标记。
- 原始资源全体 577 个文件保留且逐文件 SHA256 回读通过，裸固件 10 处副本、A 三镜像及 B/C/D 卷均由构建器验证。
- 独立运行包在 PATH 排除 MSYS2 后可启动原始镜像并生成固件画面。最终图形和运算复核见 `validation/final/README.md`。

## 重要修正

JZ4750 为 XBurst MIPS32-R1 + MXU1，默认 CPU 必须使用 `XBurstR1`。旧 24Kc 虽能进入 GUI，但在 MP3 解码的 MXU 启用指令 `0x8007F74C` 引发 RI 异常，无法正确运行媒体代码。

“桌面 hitstate 全零”曾被误判为硬件初始化阻塞，实际是首次“时间”应用同步等待用户确认。通过正常 USB 点击确认后桌面初始化恢复，不需要注入消息或修改 guest 内存。

## 构建与重建镜像

现有 QEMU 11.1.3 源码树的增量构建：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

也可通过 `-QemuSource` / `-BuildDirectory` 指向已有、已配置的 Windows UCRT QEMU 源码树。脚本同步 `src` 的六个正式设备源文件，维护 Book2 Kconfig/Meson 注册并写 SHA256 清单。不负责下载和重新配置整个上游工具链。

从同级原始资源生成新镜像，目标文件必须不存在：

```powershell
python .\tools\build_image.py --out Z:\book2-new.img --manifest Z:\book2-new.manifest.json
```

镜像构建器只需要 Python 标准库；资源名和文件内容不做修改。A/C 使用原固件规定的 FAT32 BPB 和 8 KiB 簇，但簇数低于通用 FAT32 门槛；不要用通用格式化工具改写这些卷。资源输入目录 A 的总大小超过固件 A 卷容量，因此按固件实际路径将应用和词库放到 B，全部原始输入另保存在 D 的归档位置。详见 `analysis/image-audit/README.md`。

运行包重新打包（输出目录必须不存在）：

```powershell
python .\tools\package.py --output .\dist-new
```

该工具只打包运行时，需另放 `images/book2.img`、镜像 manifest 和本说明。

## 已知范围

尚未逐一验收所有学习资源、全部词典、视频和游戏，也未验收外部 SD、麦克风录音/ADC、精确模拟音量衰减、全路由 codec、完整 CPM 时钟和迁移。按键名称和固件内键码并非全部一致；数字键已验证，运算符可用屏幕按钮。

`-Audio wav` 的原生 QEMU WAV 后端本次正常 QMP 退出后仍留下零 RIFF/data 长度字段；原始 PCM 确实存在。交付的 `validation/final/firmware-sound.wav` 仅修复容器长度，未改 PCM。默认 DirectSound 路径不依赖该 WAV 容器。

RTC 文件损坏或不可写时会报错；不会静默清空或伪造初始化。强制结束进程可能丢失最近一分钟 RTC 进度，关键 guest 设置写入会立即保存。直接绕过启动脚本自行启动多个 QEMU 时，应自行保证 state-file 互斥。
