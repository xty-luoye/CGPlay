# CGPlay installer build

Use the reviewed source and qualified Release runtime described in `OFFICIAL_BUILD.md`. Source checkout alone is not a ready-to-run installer. The full package contains player/QuickLook/plugins/Qt, Python export/ASR, FFmpeg CLI, Codex and the private recorder.

```text
CGPlay/
  CGPlay.exe, CGPlayQuickLook.exe, ffmpeg.exe, ffprobe.exe, Qt/plugins
  runtime/python/        # retain all package metadata and private DLLs
  runtime/codex/         # original license retained
  runtime/recorder/      # helper, DLL/plugins/data, runtime-manifest.json
  tools/cgplay/          # export, AB compare, recording finalizer, AI
  resources/, presets/, translations/, components/
  LICENSE.txt            # GPLv3
  licenses/              # current notices, source index/provenance, third_party/
```

Build the matched Release applications/plugins/thumbnail provider and recorder first. The driver does not download missing SDKs. Pass selected runtime paths explicitly. Defaults derive from script/project directories. Python, recorder and FFmpeg defaults may be provided through `CGPLAY_PYTHON_RUNTIME_DIR`, `CGPLAY_RECORDING_RUNTIME_DIR` and `CGPLAY_FFMPEG_BIN_DIR`; explicit parameters take priority. Use the real E-drive project, not its compatibility junction.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package_installer.ps1 -ProjectRoot <project> -ReleaseDir <verified-release> -PythonRuntime <verified-python> -CodexRuntime <codex-runtime> -RecordingRuntime <verified-recorder> -FfmpegBinDir <verified-ffmpeg-cli> -PackageMode full -Version 1.0.8
```

`-Version` selects the already-built version without an incidental bump. The driver verifies application versions and the Release/tlRender/x64 build, stages under `build_win_full/package/full/CGPlay`, preserves complete runtime subtrees and license metadata, requires recorder/compare/finalizer files, validates payload/component/source material, runs the staged Python import smoke, and compiles with Inno Setup or NSIS fallback. Reports remain under `tests/artifacts/`. Recursive cleanup is limited to staging children after checking actual filesystem location and rejecting reparse ancestors.

Expected installer: `build_win_full/installer/full/CGPlay_Setup_1.0.8_full.exe`. Optional lite retains its own `_lite.exe` name and is never copied over the full download.

Assemble the reviewed public application snapshot and checksum-fixed dependency source ZIP using `DEPENDENCY_SOURCES_1.0.8.md`. Do not include models, test environments, credentials or private artifacts. Required source/script gaps must be closed or the corresponding binary component replaced/removed before release.

After full staging passes, build the five components:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_release_components.ps1 -ProjectRoot <project> -PackageRoot <full-stage> -OutputDir <release-dir> -Version 1.0.8
```

This creates `core`, `ffmpeg`, `python-runtime`, `tools` and `recording-runtime` archives with hashes and version-pinned GitHub URLs. Every component includes notices. Tools install `cgplay/` directly under `tools/`, without `tools/tools/`. Existing source/installers are preserved; only a fresh owned work directory is cleaned.

Review the upload plan without contacting GitHub:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/publish_github_release.ps1 -ProjectRoot <project> -ReleaseDir <release-dir> -InstallerRoot <installer-root> -Version 1.0.8 -DryRun
```

The real upload requires the genuine full installer, optional separately named lite installer, all five component archives, manifest/hash, current notes, source ZIP/hash and successful source integrity validation. Each asset must fit GitHub's2GiB limit. DryRun checks plan/file presence only; full source integrity is checked before any real GitHub mutation. Packaging success does not prove visual/hardware behavior or independent clean-machine reconstruction.
