#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace n3d {

struct Assets;

// An immutable, tick-merged WinMM event. long_data points into Assets::sound_bytes;
// Assets must outlive the player and must not be moved while it exists.
struct MidiStreamEvent final {
    std::uint64_t tick = 0;
    std::uint32_t delta_ticks = 0;
    std::uint32_t event_word = 0;
    std::uint16_t track_index = 0;
    std::uint32_t track_order = 0;
    std::span<const std::byte> long_data{};
    bool prepend_sysex_start = false;
};

class MidiStreamPlayer final {
public:
    // Predecodes all fifteen validated format-1 SMFs once; malformed input throws.
    // Opening the MIDI mapper is deferred until the first play() call.
    explicit MidiStreamPlayer(const Assets& assets);
    ~MidiStreamPlayer();

    MidiStreamPlayer(const MidiStreamPlayer&) = delete;
    MidiStreamPlayer& operator=(const MidiStreamPlayer&) = delete;
    MidiStreamPlayer(MidiStreamPlayer&&) = delete;
    MidiStreamPlayer& operator=(MidiStreamPlayer&&) = delete;

    // Tunes are 1..15; play(0) stops. All device operations are main-thread only.
    [[nodiscard]] bool play(std::uint8_t tune);
    [[nodiscard]] bool pause();
    [[nodiscard]] bool resume();
    [[nodiscard]] bool stop();
    // Reclaims completed buffers, queues subsequent chunks, and restarts at the end.
    [[nodiscard]] bool update();
    [[nodiscard]] bool set_volume(unsigned percent); // 0..100, independent of effects
    [[nodiscard]] bool set_muted(bool muted);

    // A static-literal failure reason and the corresponding WinMM MMRESULT (0 otherwise).
    [[nodiscard]] std::string_view last_error() const noexcept;
    [[nodiscard]] std::uint32_t last_native_error() const noexcept;

    // Read-only decoded events for format/timing invariant checks (no MIDI device needed).
    [[nodiscard]] std::span<const MidiStreamEvent> events(std::uint8_t tune) const noexcept;
    [[nodiscard]] std::uint16_t division(std::uint8_t tune) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace n3d
