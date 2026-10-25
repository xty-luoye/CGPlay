# CGPlay 1.0.8 dependency sources

The application is GPLv3. Download the [versioned corresponding-source asset](https://github.com/xty-luoye/CGPlay/releases/download/v1.0.8/cgplay-corresponding-sources-1.0.8.zip) and its `.zip.sha256` sidecar alongside the installer. GitHub's automatic **Source code** ZIP is the application repository snapshot; it is not the additional dependency-source asset.

`DEPENDENCY_PROVENANCE_1.0.8.json` lists original archives, complete patches, exact commits where known, recorded settings, notices and SHA-256. Inside the source ZIP, `application/` is a reviewed public application tree, `dependencies/` retains original files, and `source-inventory.json` records every member hash. Original binary wheels are classified as binary packages, not source code. Model weights, test environments, credentials, private handoffs and build caches are excluded.

| Area | 1.0.8 selection | Reason and boundary |
|---|---|---|
| Native color | OCIO2.5.2 | Isolated x64 MSVC `/MD` static build; CPU/GPU consumers rebuilt and qualified. |
| Native images | Imath3.2.3, EXR3.3.14, OIIO3.1.18.1 | Matched headers/new prefix; 21 native precision/channel/window/tile/scanline/multipart checks and all21 baseline formats retained. |
| Python images | OIIO3.2.1.3 | Separately qualified wheel, with internal EXR3.5.0/Imath3.2.3/OCIO2.5.1. Active deep input is rejected before `read_image`; this does not patch the wheel with EXR3.5.1's later fix. |
| Python media/ASR | PyAV18.1.0, CT2 4.8.3, Python3.11 | Qualified together; preserve private DLL trees and original package license metadata. No Python3.12/PyAV19 migration. |
| FFmpeg (D05) | Native8.1.1; OBS/PyAV8.1.2; BtbN CLI n8.1.3-24-g92611900bb-20261024 | Exact separate source/recipes and codec licenses. The static CLI stays on the8.1 maintenance branch; it is not bare stable8.1.3 or a new native ABI. Gyan binaries are not selected. |
| Qt (D07) | Matched Qt6.11.3/WebEngine MSVC2022 x64 kit | Full-player build, deployed browser lifetime and actual workbench interaction at 100%/150% scaling qualified together; no individual DLL replacement. |
| tlRender/ftk (D08) | tlRender0.27.1 / ftk0.19.0, C++20 | Exact upstream commits and matching gitlink verified. Complete compatibility patch, header-order adapter and public rebuild recipe retained in `tools/deps/`; real playback/EXR/seek/compare/OCIO and shutdown qualified. |
| Timeline (D10) | OTIO0.18 retained | Preserve the matched consumer API; newer ABI/API families need separate qualification. |
| Recording | Private OBS33 / obs-deps2026-10-23 | Complete application patches and exact vendor source pins. OBS DLLs stay outside the Qt process. |

The native inventory preserves 15 baseline Git checkout identities, full snapshots and complete local diffs, including pystring/yaml-cpp/zlib changes. The legacy tlRender/ftk ZIP-derived trees have snapshot SHA-256 identities, not upstream commits. The historical two patches alone do not recreate all local changes.

New native upstream archives and Sigstore metadata are retained. `native-maintenance-build-settings.json` records actual CMake options, and the complete header precedence adapter is preserved. New Imath/EXR/OIIO headers must precede old broad includes; use the same architecture and MSVC runtime and rebuild all consuming libraries/application together. See `OFFICIAL_BUILD.md` for portable configuration.

The native FFmpeg DLL matches installed vcpkg8.1.1#2 byte for byte; the exact port, 14 patches, ABI/SPDX and original source are retained. The standalone CLI uses BtbN's exact recipe and the matching129-member source cache, verified against the official whole-archive SHA. Its source/patch/codec and compiler runtime materials are kept separately from the native, OBS and PyAV builds. See `FFMPEG_SOURCE_OFFER.txt`. PyAV's vendor patch changes x264/x265 license-selection lists, so its reported LGPL banner cannot classify those external codecs as LGPL.

CT2's official wheel contains Intel OpenMP and NVIDIA cuDNN9.10.2.21 without their full notices. This release adds the Intel2025.3.0 package license/third-party text (matching DLL bytes) and official NVIDIA redistribution license/index. The retained upstream CT2 Windows build script identifies CUDA12.8.1, cuDNN9.10.2 and Intel2025.3 toolchain inputs. These vendor runtimes are not relicensed under CT2's MIT license.

Python OIIO's wheel, sdist, full source, complete LICENSE/THIRD-PARTY, and embedded EXR3.5.0/OCIO2.5.1 sources are preserved separately. Both actual import orders passed `PyOpenColorIO.GetVersion()==2.5.2` plus a CPU processor. OIIO's embedded OCIO2.5.1 does not replace PyOCIO's private2.5.2 DLL.

Source availability, an offline build environment and an independent machine rebuild are different conditions. `releaseBlockers` in the manifest lists required source/script gaps, while `limitations` lists validation/environment boundaries. An outstanding required source/script must be supplied, or its distributed component replaced/removed, before release. A disclaimer does not close that requirement. No independent clean-machine reconstruction of the full installer has been verified.

The release preserves12 complete Qt6.11.3 module source archives (717,132,340 bytes), all checked against official meta4 SHA-256. Three `Qt_6.11.3_*` notice files retain2,999 text origins/1,832 distinct full texts and725 explicit license references, with documented upstream metadata exceptions and ten exact-revision license supplements. `QT_6.11.3_SOURCE_NOTICE_INDEX.json` records source identities and coverage. OpenXR README and DEPS pin differences are retained explicitly; licenses from both pins are included. This is a source-based notice collection; no binary credits extraction or independent Qt rebuild is claimed. The official [Qt core sources](https://download.qt.io/online/qtsdkrepository/all_os/qt/qt6_6113_unix_line_endings_src/) and [matched WebEngine sources](https://download.qt.io/online/qtsdkrepository/all_os/extensions/qtwebengine/6113/src_doc_examples/) provide independent access paths.

The renderer archives contain764 verified tlRender Git blobs at `0a413e542c54bd5ce89f0b782b00e00982f20e1d` and1,110 verified ftk blobs at `51d9c3d06b608a4a9bd0a0eec54bbf6814699a71`. The ftk commit matches tlRender's gitlink. The local patch preserves explicit sequence FPS overrides and existing video suffix support; the former VP slice patch was retired after actual VP8/VP9 playback and seek checks. `subprocess.h` is retained at the commit pinned by tlRender's own build recipe, with its original embedded Unlicense. Full renderer legal notices are in `tlRender_0.27.1_ftk_0.19.0_NOTICES.txt`. These engineering changes are recorded here, not as user-facing release highlights.

To assemble a release, use a reviewed public application snapshot at version1.0.8. The private source map resolves local original files and is excluded from the ZIP:

```powershell
python tools/prepare_release_sources.py --application <public-source> --provenance docs/DEPENDENCY_PROVENANCE_1.0.8.json --local-map <private-local-source-map.json> --output <release-dir>/cgplay-corresponding-sources-1.0.8.zip
python tools/validate_release_sources.py --provenance docs/DEPENDENCY_PROVENANCE_1.0.8.json --bundle <release-dir>/cgplay-corresponding-sources-1.0.8.zip
```

The collector rejects credential/artifact/handoff/media/build files, missing QRC resources and reparse sources. It never clears an existing destination. The builder checks originals, keeps already compressed archives intact, and automatically splits dependency assets at about1,400MiB with a SHA sidecar for each ZIP and an `.assets.json` index. Download every indexed `-partNN.zip` alongside the primary ZIP to obtain all dependency sources. The validator checks every member across every part; the publisher requires all indexed parts and refuses assets at or above GitHub's2GiB limit before any GitHub changes.
