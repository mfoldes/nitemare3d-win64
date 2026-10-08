#include "n3d/sound.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace n3d {
namespace {

constexpr std::size_t kDirectoryEntryCount = 160;
constexpr std::size_t kPayloadStart = 0x3c0;
constexpr std::size_t kMidiRecordCount = 15;
constexpr std::size_t kPcmRecordStart = 32;
constexpr std::size_t kPcmRecordEnd = 110;
constexpr std::size_t kNonemptyTotal = 88;
constexpr std::size_t kNonemptyEffects = 72;

bool is_tag(std::span<const std::byte> bytes, std::string_view literal) {
    return bytes.size() == literal.size() &&
           std::memcmp(bytes.data(), literal.data(), literal.size()) == 0;
}

struct SpanReader final {
    std::string_view path;
    std::span<const std::byte> bytes;
    std::size_t base_offset = 0;
    std::size_t offset = 0;

    SpanReader(std::string_view path_value,
               std::span<const std::byte> span,
               std::size_t absolute_base)
        : path(path_value), bytes(span), base_offset(absolute_base) {}

    [[nodiscard]] std::size_t absolute() const noexcept {
        return base_offset + offset;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes.size() - offset;
    }

    [[nodiscard]] bool empty() const noexcept {
        return remaining() == 0;
    }

    void require(std::size_t count, std::string_view invariant) {
        if (count > remaining()) {
            throw DataError(std::string(path), absolute(), invariant);
        }
    }

    std::uint8_t u8(std::string_view invariant) {
        require(1, invariant);
        return std::to_integer<std::uint8_t>(bytes[offset++]);
    }

    std::uint16_t u16be(std::string_view invariant) {
        const auto value = bytes_span(2, invariant);
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(value[0])) << 8u) |
            std::to_integer<std::uint8_t>(value[1]));
    }

    std::uint32_t u32be(std::string_view invariant) {
        const auto value = bytes_span(4, invariant);
        return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(value[0])) << 24u) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(value[1])) << 16u) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(value[2])) << 8u) |
               std::to_integer<std::uint8_t>(value[3]);
    }

    std::span<const std::byte> bytes_span(std::size_t count, std::string_view invariant) {
        require(count, invariant);
        const auto result = bytes.subspan(offset, count);
        offset += count;
        return result;
    }
};

std::uint64_t read_vlq(SpanReader& reader, std::string_view path) {
    std::uint64_t value = 0;
    for (std::size_t step = 0; step < 4; ++step) {
        const auto byte = reader.u8("MIDI variable-length quantity available");
        value = (value << 7u) | static_cast<std::uint64_t>(byte & 0x7Fu);
        if ((byte & 0x80u) == 0u) {
            return value;
        }
    }
    throw DataError(std::string(path), reader.absolute() - 4u,
                    "MIDI variable-length quantity fits in four bytes");
}

std::uint32_t parse_tempo_microseconds(std::span<const std::byte> payload,
                                      std::string_view path,
                                      std::size_t absolute_offset) {
    if (payload.size() != 3) {
        throw DataError(std::string(path), absolute_offset,
                        "MIDI tempo meta event uses exactly 3 payload bytes");
    }
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[0])) << 16u) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[1])) << 8u) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[2]));
}

