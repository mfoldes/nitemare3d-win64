#include "n3d/assets.hpp"
#include "n3d/platform_midi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Format-1 SMF, two tracks, PPQN 240. Track 0 contains the tempo and two
// simultaneous channel-10 notes; the second note-on (and note-off) uses running
// status. Track 1 interleaves another channel at ticks 0, 120, and 240.
constexpr std::array<std::byte, 72> smf{
    std::byte{'M'}, std::byte{'T'}, std::byte{'h'}, std::byte{'d'},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x06},
    std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x02},
    std::byte{0x00}, std::byte{0xf0},
    std::byte{'M'}, std::byte{'T'}, std::byte{'r'}, std::byte{'k'},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x1a},
    std::byte{0x00}, std::byte{0xff}, std::byte{0x51}, std::byte{0x03},
    std::byte{0x07}, std::byte{0xa1}, std::byte{0x20}, // 500,000 us/quarter
    std::byte{0x00}, std::byte{0x99}, std::byte{0x3c}, std::byte{0x40},
    std::byte{0x00}, std::byte{0x3e}, std::byte{0x45}, // running 0x99
    std::byte{0x81}, std::byte{0x70}, std::byte{0x89}, std::byte{0x3c}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x3e}, std::byte{0x00}, // running 0x89
    std::byte{0x00}, std::byte{0xff}, std::byte{0x2f}, std::byte{0x00},
    std::byte{'M'}, std::byte{'T'}, std::byte{'r'}, std::byte{'k'},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
    std::byte{0x00}, std::byte{0x90}, std::byte{0x43}, std::byte{0x50},
    std::byte{0x78}, std::byte{0xb0}, std::byte{0x07}, std::byte{0x64},
    std::byte{0x78}, std::byte{0x80}, std::byte{0x43}, std::byte{0x00},
    std::byte{0x00}, std::byte{0xff}, std::byte{0x2f}, std::byte{0x00},
};

n3d::Assets make_assets() {
    n3d::Assets assets{};
    assets.sound_bytes.reserve(smf.size() * assets.sound.midi.size());
    for (std::size_t index = 1; index <= assets.sound.midi.size(); ++index) {
        auto& entry = assets.sound.directory[index];
        entry.kind = n3d::SoundRecordKind::midi;
        entry.absolute_offset = assets.sound_bytes.size();
        entry.length = static_cast<std::uint16_t>(smf.size());

        auto& summary = assets.sound.midi[index - 1];
        summary.record_index = index;
        summary.absolute_offset = entry.absolute_offset;
        summary.length = entry.length;
        summary.track_count = 2;
        summary.division = 240;
        summary.tempo_us_per_quarter = 500000;
        summary.last_event_tick = 240;
        summary.channel_10_note_ons = 2;
        assets.sound_bytes.insert(assets.sound_bytes.end(), smf.begin(), smf.end());
    }
    return assets;
}

struct ExpectedEvent final {
    std::uint64_t tick;
    std::uint32_t delta_ticks;
    std::uint32_t event_word;
    std::uint16_t track_index;
    std::uint32_t track_order;
};

// WinMM dwEvent places MEVT_TEMPO (0x01) in the high byte and the tempo in
// the low 24 bits; short messages pack status/data1/data2 in those low bytes.
constexpr std::array<ExpectedEvent, 8> expected_events{{
    {0, 0, 0x0107a120u, 0, 0},
    {0, 0, 0x00403c99u, 0, 1},
    {0, 0, 0x00453e99u, 0, 2},
    {0, 0, 0x00504390u, 1, 0},
    {120, 120, 0x006407b0u, 1, 1},
    {240, 120, 0x00003c89u, 0, 3},
    {240, 0, 0x00003e89u, 0, 4},
    {240, 0, 0x00004380u, 1, 2},
}};

void all_fifteen_tunes_preserve_merged_events() {
    const auto assets = make_assets();
    const n3d::MidiStreamPlayer player(assets); // Decoding must not open a MIDI device.
    for (std::uint8_t tune = 1; tune <= 15; ++tune) {
        const auto label = "MIDI tune " + std::to_string(tune);
        check(player.division(tune) == 240, label + " retains PPQN division");
        const auto events = player.events(tune);
        check(events.size() == expected_events.size(), label + " retains every merged event");
        if (events.size() != expected_events.size()) continue;

        std::uint64_t elapsed_ticks = 0;
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto& event = events[index];
            const auto& expected = expected_events[index];
            const auto event_label = label + " event " + std::to_string(index);
            elapsed_ticks += event.delta_ticks;
            check(event.tick == expected.tick && event.delta_ticks == expected.delta_ticks &&
                      elapsed_ticks == expected.tick,
                  event_label + " reconstructs absolute tick from merged deltas");
            check(event.event_word == expected.event_word,
                  event_label + " preserves WinMM tempo or channel message");
            check(event.track_index == expected.track_index &&
                      event.track_order == expected.track_order,
                  event_label + " preserves order among simultaneous track events");
        }
    }
}

void malformed_vlq_is_rejected_without_a_device() {
    auto assets = make_assets();
    constexpr std::size_t first_track_body = 14 + 8; // MThd plus first MTrk header.
    const auto offset = assets.sound.directory[15].absolute_offset + first_track_body;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        assets.sound_bytes[offset + byte] = std::byte{0x80}; // No terminator in four VLQ bytes.
    }
    try {
        const n3d::MidiStreamPlayer invalid(assets);
        static_cast<void>(invalid);
        check(false, "five-byte MIDI VLQ must be rejected during predecode");
    } catch (const std::runtime_error&) {
        // Structural parse error; the text of the diagnostic is not contractual.
    } catch (...) {
        check(false, "malformed MIDI VLQ must use the decoder error type");
    }
}

} // namespace

int run_midi_tests() {
    all_fifteen_tunes_preserve_merged_events();
    malformed_vlq_is_rejected_without_a_device();
    if (failures != 0) {
        std::cerr << failures << " MIDI checks failed\n";
        return failures;
    }
    std::cout << "n3d MIDI checks passed\n";
    return 0;
}
