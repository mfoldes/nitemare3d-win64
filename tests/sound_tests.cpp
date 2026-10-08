#include "n3d/sound.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_data_error(Function&& function, std::string_view path, std::size_t offset) {
    try {
        function();
        check(false, "expected DataError");
    } catch (const n3d::DataError& error) {
        check(error.path() == path, "sound error retains source path");
        check(error.offset() == offset, "sound error identifies source byte");
    } catch (...) {
        check(false, "wrong sound exception type");
    }
}

void put_le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
}

void put_le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = std::byte{static_cast<std::uint8_t>(value >> shift)};
    }
}

void put_be16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    bytes.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

void put_be32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

std::vector<std::byte> midi_with_running_status() {
    constexpr std::array<std::byte, 18> track{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x51}, std::byte{0x03},
        std::byte{0x07}, std::byte{0xa1}, std::byte{0x20},
        std::byte{0x00}, std::byte{0x99}, std::byte{0x3c}, std::byte{0x40},
        std::byte{0x00}, std::byte{0x3e}, std::byte{0x45},
        std::byte{0x00}, std::byte{0xff}, std::byte{0x2f}, std::byte{0x00},
    };
    std::vector<std::byte> midi;
    midi.reserve(14 + 8 + track.size());
    constexpr std::array<std::byte, 4> header{
        std::byte{'M'}, std::byte{'T'}, std::byte{'h'}, std::byte{'d'}};
    constexpr std::array<std::byte, 4> track_tag{
        std::byte{'M'}, std::byte{'T'}, std::byte{'r'}, std::byte{'k'}};
    midi.insert(midi.end(), header.begin(), header.end());
    put_be32(midi, 6);
    put_be16(midi, 1);
    put_be16(midi, 1);
    put_be16(midi, 240);
    midi.insert(midi.end(), track_tag.begin(), track_tag.end());
    put_be32(midi, static_cast<std::uint32_t>(track.size()));
    midi.insert(midi.end(), track.begin(), track.end());
    return midi;
}

std::vector<std::byte> make_archive() {
    std::vector<std::byte> bytes(0x3c0, std::byte{0});
    auto record = [&](std::size_t index, std::size_t offset, std::size_t length) {
        put_le16(bytes, index * 6, static_cast<std::uint16_t>(length));
        put_le32(bytes, index * 6 + 2, static_cast<std::uint32_t>(offset));
    };
    auto append = [&](std::size_t index, std::span<const std::byte> payload) {
        const auto offset = bytes.size();
        record(index, offset, payload.size());
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return offset;
    };
    std::vector<std::byte> ibk(3204, std::byte{0});
    ibk[0] = std::byte{'I'};
    ibk[1] = std::byte{'B'};
    ibk[2] = std::byte{'K'};
    ibk[3] = std::byte{0x1a};
    append(0, ibk);
    const auto midi = midi_with_running_status();
    for (std::size_t index = 1; index <= 15; ++index) {
        append(index, midi);
    }
    record(16, bytes.size(), 0); // The music-directory terminator points to effect data.
    constexpr std::array<std::byte, 8> pcm{
        std::byte{0x80}, std::byte{0x81}, std::byte{0x82}, std::byte{0x83},
        std::byte{0x84}, std::byte{0x85}, std::byte{0x86}, std::byte{0x87}};
    for (std::size_t index = 32; index <= 110; ++index) {
        if (index == 32 || index == 33 || index == 62 || index == 72 ||
            index == 75 || index == 76 || index == 77) {
            record(index, bytes.size(), 0); // Zero-length effects retain the next offset.
        } else if (index == 70 || index == 74) {
            continue; // Their alias entries were written with records 69 and 73.
        } else {
            const auto offset = append(index, pcm);
            if (index == 69 || index == 73) {
                record(index + 1, offset, pcm.size());
            }
        }
    }
    record(111, bytes.size(), 0);
    return bytes;
}