void validate_directory_structure(std::string_view path,
                                  std::span<const SoundDirectoryEntry> directory,
                                  std::size_t total_size) {
    std::size_t nonempty_count = 0;
    for (std::size_t index = 0; index < directory.size(); ++index) {
        const auto& entry = directory[index];
        const auto field = index * 6;
        if (index == 111) {
            if (entry.length != 0) {
                throw DataError(std::string(path), field, "sound terminator is zero-length");
            }
            if (entry.absolute_offset != total_size) {
                throw DataError(std::string(path), field + 2, "sound terminator points to EOF");
            }
            continue;
        }
        if ((index >= 17 && index <= 31) || index >= 112) {
            if (entry.length != 0 || entry.absolute_offset != 0) {
                throw DataError(std::string(path), field + (entry.length == 0 ? 2 : 0),
                                "unused sound directory entry is zero");
            }
            continue;
        }
        if (index == 16) {
            if (entry.length != 0 || entry.absolute_offset != directory[32].absolute_offset) {
                throw DataError(std::string(path), field + (entry.length == 0 ? 2 : 0),
                                "sound music terminator points to effect start");
            }
            continue;
        }
        if (entry.length == 0) {
            if (index <= 15) {
                throw DataError(std::string(path), field, "MIDI and IBK records are nonempty");
            }
            if (entry.absolute_offset > total_size) {
                throw DataError(std::string(path), field + 2,
                                "empty effect offset stays within the file");
            }
            continue;
        }
        if (entry.absolute_offset < kPayloadStart || entry.absolute_offset > total_size ||
            entry.length > total_size - entry.absolute_offset) {
            throw DataError(std::string(path), field + 2,
                            "sound payload stays within file after its directory");
        }
        ++nonempty_count;
    }
    if (nonempty_count != kNonemptyTotal) {
        throw DataError(std::string(path), 0, "snd.dat has exactly 88 nonempty records");
    }
}

std::size_t validate_payload_ranges(std::string_view path,
                                    std::span<const SoundDirectoryEntry> directory,
                                    std::size_t total_size) {
    struct Range final {
        std::size_t begin;
        std::size_t end;
        std::size_t record;
    };
    std::array<Range, kNonemptyTotal> ranges{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < directory.size(); ++index) {
        const auto& entry = directory[index];
        if (entry.length != 0) {
            ranges[count++] = {entry.absolute_offset, entry.absolute_offset + entry.length, index};
        }
    }
    std::sort(ranges.begin(), ranges.begin() + static_cast<std::ptrdiff_t>(count),
              [](const Range& a, const Range& b) {
                  return a.begin < b.begin || (a.begin == b.begin && a.record < b.record);
              });
    std::size_t expected = kPayloadStart;
    std::size_t aliases = 0;
    for (std::size_t index = 0; index < count;) {
        const auto& range = ranges[index];
        if (range.begin != expected) {
            throw DataError(std::string(path), range.record * 6 + 2,
                            "sound payloads have no gap or overlap");
        }
        std::size_t next = index + 1;
        while (next < count && ranges[next].begin == range.begin) {
            if (ranges[next].end != range.end) {
                throw DataError(std::string(path), ranges[next].record * 6 + 2,
                                "aliased sound payload extents match");
            }
            ++next;
        }
        if (next != index + 1) {
            const bool permitted = next == index + 2 &&
                ((ranges[index].record == 69 && ranges[index + 1].record == 70) ||
                 (ranges[index].record == 73 && ranges[index + 1].record == 74));
            if (!permitted) {
                throw DataError(std::string(path), ranges[index].record * 6 + 2,
                                "sound aliases are only records 69/70 and 73/74");
            }
            ++aliases;
        }
        expected = range.end;
        index = next;
    }
    if (expected != total_size) {
        throw DataError(std::string(path), expected,
                        "sound payloads fill the file to EOF");
    }
    if (aliases != 2) {
        throw DataError(std::string(path), 0,
                        "sound archive has exactly two supported alias pairs");
    }
    return aliases;
}

