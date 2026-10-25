#include "Protocol.h"
#include "RecordingSession.h"
#include "RuntimeManifest.h"
#include "TestSources.h"
#include <util/base.h>
#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <util/dstr.h>
#include <util/windows/window-helpers.h>
#include <algorithm>

namespace {
using namespace recorder;
struct WindowChoice { HWND hwnd; DWORD pid; uint64_t created; std::string title, className, executable, actualClass; };
static BOOL CALLBACK collectWindow(HWND hwnd, LPARAM pointer) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    FILETIME created{}, exit{}, kernel{}, user{}; wchar_t actualClass[512];
    bool valid = process && GetProcessTimes(process, &created, &exit, &kernel, &user) && GetClassNameW(hwnd, actualClass, 512);
    if (process) CloseHandle(process);
    if (!valid) return TRUE;
    dstr title{}, name{}, executable{};
    ms_get_window_title(&title, hwnd); ms_get_window_class(&name, hwnd); ms_get_window_exe(&executable, hwnd);
    static_cast<std::vector<WindowChoice>*>(reinterpret_cast<void*>(pointer))->push_back({hwnd, pid,
        (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime,
        title.array ? title.array : "", name.array ? name.array : "", executable.array ? executable.array : "", utf8(actualClass)});
    dstr_free(&title); dstr_free(&name); dstr_free(&executable);
    return TRUE;
}
static void addChoice(obs_data_array_t* array, const char* label, const char* id, obs_data_t* plan = nullptr) {
    Data item(obs_data_create()); obs_data_set_string(item, "label", label); obs_data_set_string(item, "id", id);
    if (plan) obs_data_set_obj(item, "plan", plan);
    obs_data_array_push_back(array, item);
}
static Data inspectRuntime(RuntimeManifest& runtime, bool catalog, bool synthetic) {
    bool initialized = false;
    Data result(obs_data_create());
    try {
        if (!obs_startup("en-US", nullptr, nullptr)) throw std::runtime_error("probe-startup-failed");
        initialized = true;
        auto dataPath = utf8((runtime.root / L"data/libobs").wstring()) + "/"; obs_add_data_path(dataPath.c_str());
        auto graphics = utf8((runtime.root / L"libobs-d3d11.dll").wstring());
        obs_video_info video{}; video.graphics_module = graphics.c_str(); video.fps_num = 30; video.fps_den = 1;
        video.base_width = video.output_width = 320; video.base_height = video.output_height = 180;
        video.output_format = VIDEO_FORMAT_NV12; video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_PARTIAL;
        video.gpu_conversion = true; video.scale_type = OBS_SCALE_BICUBIC;
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) throw std::runtime_error("probe-d3d11-unavailable");
        obs_audio_info audio{48000, SPEAKERS_STEREO};
        if (!obs_reset_audio(&audio)) throw std::runtime_error("probe-audio-mixer-unavailable");
        runtime.loadModules();
        obs_data_set_int(result, "schema", 1); obs_data_set_bool(result, "ok", true);
        obs_data_set_string(result, "obsVersion", obs_get_version_string()); obs_data_set_int(result, "obsApi", obs_get_version());
        obs_data_set_string(result, "sourceCommit", "72e86655715d9a25d69ce446aa4dd44ba58e10d2");
        obs_data_set_string(result, "manifestSHA256", runtime.identity().c_str()); obs_data_set_string(result, "runtimeIdentity", runtime.identity().c_str());
        obs_data_set_int(result, "capturePatch", 1); obs_data_set_int(result, "wasapiPatch", 1); obs_data_set_string(result, "error", "");
        if (catalog) {
            for (auto* key : {"monitors", "windows", "systemAudio", "microphones"}) {
                auto* array = obs_data_array_create();
                if (synthetic) {
                    Data plan(obs_data_create()); obs_data_set_string(plan, "source", "synthetic");
                    addChoice(array, key == std::string("microphones") ? "Synthetic microphone 880 Hz" : "Synthetic source 440 Hz", key, (key == std::string("monitors") || key == std::string("windows")) ? plan.value : nullptr);
                } else {
                    const char* source = key == std::string("monitors") ? "monitor_capture" : key == std::string("windows") ? "window_capture" : key == std::string("systemAudio") ? "wasapi_output_capture" : "wasapi_input_capture";
                    auto* properties = obs_get_source_properties(source);
                    auto* property = properties ? obs_properties_get(properties, key == std::string("monitors") ? "monitor_id" : key == std::string("windows") ? "window" : "device_id") : nullptr;
                    if (!property) { if (properties) obs_properties_destroy(properties); obs_data_array_release(array); throw std::runtime_error("enumeration-property-unavailable"); }
                    std::vector<WindowChoice> windows;
                    if (key == std::string("windows")) EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&windows));
                    for (size_t i = 0; i < obs_property_list_item_count(property); ++i) {
                        const char* id = obs_property_list_item_string(property, i); const char* label = obs_property_list_item_name(property, i);
                        if (!id || !*id || std::string(id) == "default" || std::string(id) == "DUMMY" || obs_property_list_item_disabled(property, i)) continue;
                        Data plan(obs_data_create());
                        if (key == std::string("monitors")) { obs_data_set_string(plan, "source", "monitor"); obs_data_set_string(plan, "monitorId", id); addChoice(array, label, id, plan); }
                        else if (key == std::string("windows")) {
                            char *name = nullptr, *title = nullptr, *executable = nullptr;
                            ms_build_window_strings(id, &name, &title, &executable);
                            for (const auto& window : windows) if (name && title && executable && _stricmp(name, window.className.c_str()) == 0 && std::string(title) == window.title && _stricmp(executable, window.executable.c_str()) == 0) {
                                Data windowPlan(obs_data_create());
                                auto hwnd = std::to_string(reinterpret_cast<uintptr_t>(window.hwnd));
                                obs_data_set_string(windowPlan, "source", "window"); obs_data_set_string(windowPlan, "hwnd", hwnd.c_str());
                                obs_data_set_int(windowPlan, "pid", window.pid); obs_data_set_string(windowPlan, "processCreated", std::to_string(window.created).c_str());
                                obs_data_set_string(windowPlan, "windowClass", window.actualClass.c_str()); obs_data_set_string(windowPlan, "windowToken", id);
                                addChoice(array, label, (hwnd + ":" + std::to_string(window.created)).c_str(), windowPlan);
                            }
                            bfree(name); bfree(title); bfree(executable);
                        } else addChoice(array, label, id);
                    }
                    obs_properties_destroy(properties);
                }
                obs_data_set_array(result, key, array); obs_data_array_release(array);
            }
        }
        // Enumeration only uses source properties; no obs_source_create call.
        obs_shutdown(); initialized = false;
        return result;
    } catch (...) { if (initialized) obs_shutdown(); throw; }
}
static void printObject(obs_data_t* object) {
    std::string line = obs_data_get_json(object);
    line.erase(std::remove(line.begin(), line.end(), '\n'), line.end()); line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    if (line.size() > 65535) throw std::runtime_error("catalog-too-large");
    printf("%s\n", line.c_str()); fflush(stdout);
}
}

