#include "n3d/assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void put16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
}

void put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8] = std::byte{static_cast<std::uint8_t>(value >> shift)};
}

std::size_t append_chunk(std::vector<std::byte>& bytes, std::uint16_t type,
                         std::span<const std::byte> payload) {
    const auto offset = bytes.size();
    bytes.resize(offset + 6 + payload.size());
    put32(bytes, offset, static_cast<std::uint32_t>(6 + payload.size()));
    put16(bytes, offset + 4, type);
    for (std::size_t i = 0; i < payload.size(); ++i) bytes[offset + 6 + i] = payload[i];
    return offset;
}

std::size_t append_frame(std::vector<std::byte>& bytes, std::uint16_t chunks) {
    const auto offset = bytes.size();
    bytes.resize(offset + 16, std::byte{0});
    put16(bytes, offset + 4, 0xf1fa);
    put16(bytes, offset + 6, chunks);
    return offset;
}

struct Fixture final {
    std::vector<std::byte> bytes;
    std::size_t palette_component_offset;
};

Fixture make_fixture() {
    Fixture fixture{{}, 0};
    auto& bytes = fixture.bytes;
    bytes.resize(128, std::byte{0});
    put16(bytes, 4, 0xaf11);
    put16(bytes, 6, 3); // The fourth record is an undeclared terminal record.
    put16(bytes, 8, 320);
    put16(bytes, 10, 200);
    put16(bytes, 12, 8);
    put16(bytes, 16, 9);

    const auto first = append_frame(bytes, 2);
    // COLOR_64 sets index 7 to (1,2,63), which presents as (4,8,252).
    const std::array palette1{std::byte{1}, std::byte{0}, std::byte{7}, std::byte{1},
                              std::byte{1}, std::byte{2}, std::byte{63}};
    const auto palette1_offset = append_chunk(bytes, 0x000b, palette1);
    fixture.palette_component_offset = palette1_offset + 6 + 4;

    // Three BRUN repeats per scanline fill 127 + 127 + 66 pixels, index 7.
    std::vector<std::byte> brun;
    brun.reserve(200 * 7);
    for (int line = 0; line < 200; ++line) {
        brun.insert(brun.end(), {std::byte{3}, std::byte{127}, std::byte{7},
                                 std::byte{127}, std::byte{7}, std::byte{66}, std::byte{7}});
    }
    append_chunk(bytes, 0x000f, brun);
    put32(bytes, first, static_cast<std::uint32_t>(bytes.size() - first));

    const auto second = append_frame(bytes, 2);
    const std::array palette2{std::byte{1}, std::byte{0}, std::byte{9}, std::byte{1},
                              std::byte{63}, std::byte{4}, std::byte{0}};
    append_chunk(bytes, 0x000b, palette2);
    // One LC scanline: literal index 3 at x=4, then index 9 at x=6..8.
    // One trailing zero is a legal LC chunk pad.
    const std::array lc{std::byte{100}, std::byte{0}, std::byte{1}, std::byte{0},
                        std::byte{2}, std::byte{4}, std::byte{1}, std::byte{3},
                        std::byte{1}, std::byte{0xfd}, std::byte{9}, std::byte{0}};
    append_chunk(bytes, 0x000c, lc);
    put32(bytes, second, static_cast<std::uint32_t>(bytes.size() - second));

    const auto third = append_frame(bytes, 0); // Unchanged frame is still presented.
    put32(bytes, third, 16);
    const auto terminal = append_frame(bytes, 0);
    put32(bytes, terminal, 16);
    put32(bytes, 0, static_cast<std::uint32_t>(bytes.size()));
    return fixture;
}