void parse_midi_record(std::string_view path,
                       std::span<const std::byte> record_bytes,
                       std::size_t record_absolute_offset,
                       SoundMidiSummary& summary) {
    SpanReader reader(path, record_bytes, record_absolute_offset);
    const auto header_tag_offset = reader.absolute();
    const auto header_tag = reader.bytes_span(4, "MIDI header tag available");
    if (!is_tag(header_tag, "MThd")) {
        throw DataError(std::string(path), header_tag_offset,
                        "MIDI record begins with MThd");
    }

    const auto header_length_offset = reader.absolute();
    const auto mthd_length = reader.u32be("MIDI header length available");
    if (mthd_length != 6) {
        throw DataError(std::string(path), header_length_offset,
                        "MIDI header length is 6 bytes");
    }

    const auto format_offset = reader.absolute();
    const auto format = reader.u16be("MIDI format available");
    if (format != 1) {
        throw DataError(std::string(path), format_offset,
                        "MIDI format is 1");
    }

    const auto track_count_offset = reader.absolute();
    const auto track_count = reader.u16be("MIDI track count available");
    if (track_count == 0) {
        throw DataError(std::string(path), track_count_offset,
                        "MIDI track count is positive");
    }

    const auto division_offset = reader.absolute();
    const auto division = reader.u16be("MIDI division available");
    if (division != 240) {
        throw DataError(std::string(path), division_offset,
                        "MIDI division is 240 PPQN");
    }

    summary.track_count = track_count;
    summary.division = division;
    summary.tempo_us_per_quarter = 0;
    summary.last_event_tick = 0;
    summary.channel_10_note_ons = 0;

    bool saw_tempo = false;
    bool saw_end_of_track = false;
    std::uint32_t tempo_us = 0;
    std::uint64_t max_tick = 0;

    for (std::uint16_t track_index = 0; track_index < track_count; ++track_index) {
        const auto track_tag_offset = reader.absolute();
        const auto track_tag = reader.bytes_span(4, "MTrk tag available");
        if (!is_tag(track_tag, "MTrk")) {
            throw DataError(std::string(path), track_tag_offset,
                            "MIDI chunk is MTrk");
        }

        const auto chunk_length_offset = reader.absolute();
        const auto chunk_length = reader.u32be("MTrk chunk length available");
        if (reader.remaining() < static_cast<std::size_t>(chunk_length)) {
            throw DataError(std::string(path), chunk_length_offset,
                            "MTrk payload available");
        }

        const auto track_begin = reader.absolute();
        const auto track_span = reader.bytes_span(static_cast<std::size_t>(chunk_length),
                                                 "MTrk payload available");
        SpanReader track_reader(path, track_span, track_begin);

        std::uint64_t current_tick = 0;
        std::uint8_t last_status = 0;
        bool track_has_end_of_track = false;
        while (!track_reader.empty()) {
            const auto event_offset = track_reader.absolute();
            const auto delta_ticks = read_vlq(track_reader, path);
            current_tick += delta_ticks;
            if (track_reader.empty()) {
                throw DataError(std::string(path), event_offset,
                                "MIDI event status available");
            }

            const auto first_byte = track_reader.u8("MIDI event status available");
            bool running = false;
            std::uint8_t status = 0;
            if (first_byte < 0x80u) {
                if (last_status == 0) {
                    throw DataError(std::string(path), event_offset,
                                    "running status requires a previous MIDI status byte");
                }
                status = last_status;
                running = true;
            } else {
                status = first_byte;
            }

            if (status == 0xFFu) {
                const auto meta_type = track_reader.u8("MIDI meta event type available");
                const auto meta_length = read_vlq(track_reader, path);
                if (meta_length > static_cast<std::uint64_t>(track_reader.remaining())) {
                    throw DataError(std::string(path), event_offset,
                                    "MIDI meta payload available");
                }
                const auto meta_payload = track_reader.bytes_span(static_cast<std::size_t>(meta_length),
                                                                 "MIDI meta payload available");
                if (meta_type == 0x2Fu) {
                    if (meta_length != 0) {
                        throw DataError(std::string(path), event_offset,
                                        "MIDI end-of-track meta payload has zero length");
                    }
                    if (!track_reader.empty()) {
                        throw DataError(std::string(path), track_reader.absolute(),
                                        "MIDI end-of-track is terminal");
                    }
                    track_has_end_of_track = true;
                    saw_end_of_track = true;
                    last_status = 0;
                    max_tick = std::max(max_tick, current_tick);
                    break;
                }
                if (meta_type == 0x51u) {
                    if (current_tick != 0) {
                        throw DataError(std::string(path), event_offset,
                                        "MIDI tempo event is at tick 0");
                    }
                    if (saw_tempo) {
                        throw DataError(std::string(path), event_offset,
                                        "MIDI file has exactly one tempo event at tick 0");
                    }
                    tempo_us = parse_tempo_microseconds(meta_payload, path, event_offset + 2u);
                    saw_tempo = true;
                }
                last_status = 0;
                continue;
            }

            if (status == 0xF0u || status == 0xF7u) {
                const auto sys_length = read_vlq(track_reader, path);
                if (sys_length > static_cast<std::uint64_t>(track_reader.remaining())) {
                    throw DataError(std::string(path), event_offset,
                                    "MIDI sysex payload available");
                }
                track_reader.bytes_span(static_cast<std::size_t>(sys_length),
                                       "MIDI sysex payload available");
                last_status = 0;
                continue;
            }

            if (status == 0xF1u || status == 0xF2u || status == 0xF3u) {
                const auto size = (status == 0xF2u) ? 2u : 1u;
                if (track_reader.remaining() < size) {
                    throw DataError(std::string(path), event_offset,
                                    "MIDI system-common payload available");
                }
                track_reader.bytes_span(size, "MIDI system-common payload available");
                last_status = 0;
                continue;
            }

            if (status == 0xF6u) {
                last_status = 0;
                continue;
            }
            if (status >= 0xF8u && status <= 0xFEu) {
                continue; // Real-time status does not cancel channel running status.
            }

            const auto channel_type = status & 0xF0u;
            if (channel_type != 0x80u && channel_type != 0x90u && channel_type != 0xA0u &&
                channel_type != 0xB0u && channel_type != 0xC0u && channel_type != 0xD0u &&
                channel_type != 0xE0u) {
                throw DataError(std::string(path), event_offset,
                                "MIDI status byte is supported");
            }

            const auto read_data = [&track_reader, &path](std::string_view invariant) {
                const auto data_offset = track_reader.absolute();
                const auto value = track_reader.u8(invariant);
                if (value >= 0x80u) {
                    throw DataError(std::string(path), data_offset,
                                    "MIDI data byte is 7-bit");
                }
                return value;
            };
            if (!running) {
                static_cast<void>(read_data("MIDI data1 available"));
            }
            std::uint8_t data2 = 0;
            if (channel_type != 0xC0u && channel_type != 0xD0u) {
                data2 = read_data("MIDI data2 available");
            }
            if (channel_type == 0x90u && data2 != 0 && (status & 0x0Fu) == 0x09u) {
                ++summary.channel_10_note_ons;
            }
            last_status = status;
            max_tick = std::max(max_tick, current_tick);
        }

        if (!track_has_end_of_track) {
            throw DataError(std::string(path), track_begin,
                            "MIDI track ends with end-of-track meta event");
        }
    }

    if (!saw_tempo) {
        throw DataError(std::string(path), record_absolute_offset,
                        "MIDI file has exactly one tempo event at tick 0");
    }
    if (!saw_end_of_track) {
        throw DataError(std::string(path), record_absolute_offset,
                        "MIDI track ends with end-of-track meta event");
    }
    if (!reader.empty()) {
        throw DataError(std::string(path), record_absolute_offset + reader.offset,
                        "MIDI record has no trailing bytes after last MTrk");
    }

    summary.tempo_us_per_quarter = tempo_us;
    summary.last_event_tick = max_tick;
}

