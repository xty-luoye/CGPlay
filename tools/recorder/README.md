# CGPlayRecorder native backend

This directory builds the isolated native backend. The host panel and asynchronous
supervisor live in `src/ui/recording` and `src/services/recording`; MP4 publishing
and recovery validation live in `tools/cgplay/export/recording_finalize.py`.

## Build and runtime isolation

Windows x64, Visual Studio 2022 BuildTools, CMake 3.28+, Ninja. The default pinned
dependency is `E:/rjkf/CGPlay-deps/obs-33.0.0`. Run `build_recorder.ps1` through the
repository background wrapper. The standalone CMake target never modifies the
player CMake or copies OBS DLLs beside CGPlay.exe.

The original official OBS33 runtime and commit-addressed source archives remain
under that dependency directory. Commit:
`72e86655715d9a25d69ce446aa4dd44ba58e10d2`. The build creates import libraries from
the **same** official `obs.dll` and `w32-pthreads.dll` export tables. It compiles
only the two adapted capture modules, retaining the official core, D3D11/WinRT,
x264, AAC and muxer binaries. The helper's private directory is `stage/`.

`prepare_runtime.py` follows actual import tables, rejects frontend/Qt imports,
copies shaders/data and required private DLLs, and hashes the complete staged
payload. The helper verifies every listed file and rejects an unlisted native
binary. It uses a delayed OBS import, restricted DLL directories, explicit module
paths and a fixed allowlist: `obs-x264`, `obs-ffmpeg`, `win-capture`, `win-wasapi`.
It never scans user plugins or loads the OBS frontend. `--synthetic` additionally
registers only `cgplay_test_pattern` and `cgplay_test_tone`.

The reproducible patch scripts require exact OBS33 baseline hashes. Original
source files, unified diffs and before/after hashes are retained in the private
dependency's `patches/`. Capture patches remove game hooks, injection helpers and
compatibility-network initialization. WGC binds the exact HWND/PID, never rematches
a similar window, and exposes actual rendered-frame health. Monitor capture
likewise exposes health and rejects a changed monitor identity. WASAPI exposes
atomic initialization/invalidation health after real `IAudioClient::Start`, with
failure latched; a non-null OBS source is insufficient to start recording.

## Control protocol v1

Use inherited stdin/stdout pipes, UTF-8 NDJSON. stdout contains protocol objects
only; diagnostics use stderr. The host must asynchronously drain both streams.
One helper owns one UUID session. Each command contains `protocol:1`, `session`
(a UUID without braces), increasing integer `seq`, and `command`. Messages are
limited to 64KiB, incoming/outgoing queues to 32 messages; progress is coalesced.
Old session/sequence commands are ignored. Unknown fields/types and duplicate JSON
keys are rejected. Stop remains responsive under stdout backpressure: a separate
cancelable writer owns blocking writes, and both IPC threads are joined on exit.

Example **synthetic only**, launched with `CGPlayRecorder.exe --synthetic`:

```json
{"protocol":1,"session":"00000000-0000-4000-8000-000000000001","seq":1,"command":"start","plan":{"source":"synthetic","output":"E:/chosen/test.recording.00000000-0000-4000-8000-000000000001.mkv","width":1920,"height":1080,"fps":30,"systemEnabled":true,"micEnabled":true,"systemGain":1.0,"micGain":1.0}}
{"protocol":1,"session":"00000000-0000-4000-8000-000000000001","seq":2,"command":"pause"}
{"protocol":1,"session":"00000000-0000-4000-8000-000000000001","seq":3,"command":"resume"}
{"protocol":1,"session":"00000000-0000-4000-8000-000000000001","seq":4,"command":"setAudio","systemGain":0.5,"micMute":true}
{"protocol":1,"session":"00000000-0000-4000-8000-000000000001","seq":5,"command":"stop"}
```

Start plans:

| Field | Contract |
|---|---|
| `source` | `monitor`, `window`, `player`; `synthetic` requires the explicit test process flag |
| `output` | Absolute, new `.recording.<session>.mkv` in an existing non-reparse directory; existing file/sidecar is rejected |
| `width,height,fps` | Defaults 1920/1080/30; width divisible by 4, height by 2; axes at most 8192, canvas area at most 8192x4320, fps 1..120; fixed for a session. UI presets end at UHD 7680x4320/120 |
| `nativeResolution` | Optional boolean: derives physical capture-source dimensions, or the player crop dimensions, and pads width up to a multiple of 4 and height up to a multiple of 2. Keeps source pixels at 1:1; does not expose the original media-file resolution. Same 8K size bounds apply; geometry changes stop recording |
| `hwnd,pid,processCreated,windowClass,windowToken` | Required for window/player; HWND and process FILETIME are decimal strings to avoid JSON number precision loss; token must come from OBS selection properties |
| `monitorId` | Exact OBS monitor property value for monitor capture, not a display index |
| `systemEnabled,micEnabled` | Independent booleans, default false; enabled production sources require explicit `systemDeviceId` / `micDeviceId`; `default` is rejected |
| `systemGain,micGain,systemMute,micMute` | Gain 0..2 and independent mute; monitoring is disabled |
| `splitTracks` | Optional diagnostic tracks, default false; first track always mixes both enabled sources |
| `geometryEpoch,cropLeft,cropTop,cropWidth,cropHeight,clientWidth,clientHeight` | Required for player crop; immutable host geometry snapshot; each edge is rescaled independently from client to captured pixels; stale epochs ignored |
| `hdr` | True is rejected. Actual production monitor advanced-color state is also checked; window capture rechecks its current monitor |