void player_advances_and_resets_without_presenting_terminal_record() {
    auto fixture = make_fixture();
    n3d::Assets assets{};
    assets.ending_bytes = std::move(fixture.bytes);
    n3d::FliPlayer player(assets);
    check(player.frame_count() == 3 && player.frame_interval_ms() == 135 &&
              player.decoded_frames() == 0 && !player.finished(),
          "player starts with three declared frames and 135 ms FLI speed");
    try {
        static_cast<void>(player.frame());
        check(false, "an undecoded frame is not displayable");
    } catch (const std::logic_error&) {
    }

    if (!player.next_frame()) {
        check(false, "first declared frame decodes");
        return;
    }
    const auto& first = player.frame();
    check(player.decoded_frames() == 1 && !player.finished() &&
              first.pixels[0] == 7 && first.pixels[126] == 7 &&
              first.pixels[127] == 7 && first.pixels[253] == 7 &&
              first.pixels[254] == 7 && first.pixels[319] == 7 &&
              first.pixels[100 * 320 + 4] == 7 &&
              first.palette[7].r == 4 && first.palette[7].g == 8 &&
              first.palette[7].b == 252 && first.palette[9].r == 0,
          "BRUN covers all scanline packet edges and COLOR_64 expands six-bit RGB");

    if (!player.next_frame()) {
        check(false, "second declared frame decodes");
        return;
    }
    const auto& second = player.frame();
    check(player.decoded_frames() == 2 && !player.finished() &&
              second.pixels[99 * 320 + 4] == 7 &&
              second.pixels[100 * 320 + 3] == 7 &&
              second.pixels[100 * 320 + 4] == 3 &&
              second.pixels[100 * 320 + 5] == 7 &&
              second.pixels[100 * 320 + 6] == 9 &&
              second.pixels[100 * 320 + 8] == 9 &&
              second.pixels[100 * 320 + 9] == 7 &&
              second.pixels[101 * 320 + 4] == 7 &&
              second.palette[7].r == 4 && second.palette[7].g == 8 &&
              second.palette[7].b == 252 &&
              second.palette[9].r == 252 && second.palette[9].g == 16 &&
              second.palette[9].b == 0,
          "LC literal/repeat modify one scanline while prior pixels/palette persist");

    if (!player.next_frame()) {
        check(false, "third zero-chunk frame decodes");
        return;
    }
    const auto& third = player.frame();
    check(player.decoded_frames() == 3 && player.finished() &&
              third.pixels[100 * 320 + 4] == 3 &&
              third.pixels[100 * 320 + 6] == 9 &&
              third.palette[7].b == 252 && third.palette[9].g == 16,
          "zero-chunk frame holds the preceding indexed image and palette");
    check(!player.next_frame() && player.decoded_frames() == 3 &&
              player.frame().pixels[100 * 320 + 6] == 9,
          "undeclared terminal record is not decoded or presented");

    player.reset();
    check(player.decoded_frames() == 0 && !player.finished(), "reset rewinds the stream");
    try {
        static_cast<void>(player.frame());
        check(false, "reset invalidates the previously presented frame");
    } catch (const std::logic_error&) {
    }
    if (!player.next_frame()) {
        check(false, "first frame decodes again after reset");
        return;
    }
    const auto& replay = player.frame();
    check(player.decoded_frames() == 1 && replay.pixels[100 * 320 + 4] == 7 &&
              replay.pixels[100 * 320 + 6] == 7 &&
              replay.palette[7].r == 4 && replay.palette[7].b == 252 &&
              replay.palette[9].r == 0,
          "reset discards LC changes and palette delta before replaying BRUN");
}

void player_rejects_out_of_range_palette_channels() {
    auto fixture = make_fixture();
    fixture.bytes[fixture.palette_component_offset] = std::byte{64};
    n3d::Assets assets{};
    assets.ending_bytes = std::move(fixture.bytes);
    n3d::FliPlayer player(assets);
    try {
        static_cast<void>(player.next_frame());
        check(false, "out-of-range COLOR_64 channel must be rejected");
    } catch (const n3d::DataError& error) {
        check(error.path() == "ending.fli" &&
                  error.offset() == fixture.palette_component_offset,
              "invalid six-bit palette has the exact source path and offset");
    }
    check(player.decoded_frames() == 0, "failed decode does not advance the frame cursor");
}

void clipped_last_chunk_remains_within_declared_frame() {
    n3d::Assets assets{};
    auto& bytes = assets.ending_bytes;
    bytes.resize(128, std::byte{0});
    put16(bytes, 4, 0xaf11);
    put16(bytes, 6, 2);
    put16(bytes, 8, 320);
    put16(bytes, 10, 200);
    put16(bytes, 12, 8);
    const auto first = append_frame(bytes, 1);
    const std::array empty_palette{std::byte{0}, std::byte{0}};
    const auto chunk = append_chunk(bytes, 0x000b, empty_palette);
    put32(bytes, chunk, 9); // One byte past frame end; next frame begins at frame end.
    put32(bytes, first, static_cast<std::uint32_t>(bytes.size() - first));
    const auto second = append_frame(bytes, 0);
    put32(bytes, second, 16);
    const auto terminal = append_frame(bytes, 0);
    put32(bytes, terminal, 16);
    put32(bytes, 0, static_cast<std::uint32_t>(bytes.size()));

    n3d::FliPlayer player(assets);
    check(player.next_frame() && player.next_frame() && !player.next_frame() &&
              player.decoded_frames() == 2 && player.finished(),
          "one-byte final-chunk overrun leaves the next declared frame intact");

    put32(bytes, chunk, 10); // Two bytes beyond the frame must not be accepted.
    player.reset();
    try {
        static_cast<void>(player.next_frame());
        check(false, "two-byte chunk overrun must be rejected");
    } catch (const n3d::DataError& error) {
        check(error.path() == "ending.fli" && error.offset() == chunk,
              "two-byte chunk overrun retains its source offset");
    }
    check(player.decoded_frames() == 0, "invalid chunk does not advance playback");
}

} // namespace

int run_fli_player_tests() {
    player_advances_and_resets_without_presenting_terminal_record();
    player_rejects_out_of_range_palette_channels();
    clipped_last_chunk_remains_within_declared_frame();
    if (failures) std::cerr << failures << " FLI player checks failed\n";
    else std::cout << "n3d FLI player checks passed\n";
    return failures;
}
