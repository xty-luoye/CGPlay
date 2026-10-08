# CGPlay Installer Build

## Goal

This flow builds the official CGPlay installer from a staged app directory whose layout matches the runtime directory that already passes `--check-components`.

Target install layout:

```text
CGPlay/
├── CGPlay.exe
├── CGPlayQuickLook.exe
├── plugins/
├── runtime/
│   └── python/
├── tools/
│   └── cgplay/
├── resources/
├── presets/
├── translations/
└── components/
```

## Files

- Primary installer script: `scripts/installer/CGPlay.iss`
- NSIS fallback script: `installer/CGPlay_Installer.nsi`
- Build entrypoint: `build_installer.bat`
- Staging + compiler driver: `tools/package_installer.ps1`

## Installer selection

Priority order:

1. Inno Setup 6
2. NSIS

`build_installer.bat` and `tools/package_installer.ps1` use `Inno Setup` when `ISCC.exe` is installed. If Inno is not available, the flow falls back to `makensis.exe`.

## Requirements

Before building the installer:

1. Configure/build with the official presets so both Release executables exist and tlRender is enabled.
2. Supply a complete Python runtime with `-PythonRuntime` or `CGPLAY_PYTHON_RUNTIME` if it is not already in the Release directory.
3. Supply the directory containing `ffmpeg.exe` and `ffprobe.exe` with `-FfmpegBinDir` or `CGPLAY_FFMPEG_BIN_DIR` if they are not already in Release. Missing inputs fail before version changes or staging.
4. Complete the binary distribution checks in `COMMERCIAL_RELEASE_CHECKLIST.md` (historical filename). This source-only publication does not include or approve an installer.

`-ProjectRoot` defaults to this checkout. `-ReleaseDir` and `-CodexRuntime`
default to its `build_win_full/bin/Release` and `build_win_full/runtime/codex`
directories; explicit parameters override those locations.

Validate the tree before packaging:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_official_build_config.ps1 -RequireBuiltApps
~~~

## Build commands

Default official build:

```bat
build_installer.bat
```

Explicitly request the full package with automatic installer selection:

```bat
build_installer.bat full auto
```

Force Inno Setup:

```bat
build_installer.bat full inno
```

Force NSIS fallback:

```bat
build_installer.bat full nsis
```

## What the build does

`tools/package_installer.ps1` performs these steps:

1. Validates the official Release/tlRender/x64 build before clearing staging
2. Creates a clean staging directory at `build_win_full/package/full/CGPlay`
3. Copies the validated `Release` runtime payload into staging
3. Ensures staging contains:
   - `CGPlay.exe`
   - `plugins/`
   - `runtime/python/`
   - `tools/cgplay/`
   - `resources/`
   - `presets/`
   - `translations/`
   - bundled Qt DLLs and plugin folders
   - bundled VC++ runtime DLLs
5. Runs tools/validate_package_payload.ps1 and fails if either executable,
   required application plugins, Python, FFmpeg, Qt DLLs, or qwindows.dll
   is missing or empty
6. Validates the staged component manifest
7. Runs a Python runtime smoke import for the full package
8. Compiles the installer with Inno Setup or NSIS

The preflight reports are written under `tests/artifacts/productization_top10_20260710`.

The strict validator can also be run directly against a Release or staged tree:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_package_payload.ps1 -AppRoot build_win_full\bin\Release -PackageMode full
~~~

## Output locations

Staging directory:

```text
build_win_full/package/full/CGPlay
```

Installer output:

```text
build_win_full/installer/full
```

Expected artifact names:

- `CGPlay_Setup_<version>_full.exe`
- `CGPlay_Setup_<version>_lite.exe` when building the legacy lite mode

## Notes

- The official package is the `full` package.
- The staged package must contain the root `LICENSE.txt` (CGPlay GPLv3 license),
  `licenses/THIRD_PARTY_NOTICES.txt`, `licenses/FFMPEG_SOURCE_OFFER.txt`, and
  `licenses/third_party/` before distribution. Both installers display the GPLv3
  license; the former draft commercial EULA is not a term of this release.
- `tools/validate_commercial_release.ps1` retains its historical name. It checks
  basic material presence and the standalone FFmpeg license report; a pass does
  not verify corresponding-source completeness or approve binary publication.
- The installer defaults to a per-user writable install directory so CGPlay component download and update flows can write into the install tree without requiring admin rights.
- The staged package intentionally creates `presets/`, `translations/`, and `components/` even when they are empty so the installed layout stays stable.
