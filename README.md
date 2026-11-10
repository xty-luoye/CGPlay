# CGPlay

CGPlay 是面向 VFX、动画和合成审片流程的 Windows 播放器，使用 C++20、Qt 6、tlRender 和 Python 构建。项目开发目录曾使用 RVLite 名称，代码中仍保留部分命名。

当前源码版本：**1.0.8**。正式下载以 Releases 为准。

- [下载 1.0.8 Windows 完整安装包](https://github.com/xty-luoye/CGPlay/releases/download/v1.0.8/CGPlay_Setup_1.0.8_full.exe)
- [版本发布页](https://github.com/xty-luoye/CGPlay/releases/tag/v1.0.8)
- [使用说明](docs/CGPlay_使用说明_1.0.7.md)
- [构建说明与当前限制](docs/OFFICIAL_BUILD.md)
- [贡献指南](CONTRIBUTING.md)

1.0.8 默认在后台检查 GitHub 最新正式版，支持下载、校验和安装。1.0.7.11 及更早版本请先手动安装一次本版，再使用新的自动更新通道。

## 主要功能

- 视频和图像序列播放、逐帧查看、倍速、循环播放与时间线。
- A/B 对比、划像、叠加和差异查看，并可合成为一个视频导出。
- 屏幕、指定窗口和播放器区域录屏，支持原始分辨率、最高 8K / 120 FPS、系统声音、麦克风及暂停、恢复保存。
- 帧批注、审片列表，以及 JSON、HTML 和视频导出。
- OpenEXR / OpenImageIO 图像处理与 OCIO 色彩管理。
- 播放列表、OTIO 时间线导入导出和多窗口查看。
- 字幕识别、OCR 与翻译；相关功能需要单独准备模型、运行环境或服务配置。
- Windows QuickLook 预览、文件关联与视频缩略图。
- 可选的 Codex 工作台集成；运行时、登录凭据和服务访问权限需自行配置，不包含在源码中。

实际支持的媒体格式取决于 tlRender、FFmpeg、OpenImageIO 和编码器的构建配置。

## 1.0.8 更新

- 新增 A/B 合成视频导出：保存当前对比布局，选择 A、B 或无音轨，支持 H.264、H.265 和 ProRes HQ。
- 新增录屏面板：选择屏幕、窗口或播放器区域，支持原始分辨率及最高 8K / 120 FPS，显示实际编码帧率和跳帧数；独立调节系统声音和麦克风；异常时保留 MKV 与恢复记录。
- 修复导出文件保护、色彩快照、高位深及异步任务的取消、关闭和防重入。
- 新增缩略图磁盘缓存位置、容量和使用量设置，修正实际播放器的内存缓存参数。
- 将系统性能采样移到后台，减少暂停和隐藏画面的显示轮询；修正批注帧率和任务释放，组件更新期间已有后台任务可继续完成。

详见[完整更新说明](docs/RELEASE_NOTES_1.0.8.md)。录屏目前面向 Windows SDR，不含直播、摄像头编排或 OBS 插件管理；实际设备仍受 Windows、权限和驱动影响。

## 构建状态

当前支持的开发目标为 **Windows x64 + Visual Studio 2022 + Qt 6.11.3 / WebEngine 6.11.3**，Qt WebEngineWidgets 为必需组件。完整播放器需要 tlRender / feather-tk 及其原生依赖；当前依赖源码要求 CMake 3.31 或更新版本。

**应用源码与第三方运行组件分别提供。** 本版已记录实际依赖版本、固定上游提交、完整源码及本地修改，提供匹配组件的构建脚本。尚未验证从独立干净系统重建官方安装包；请按[构建说明](docs/OFFICIAL_BUILD.md)和[依赖来源记录](docs/DEPENDENCY_SOURCES_1.0.8.md)配置经过匹配的依赖。

`build_release.bat` 用于已经配置好的构建目录，首次构建需要先配置 CMake。开发源码编译与制作完整安装包是两个独立步骤，运行时和分发许可要求见 [INSTALLER_BUILD.md](docs/INSTALLER_BUILD.md)。

## 源码结构

```text
src/
  common/       通用组件、设置、任务与输入
  core/         播放、缓存与会话
  features/     批注、播放列表与时间线
  services/     媒体、色彩、平台与 AI 服务
  plugins/      功能插件与接口
  shell/        Windows 缩略图扩展
  ui/           Qt 窗口、控件与查看器
tools/cgplay/   Python 导出、字幕与 AI 工具
tools/recorder/ 独立的原生录屏辅助程序与构建脚本
cmake/         构建支持和已有 tlRender 补丁
resources/     图标、资源与组件清单
tests/         测试源码及自动化覆盖清单
docs/          使用、构建与第三方许可说明
```

源码仓库不包含安装包、编译输出、第三方运行时、模型、私人媒体、凭据和内部开发记录。

## 参与开发

欢迎通过 Issues 报告可复现的问题，通过 Pull Request 提交范围明确的修复。请说明系统、版本、输入格式和复现步骤，提交前检查日志及附件中的私人路径、账号和媒体内容。

测试只运行与改动相关的范围。涉及 GUI 的自动化应在后台模式运行，证据放在 `tests/artifacts/<task>/`；详见[贡献指南](CONTRIBUTING.md)。

## 许可证

当前源码版本按 [GNU GPL v3.0](LICENSE.txt)（GPL-3.0-only）发布，允许商用；分发本程序或其修改版时，须遵守 GPLv3，包括向接收者提供相应源码和保留许可声明。第三方组件保留各自许可证。此前已依据 MIT 取得的授权继续有效；这不表示所有旧安装包均采用 MIT。

第三方声明及二进制分发注意事项见 [THIRD_PARTY_NOTICES.txt](docs/THIRD_PARTY_NOTICES.txt)。源码公开并不代表任意重新打包的依赖或安装包已经满足分发条件。
