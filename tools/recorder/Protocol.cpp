#include "Protocol.h"
#include <cmath>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <regex>

namespace recorder {
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("invalid-utf8");
    std::wstring output(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), size);
    return output;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("invalid-unicode");
    std::string output(size, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), size, nullptr, nullptr);
    return output;
}
bool fieldType(obs_data_t* data, const char* name, obs_data_type expected, bool required) {
    auto* item = obs_data_item_byname(data, name);
    if (!item) { if (required) throw std::runtime_error(std::string("missing-field:") + name); return false; }
    bool valid = obs_data_item_gettype(item) == expected;
    obs_data_item_release(&item);
    if (!valid) throw std::runtime_error(std::string("invalid-type:") + name);
    return true;
}
void onlyFields(obs_data_t* data, std::initializer_list<const char*> names) {
    std::set<std::string> allowed;
    for (auto* name : names) allowed.insert(name);
    auto* item = obs_data_first(data);
    while (item) {
        std::string name = obs_data_item_get_name(item);
        if (!allowed.count(name)) { obs_data_item_release(&item); throw std::runtime_error("unknown-field:" + name); }
        obs_data_item_next(&item);
    }
}
std::string stringField(obs_data_t* data, const char* name, bool required) {
    if (!fieldType(data, name, OBS_DATA_STRING, required)) return {};
    std::string value = obs_data_get_string(data, name);
    wide(value);
    if (value.size() > 4096 || (required && value.empty())) throw std::runtime_error(std::string("invalid-string:") + name);
    return value;
}
long long integerField(obs_data_t* data, const char* name, long long low, long long high, long long fallback) {
    if (!fieldType(data, name, OBS_DATA_NUMBER, false)) return fallback;
    auto* item = obs_data_item_byname(data, name);
    bool integer = obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
    obs_data_item_release(&item);
    long long value = obs_data_get_int(data, name);
    if (!integer || value < low || value > high) throw std::runtime_error(std::string("invalid-integer:") + name);
    return value;
}
double numberField(obs_data_t* data, const char* name, double low, double high, double fallback) {
    if (!fieldType(data, name, OBS_DATA_NUMBER, false)) return fallback;
    double value = obs_data_get_double(data, name);
    if (!std::isfinite(value) || value < low || value > high) throw std::runtime_error(std::string("invalid-number:") + name);
    return value;
}
bool boolField(obs_data_t* data, const char* name, bool fallback) {
    return fieldType(data, name, OBS_DATA_BOOLEAN, false) ? obs_data_get_bool(data, name) : fallback;
}
Protocol::Protocol() {
    writer = std::thread(&Protocol::write, this);
    try { reader = std::thread(&Protocol::read, this); }
    catch (...) { writerStop.store(true); outputChanged.notify_all(); writer.join(); throw; }
}
Protocol::~Protocol() { close(); }
void Protocol::close() {
    closing.store(true);
    if (reader.joinable()) {
        while (!readerExited.load()) { CancelSynchronousIo(reader.native_handle()); Sleep(1); }
        reader.join();
    }
    if (writer.joinable()) {
        {
            std::unique_lock lock(outputMutex);
            draining = true; outputChanged.notify_all();
            outputChanged.wait_for(lock, std::chrono::milliseconds(500), [this] { return (outputLines.empty() && !writing) || outputFailed.load(); });
        }
        writerStop.store(true); outputChanged.notify_all();
        while (!writerExited.load()) { CancelSynchronousIo(writer.native_handle()); Sleep(1); }
        writer.join();
    }
}
void Protocol::write() {
    struct Exit { std::atomic<bool>& flag; ~Exit() { flag.store(true); } } exited{writerExited};
    for (;;) {
        std::string line;
        {
            std::unique_lock lock(outputMutex);
            outputChanged.wait(lock, [this] { return writerStop.load() || draining || !outputLines.empty(); });
            if (writerStop.load() || (draining && outputLines.empty())) return;
            line = std::move(outputLines.front().second); outputLines.pop_front(); writing = true;
        }
        DWORD done = 0;
        while (done < line.size() && !writerStop.load()) {
            DWORD count = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data() + done, static_cast<DWORD>(line.size() - done), &count, nullptr) || !count) { outputFailed.store(true); break; }
            done += count;
        }
        {
            std::lock_guard lock(outputMutex); writing = false; outputChanged.notify_all();
        }
        if (outputFailed.load()) return;
    }
}
void Protocol::read() {
    struct Exit { std::atomic<bool>& flag; ~Exit() { flag.store(true); } } exited{readerExited};
    char buffer[4096];
    std::string line;
    while (!closing.load()) {
        DWORD count = 0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) || !count) break;
        for (DWORD i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::lock_guard lock(mutex);
                if (lines.size() >= 32) { overflow.store(true); eof.store(true); return; }
                lines.push_back(std::move(line)); line.clear();
            } else {
                line += buffer[i];
                if (line.size() > 65536) { overflow.store(true); eof.store(true); return; }
            }
        }
    }
    if (!line.empty() && !closing.load()) overflow.store(true); // truncated JSON frame
    eof.store(true);
}
bool Protocol::pop(std::string& line) {
    std::lock_guard lock(mutex);
    if (lines.empty()) return false;
    line = std::move(lines.front()); lines.pop_front(); return true;
}
bool Protocol::accept(obs_data_t* data) {
    onlyFields(data, {"protocol", "session", "seq", "command", "plan", "systemGain", "micGain", "systemMute", "micMute", "geometryEpoch", "cropLeft", "cropTop", "cropWidth", "cropHeight", "clientWidth", "clientHeight", "target"});
    if (integerField(data, "protocol", 1, 1, 0) != 1) throw std::runtime_error("protocol-version");
    std::string id = stringField(data, "session");
    if (!std::regex_match(id, std::regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"))) throw std::runtime_error("invalid-session");
    long long seq = integerField(data, "seq", 1, 9007199254740991LL, 0);
    if (!seq) throw std::runtime_error("missing-sequence");
    if (session.empty()) { if (stringField(data, "command") != "start") throw std::runtime_error("start-required"); session = id; }
    if (id != session || seq <= incomingSeq) return false;
    incomingSeq = seq;
    return true;
}
void Protocol::emit(const char* event, const std::string& state, const std::string& error, long long durationNs, long long bytes, int liveThreads, obs_data_t* stats) {
    Data data(obs_data_create());
    obs_data_set_int(data, "protocol", 1);
    obs_data_set_string(data, "session", session.c_str());
    obs_data_set_int(data, "seq", ++outgoingSeq);
    obs_data_set_int(data, "replyTo", incomingSeq);
    obs_data_set_string(data, "event", event);
    obs_data_set_string(data, "state", state.c_str());
    obs_data_set_string(data, "error", error.c_str());
    obs_data_set_int(data, "durationNs", durationNs);
    obs_data_set_int(data, "bytes", bytes);
    if (stats) obs_data_set_obj(data, "stats", stats);
    if (liveThreads >= 0) obs_data_set_int(data, "liveSourceThreads", liveThreads);
    std::string line = obs_data_get_json(data);
    line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    line += '\n';
    if (line.size() > 65536) throw std::runtime_error("protocol-output-size");
    if (outputFailed.load()) return;
    bool progress = std::string(event) == "progress";
    std::lock_guard lock(outputMutex);
    if (progress && !outputLines.empty() && outputLines.back().first) outputLines.back().second = std::move(line);
    else if (outputLines.size() >= 32) overflow.store(true);
    else outputLines.emplace_back(progress, std::move(line));
    outputChanged.notify_all();
}
}
