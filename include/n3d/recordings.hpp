#pragma once

#include "n3d/binary_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace n3d {

// The Windows DEMO record header stores three little-endian words followed by
// 8-byte event records. The names below reflect the storage order from the
// original binary rather than assigning gameplay semantics.
struct DemoHeader final {
    std::uint16_t word_0 = 0;
    std::uint16_t word_1 = 0;
    std::uint16_t word_2 = 0;
    std::uint32_t record_count = 0;
};

struct DemoEvent final {
    std::uint8_t key = 0;
    std::uint16_t flags = 0;
    std::uint8_t pad = 0;
    std::uint32_t tick = 0;
};

struct DemoRecording final {
    DemoHeader header{};
    std::vector<DemoEvent> events;
};

// parse_demo_file accepts a raw DEMO.* payload and validates the exact 6-byte
// header + 8-byte record layout without reading from disk.
DemoRecording parse_demo_file(std::string_view path, std::span<const std::byte> bytes);

// Windows BSF stores an encrypted 54-byte header whose XOR checksum must be zero.
struct BsfHeader final {
    std::uint16_t word_0 = 0;
    std::uint8_t map_check_byte = 0;
    std::uint8_t flag_byte = 0;
    std::array<std::byte, 32> name_bytes{};
    std::array<std::uint32_t, 3> block_offsets{};
    std::array<std::uint16_t, 3> block_lengths{};
    std::array<std::byte, 54> decrypted_header{};
    std::uint8_t encrypted_header_checksum = 0;
};

struct BsfArchive final {
    BsfHeader header{};
    std::array<std::vector<std::byte>, 3> blocks{};
};

// parse_bsf validates the encrypted header checksum, decodes the preserved
// header, and returns the three raw decrypted text blocks in file order.
BsfArchive parse_bsf(std::string_view path, std::span<const std::byte> bytes);

} // namespace n3d
