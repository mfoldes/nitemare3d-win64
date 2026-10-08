#pragma once

#include "n3d/recordings.hpp"

#include <cstddef>
#include <cstdint>

namespace n3d {

// One invocation of the original 90ce playback hook. The caller supplies the
// current game logical tick (53dc); only the modal idle path can advance it.
// A key is a one-shot action, while held is the persistent 3756/3757 word.
struct DemoStep final {
    std::uint8_t key = 0;
    std::uint16_t held = 0;
    bool event_applied = false;
    bool dispatch_modal_key = false; // Call the modal key handler, even when key == 0.
};

// Borrows an already parsed, immutable Assets::demos recording. The recording
// must outlive this controller; neither advancing nor restarting touches disk
// or allocates. Call advance once per gameplay B pass, or once per modal idle
// dispatch, after the corresponding clock update.
class DemoPlayback final {
public:
    explicit DemoPlayback(const DemoRecording& recording) noexcept;
    DemoPlayback(const DemoRecording&&) = delete;

    [[nodiscard]] const DemoHeader& header() const noexcept;
    [[nodiscard]] DemoStep advance(std::uint32_t& logical_tick, bool modal) noexcept;

    // The original WM_KEYDOWN route aborts on *any* live key-down, including
    // modifiers and repeated keys, before the next demo input dispatch.
    void abort_on_live_keydown() noexcept;
    [[nodiscard]] bool aborted() const noexcept;

    // Death-loop restart reuses the same parsed recording from its first event.
    void reset() noexcept;

private:
    const DemoRecording* recording_;
    std::size_t next_event_ = 0;
    std::uint16_t held_ = 0;
    bool aborted_ = false;
};

} // namespace n3d
