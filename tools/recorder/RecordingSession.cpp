#include "RecordingSession.h"
#include "TestSources.h"
#include <util/platform.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <iomanip>
#include <sstream>

namespace recorder {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
static std::wstring nativeFilePath(const fs::path& path) {
    auto value = fs::absolute(path).wstring();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
static uint64_t decimal(const std::string& text) {
    if (text.empty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) throw std::runtime_error("invalid-identity-number");
    size_t count = 0; auto result = std::stoull(text, &count);
    if (!result || count != text.size()) throw std::runtime_error("invalid-identity-number");
    return result;
}
static uint64_t processTime(HANDLE process) {
    FILETIME created, exit, kernel, user;
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) throw std::runtime_error("process-identity-unavailable");
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
static void activeEndpoint(const std::string& id) {
    if (id.empty() || id == "default") throw std::runtime_error("explicit-audio-device-required");
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    DWORD state = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDevice(wide(id).c_str(), &device)) || FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) throw std::runtime_error("audio-device-unavailable");
}
struct FindMonitor { std::wstring id; HMONITOR handle = nullptr; };
static BOOL CALLBACK findMonitor(HMONITOR handle, HDC, LPRECT, LPARAM parameter) {
    auto& find = *reinterpret_cast<FindMonitor*>(parameter);
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return TRUE;
    DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
    if (find.id == info.szDevice || (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && find.id == device.DeviceID)) { find.handle = handle; return FALSE; }
    return TRUE;
}
static void requireSdr(HMONITOR monitor) {
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) throw std::runtime_error("monitor-unavailable");
    UINT32 pathsCount = 0, modesCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathsCount, &modesCount) != ERROR_SUCCESS) throw std::runtime_error("display-color-state-unavailable");
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathsCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathsCount, paths.data(), &modesCount, modes.data(), nullptr) != ERROR_SUCCESS) throw std::runtime_error("display-color-state-unavailable");
    for (UINT32 i = 0; i < pathsCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || std::wstring(name.viewGdiDeviceName) != info.szDevice) continue;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(color), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) throw std::runtime_error("display-color-state-unavailable");
        if (color.advancedColorEnabled) throw std::runtime_error("hdr-capture-not-supported");
        return;
    }
    throw std::runtime_error("display-color-state-unavailable");
}
RecordingSession::RecordingSession(RuntimeManifest& r, Protocol& p, bool test) : runtime(r), protocol(p), synthetic(test) {
    watchdog = std::thread(&RecordingSession::watchdogLoop, this);
}
RecordingSession::~RecordingSession() { cleanup(); }
void RecordingSession::watchdogLoop() {
    std::unique_lock lock(watchdogMutex);
    while (!watchdogExit.load()) {
        uint64_t deadline = watchdogDeadline.load();
        if (deadline && GetTickCount64() >= deadline) {
            lock.unlock();
            // Do not enter DLL detach or release a live callback after a stuck
            // third-party stop/shutdown. The private Job kills the mux child.
            TerminateProcess(GetCurrentProcess(), 74); // recoverable hard timeout
            return;
        }
        watchdogChanged.wait_for(lock, std::chrono::milliseconds(50));
    }
}
void RecordingSession::joinWatchdog() {
    watchdogExit.store(true); watchdogChanged.notify_all();
    if (watchdog.joinable()) watchdog.join();
}
void RecordingSession::transition(const std::string& value, const std::string& failure) {
    state = value;
    if (!failure.empty()) error = failure;
    try { sidecar(); }
    catch (const std::exception& failure) {
        // Disk/full/permission failure must never bypass stop, flush or joins.
        if (error.empty()) error = failure.what();
    }
    protocol.emit("state", state, error, finalDuration, finalBytes);
}
void RecordingSession::sidecar() {
    if (sidecarPath.empty()) return;
    Data value(obs_data_create());
    obs_data_set_int(value, "schema", 1);
    obs_data_set_string(value, "session", protocol.sessionId().c_str());
    obs_data_set_string(value, "state", state.c_str());
    obs_data_set_string(value, "error", error.c_str());
    obs_data_set_string(value, "mkv", utf8(outputPath.wstring()).c_str());
    obs_data_set_string(value, "runtime", utf8(runtime.root.wstring()).c_str());
    obs_data_set_int(value, "durationNs", finalDuration);
    obs_data_set_int(value, "bytes", finalBytes);
    HANDLE recordedFile = CreateFileW(nativeFilePath(outputPath).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (recordedFile != INVALID_HANDLE_VALUE) {
        FILE_ID_INFO fileId{};
        if (GetFileInformationByHandleEx(recordedFile, FileIdInfo, &fileId, sizeof(fileId))) {
            Data identity(obs_data_create());
            obs_data_set_string(identity, "volume", std::to_string(fileId.VolumeSerialNumber).c_str());
            std::ostringstream id;
            for (unsigned char byte : fileId.FileId.Identifier) id << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
            obs_data_set_string(identity, "fileId", id.str().c_str());
            obs_data_set_obj(value, "mkvIdentity", identity);
        }
        CloseHandle(recordedFile);
    }
    const fs::path temporary = sidecarPath.wstring() + L".new";
    HANDLE file = CreateFileW(nativeFilePath(temporary).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("sidecar-create-failed");
    std::string data = obs_data_get_json(value);
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(nativeFilePath(temporary).c_str(), nativeFilePath(sidecarPath).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(nativeFilePath(temporary).c_str()); throw std::runtime_error("sidecar-commit-failed"); }
}
void RecordingSession::start(obs_data_t* plan) {
    if (state != "Idle") throw std::runtime_error("session-already-started");
    transition("Preparing");
    preparingAt = os_gettime_ns();
    watchdogDeadline.store(GetTickCount64() + 30000);
    try { initialize(plan); }
    catch (const std::exception& failure) { error = failure.what(); cleanup(); terminal = true; transition("Failed", error); }
}
void RecordingSession::initialize(obs_data_t* plan) {
    onlyFields(plan, {"source", "output", "width", "height", "fps", "sourceWidth", "sourceHeight", "testStatic", "systemEnabled", "micEnabled", "systemDeviceId", "micDeviceId", "systemGain", "micGain", "systemMute", "micMute", "splitTracks", "hwnd", "pid", "processCreated", "windowClass", "windowToken", "monitorId", "geometryEpoch", "cropLeft", "cropTop", "cropWidth", "cropHeight", "clientWidth", "clientHeight", "hdr"});
    sourceKind = stringField(plan, "source");
    if (sourceKind != "synthetic" && sourceKind != "monitor" && sourceKind != "window" && sourceKind != "player") throw std::runtime_error("source-not-supported");
    if (synthetic != (sourceKind == "synthetic")) throw std::runtime_error("synthetic-source-allowlist");
    if (boolField(plan, "hdr")) throw std::runtime_error("hdr-capture-not-supported");
    width = static_cast<uint32_t>(integerField(plan, "width", 160, 3840, 1920));
    height = static_cast<uint32_t>(integerField(plan, "height", 90, 2160, 1080));
    fps = static_cast<uint32_t>(integerField(plan, "fps", 1, 60, 30));
    if ((width & 1) || (height & 1) || uint64_t(width) * height * fps > 1920ULL * 1080 * 60) throw std::runtime_error("unsupported-encoding-size-rate");
    outputPath = fs::path(wide(stringField(plan, "output")));
    if (!outputPath.is_absolute() || outputPath.extension() != L".mkv" || outputPath.filename().wstring().find(wide(".recording." + protocol.sessionId() + ".mkv")) == std::wstring::npos) throw std::runtime_error("unique-session-mkv-required");
    outputPath = outputPath.lexically_normal();
    fs::path check;
    for (const auto& part : outputPath.parent_path()) {
        check /= part;
        DWORD attributes = GetFileAttributesW(nativeFilePath(check).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("output-parent-missing-or-reparse");
    }
    const fs::path requestedSidecar = outputPath.wstring() + L".json";
    if (fs::exists(outputPath) || fs::exists(requestedSidecar)) throw std::runtime_error("output-already-exists");
    reservation = CreateFileW(nativeFilePath(outputPath).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE) throw std::runtime_error("output-create-failed");
    sidecarPath = requestedSidecar;
    sidecar(); // Establish recoverable metadata before starting any capture.
    systemEnabled = boolField(plan, "systemEnabled"); micEnabled = boolField(plan, "micEnabled");
    splitTracks = boolField(plan, "splitTracks");
    if (!synthetic) {
        if (sourceKind == "window" || sourceKind == "player") {
            window = reinterpret_cast<HWND>(static_cast<uintptr_t>(decimal(stringField(plan, "hwnd"))));
            processId = static_cast<DWORD>(integerField(plan, "pid", 1, MAXDWORD, 0));
            processCreated = decimal(stringField(plan, "processCreated"));
            windowClass = wide(stringField(plan, "windowClass"));
            processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
            validateIdentity();
            monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        } else {
            monitorId = stringField(plan, "monitorId");
            FindMonitor find{wide(monitorId)};
            EnumDisplayMonitors(nullptr, nullptr, findMonitor, reinterpret_cast<LPARAM>(&find));
            monitor = find.handle;
        }
        requireSdr(monitor);
        if (systemEnabled) activeEndpoint(stringField(plan, "systemDeviceId"));
        if (micEnabled) activeEndpoint(stringField(plan, "micDeviceId"));
    }
    std::string config = utf8((outputPath.parent_path() / wide(".recorder-config-" + protocol.sessionId())).wstring());
    // OBS config directories are session-owned; plugins never scan user paths.
    if (!obs_startup("en-US", config.c_str(), nullptr)) throw std::runtime_error("obs-startup-failed");
    initialized = true;
    auto data = utf8((runtime.root / L"data/libobs").wstring()) + "/";
    obs_add_data_path(data.c_str());
    std::string graphics = utf8((runtime.root / L"libobs-d3d11.dll").wstring());
    obs_video_info video{};
    video.graphics_module = graphics.c_str(); video.fps_num = fps; video.fps_den = 1;
    video.base_width = video.output_width = width; video.base_height = video.output_height = height;
    video.output_format = VIDEO_FORMAT_NV12; video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_PARTIAL;
    video.gpu_conversion = true; video.scale_type = OBS_SCALE_BICUBIC;
    if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) throw std::runtime_error("video-initialization-failed");
    obs_audio_info audioInfo{48000, SPEAKERS_STEREO};
    if (!obs_reset_audio(&audioInfo)) throw std::runtime_error("audio-initialization-failed");
    runtime.loadModules();
    if (synthetic) registerTestSources();
    scene = obs_scene_create_private("CGPlay Recording Scene");
    if (!scene) throw std::runtime_error("scene-create-failed");
    Data settings(obs_data_create());
    if (synthetic) {
        obs_data_set_int(settings, "width", integerField(plan, "sourceWidth", 16, 3840, width));
        obs_data_set_int(settings, "height", integerField(plan, "sourceHeight", 16, 2160, height));
        obs_data_set_bool(settings, "static", boolField(plan, "testStatic"));
        source = obs_source_create_private("cgplay_test_pattern", "synthetic-video", settings);
    } else if (sourceKind == "monitor") {
        obs_data_set_string(settings, "monitor_id", monitorId.c_str());
        obs_data_set_int(settings, "method", 2); // WGC; do not fall back to BitBlt.
        obs_data_set_bool(settings, "force_sdr", false);
        obs_data_set_bool(settings, "capture_cursor", true);
        source = obs_source_create_private("monitor_capture", "selected-monitor", settings);
    } else {
        obs_data_set_string(settings, "window", stringField(plan, "windowToken").c_str());
        obs_data_set_int(settings, "cgplay_hwnd", reinterpret_cast<intptr_t>(window));
        obs_data_set_int(settings, "cgplay_pid", processId);
        obs_data_set_int(settings, "method", 2);
        obs_data_set_bool(settings, "client_area", true);
        obs_data_set_bool(settings, "capture_audio", false);
        obs_data_set_bool(settings, "force_sdr", false);
        obs_data_set_bool(settings, "cursor", true);
        source = obs_source_create_private("window_capture", "strict-window", settings);
    }
    if (!source) throw std::runtime_error("capture-source-create-failed");
    item = obs_scene_add(scene, source);
    if (!item) throw std::runtime_error("scene-item-create-failed");
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
    vec2 bounds{static_cast<float>(width), static_cast<float>(height)};
    vec2 position{width * .5F, height * .5F};
    obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
    obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
    obs_sceneitem_set_bounds(item, &bounds); obs_sceneitem_set_pos(item, &position);
    // A player crop is set before the scene becomes an active output source.
    cropEnabled = sourceKind == "player" || (synthetic && obs_data_has_user_value(plan, "cropWidth"));
    if (cropEnabled) crop(plan); else geometryReady = true;
    obs_set_output_source(0, obs_scene_get_source(scene));
    auto makeAudio = [&](bool enabled, const char* device, const char* id, const char* name, double frequency, uint32_t mixers) -> obs_source_t* {
        if (!enabled) return nullptr;
        Data value(obs_data_create());
        obs_data_set_string(value, "device_id", synthetic ? "synthetic" : stringField(plan, device).c_str());
        obs_data_set_double(value, "frequency", frequency);
        obs_data_set_bool(value, "use_device_timing", true);
        auto* result = obs_source_create_private(synthetic ? "cgplay_test_tone" : id, name, value);
        if (!result) throw std::runtime_error("audio-source-create-failed");
        obs_source_set_audio_mixers(result, mixers);
        obs_source_set_monitoring_type(result, OBS_MONITORING_TYPE_NONE);
        return result;
    };
    system = makeAudio(systemEnabled, "systemDeviceId", "wasapi_output_capture", "system-audio", 440, splitTracks ? 3 : 1);
    mic = makeAudio(micEnabled, "micDeviceId", "wasapi_input_capture", "microphone", 880, splitTracks ? 5 : 1);
    if (system) obs_set_output_source(1, system);
    if (mic) obs_set_output_source(2, mic);
    audio(plan);
    sidecar();
}
void RecordingSession::validateIdentity() {
    if (!window) return;
    DWORD pid = 0;
    wchar_t actualClass[512];
    if (!processHandle || WaitForSingleObject(processHandle, 0) != WAIT_TIMEOUT || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GetWindowThreadProcessId(window, &pid) || pid != processId || processTime(processHandle) != processCreated ||
        !GetClassNameW(window, actualClass, 512) || windowClass != actualClass) throw std::runtime_error("window-identity-lost");
}
bool RecordingSession::readySources() {
    if (!source) return false;
    bool ready = true;
    if (!synthetic) {
        calldata_t data{};
        bool called = proc_handler_call(obs_source_get_proc_handler(source), "cgplay_capture_health", &data);
        auto status = calldata_int(&data, "state");
        ready = called && status == 1 && calldata_int(&data, "frames") > 0;
        calldata_free(&data);
        if (!called || status < 0) throw std::runtime_error("video-capture-unhealthy");
    } else if (synthetic) ready = testVideoFrameCount() > 1;
    ready = ready && obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0;
    for (auto* audioSource : {system, mic}) if (audioSource && !synthetic) {
        calldata_t data{};
        bool called = proc_handler_call(obs_source_get_proc_handler(audioSource), "cgplay_wasapi_health", &data);
        auto status = calldata_int(&data, "state"); calldata_free(&data);
        if (!called || status < 0) throw std::runtime_error("audio-source-unhealthy");
        ready = ready && status == 1;
    }
    if (cropEnabled && ready && geometryReady &&
        (croppedSourceWidth != obs_source_get_width(source) || croppedSourceHeight != obs_source_get_height(source))) throw std::runtime_error("player-geometry-refresh-required");
    if (cropEnabled && ready && !geometryReady) {
        // The first crop snapshot is delayed until WGC has a real size.
        Data geometry(obs_data_create());
        obs_data_set_int(geometry, "geometryEpoch", geometryEpoch);
        obs_data_set_int(geometry, "cropLeft", cropLeft); obs_data_set_int(geometry, "cropTop", cropTop);
        obs_data_set_int(geometry, "cropWidth", cropWidth); obs_data_set_int(geometry, "cropHeight", cropHeight);
        obs_data_set_int(geometry, "clientWidth", clientWidth); obs_data_set_int(geometry, "clientHeight", clientHeight);
        crop(geometry, true);
    }
    return ready && geometryReady;
}
void RecordingSession::beginOutput() {
    Data video(obs_data_create());
    obs_data_set_string(video, "rate_control", "CRF"); obs_data_set_int(video, "crf", 20);
    obs_data_set_string(video, "preset", "veryfast"); obs_data_set_int(video, "keyint_sec", 2);
    // Recording is realtime: avoid reordered B-frames/lookahead whose initial
    // missing DTS and delayed tail complicate recovery of an interrupted MKV.
    obs_data_set_string(video, "tune", "zerolatency");
    videoEncoder = obs_video_encoder_create("obs_x264", "cgplay-h264", video, nullptr);
    if (!videoEncoder) throw std::runtime_error("video-encoder-create-failed");
    obs_encoder_set_video(videoEncoder, obs_get_video());
    // OBS's AV muxer requires an audio encoder. An empty global mixer is a
    // genuine silent track and opens no microphone or loopback device.
    size_t audioCount = splitTracks && (systemEnabled || micEnabled) ? 3 : 1;
    for (size_t i = 0; i < audioCount; ++i) {
        Data audioSettings(obs_data_create()); obs_data_set_int(audioSettings, "bitrate", 160);
        auto name = "cgplay-aac-" + std::to_string(i);
        auto* encoder = obs_audio_encoder_create("ffmpeg_aac", name.c_str(), audioSettings, i, nullptr);
        if (!encoder) throw std::runtime_error("audio-encoder-create-failed");
        audioEncoders.push_back(encoder); obs_encoder_set_audio(encoder, obs_get_audio());
    }
    Data settings(obs_data_create());
    obs_data_set_string(settings, "path", utf8(nativeFilePath(outputPath)).c_str());
    obs_data_set_string(settings, "muxer_settings", "cluster_time_limit=1000 cluster_size_limit=1048576");
    output = obs_output_create("ffmpeg_muxer", "cgplay-mkv", settings, nullptr);
    if (!output) throw std::runtime_error("output-create-failed");
    obs_output_set_video_encoder(output, videoEncoder);
    for (size_t i = 0; i < audioEncoders.size(); ++i) obs_output_set_audio_encoder(output, audioEncoders[i], i);
    auto* handler = obs_output_get_signal_handler(output);
    signal_handler_connect(handler, "start", started, this);
    signal_handler_connect(handler, "stop", stopped, this);
    callbacksConnected = true;
    if (reservation != INVALID_HANDLE_VALUE) { CloseHandle(reservation); reservation = INVALID_HANDLE_VALUE; }
    transition("Starting");
    outputAccepted = obs_output_start(output);
    if (!outputAccepted) {
        const char* detail = obs_output_get_last_error(output);
        throw std::runtime_error(std::string("output-start-failed:") + (detail ? detail : "unknown-output-start-failure"));
    }
}
void RecordingSession::started(void* pointer, calldata_t*) { static_cast<RecordingSession*>(pointer)->startSignal.store(true, std::memory_order_release); }
void RecordingSession::stopped(void* pointer, calldata_t* data) {
    auto* self = static_cast<RecordingSession*>(pointer);
    self->stopCode.store(static_cast<int>(calldata_int(data, "code")), std::memory_order_relaxed);
    self->stopSignal.store(true, std::memory_order_release);
}
void RecordingSession::audio(obs_data_t* settings) {
    double systemGain = numberField(settings, "systemGain", 0, 2, system ? obs_source_get_volume(system) : 1);
    double micGain = numberField(settings, "micGain", 0, 2, mic ? obs_source_get_volume(mic) : 1);
    bool systemMute = boolField(settings, "systemMute", system && obs_source_muted(system));
    bool micMute = boolField(settings, "micMute", mic && obs_source_muted(mic));
    if (system) { obs_source_set_volume(system, static_cast<float>(systemGain)); obs_source_set_muted(system, systemMute); }
    if (mic) { obs_source_set_volume(mic, static_cast<float>(micGain)); obs_source_set_muted(mic, micMute); }
}
void RecordingSession::crop(obs_data_t* settings, bool reapply) {
    long long epoch = integerField(settings, "geometryEpoch", 0, 9007199254740991LL, 0);
    if (epoch < geometryEpoch || (epoch == geometryEpoch && !reapply)) return;
    long long left = integerField(settings, "cropLeft", 0, 32768, 0), top = integerField(settings, "cropTop", 0, 32768, 0);
    long long w = integerField(settings, "cropWidth", 1, 32768, 0), h = integerField(settings, "cropHeight", 1, 32768, 0);
    long long cw = integerField(settings, "clientWidth", 1, 32768, 0), ch = integerField(settings, "clientHeight", 1, 32768, 0);
    if (!w || !h || !cw || !ch || left + w > cw || top + h > ch) throw std::runtime_error("invalid-player-crop");
    auto sw = source ? obs_source_get_width(source) : 0, sh = source ? obs_source_get_height(source) : 0;
    auto commitSnapshot = [&] {
        cropLeft = left; cropTop = top; cropWidth = w; cropHeight = h; clientWidth = cw; clientHeight = ch; geometryEpoch = epoch;
    };
    if (!sw || !sh) { commitSnapshot(); geometryReady = false; return; }
    auto edge = [](long long n, long long total, uint32_t size) { return static_cast<int>(std::clamp(std::llround(double(n) * size / total), 0LL, static_cast<long long>(size))); };
    obs_sceneitem_crop rectangle{edge(left, cw, sw), edge(top, ch, sh), static_cast<int>(sw) - edge(left + w, cw, sw), static_cast<int>(sh) - edge(top + h, ch, sh)};
    if (rectangle.left + rectangle.right >= static_cast<int>(sw) || rectangle.top + rectangle.bottom >= static_cast<int>(sh)) throw std::runtime_error("empty-player-crop");
    obs_sceneitem_set_crop(item, &rectangle); commitSnapshot(); geometryReady = true;
    croppedSourceWidth = sw; croppedSourceHeight = sh;
}
void RecordingSession::stop(const std::string& reason) {
    if (terminal || stoppingRequested) return;
    stoppingRequested = true;
    if (!reason.empty()) error = reason;
    stoppingAt = os_gettime_ns();
    watchdogDeadline.store(GetTickCount64() + 25000);
    transition("Stopping");
    if (outputAccepted && output && !stopSignal.load(std::memory_order_acquire)) {
        // Pause stops only the output clock; OBS stop owns final packet drain.
        obs_output_stop(output);
    } else {
        if (error.empty()) error = "stopped-before-recording";
        transition("Finalizing");
        cleanup(); terminal = true;
        transition("Failed", error);
        protocol.emit("finalized", state, error, finalDuration, finalBytes, liveTestSourceThreads());
    }
}
void RecordingSession::disconnect(const std::string& reason) { stop(reason); }
void RecordingSession::command(obs_data_t* value) {
    auto name = stringField(value, "command");
    if (name == "start") { fieldType(value, "plan", OBS_DATA_OBJECT); Data plan(obs_data_get_obj(value, "plan")); start(plan); return; }
    if (name == "stop" || name == "shutdown") { stop(); return; }
    if (terminal || stoppingRequested) throw std::runtime_error("session-not-running");
    if (name == "pause" || name == "resume") {
        bool pause = name == "pause";
        if ((pause && state == "Paused") || (!pause && state == "Recording")) return;
        if ((state != "Recording" && state != "Paused") || !obs_output_can_pause(output) || !obs_output_pause(output, pause)) throw std::runtime_error("pause-not-available");
        transition(pause ? "Paused" : "Recording");
    } else if (name == "setAudio") { audio(value); protocol.emit("audio", state); }
    else if (name == "crop" && cropEnabled) { crop(value); protocol.emit("geometry", state); }
    else if (name == "injectFailure" && synthetic) {
        auto target = stringField(value, "target");
        if (target == "blocked-stop") {
            // Explicit test-only fault: emulate a plugin blocked in stop.
            watchdogDeadline.store(GetTickCount64() + 2000);
            transition("Stopping", "synthetic-blocked-stop");
            Sleep(INFINITE);
        } else stop("synthetic-" + target + "-lost");
    }
    else throw std::runtime_error("unknown-command");
}
void RecordingSession::poll() {
    if (terminal || state == "Idle") return;
    try {
        if (!stoppingRequested) {
            validateIdentity();
            if (window) {
                monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
                if (!monitor) throw std::runtime_error("window-monitor-unavailable");
            }
            if (!synthetic && monitor) requireSdr(monitor);
            if (!error.empty()) { stop(error); return; }
            bool ready = readySources();
            if (state == "Preparing" && ready) beginOutput();
            if ((state == "Preparing" || state == "Starting") && os_gettime_ns() - preparingAt > 15000000000ULL) throw std::runtime_error("source-start-timeout");
            if (state == "Starting" && startSignal.load(std::memory_order_acquire)) { transition("Recording"); watchdogDeadline.store(0); }
        }
        if (output) {
            finalDuration = static_cast<long long>(recordingFrameTimeNs(static_cast<uint64_t>(obs_output_get_total_frames(output)), fps));
            finalBytes = static_cast<long long>(obs_output_get_total_bytes(output));
        }
        if (stopSignal.load(std::memory_order_acquire)) {
            if (stopCode.load() != OBS_OUTPUT_SUCCESS && error.empty()) error = "output-stop-error:" + std::to_string(stopCode.load());
            transition("Finalizing");
            cleanup(); terminal = true;
            std::error_code fileError;
            if (fs::exists(outputPath, fileError)) finalBytes = static_cast<long long>(fs::file_size(outputPath, fileError));
            transition(error.empty() ? "Completed" : "Recoverable", error);
            protocol.emit("finalized", state, error, finalDuration, finalBytes, liveTestSourceThreads());
        } else if (stoppingRequested && os_gettime_ns() - stoppingAt > 15000000000ULL) {
            if (output) obs_output_force_stop(output);
            error = "output-stop-timeout";
            // Force stop still emits stop; never release a live output callback.
            // The independent deadline also covers force_stop itself blocking.
        }
        if (!terminal && os_gettime_ns() - lastStatus >= 250000000ULL) {
            if (os_gettime_ns() / 1000000000ULL != lastStatus / 1000000000ULL) {
                try { sidecar(); } catch (const std::exception& failure) { stop(failure.what()); return; }
            }
            lastStatus = os_gettime_ns(); protocol.emit("progress", state, error, finalDuration, finalBytes);
        }
    } catch (const std::exception& failure) { stop(failure.what()); }
}
void RecordingSession::cleanup() {
    if (initialized && !watchdogDeadline.load()) watchdogDeadline.store(GetTickCount64() + 25000);
    if (output) {
        if (outputAccepted && obs_output_active(output) && !stopSignal.load(std::memory_order_acquire)) {
            obs_output_force_stop(output);
            // Only emergency unwinding uses this helper-local bounded drain.
            for (int i = 0; i < 1000 && !stopSignal.load(std::memory_order_acquire); ++i) Sleep(10);
        }
        if (callbacksConnected) {
            auto* handler = obs_output_get_signal_handler(output);
            signal_handler_disconnect(handler, "start", started, this);
            signal_handler_disconnect(handler, "stop", stopped, this);
            callbacksConnected = false;
        }
        obs_output_release(output); output = nullptr;
    }
    if (videoEncoder) { obs_encoder_release(videoEncoder); videoEncoder = nullptr; }
    for (auto* encoder : audioEncoders) obs_encoder_release(encoder);
    audioEncoders.clear();
    if (initialized) for (uint32_t channel = 0; channel < 3; ++channel) obs_set_output_source(channel, nullptr);
    if (scene) { obs_scene_release(scene); scene = nullptr; item = nullptr; }
    if (source) { obs_source_release(source); source = nullptr; }
    if (system) { obs_source_release(system); system = nullptr; }
    if (mic) { obs_source_release(mic); mic = nullptr; }
    if (initialized) { obs_shutdown(); initialized = false; }
    if (reservation != INVALID_HANDLE_VALUE) { CloseHandle(reservation); reservation = INVALID_HANDLE_VALUE; }
    if (processHandle) { CloseHandle(processHandle); processHandle = nullptr; }
    joinWatchdog();
}
}
