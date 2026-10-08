# CGPlay

CGPlay 是面向 VFX、动画和合成审片流程的 Windows 播放器，使用 C++17、Qt 6、tlRender 和 Python 构建。项目开发目录曾使用 RVLite 名称，代码中仍保留部分命名。

当前源码版本：**1.0.7.14**。

- [下载 1.0.7.14 Windows 完整安装包](https://github.com/xty-luoye/CGPlay/releases/download/v1.0.7.14/CGPlay_Setup_1.0.7.14_full.exe)
- [版本发布页](https://github.com/xty-luoye/CGPlay/releases/tag/v1.0.7.14)
- [使用说明](docs/CGPlay_使用说明_1.0.7.md)
- [构建说明与当前限制](docs/OFFICIAL_BUILD.md)
- [贡献指南](CONTRIBUTING.md)

1.0.7.14 默认在后台检查 GitHub 最新正式版，支持下载、校验和安装。1.0.7.11 及更早版本请先手动安装一次本版，再使用新的自动更新通道。

## 主要功能

- 视频和图像序列播放、逐帧查看、倍速、循环播放与时间线。
- A/B 对比、划像、叠加和差异查看。
- 帧批注、审片列表，以及 JSON、HTML 和视频导出。
- OpenEXR / OpenImageIO 图像处理与 OCIO 色彩管理。
- 播放列表、OTIO 时间线导入导出和多窗口查看。
- 字幕识别、OCR 与翻译；相关功能需要单独准备模型、运行环境或服务配置。
- Windows QuickLook 预览、文件关联与视频缩略图。
- 可选的 Codex 工作台集成；运行时、登录凭据和服务访问权限需自行配置，不包含在源码中。

实际支持的媒体格式取决于 tlRender、FFmpeg、OpenImageIO 和编码器的构建配置。

## 1.0.7.14 更新

- 修复部分 Windows 设备按 F11 进入全屏后，视频画面停在旧帧的问题。
- 修复上述情况下按空格恢复播放后，画面仍不更新的表现。
- 统一程序内版本标识，让关于页面与更新检查正确识别当前版本。

继续保留全屏浮层控制条：移动鼠标时滑出，隐藏和显示时不改变视频大小；AI 工作台启动时默认关闭，需要时按需打开；支持 GitHub 自动更新。

详见[完整更新说明](docs/RELEASE_NOTES_1.0.7.14.md)。

## 构建状态

当前支持的开发目标为 **Windows x64 + Visual Studio 2022 + Qt 6.5.3**，Qt WebEngineWidgets 为必需组件。完整播放器需要 tlRender / feather-tk 及其原生依赖；当前依赖源码要求 CMake 3.31 或更新版本。

**本次公开的是应用源码快照，不包含完整第三方构建环境。** 原开发环境中的 tlRender / feather-tk 版本尚未锁定到可核验的上游提交，部分本地依赖改动也尚未整理为完整补丁集。因此暂未验证从干净系统重建官方安装包；请先阅读[构建说明](docs/OFFICIAL_BUILD.md)，不要直接使用最新上游版本并假定兼容。

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
