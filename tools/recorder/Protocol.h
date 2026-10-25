#pragma once
#include <windows.h>
#include <obs.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <initializer_list>
#include <condition_variable>

namespace recorder {
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
struct Data {
    obs_data_t* value = nullptr;
    explicit Data(obs_data_t* data = nullptr) : value(data) {}
    ~Data() { if (value) obs_data_release(value); }
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
    Data(Data&& other) noexcept : value(other.value) { other.value = nullptr; }
    operator obs_data_t*() const { return value; }
};
bool fieldType(obs_data_t*, const char*, obs_data_type, bool required = true);
void onlyFields(obs_data_t*, std::initializer_list<const char*> names);
std::string stringField(obs_data_t*, const char*, bool required = true);
long long integerField(obs_data_t*, const char*, long long low, long long high, long long fallback);
double numberField(obs_data_t*, const char*, double low, double high, double fallback);
bool boolField(obs_data_t*, const char*, bool fallback = false);
class Protocol {
public:
    Protocol();
    ~Protocol();
    Protocol(const Protocol&) = delete;
    bool pop(std::string& line);
    void emit(const char* event, const std::string& state, const std::string& error = {}, long long durationNs = 0, long long bytes = 0, int liveThreads = -1);
    bool accept(obs_data_t* command);
    bool failed() const { return overflow.load() || outputFailed.load(); }
    bool ended() const { return eof.load(); }
    const std::string& sessionId() const { return session; }
    void close();
private:
    void read();
    void write();
    std::thread reader, writer;
    std::atomic<bool> closing{false}, eof{false}, overflow{false};
    std::atomic<bool> readerExited{false}, writerExited{false};
    std::mutex mutex;
    std::deque<std::string> lines;
    std::mutex outputMutex;
    std::condition_variable outputChanged;
    std::deque<std::pair<bool, std::string>> outputLines;
    std::atomic<bool> outputFailed{false}, writerStop{false};
    bool draining = false, writing = false;
    std::string session;
    long long incomingSeq = 0, outgoingSeq = 0;
};
}