std::size_t count_nonempty_effects(std::span<const SoundDirectoryEntry> directory) {
    std::size_t total = 0;
    for (std::size_t index = kPcmRecordStart; index <= kPcmRecordEnd; ++index) {
        if (directory[index].length != 0) {
            ++total;
        }
    }
    return total;
}

} // namespace

SoundArchiveMetadata parse_sound_archive(std::string_view path, std::span<const std::byte> bytes) {
    SoundArchiveMetadata archive{};
    archive.payload_start = kPayloadStart;
    if (bytes.size() < kPayloadStart) {
        throw DataError(std::string(path), 0,
                        "snd.dat payload start fits within the file");
    }

    ByteReader directory_reader(path, bytes);
    for (std::size_t index = 0; index < kDirectoryEntryCount; ++index) {
        SoundDirectoryEntry entry{};
        if (directory_reader.remaining() < 6) {
            throw DataError(std::string(path), directory_reader.offset(),
                            "directory record available");
        }
        entry.length = directory_reader.u16le("directory length available");
        entry.absolute_offset = directory_reader.u32le("directory absolute offset available");

        if (index == 0) {
            entry.kind = SoundRecordKind::ibk;
        } else if (index >= 1 && index <= 15) {
            entry.kind = SoundRecordKind::midi;
        } else if (index == 16) {
            entry.kind = SoundRecordKind::music_terminator;
        } else if (index <= 31) {
            entry.kind = SoundRecordKind::placeholder;
        } else if (index >= 32 && index <= 110) {
            entry.kind = SoundRecordKind::pcm;
        } else if (index == 111) {
            entry.kind = SoundRecordKind::terminator;
        } else {
            entry.kind = SoundRecordKind::zero;
        }
        archive.directory[index] = entry;
    }

    validate_directory_structure(path, archive.directory, bytes.size());
    archive.alias_pair_count = validate_payload_ranges(path, archive.directory, bytes.size());

    const auto& ibk = archive.directory[0];
    if (ibk.length != 3204) {
        throw DataError(std::string(path), ibk.absolute_offset,
                        "record 0 length matches the IBK bank size");
    }
    if (ibk.absolute_offset != kPayloadStart) {
        throw DataError(std::string(path), ibk.absolute_offset,
                        "record 0 begins at payload start 0x3c0");
    }
    const auto ibk_span = bytes.subspan(ibk.absolute_offset, static_cast<std::size_t>(ibk.length));
    if (ibk_span.size() < 4 || !is_tag(ibk_span.subspan(0, 4), "IBK\x1a")) {
        throw DataError(std::string(path), ibk.absolute_offset,
                        "record 0 signature is IBK\x1a");
    }


    const auto effect_count = count_nonempty_effects(archive.directory);
    if (effect_count != kNonemptyEffects) {
        throw DataError(std::string(path), 0,
                        "snd.dat has exactly 72 nonempty PCM effect slots");
    }
    archive.nonempty_effect_count = effect_count;

    std::size_t midi_count = 0;
    for (std::size_t index = 1; index <= 15; ++index) {
        const auto& entry = archive.directory[index];
        if (entry.length == 0) {
            throw DataError(std::string(path), entry.absolute_offset,
                            "MIDI records are nonempty");
        }
        auto& summary = archive.midi[midi_count];
        summary.record_index = index;
        summary.absolute_offset = entry.absolute_offset;
        summary.length = entry.length;
        parse_midi_record(path, bytes.subspan(entry.absolute_offset, entry.length),
                          entry.absolute_offset, summary);
        ++midi_count;
    }
    if (midi_count != kMidiRecordCount) {
        throw DataError(std::string(path), 0,
                        "snd.dat contains exactly fifteen MIDI records");
    }

    std::size_t effect_index = 0;
    for (std::size_t index = kPcmRecordStart; index <= kPcmRecordEnd; ++index) {
        const auto& entry = archive.directory[index];
        if (entry.length == 0) {
            continue;
        }
        auto& effect = archive.effects[effect_index];
        effect.record_index = index;
        effect.absolute_offset = entry.absolute_offset;
        effect.length = entry.length;
        effect.is_alias = (index == 69 || index == 70 || index == 73 || index == 74);
        ++effect_index;
    }
    if (effect_index != kNonemptyEffects) {
        throw DataError(std::string(path), 0,
                        "snd.dat stores exactly 72 nonempty effect payloads");
    }

    archive.nonempty_record_count = kNonemptyTotal;
    return archive;
}

} // namespace n3d
