#pragma once
#include <cstdint>

namespace recorder {
// Registration is explicit and only allowed in --synthetic processes.
void registerTestSources();
int liveTestSourceThreads();
uint64_t testVideoFrameCount();
uint64_t recordingFrameTimeNs(uint64_t frames, uint32_t rate);
bool recordingClockSelfTest();
}
