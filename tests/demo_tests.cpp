#include "n3d/demo.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void put_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
}

void put_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = std::byte{static_cast<std::uint8_t>(value >> shift)};
    }
}

std::array<std::byte, 6 + 4 * 8> recording_bytes() {
    std::array<std::byte, 6 + 4 * 8> bytes{};
    put_u16(bytes, 0, 10);
    put_u16(bytes, 2, 5);
    put_u16(bytes, 4, 20);
    constexpr std::array<n3d::DemoEvent, 4> events{{
        {0x41, 0x0002, 0, 2},
        {0x00, 0x0008, 0, 5},
        {0x42, 0x0080, 0, 5},
        {0x43, 0x0010, 0, 5},
    }};
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto offset = 6 + index * 8;
        bytes[offset] = std::byte{events[index].key};
        put_u16(bytes, offset + 1, events[index].flags);
        put_u32(bytes, offset + 4, events[index].tick);
    }
    return bytes;
}

void due_events_and_modal_tick() {
    const auto bytes = recording_bytes();
    const auto recording = n3d::parse_demo_file("demo.test", bytes);
    n3d::DemoPlayback playback(recording);

    std::uint32_t tick = 0;
    auto frame = playback.advance(tick, false);
    check(!frame.event_applied && frame.key == 0 && frame.held == 0 && tick == 0,
          "future event does not emit a premature key or advance gameplay time");
    tick = 2; // Gameplay clock B advances its own logical counter.
    frame = playback.advance(tick, false);
    check(frame.event_applied && frame.key == 0x41 && frame.held == 0x0002 &&
              !frame.dispatch_modal_key, "first due event applies once");
    frame = playback.advance(tick, false);
    check(!frame.event_applied && frame.key == 0 && frame.held == 0x0002,
          "future event clears the one-shot key but retains held flags");

    tick = 4;
    frame = playback.advance(tick, true);
    check(!frame.event_applied && tick == 5 && frame.key == 0 &&
              frame.held == 0x0002 && !frame.dispatch_modal_key,
          "modal idle increments the tick only while waiting for a future record");
    frame = playback.advance(tick, true);
    check(frame.event_applied && frame.dispatch_modal_key && frame.key == 0 &&
              frame.held == 0x0008 && tick == 5,
          "due modal record dispatches even its zero key without advancing time");
    frame = playback.advance(tick, true);
    check(frame.event_applied && frame.dispatch_modal_key && frame.key == 0x42 &&
              frame.held == 0x0080 && tick == 5,
          "equal-tick records apply on successive modal calls, never in a batch");
    frame = playback.advance(tick, true);
    check(frame.event_applied && frame.key == 0x43 && frame.held == 0x0010,
          "third equal-tick event is not lost");
    frame = playback.advance(tick, true);
    check(frame.event_applied && frame.dispatch_modal_key && frame.key == 0x43 &&
              frame.held == 0x0010 && tick == 5,
          "EOF keeps reapplying the last record, including its key");
}

void overdue_restart_and_abort() {
    const auto bytes = recording_bytes();
    const auto recording = n3d::parse_demo_file("demo.test", bytes);
    n3d::DemoPlayback playback(recording);
    std::uint32_t tick = 100;
    auto frame = playback.advance(tick, false);
    check(frame.key == 0x41 && frame.held == 0x0002,
          "large overdue tick still applies only the first record");
    frame = playback.advance(tick, false);
    check(frame.key == 0 && frame.held == 0x0008,
          "second overdue call applies only the second record");
    frame = playback.advance(tick, false);
    check(frame.key == 0x42 && frame.held == 0x0080,
          "third overdue call reaches the next equal-tick record");
    frame = playback.advance(tick, false);
    check(frame.key == 0x43 && frame.held == 0x0010,
          "fourth overdue call consumes the final record");
    frame = playback.advance(tick, false);
    check(frame.key == 0x43 && frame.held == 0x0010,
          "overdue EOF sticks to the final key and held flags");
    playback.abort_on_live_keydown();
    check(playback.aborted(), "any live key-down signals playback abort");
    frame = playback.advance(tick, true);
    check(!frame.event_applied && frame.key == 0 && frame.held == 0 &&
              !frame.dispatch_modal_key && tick == 100,
          "aborted playback has no further input or modal clock progression");

    playback.reset();
    tick = 0; // The caller reloads the level, seeds RNG and resets 53dc on restart.
    check(!playback.aborted(), "reset clears live-key abort state");
    frame = playback.advance(tick, false);
    check(!frame.event_applied && frame.key == 0 && frame.held == 0,
          "restart clears previous held flags and does not skip the first deadline");
    tick = 2;
    frame = playback.advance(tick, false);
    check(frame.event_applied && frame.key == 0x41 && frame.held == 0x0002,
          "restart replays the first record rather than the former EOF cursor");
}

} // namespace

int run_demo_tests() {
    due_events_and_modal_tick();
    overdue_restart_and_abort();
    if (failures != 0) {
        std::cerr << failures << " demo checks failed\n";
        return failures;
    }
    std::cout << "n3d demo checks passed\n";
    return 0;
}