`crop` accepts the same geometry fields with a newer epoch. A changed capture size
requires a matching new geometry snapshot, otherwise recording stops explicitly.
Controls overlaid on the viewer are part of its visible recording; the helper
does not promise to remove them. The host must collect geometry in the GUI thread.

Audio is 48kHz stereo. Default output is **one mixed AAC track** containing both
enabled sources. Optional tracks 2/3 carry system/microphone separately. Disabling
both devices opens neither device and writes a genuinely silent AAC mixer track
(the OBS AV muxer requires an audio encoder). H264 uses x264 veryfast/CRF20,
zerolatency, two-second keyframes. This backend does not claim HDR, native HDR
retention or hardware encoder support. It never re-applies OCIO to captured pixels.
High resolutions and frame rates are selectable limits, not a promise of sustained
realtime performance. The panel reports measured encoded throughput and render/
encode skipped-frame counters; reduce the selected format when overloaded.

Catalogs advertise `recordingConfigRevision:2`, `maxFps:120`, `nativeResolution:true`
and the size bounds. The updated panel rejects an older recording component.

Responses include protocol/session, output `seq`, `replyTo`, event, state, error,
`durationNs` and bytes. Duration is OBS's output-frame counter at the configured
rate, excluding recording pause; final published duration must be verified by the
host's media probe. `finalized` means normal output stop, release and `obs_shutdown`
returned, and synthetic source threads were joined; process exit confirms all IPC
threads joined too. The successful chain is Preparing -> Starting -> Recording
<-> Paused -> Stopping -> Finalizing -> Completed. Source/transport failures retain
MKV and report Recoverable. Stopping before the first encoded recording reports
Failed rather than publishing an empty successful video.

State/progress/finalized responses also contain a `stats` object with canvas size,
target FPS, native-size selection, encoder ID, rolling `encodedFps`, `encodedFrames`,
`renderSkippedFrames` and `encodeSkippedFrames`. These are libobs pipeline metrics,
not a decoded file-frame count: `send_interleaved` counts before the muxer's stop
timestamp cutoff. Final file timestamps and frame counts are validated separately.

## Shutdown, files and recovery

A non-inheritable Windows Job handle lives for the process lifetime and constrains
only this helper and its descendants, including `obs-ffmpeg-mux.exe`. Abnormal
helper/parent termination closes that handle and kills its mux descendant; no
process is killed by executable name. Normal stop waits for the OBS stop signal,
then releases output/encoders/global sources/scene/capture sources and calls
`obs_shutdown`, joins the watchdog, then closes and joins IPC threads.

After 15s without a stop signal, the helper requests force stop. An independent
watchdog covers blocking plugin/API/shutdown calls; initialization has a 30s bound
and stop/shutdown a 25s bound. Hard timeout is exit code **74**, a recoverable
forced termination, not a successful flush or join. The host still needs its own
supervisor timeout. Ordinary success is 0; handled failure/recoverable recording
is 1; configuration/runtime failure is 2.

The helper never deletes recorded MKV or overwrites an existing target. It creates
an atomic per-session JSON sidecar before capture and updates it during state
changes; sidecar update errors must not bypass stop or cleanup. File paths in
recovery metadata are untrusted inputs for a future host recovery implementation.
This backend intentionally does not remux or publish a final MP4. The host must
probe and remux to a new file, protect open media/original output identity, then
commit atomically. Forced termination can lose the final incomplete MKV cluster;
recoverability does not mean zero loss on crash.

## Verification and limits

`tests/test_recording_helper.py` runs the real private executable with generated
pixels and 440/880Hz sources. It checks real encoded frames, PTS/DTS, pause/stop,
default mix/gain/mute, optional tracks, static 1080p, crop epochs, path collisions,
invalid identities, HDR rejection, controller loss, pipe backpressure, source
loss injection and killed-helper MKV recovery. Output goes only beneath
`tests/artifacts/migration_1_0_8_20261025/recording_backend_validation/`.

Actual screen/window capture, real microphone/loopback permissions, DPI/fullscreen
WGC behavior, hot-plug devices and HDR-display rejection remain explicit device
integration items. Synthetic passing results never stand for those checks. Do not
start real capture or OBS UI merely to complete automated acceptance.

OBS/libobs is GPL-2.0-or-later; preserve its license, original pinned source and
patches when packaging. The actual FFmpeg/x264/VC runtime closure also needs its
corresponding notices and source/build provenance. The staged OBS license alone
is not a completed distributor license audit. No installer/release has been made
by this backend build.
