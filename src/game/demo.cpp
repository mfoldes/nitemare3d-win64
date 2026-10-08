#include "n3d/demo.hpp"

namespace n3d {

DemoPlayback::DemoPlayback(const DemoRecording& recording) noexcept : recording_(&recording) {}

const DemoHeader& DemoPlayback::header() const noexcept {
    return recording_->header;
}

DemoStep DemoPlayback::advance(std::uint32_t& logical_tick, bool modal) noexcept {
    if (aborted_) return {};

    const auto& events = recording_->events;
    if (events.empty()) return {}; // No parsed event can supply an input action.
    const DemoEvent& event = events[next_event_];
    if (event.tick > logical_tick) {
        if (modal) ++logical_tick; // State 8 has no simulation tick of its own.
        return DemoStep{0, held_, false, false};
    }

    held_ = event.flags;
    if (next_event_ + 1 < events.size()) {
        ++next_event_; // One due record per call, even with equal or overdue ticks.
    }
    // A read at EOF changes no buffer bytes. Keep applying the final event,
    // including its one-shot key, on all later calls until death or live abort.
    return DemoStep{event.key, held_, true, modal};
}

void DemoPlayback::abort_on_live_keydown() noexcept {
    aborted_ = true;
    held_ = 0;
}

bool DemoPlayback::aborted() const noexcept {
    return aborted_;
}

void DemoPlayback::reset() noexcept {
    next_event_ = 0;
    held_ = 0;
    aborted_ = false;
}

} // namespace n3d
