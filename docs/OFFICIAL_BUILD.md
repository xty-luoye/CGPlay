# Windows source build

The 1.0.8 build layout is Windows x64, Visual Studio 2022, the matched Qt 6.11.3/WebEngine 6.11.3 MSVC kit, C++20, Release configuration, and `CGPLAY_USE_TLRENDER=ON`. The application source is published separately from the dependencies and bundled runtime files.

## Current reproducibility status

**An independent clean-machine rebuild of this source release has not been verified.** The 1.0.8 engine pair is tlRender `0.27.1` at `0a413e542c54bd5ce89f0b782b00e00982f20e1d` and feather-tk `0.19.0` at `51d9c3d06b608a4a9bd0a0eec54bbf6814699a71`. Complete source contents were checked against the pinned Git trees. The [native build recipe](../tools/deps/README.md) preserves the compatibility patch and required image/color header ordering. Local builds and targeted player/color checks do not imply a clean-machine or bit-for-bit rebuild. Historical ZIP-derived snapshots remain identified by their actual content hashes, rather than invented upstream commits; they are not the selected new engine sources.

The instructions below describe how to configure the project **when compatible, already-built dependencies are available**. A successful CMake configure does not establish that the source can reproduce the official installer. Dependency maintenance candidates are built in separate E-drive prefixes. Only combinations that pass consumer rebuilds and runtime checks may be selected; independent machine and CI verification remain separate requirements.

## Prerequisites

- Windows x64, Visual Studio 2022 C++ tools and a Windows SDK.
- CMake 3.31 or newer for the current tlRender/feather-tk source baseline, plus Git.
- The complete matching Qt 6.11.3/WebEngine 6.11.3 kit for MSVC2022 x64, including Widgets, OpenGL, Concurrent, Network and **WebEngineWidgets**. WebEngine is required at configure time. Do not mix isolated Qt DLLs from different kits.
- Compatible tlRender and feather-tk headers, libraries and CMake package files. Their dependency installation must include FFmpeg development files, Freetype, minizip-ng, SDL2, Imath, nlohmann-json, OpenTimelineIO, OpenColorIO, OpenEXR, OpenImageIO and the image/compression libraries selected by the dependency build. Keep compiler, architecture and runtime-library settings consistent.
- Python and its feature-specific packages are runtime requirements for video export and subtitle tools, not a replacement for the C++ dependencies.

`CGPLAY_USE_TLRENDER=OFF` is a legacy stub configuration, not a dependency-free or fully functional player build.

## Configure with local dependency locations

Run from the source root. Replace the example locations with your own paths; do not copy another computer's CMake cache. Forward slashes avoid ambiguity in CMake paths.

```powershell
$env:QT_ROOT = "E:/deps/Qt/6.11.3/msvc2022_64"
$env:TLRENDER_SOURCE_DIR = "E:/deps/engine-build-1.0.8/source/tlRender"
$env:TLRENDER_INSTALL_DIR = "E:/deps/engine-build-1.0.8/install/tlRender"
$env:FEATHER_TK_INSTALL_DIR = "E:/deps/engine-build-1.0.8/install/ftk"
$env:CGPLAY_FFMPEG_INSTALL_DIR = "E:/deps/native"
$env:CGPLAY_DEPENDENCY_PREFIX = "E:/deps/ocio;E:/deps/images"

# Optional validated maintenance prefix, searched before the baseline libraries:
# $env:CGPLAY_DEPENDENCY_PREFIX = "D:/deps/maintenance-prefix"
# Optional additional dependency prefix, if your libraries are split across prefixes:
# $env:CGV_INSTALL_DIR = "D:/deps/extra-install"
# Optional feather-tk source fallback when no installed ftk package is available:
# $env:FEATHER_TK_SOURCE_DIR = "D:/deps/feather-tk"
# Optional legacy OpenEXR fallback (full triplet library directory):
# $env:CGPLAY_VCPKG_LIBRARY_DIR = "D:/vcpkg/installed/x64-windows/lib"

cmake --preset windows-full-release -DCGPLAY_REQUIRE_RUNTIME=OFF
```

The preset reads `CGPLAY_DEPENDENCY_PREFIX`, `QT_ROOT`, `TLRENDER_SOURCE_DIR`, `TLRENDER_INSTALL_DIR`, `FEATHER_TK_INSTALL_DIR`, `CGV_INSTALL_DIR` and the optional `CMAKE_PREFIX_PATH` environment value. `CGPLAY_FFMPEG_INSTALL_DIR` selects native FFmpeg headers/libraries independently of the renderer prefix. Standard CMake command-line arguments, such as `-DQt6_DIR=...`, `-DCMAKE_PREFIX_PATH=...` and `-DTLRENDER_INSTALL_DIR=...`, can override preset values. The remaining dependency location options accept either `-D` values or environment variables with the same names. `TLRENDER_BUILD_DIR` controls the existing dependency build tree used only by the legacy stub fallback.

Use `CGPLAY_REQUIRE_RUNTIME=OFF` for source development without bundled binaries. This skips missing FFmpeg/Codex deployment with a warning; it does not provide those features at runtime. `CGPLAY_REQUIRE_RUNTIME=ON` remains the release preset default and requires the following files before configuration:

Full 1.0.8 builds also require `CGPLAY_RECORDING_RUNTIME_DIR` to point to the separately built private recorder directory, containing `CGPlayRecorder.exe` and `runtime-manifest.json`. Set `CGPLAY_REQUIRE_RECORDING_RUNTIME=OFF` explicitly for development without this optional feature; a missing recorder is not a complete recording-enabled release. `CGPLAY_PYTHON_RUNTIME_DIR` can select an explicitly verified embedded Python directory for deployment, instead of using a previously staged package. Both paths accept environment variables or `-D` values and never cause OBS DLLs to be loaded into the player's Qt process.

```text
build_win_full/runtime/ffmpeg/ffmpeg.exe
build_win_full/runtime/ffmpeg/ffprobe.exe
build_win_full/runtime/codex/codex.exe
build_win_full/runtime/codex/codex-code-mode-host.exe
build_win_full/runtime/codex/LICENSE-OPENAI-CODEX.txt
```

## Dependency patches

For the selected 0.27.1/0.19.0 pair, use [tools/deps/build_engine_stack_1_0_8.ps1](../tools/deps/build_engine_stack_1_0_8.ps1). It stages a copy of the pinned tlRender source, applies the current patch, builds the matching ftk and uses the selected image/color headers before general native headers. Existing output roots are rejected. The original sources remain untouched.

The following helper is retained only for the historical 0.19 baseline and must not be forced onto 0.27.1:

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
& "$env:QT_ROOT/bin/windeployqt.exe" --dir build_win_full/bin/Release build_win_full/bin/Release/plugins/CGPlayCodexPlugin.dll --no-translations
```

The Codex plugin imports Qt WebEngine. Deploying only the two application executables does not discover that plugin's WebEngine dependency; deploy it into the same application directory as shown above.

An installer also needs the complete Python runtime, export/subtitle dependencies, FFmpeg/Codex runtimes, the recorder, third-party licenses and the package validation described in `INSTALLER_BUILD.md`. Packaging defaults derive from the selected project; pass the exact qualified runtime paths when producing a release. The application source tree does not contain those runtime binaries and a source-only build is not a ready-to-distribute installer.

## Checks

```powershell
cmake --list-presets
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_official_build_config.ps1
```

The configuration validator checks the Release/tlRender/x64 layout. It does not replace compilation, deployment, runtime checks or independent clean-machine validation. Run automated product checks in background mode and keep generated evidence under `tests/artifacts/`; do not commit local build caches, media or test output.

