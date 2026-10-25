#include "TestSources.h"
#include <obs.h>
#include <util/platform.h>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
#include <limits>
#include <stdexcept>

namespace recorder {
static std::atomic<int> liveThreads{0};
static std::atomic<uint64_t> videoFrames{0};
uint64_t recordingFrameTimeNs(uint64_t frames, uint32_t rate) {
    if (!rate) throw std::runtime_error("zero-media-rate");
    uint64_t seconds = frames / rate, fraction = frames % rate;
    if (seconds > std::numeric_limits<uint64_t>::max() / 1000000000ULL) throw std::runtime_error("media-time-overflow");
    uint64_t whole = seconds * 1000000000ULL, part = fraction * 1000000000ULL / rate;
    if (part > std::numeric_limits<uint64_t>::max() - whole) throw std::runtime_error("media-time-overflow");
    return whole + part;
}
bool recordingClockSelfTest() {
    constexpr uint64_t twoHours = 7200000000000ULL;
    for (uint32_t rate : {1U, 24U, 25U, 30U, 60U, 90U, 120U, 48000U}) {
        if (recordingFrameTimeNs(uint64_t(rate) * 7200, rate) != twoHours ||
            recordingFrameTimeNs(uint64_t(rate) * 7200 + 1, rate) <= twoHours) return false;
    }
    try { recordingFrameTimeNs(1, 0); return false; } catch (const std::runtime_error&) {}
    try { recordingFrameTimeNs(std::numeric_limits<uint64_t>::max(), 1); return false; } catch (const std::runtime_error&) {}
    return true;
}
struct Synthetic {
    obs_source_t* source;
    bool audio;
    bool still = false;
    uint32_t width = 320, height = 180;
    double frequency = 440;
    std::atomic<bool> stop{false};
    std::thread thread;
    Synthetic(obs_data_t* settings, obs_source_t* owner, bool tone) : source(owner), audio(tone) {
        width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
        height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
        frequency = obs_data_get_double(settings, "frequency");
        still = obs_data_get_bool(settings, "static");
        thread = std::thread([this] { produce(); });
    }
    ~Synthetic() { stop.store(true); if (thread.joinable()) thread.join(); }
    void produce() {
        liveThreads.fetch_add(1);
        const uint64_t epoch = os_gettime_ns();
        uint64_t count = 0;
        std::vector<uint8_t> pixels(audio ? 0 : size_t(width) * height * 4);
        std::vector<float> left(480), right(480);
        while (!stop.load()) {
            uint64_t timestamp;
            if (audio) {
                // One 10ms block. Timestamp units are ns, never audio samples.
                timestamp = epoch + recordingFrameTimeNs(count * 480, 48000);
                for (size_t i = 0; i < left.size(); ++i) {
                    float sample = .1F * static_cast<float>(std::sin(6.283185307179586 * frequency * (count * 480 + i) / 48000.0));
                    left[i] = right[i] = sample;
                }
                obs_source_audio frame{};
                frame.data[0] = reinterpret_cast<const uint8_t*>(left.data());
                frame.data[1] = reinterpret_cast<const uint8_t*>(right.data());
                frame.frames = 480; frame.samples_per_sec = 48000;
                frame.speakers = SPEAKERS_STEREO; frame.format = AUDIO_FORMAT_FLOAT_PLANAR;
                frame.timestamp = timestamp;
                obs_source_output_audio(source, &frame);
            } else {
                timestamp = epoch + recordingFrameTimeNs(count, 30);
                for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                    auto i = (size_t(y) * width + x) * 4;
                    const uint64_t phase = still ? 0 : count;
                    pixels[i] = static_cast<uint8_t>((x * 255 / width + phase * 3) % 256);
                    pixels[i + 1] = static_cast<uint8_t>(y * 255 / height);
                    pixels[i + 2] = static_cast<uint8_t>((phase * 5) % 256);
                    pixels[i + 3] = 255;
                }
                obs_source_frame frame{};
                frame.data[0] = pixels.data(); frame.linesize[0] = width * 4;
                frame.width = width; frame.height = height;
                frame.timestamp = timestamp; frame.format = VIDEO_FORMAT_BGRA; frame.full_range = true;
                obs_source_output_video(source, &frame);
                videoFrames.fetch_add(1);
            }
            ++count;
            // Match OBS's monotonic clock and precision. Windows coarse CV
            // waits can starve 10ms tone blocks and insert a silent mixer block.
            // Stop/join waits at most one source period (10ms audio, 33ms video).
            os_sleepto_ns(epoch + recordingFrameTimeNs(audio ? count * 480 : count, audio ? 48000 : 30));
        }
        if (!audio) obs_source_output_video(source, nullptr);
        liveThreads.fetch_sub(1);
    }
};
static const char* patternName(void*) { return "CGPlay deterministic test pattern"; }
static const char* toneName(void*) { return "CGPlay deterministic test audio"; }
static void* createPattern(obs_data_t* settings, obs_source_t* source) { return new Synthetic(settings, source, false); }
static void* createTone(obs_data_t* settings, obs_source_t* source) { return new Synthetic(settings, source, true); }
static void destroy(void* value) { delete static_cast<Synthetic*>(value); }
void registerTestSources() {
    obs_source_info video{};
    video.id = "cgplay_test_pattern"; video.type = OBS_SOURCE_TYPE_INPUT;
    video.output_flags = OBS_SOURCE_ASYNC_VIDEO;
    video.get_name = patternName; video.create = createPattern; video.destroy = destroy;
    obs_register_source(&video);
    obs_source_info audio{};
    audio.id = "cgplay_test_tone"; audio.type = OBS_SOURCE_TYPE_INPUT;
    audio.output_flags = OBS_SOURCE_AUDIO;
    audio.get_name = toneName; audio.create = createTone; audio.destroy = destroy;
    obs_register_source(&audio);
}
int liveTestSourceThreads() { return liveThreads.load(); }
uint64_t testVideoFrameCount() { return videoFrames.load(); }
}
