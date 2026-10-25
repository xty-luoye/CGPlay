#pragma once
#include "Protocol.h"
#include "RuntimeManifest.h"
#include <filesystem>
#include <atomic>
#include <vector>

namespace recorder {
class RecordingSession {
public:
    RecordingSession(RuntimeManifest& runtime, Protocol& protocol, bool synthetic);
    ~RecordingSession();
    void command(obs_data_t* command);
    void poll();
    void disconnect(const std::string& reason);
    bool finished() const { return terminal; }
    bool success() const { return state == "Completed"; }
private:
    void start(obs_data_t* plan);
    void initialize(obs_data_t* plan);
    void beginOutput();
    void stop(const std::string& reason = {});
    void cleanup();
    void transition(const std::string& value, const std::string& error = {});
    void audio(obs_data_t* settings);
    void crop(obs_data_t* settings, bool reapply = false);
    void validateIdentity();
    bool readySources();
    static void started(void*, calldata_t*);
    static void stopped(void*, calldata_t*);
    void sidecar();
    Data statistics();
    void watchdogLoop();
    void joinWatchdog();
    RuntimeManifest& runtime;
    Protocol& protocol;
    const bool synthetic;
    bool initialized = false, terminal = false, stoppingRequested = false, outputAccepted = false;
    bool splitTracks = false, systemEnabled = false, micEnabled = false;
    bool callbacksConnected = false;
    std::string state = "Idle", error, sourceKind;
    std::filesystem::path outputPath, sidecarPath;
    HANDLE reservation = INVALID_HANDLE_VALUE;
    HANDLE processHandle = nullptr;
    HWND window = nullptr;
    DWORD processId = 0;
    uint64_t processCreated = 0;
    std::wstring windowClass;
    std::string monitorId;
    HMONITOR monitor = nullptr;
    uint32_t width = 0, height = 0, fps = 30;
    long long geometryEpoch = -1, cropLeft = 0, cropTop = 0, cropWidth = 0, cropHeight = 0, clientWidth = 0, clientHeight = 0;
    bool geometryReady = false, cropEnabled = false;
    bool nativeOutput = false;
    uint32_t nativeWidth = 0, nativeHeight = 0;
    uint32_t croppedSourceWidth = 0, croppedSourceHeight = 0;
    uint64_t preparingAt = 0, stoppingAt = 0, lastStatus = 0;
    uint64_t statsAt = 0, statsFrames = 0;
    double encodedFps = 0.;
    uint64_t recordedFrames = 0, renderSkipped = 0, encodeSkipped = 0;
    std::string lastStatsJson;
    long long finalDuration = 0, finalBytes = 0;
    obs_scene_t* scene = nullptr;
    obs_sceneitem_t* item = nullptr;
    obs_source_t* source = nullptr;
    obs_source_t* system = nullptr;
    obs_source_t* mic = nullptr;
    obs_encoder_t* videoEncoder = nullptr;
    std::vector<obs_encoder_t*> audioEncoders;
    obs_output_t* output = nullptr;
    std::atomic<bool> startSignal{false}, stopSignal{false};
    std::atomic<int> stopCode{0};
    std::atomic<uint64_t> watchdogDeadline{0};
    std::atomic<bool> watchdogExit{false};
    std::mutex watchdogMutex;
    std::condition_variable watchdogChanged;
    std::thread watchdog;
};
}