std::uint32_t directory_offset(const std::vector<std::byte>& bytes, std::size_t index) {
    const auto field = index * 6 + 2;
    return std::to_integer<std::uint32_t>(bytes[field]) |
        (std::to_integer<std::uint32_t>(bytes[field + 1]) << 8) |
        (std::to_integer<std::uint32_t>(bytes[field + 2]) << 16) |
        (std::to_integer<std::uint32_t>(bytes[field + 3]) << 24);
}

void sound_reader_validates_playable_metadata() {
    const auto bytes = make_archive();
    const auto sound = n3d::parse_sound_archive("fixture.bin", bytes);
    check(sound.midi[0].track_count == 1 && sound.midi[0].division == 240,
          "SMF format-1 track header is decoded big-endian");
    check(sound.midi[0].tempo_us_per_quarter == 500000 &&
          sound.midi[0].channel_10_note_ons == 2,
          "SMF tempo and channel-10 running-status notes survive decoding");
    check(sound.nonempty_effect_count == 72 && sound.alias_pair_count == 2,
          "PCM slots preserve both supported aliases");
    check(sound.midi[0].absolute_offset == directory_offset(bytes, 1) &&
          sound.effects[0].absolute_offset == directory_offset(bytes, 34),
          "sound metadata locates caller-owned MIDI and PCM payloads");
}

void sound_reader_rejects_directory_and_midi_corruption() {
    const auto bytes = make_archive();
    auto bad_alias = bytes;
    put_le32(bad_alias, 70 * 6 + 2, directory_offset(bytes, 69) + 1);
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_alias)); },
                      "fixture.bin", 70 * 6 + 2);

    auto bad_track = bytes;
    const auto midi_start = directory_offset(bytes, 1);
    const auto track_length = static_cast<std::size_t>(midi_start) + 18;
    bad_track[track_length] = std::byte{0x7f};
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_track)); },
                      "fixture.bin", track_length);

    auto bad_running = bytes;
    bad_running[midi_start + 22 + 8] = std::byte{0x3c}; // Erase first channel status after tempo.
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_running)); },
                      "fixture.bin", midi_start + 22 + 7);

    auto bad_data = bytes;
    bad_data[midi_start + 22 + 9] = std::byte{0xff};
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_data)); },
                      "fixture.bin", midi_start + 22 + 9);

    auto bad_terminator = bytes;
    bad_terminator[111 * 6] = std::byte{1};
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_terminator)); },
                      "fixture.bin", 111 * 6);

    auto bad_gap = bytes;
    put_le32(bad_gap, 110 * 6 + 2, directory_offset(bytes, 110) + 1);
    bad_gap.push_back(std::byte{0});
    put_le32(bad_gap, 111 * 6 + 2, static_cast<std::uint32_t>(bad_gap.size()));
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_gap)); },
                      "fixture.bin", 110 * 6 + 2);
}

void sound_reader_rejects_provenance_and_bounds_violations() {
    const auto bytes = make_archive();

    auto truncated = bytes;
    truncated.resize(0x3c0 - 1);
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", truncated)); },
                      "fixture.bin", 0);

    auto bad_ibk_tag = bytes;
    bad_ibk_tag[0x3c0] = std::byte{'X'};
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_ibk_tag)); },
                      "fixture.bin", 0x3c0);

    auto bad_music_terminator = bytes;
    put_le32(bad_music_terminator, 16 * 6 + 2, directory_offset(bytes, 32) + 1);
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_music_terminator)); },
                      "fixture.bin", 16 * 6 + 2);

    auto bad_terminator_offset = bytes;
    put_le32(bad_terminator_offset, 111 * 6 + 2, directory_offset(bytes, 111) + 1);
    expect_data_error([&] { static_cast<void>(n3d::parse_sound_archive("fixture.bin", bad_terminator_offset)); },
                      "fixture.bin", 111 * 6 + 2);
}

} // namespace

int run_sound_tests() {
    sound_reader_validates_playable_metadata();
    sound_reader_rejects_directory_and_midi_corruption();
    sound_reader_rejects_provenance_and_bounds_violations();
    if (failures != 0) {
        std::cerr << failures << " sound checks failed\n";
        return failures;
    }
    std::cout << "n3d sound checks passed\n";
    return 0;
}
