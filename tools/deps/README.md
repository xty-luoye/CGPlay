# 1.0.8 native engine build inputs

This is an engineering build recipe, separate from user-facing release notes.
Use Visual Studio 2022 x64, CMake, Git and the complete matching Qt 6.11.3 /
WebEngine 6.11.3 MSVC kit. The application and native engine use C++20.
An independent clean-machine build is not implied by a local SDK build.

Pinned upstream source pairs:

- tlRender 0.27.1: `0a413e542c54bd5ce89f0b782b00e00982f20e1d`
- feather-tk 0.19.0: `51d9c3d06b608a4a9bd0a0eec54bbf6814699a71`
- sheredom/subprocess.h: `0d76f78ff8b56d1240ffe6571d689c5e26299527`

The tlRender patch adds an explicit, finite positive `SeqIO/SpeedOverride` so a
user-selected sequence rate takes precedence over EXR metadata; it also preserves
the existing MPEG/TS/other video suffix aliases without reclassifying WAV audio.
Do not apply the legacy 0.19 patches to this source pair. The new asynchronous
reader is qualified separately for VP8/VP9 playback and seeking.

`qualified_native_headers.cmake` places the selected OCIO and image SDK headers
before the general native prefix. Selecting only a newer `.lib` is insufficient:
an older OCIO header can compile but call the wrong shader descriptor interface.
The qualified native combination is OCIO 2.5.2, Imath 3.2.3, OpenEXR 3.3.14 and
OpenImageIO 3.1.18.1. Preserve the matching native FFmpeg/SDL/freetype/etc. prefix;
the command-line FFmpeg runtime is a separately qualified component.

```powershell
pwsh -NoProfile -File tools/deps/build_engine_stack_1_0_8.ps1 `
  -TlRenderSourceDir 'E:/deps/src/tlRender-0.27.1' `
  -FtkSourceDir 'E:/deps/src/feather-tk-0.19.0' `
  -NativePrefix 'E:/deps/native' `
  -OpenColorIOPrefix 'E:/deps/ocio' `
  -ImagePrefix 'E:/deps/images' `
  -SubprocessIncludeDir 'E:/deps/subprocess/include' `
  -BuildRoot 'E:/deps/engine-build-1.0.8'
```

The output root must be new. The script stages and patches a copy of tlRender;
it does not overwrite the original sources or delete an existing build. Source
archive hashes and the exact corresponding sources are recorded in the release
dependency manifest. The receipt records compilation only; player, color/export,
browser, device and installer acceptance remain separate.

When configuring the player, select the resulting `install/tlRender` and
`install/ftk` prefixes and retain `CGPLAY_FFMPEG_INSTALL_DIR` for native FFmpeg.
Qt ownership of the OpenGL context is handled by `NativeGLLoader` in the player.

The public plugin IID is `com.cgplay.IPlugin/2.0`. Recompile external plugins
against the current headers. Playback native handles are opaque; their virtual
slots and the queued player signal remain present even for SDK-free consumers.
The host rejects a legacy IID before constructing the plugin. Never remove that
check to load an old binary across the new SDK boundary.