static void logMessage(int level, const char* format, va_list arguments, void*) {
    if (level > LOG_INFO) return;
    char buffer[8192]; vsnprintf(buffer, sizeof(buffer), format, arguments);
    fprintf(stderr, "[libobs] %s\n", buffer); fflush(stderr);
}
int main(int argc, char** argv) {
    using namespace recorder;
    bool probe = false;
    try {
        bool synthetic = false, clockTest = false, enumerate = false;
        if (argc == 2 && std::string(argv[1]) == "--synthetic") synthetic = true;
        else if (argc == 2 && std::string(argv[1]) == "--clock-selftest") clockTest = true;
        else if (argc >= 2 && argc <= 3 && (std::string(argv[1]) == "--enumerate" || std::string(argv[1]) == "--check-runtime")) {
            probe = true; enumerate = std::string(argv[1]) == "--enumerate";
            if (argc == 3) { if (!enumerate || std::string(argv[2]) != "--synthetic") throw std::runtime_error("unsupported-arguments"); synthetic = true; }
        }
        else if (argc != 1) throw std::runtime_error("unsupported-arguments");
        // Kept until process exit, never inherited by a mux child. Killing the
        // helper closes its last handle and kills only this session's children.
        HANDLE sessionJob = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!sessionJob || !SetInformationJobObject(sessionJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(sessionJob, GetCurrentProcess())) throw std::runtime_error("session-process-job-required");
        wchar_t executable[32768];
        if (!GetModuleFileNameW(nullptr, executable, 32768)) throw std::runtime_error("executable-path");
        RuntimeManifest runtime(std::filesystem::path(executable).parent_path());
        base_set_log_handler(logMessage, nullptr);
        runtime.verify(); // Search policy is established before the delayed import.
        if (clockTest) {
            bool passed = recordingClockSelfTest();
            printf("{\"clockSelfTest\":%s,\"twoHourVideoNs\":%llu,\"twoHourAudioNs\":%llu,\"backgroundMode\":true}\n",
                   passed ? "true" : "false", static_cast<unsigned long long>(recordingFrameTimeNs(216000, 30)),
                   static_cast<unsigned long long>(recordingFrameTimeNs(345600000, 48000)));
            return passed ? 0 : 1;
        }
        // OBS first checks ../../data/libobs relative to cwd. Anchor that
        // legacy lookup to the same verified private shader directory.
        if (!SetCurrentDirectoryW((runtime.root / L"data/libobs").c_str())) throw std::runtime_error("private-data-directory");
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) throw std::runtime_error("com-initialization");
        if (probe) { Data result = inspectRuntime(runtime, enumerate, synthetic); printObject(result); CoUninitialize(); return 0; }
        int result = 1;
        {
            Protocol protocol;
            RecordingSession session(runtime, protocol, synthetic);
            protocol.emit("ready", "Idle");
            bool disconnected = false;
            while (!session.finished()) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                std::string line;
                while (protocol.pop(line)) {
                    try {
                        if (line.empty()) throw std::runtime_error("empty-protocol-frame");
                        wide(line);
                        if (line.find('\0') != std::string::npos) throw std::runtime_error("nul-protocol-frame");
                        Data command(obs_data_create_from_json(line.c_str()));
                        if (!command.value) throw std::runtime_error("invalid-json");
                        if (protocol.accept(command)) session.command(command);
                    } catch (const std::exception& failure) {
                        protocol.emit("commandError", "Rejected", failure.what());
                    }
                }
                if ((protocol.failed() || protocol.ended()) && !disconnected) {
                    disconnected = true; session.disconnect(protocol.failed() ? "protocol-limit-or-truncated-frame" : "controller-disconnected");
                }
                session.poll();
                Sleep(10);
            }
            result = session.success() ? 0 : 1;
            protocol.close();
        }
        CoUninitialize();
        return result;
    } catch (const std::exception& failure) {
        if (probe) { printf("{\"schema\":1,\"ok\":false,\"error\":\"recording-runtime-check-failed\"}\n"); fflush(stdout); }
        fprintf(stderr, "CGPlayRecorder fatal: %s\n", failure.what());
        return 2;
    }
}
