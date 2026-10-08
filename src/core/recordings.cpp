#include "n3d/recordings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace n3d {
namespace {

constexpr std::size_t kDemoHeaderSize = 6;
constexpr std::size_t kDemoRecordSize = 8;
constexpr std::size_t kBsfHeaderSize = 54;
constexpr std::size_t kBsfKeySize = 52;
constexpr std::string_view kBsfKeyLiteral =
    "Copyright 1992, David P Gray, Gray Design Associates";

constexpr std::array<std::byte, kBsfKeySize> make_bsf_key() {
    std::array<std::byte, kBsfKeySize> key{};
    for (std::size_t index = 0; index < kBsfKeyLiteral.size(); ++index) {
        key[index] = std::byte{static_cast<unsigned char>(kBsfKeyLiteral[index])};
    }
    return key;
}

constexpr auto kBsfKey = make_bsf_key();

std::uint16_t read_u16le_at(std::string_view path,
                           std::span<const std::byte> bytes,
                           std::size_t offset,
                           std::string_view invariant) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        throw DataError(std::string(path), offset, invariant);
    }
    const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(low | (high << 8));
}

std::uint32_t read_u32le_at(std::string_view path,
                           std::span<const std::byte> bytes,
                           std::size_t offset,
                           std::string_view invariant) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw DataError(std::string(path), offset, invariant);
    }
    const auto b0 = std::to_integer<std::uint32_t>(bytes[offset]);
    const auto b1 = std::to_integer<std::uint32_t>(bytes[offset + 1]);
    const auto b2 = std::to_integer<std::uint32_t>(bytes[offset + 2]);
    const auto b3 = std::to_integer<std::uint32_t>(bytes[offset + 3]);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

std::uint8_t to_u8(std::byte value) {
    return std::to_integer<std::uint8_t>(value);
}

std::byte xor_byte(std::byte left, std::byte right) {
    return std::byte{static_cast<unsigned char>(to_u8(left) ^ to_u8(right))};
}

std::uint8_t xor_checksum(std::span<const std::byte> bytes) {
    std::uint8_t checksum = 0;
    for (const auto value : bytes) {
        checksum ^= to_u8(value);
    }
    return checksum;
}

void require_demo_payload_size(std::string_view path, std::size_t total_size) {
    if (total_size < kDemoHeaderSize) {
        throw DataError(std::string(path), total_size,
                        "demo file has a six-byte header");
    }
    if ((total_size - kDemoHeaderSize) % kDemoRecordSize != 0) {
        throw DataError(std::string(path), total_size,
                        "demo payload size is a multiple of eight-byte records");
    }
}

void require_bsf_header(std::string_view path, std::size_t total_size) {
    if (total_size < kBsfHeaderSize) {
        throw DataError(std::string(path), total_size,
                        "bsf file has a 54-byte encrypted header");
    }
}

bool is_supported_demo_header(const DemoHeader& header) {
    return header.word_0 == 10u && header.word_1 == 5u && header.word_2 == 20u;
}


std::array<std::byte, kBsfHeaderSize> decrypt_bsf_header(std::span<const std::byte> bytes) {
    std::array<std::byte, kBsfHeaderSize> decrypted{};
    for (std::size_t index = 0; index < decrypted.size(); ++index) {
        decrypted[index] = xor_byte(bytes[index], kBsfKey[index % kBsfKey.size()]);
    }
    return decrypted;
}

} // namespace

