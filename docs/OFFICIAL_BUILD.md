# Windows source build

The release layout is Windows x64, Visual Studio 2022, Qt 6.5.3, Release configuration, and `CGPLAY_USE_TLRENDER=ON`. The application source is published separately from the dependencies and bundled runtime files.

## Current reproducibility status

**An independent clean-machine rebuild of this source release has not been verified.** The maintainer's dependency trees identify themselves as tlRender `0.19.0-dev` and feather-tk `0.10.0-dev`, but their exact upstream revisions and all local changes have not yet been captured. The two patches in `cmake/patches/` do not describe every modification in those dependency trees. Fetching the latest upstream branches is not a verified substitute.

The instructions below describe how to configure the project **when compatible, already-built dependencies are available**. A successful CMake configure does not establish that the source can reproduce the official installer. Dependency revision pinning, complete patches and independent build verification remain to be completed.

## Prerequisites

- Windows x64, Visual Studio 2022 C++ tools and a Windows SDK.
- CMake 3.31 or newer for the current tlRender/feather-tk source baseline, plus Git.
- Qt 6.5.3 for MSVC x64, including Widgets, OpenGL, Concurrent, Network and **WebEngineWidgets**. WebEngine is currently required at configure time.
- Compatible tlRender and feather-tk headers, libraries and CMake package files. Their dependency installation must include FFmpeg development files, Freetype, minizip-ng, SDL2, Imath, nlohmann-json, OpenTimelineIO, OpenColorIO, OpenEXR, OpenImageIO and the image/compression libraries selected by the dependency build. Keep compiler, architecture and runtime-library settings consistent.
- Python and its feature-specific packages are runtime requirements for video export and subtitle tools, not a replacement for the C++ dependencies.

`CGPLAY_USE_TLRENDER=OFF` is a legacy stub configuration, not a dependency-free or fully functional player build.

## Configure with local dependency locations

Run from the source root. Replace the example locations with your own paths; do not copy another computer's CMake cache. Forward slashes avoid ambiguity in CMake paths.

```powershell
$env:QT_ROOT = "D:/deps/Qt/6.5.3/msvc2019_64"
$env:TLRENDER_SOURCE_DIR = "D:/deps/tlRender"
$env:TLRENDER_INSTALL_DIR = "D:/deps/tlRender/install-Release"
$env:FEATHER_TK_INSTALL_DIR = "D:/deps/feather-tk-install"

# Optional additional dependency prefix, if your libraries are split across prefixes:
# $env:CGV_INSTALL_DIR = "D:/deps/extra-install"
# Optional feather-tk source fallback when no installed ftk package is available:
# $env:FEATHER_TK_SOURCE_DIR = "D:/deps/feather-tk"
# Optional legacy OpenEXR fallback (full triplet library directory):
# $env:CGPLAY_VCPKG_LIBRARY_DIR = "D:/vcpkg/installed/x64-windows/lib"

cmake --preset windows-full-release -DCGPLAY_REQUIRE_RUNTIME=OFF
```

The preset reads `QT_ROOT`, `TLRENDER_SOURCE_DIR`, `TLRENDER_INSTALL_DIR`, `FEATHER_TK_INSTALL_DIR`, `CGV_INSTALL_DIR` and the optional `CMAKE_PREFIX_PATH` environment value. Standard CMake command-line arguments, such as `-DQt6_DIR=...`, `-DCMAKE_PREFIX_PATH=...` and `-DTLRENDER_INSTALL_DIR=...`, can override preset values. The remaining dependency location options accept either `-D` values or environment variables with the same names. `TLRENDER_BUILD_DIR` controls the existing dependency build tree used only by the legacy stub fallback.

Use `CGPLAY_REQUIRE_RUNTIME=OFF` for source development without bundled binaries. This skips missing FFmpeg/Codex deployment with a warning; it does not provide those features at runtime. `CGPLAY_REQUIRE_RUNTIME=ON` remains the release preset default and requires the following files before configuration:

```text
build_win_full/runtime/ffmpeg/ffmpeg.exe
build_win_full/runtime/ffmpeg/ffprobe.exe
build_win_full/runtime/codex/codex.exe
build_win_full/runtime/codex/codex-code-mode-host.exe
build_win_full/runtime/codex/LICENSE-OPENAI-CODEX.txt
```

## Dependency patches

Use a separate compatible tlRender source checkout. This command modifies that checkout, applies the two recorded patches when possible, recognizes already-applied patches and fails when the baseline does not match:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/apply_tlrender_patches.ps1 `
    -TlRenderSourceDir $env:TLRENDER_SOURCE_DIR
```

The script requires either the parameter or the environment variable; it no longer defaults to a maintainer-specific path. These patches alone do not supply the other dependency changes mentioned above. Do not force them onto an incompatible upstream revision.

## Build and deploy

After dependencies are prepared and configuration succeeds:

```powershell
cmake --build --preset windows-full-release-apps
```

The preset builds `CGPlay`, `CGPlayQuickLook`, the player plugins and `CGPlayThumbnailProvider`. `build_release.bat` is a maintainer convenience for an already-configured, runtime-staged build; it is not a first-time dependency setup command.

Qt deployment is a separate step:

```powershell
& "$env:QT_ROOT/bin/windeployqt.exe" build_win_full/bin/Release/CGPlay.exe --no-translations
& "$env:QT_ROOT/bin/windeployqt.exe" build_win_full/bin/Release/CGPlayQuickLook.exe --no-translations
```

An installer also needs the complete Python runtime, export/subtitle dependencies, FFmpeg/Codex runtimes, third-party licenses and the package validation described in `INSTALLER_BUILD.md`. Packaging scripts still contain maintainer defaults; pass their documented path parameters for your environment. This source release does not include those runtime binaries and does not claim that a source-only build is a ready-to-distribute installer.

## Checks

```powershell
cmake --list-presets
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_official_build_config.ps1
```

The configuration validator checks the Release/tlRender/x64 layout. It does not replace compilation, deployment, runtime checks or independent clean-machine validation. Run automated product checks in background mode and keep generated evidence under `tests/artifacts/`; do not commit local build caches, media or test output.
