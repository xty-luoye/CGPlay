// CGPlay TlStubs.cpp — Stub implementations for tlRender symbols
// that are declared in the headers but only available when building
// tlRender from source.  These symbols are needed by Qt6's MOC
// generated code for type comparison, and by the TlViewport and
// PlaybackController classes.

#include <tlRender/Timeline/BackgroundOptions.h>
#include <tlRender/Timeline/ColorOptions.h>
#include <tlRender/Timeline/CompareOptions.h>
#include <tlRender/Timeline/DisplayOptions.h>
#include <tlRender/Timeline/ForegroundOptions.h>
#include <tlRender/Timeline/Player.h>
#include <tlRender/Timeline/PlayerOptions.h>
#include <tlRender/Timeline/Timeline.h>
#include <tlRender/UI/Viewport.h>
#include <tlRender/GL/Render.h>
#include "playback/PlaybackController.h"
#include <opentime/rationalTime.h>
#include <opentime/timeRange.h>

namespace tl {

// ── Operator stubs for Qt MOC ──────────────────────────────────────────────

bool BackgroundOptions::operator==(const BackgroundOptions&) const { return true; }
bool BackgroundOptions::operator!=(const BackgroundOptions&) const { return false; }

bool OCIOOptions::operator==(const OCIOOptions& o) const {
    return enabled == o.enabled && config == o.config && fileName == o.fileName &&
        input == o.input && display == o.display && view == o.view && look == o.look;
}
bool OCIOOptions::operator!=(const OCIOOptions& o) const { return !(*this == o); }

bool LUTOptions::operator==(const LUTOptions&) const { return true; }
bool LUTOptions::operator!=(const LUTOptions&) const { return false; }

bool CompareOptions::operator==(const CompareOptions&) const { return true; }
bool CompareOptions::operator!=(const CompareOptions&) const { return false; }

bool DisplayOptions::operator==(const DisplayOptions& o) const {
    return channels == o.channels && mirror == o.mirror && aspectRatio == o.aspectRatio &&
        color == o.color && levels == o.levels && exposure == o.exposure &&
        softClip == o.softClip && imageFilters == o.imageFilters;
}
bool DisplayOptions::operator!=(const DisplayOptions& o) const { return !(*this == o); }

bool ForegroundOptions::operator==(const ForegroundOptions&) const { return true; }
bool ForegroundOptions::operator!=(const ForegroundOptions&) const { return false; }

// ── Player stubs ───────────────────────────────────────────────────────────

bool Player::isMuted() const { return false; }
void Player::setMute(bool) {}
double Player::getAudioOffset() const { return 0.0; }
void Player::setAudioOffset(double) {}
void Player::setChannelMute(const std::vector<bool>&) {}
void Player::setVolume(float) {}
float Player::getVolume() const { return 1.0f; }
double Player::getDefaultSpeed() const { return 24.0; }
double Player::getSpeed() const { return 24.0; }
void Player::setSpeed(double) {}
const std::vector<bool>& Player::getChannelMute() const { static std::vector<bool> v; return v; }
void Player::setCompare(const std::vector<std::shared_ptr<Timeline>>&) {}
const std::vector<std::shared_ptr<Timeline>>& Player::getCompare() const {
    static std::vector<std::shared_ptr<Timeline>> v; return v;
}
void Player::setCompareTime(CompareTime) {}
const AudioDeviceID& Player::getAudioDevice() const { static AudioDeviceID id; return id; }
void Player::setAudioDevice(const AudioDeviceID&) {}
void Player::setPlayback(Playback) {}
void Player::togglePlayback() {}
bool Player::isStopped() const { return true; }
void Player::stop() {}
void Player::forward() {}
void Player::reverse() {}
void Player::setLoop(Loop) {}
void Player::seek(const OTIO_NS::RationalTime&) {}
void Player::timeAction(TimeAction) {}
void Player::gotoStart() {}
void Player::gotoEnd() {}
void Player::framePrev() {}
void Player::frameNext() {}
void Player::setInOutRange(const OTIO_NS::TimeRange&) {}
void Player::setInPoint() {}
void Player::resetInPoint() {}
void Player::setOutPoint() {}
void Player::resetOutPoint() {}
void Player::setIOOptions(const IOOptions&) {}
void Player::setVideoLayer(int) {}
void Player::setCompareVideoLayers(const std::vector<int>&) {}
std::shared_ptr<Player> Player::create(const std::shared_ptr<ftk::Context>&,
                                       const std::shared_ptr<Timeline>&,
                                       const PlayerOptions&) {
    return nullptr;
}
const OTIO_NS::TimeRange& Player::getTimeRange() const {
    static OTIO_NS::TimeRange tr; return tr;
}
Playback Player::getPlayback() const { return Playback::Stop; }
const OTIO_NS::RationalTime& Player::getCurrentTime() const {
    static OTIO_NS::RationalTime t; return t;
}

// ── Timeline stubs ────────────────────────────────────────────────────────

std::shared_ptr<Timeline> Timeline::create(
    const std::shared_ptr<ftk::Context>&,
    const ftk::Path&,
    const Options&) {
    return nullptr;
}

// ── IOInfo stub ────────────────────────────────────────────────────────────

const IOInfo& Player::getIOInfo() const {
    static IOInfo info; return info;
}

// Let PlaybackController.h define CGPLAY_HAS_TLRENDER locally before we include it
#include "playback/PlaybackController.h"

} // namespace tl

// playerReady signal stub (MOC can't generate the impl for shared_ptr<tl::Player>)
void cgplay::PlaybackController::playerReady(const std::shared_ptr<tl::Player>&) {}

namespace tl {

// PlayerOptions stubs
void to_json(nlohmann::json&, const PlayerOptions&) {}
void from_json(const nlohmann::json&, PlayerOptions&) {}

// ── tl::init stub ──────────────────────────────────────────────────────────
void init(const std::shared_ptr<ftk::Context>&) {}

} // namespace tl

// ── Viewport stubs ─────────────────────────────────────────────────────────
namespace tl { namespace ui {

bool Viewport::hasFrameView() const { return true; }
const ftk::V2I& Viewport::getViewPos() const { static ftk::V2I v(0, 0); return v; }
void Viewport::setViewPosAndZoom(const ftk::V2I&, double) {}
void Viewport::setFrameView(bool) {}
void Viewport::zoomIn() {}
void Viewport::zoomOut() {}
void Viewport::resetZoom() {}
double Viewport::getZoom() const { return 1.0; }
void Viewport::setZoom(double, const ftk::V2I&) {}
void Viewport::setImageOptions(const std::vector<ftk::ImageOptions>&) {}
void Viewport::setDisplayOptions(const std::vector<DisplayOptions>&) {}
void Viewport::setOCIOOptions(const OCIOOptions&) {}
void Viewport::setLUTOptions(const LUTOptions&) {}
void Viewport::setCompareOptions(const CompareOptions&) {}
void Viewport::setBackgroundOptions(const BackgroundOptions&) {}
void Viewport::setForegroundOptions(const ForegroundOptions&) {}

std::shared_ptr<Viewport> Viewport::create(
    const std::shared_ptr<ftk::Context>&,
    const std::shared_ptr<ftk::IWidget>&) {
    return {};
}

}} // namespace tl::ui

// ── Render stub ────────────────────────────────────────────────────────────
namespace tl { namespace gl {
std::shared_ptr<Render> Render::create(
    const std::shared_ptr<ftk::LogSystem>&,
    const std::shared_ptr<ftk::FontSystem>&) {
    return {};
}
}} // namespace tl::gl