DemoRecording parse_demo_file(std::string_view path, std::span<const std::byte> bytes) {
    require_demo_payload_size(path, bytes.size());

    DemoRecording recording{};
    recording.header.word_0 = read_u16le_at(path, bytes, 0, "demo header word 0");
    recording.header.word_1 = read_u16le_at(path, bytes, 2, "demo header word 1");
    recording.header.word_2 = read_u16le_at(path, bytes, 4, "demo header word 2");
    if (!is_supported_demo_header(recording.header)) {
        throw DataError(std::string(path), 0,
                        "demo header words are the supported (10,5,20) layout");
    }
    recording.header.record_count =
        static_cast<std::uint32_t>((bytes.size() - kDemoHeaderSize) / kDemoRecordSize);

    recording.events.reserve(recording.header.record_count);
    std::uint32_t previous_tick = 0;
    bool saw_tick = false;

    for (std::uint32_t event_index = 0; event_index < recording.header.record_count; ++event_index) {
        const auto offset = static_cast<std::size_t>(kDemoHeaderSize +
                                                    static_cast<std::size_t>(event_index) * kDemoRecordSize);
        const auto key = std::to_integer<std::uint8_t>(bytes[offset]);
        const auto flags = static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset + 1]) |
            (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 8u));
        const auto pad = std::to_integer<std::uint8_t>(bytes[offset + 3]);
        if (pad != 0u) {
            throw DataError(std::string(path), offset + 3, "demo pad byte is zero");
        }

        const auto tick = static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + 4]) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 5])) << 8u) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 6])) << 16u) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 7])) << 24u));
        if (saw_tick && tick < previous_tick) {
            throw DataError(std::string(path), offset + 4,
                            "demo tick is nondecreasing");
        }

        recording.events.push_back(DemoEvent{key, flags, pad, tick});
        previous_tick = tick;
        saw_tick = true;
    }


    return recording;
}

BsfArchive parse_bsf(std::string_view path, std::span<const std::byte> bytes) {
    require_bsf_header(path, bytes.size());
    const auto encrypted_xor = xor_checksum(bytes.subspan(0, kBsfHeaderSize));
    if (encrypted_xor != 0u) {
        throw DataError(std::string(path), 0,
                        "encrypted header XOR checksum is zero");
    }

    const auto decrypted_header = decrypt_bsf_header(bytes);
    BsfArchive archive{};
    archive.header.word_0 = read_u16le_at(path, decrypted_header, 0,
                                          "bsf decrypted word 0");
    archive.header.map_check_byte = to_u8(decrypted_header[2]);
    archive.header.flag_byte = to_u8(decrypted_header[3]);
    for (std::size_t index = 0; index < archive.header.name_bytes.size(); ++index) {
        archive.header.name_bytes[index] = decrypted_header[4 + index];
    }
    for (std::size_t index = 0; index < archive.header.block_offsets.size(); ++index) {
        archive.header.block_offsets[index] =
            read_u32le_at(path, decrypted_header, 0x24u + (index * 4u),
                          "bsf block offset is available");
    }
    for (std::size_t index = 0; index < archive.header.block_lengths.size(); ++index) {
        archive.header.block_lengths[index] =
            read_u16le_at(path, decrypted_header, 0x30u + (index * 2u),
                          "bsf block length is available");
    }
    archive.header.decrypted_header = decrypted_header;
    archive.header.encrypted_header_checksum = encrypted_xor;

    if (archive.header.word_0 != 0x681eu) {
        throw DataError(std::string(path), 0, "bsf header word matches the Windows content");
    }
    if (archive.header.flag_byte != 1) {
        throw DataError(std::string(path), 3, "bsf header flag matches the Windows content");
    }

    std::size_t previous_end = kBsfHeaderSize;
    for (std::size_t index = 0; index < archive.header.block_offsets.size(); ++index) {
        const auto offset = static_cast<std::size_t>(archive.header.block_offsets[index]);
        const auto length = static_cast<std::size_t>(archive.header.block_lengths[index]);
        if (offset != previous_end) {
            throw DataError(std::string(path), 0x24u + (index * 4u),
                            "bsf block offsets are contiguous after the 54-byte header");
        }
        if (offset > bytes.size() || length > bytes.size() - offset) {
            throw DataError(std::string(path), 0x24u + (index * 4u),
                            "bsf block spans stay within the file");
        }
        previous_end = offset + length;
    }
    if (previous_end != bytes.size()) {
        throw DataError(std::string(path), 0x30u + 6u,
                        "bsf blocks cover the file to EOF");
    }

    for (std::size_t index = 0; index < archive.blocks.size(); ++index) {
        const auto offset = static_cast<std::size_t>(archive.header.block_offsets[index]);
        auto& block = archive.blocks[index];
        block.resize(archive.header.block_lengths[index]);
        for (std::size_t byte = 0; byte < block.size(); ++byte) {
            block[byte] = xor_byte(bytes[offset + byte], kBsfKey[byte % kBsfKey.size()]);
        }
    }

    return archive;
}

} // namespace n3d
