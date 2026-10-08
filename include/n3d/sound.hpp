#pragma once

#include "n3d/binary_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace n3d {

enum class SoundRecordKind : std::uint8_t {
    ibk,
    midi,
    music_terminator,
    pcm,
    placeholder,
    terminator,
    zero,
};

struct SoundDirectoryEntry final {
    std::uint16_t length = 0;
    std::size_t absolute_offset = 0;
    SoundRecordKind kind = SoundRecordKind::zero;
};

struct SoundMidiSummary final {
    std::size_t record_index = 0;
    std::size_t absolute_offset = 0;
    std::size_t length = 0;
    std::uint16_t track_count = 0;
    std::uint16_t division = 0;
    std::uint32_t tempo_us_per_quarter = 0;
    std::uint64_t last_event_tick = 0;
    std::size_t channel_10_note_ons = 0;
};

struct SoundPcmEffect final {
    std::size_t record_index = 0;
    std::size_t absolute_offset = 0;
    std::size_t length = 0;
    bool is_alias = false;
};

struct SoundArchiveMetadata final {
    std::array<SoundDirectoryEntry, 160> directory{};
    std::array<SoundMidiSummary, 15> midi{};
    std::array<SoundPcmEffect, 72> effects{};
    std::size_t nonempty_record_count = 0;
    std::size_t nonempty_effect_count = 0;
    std::size_t alias_pair_count = 0;
    std::size_t payload_start = 0x3c0;
};

SoundArchiveMetadata parse_sound_archive(std::string_view path, std::span<const std::byte> bytes);

} // namespace n3d
