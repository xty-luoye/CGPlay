# CGPlay

面向 VFX / 动画 / 合成审片流程的专业审片播放器，支持高分辨率素材播放、A/B 对比、批注、色彩管理、播放列表和导出工作流。

## 下载

- 完整安装包: [CGPlay_Setup_1.0.0_full.exe](https://github.com/xty-luoye/CGPlay/releases/download/v1.0.0/CGPlay_Setup_1.0.0_full.exe)
- 精简安装包: [CGPlay_Setup_1.0.0_lite.exe](https://github.com/xty-luoye/CGPlay/releases/download/v1.0.0/CGPlay_Setup_1.0.0_lite.exe)
- 发布页: [CGPlay Releases](https://github.com/xty-luoye/CGPlay/releases/tag/v1.0.0)

## 安装包选择

| 安装包 | 说明 |
|------|------|
| Full | 包含主程序和常用运行组件，适合离线安装 |
| Lite | 体积更小，首次运行时可按需下载缺失组件 |

## 功能总览

| 模块 | 能力 |
|------|------|
| 播放控制 | Space 播放/暂停，J/K/L 审片播放，逐帧步进，入点/出点循环 |
| 视图控制 | Fit、1:1、缩放、平移、全屏播放 |
| A/B 对比 | 并排、划像、叠加、差异、水平分割、垂直分割 |
| 批注系统 | 箭头、矩形、圆形、文字、自由绘制、点标记，支持撤销/重做和持久化 |
| 色彩管理 | OCIO、ACES、曝光、伽马、显示视图切换 |
| 播放列表 | Shot 管理、状态标记、搜索过滤、缩略图、拖放导入 |
| 导出系统 | Clean/Annotated 视频导出、JSON 批注导出、HTML 审片报告 |
| 自动组件 | 启动后检测缺失组件，支持从公开 Release 自动下载补齐 |

## 支持格式

| 类别 | 格式 |
|------|------|
| 图像序列 | EXR、DPX、TIFF、PNG、JPG |
| 视频容器 | MOV、MP4、MXF |
| 时间线 | OTIO |
| 导出 | MP4、MOV、H.264、H.265、ProRes |
| 批注 | `.review.json`、JSON、HTML |

## 组件

CGPlay 支持通过公开 Release 自动补齐缺失组件：

- `cgplay-core-win64.zip`
- `cgplay-ffmpeg-win64.zip`
- `cgplay-python-runtime-win64.zip`
- `cgplay-tools-win64.zip`
- `manifest.json`

## 快捷键

| 快捷键 | 功能 |
|------|------|
| `Space` | 播放 / 暂停 |
| `J` / `K` / `L` | 倒放 / 停止 / 正放 |
| `←` / `→` | 上一帧 / 下一帧 |
| `Shift+←` / `Shift+→` | 跳 10 帧 |
| `F` | 适应窗口 |
| `1` | 1:1 缩放 |
| `F11` | 全屏 |
| `B` | A/B 对比开关 |
| `Ctrl+S` | 保存批注 |
| `Ctrl+E` | 导出视频 |

## 说明

这个仓库只用于发布 CGPlay 安装包、自动更新清单和组件包，不包含 CGPlay 源码。

## 致谢

CGPlay 的播放架构基于 tlRender 相关技术生态构建。感谢开源图形、色彩管理、媒体处理工具链为专业审片流程提供的基础能力。
## Latest Update

- Fixed the timeline `Frame / Total` display bug.
- Optimized the main window default launch size so it no longer opens overly large.
- Restored manual resize support on the frameless main window edges and corners.
