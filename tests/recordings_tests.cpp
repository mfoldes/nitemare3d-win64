#include "n3d/recordings.hpp"

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
        check(error.path() == path, "recording error retains source path");
        check(error.offset() == offset, "recording error identifies source byte");
    } catch (...) {
        check(false, "wrong recording exception type");
    }
}

void put_le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
    bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
}

void put_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = std::byte{static_cast<std::uint8_t>(value >> shift)};
    }
}

std::vector<std::byte> make_demo() {
    std::vector<std::byte> bytes(6 + 3 * 8, std::byte{0});
    put_le16(bytes, 0, 10);
    put_le16(bytes, 2, 5);
    put_le16(bytes, 4, 20);
    for (std::size_t index = 0; index < 3; ++index) {
        const auto offset = 6 + index * 8;
        bytes[offset] = std::byte{static_cast<std::uint8_t>(index + 1)};
        put_le16(bytes, offset + 1, static_cast<std::uint16_t>(0x10 + index));
        put_le32(bytes, offset + 4, static_cast<std::uint32_t>(10 + index * 15));
    }
    return bytes;
}

constexpr std::string_view bsf_key = "Copyright 1992, David P Gray, Gray Design Associates";
static_assert(bsf_key.size() == 52);

std::vector<std::byte> make_bsf() {
    constexpr std::array<std::array<std::uint8_t, 2>, 3> text{{
        {{0x41, 0x00}}, {{0x42, 0xc3}}, {{0x10, 0x20}},
    }};
    std::array<std::byte, 54> header{};
    put_le16(header, 0, 0x681e);
    header[2] = std::byte{0xd2}; // The field is retained even though shipped flag 1 skips its check.
    header[3] = std::byte{1};
    constexpr std::array<std::uint32_t, 3> offsets{54, 56, 58};
    for (std::size_t index = 0; index < 3; ++index) {
        put_le32(header, 0x24 + index * 4, offsets[index]);
        put_le16(header, 0x30 + index * 2, 2);
    }
    std::vector<std::byte> bytes(54);
    std::uint8_t checksum = 0;
    for (std::size_t index = 0; index < header.size(); ++index) {
        const auto value = std::to_integer<std::uint8_t>(header[index]) ^
                           static_cast<std::uint8_t>(bsf_key[index % bsf_key.size()]);
        bytes[index] = std::byte{static_cast<std::uint8_t>(value)};
        checksum ^= static_cast<std::uint8_t>(value);
    }
    bytes[35] ^= std::byte{checksum}; // Adjust an opaque name byte, not an offset or length.
    for (const auto& block : text) {
        for (std::size_t index = 0; index < block.size(); ++index) {
            bytes.push_back(std::byte{static_cast<std::uint8_t>(
                block[index] ^ static_cast<std::uint8_t>(bsf_key[index]))});
        }
    }
    return bytes;
}

void demo_round_trip_and_corruption() {
    const auto bytes = make_demo();
    const auto demo = n3d::parse_demo_file("fixture.bin", bytes);
    check(demo.events.size() == 3 && demo.events[1].key == 2 &&
          demo.events[1].flags == 0x11 && demo.events[2].tick == 40,
          "DEMO fields retain their original record order and absolute ticks");

    auto bad_pad = bytes;
    bad_pad[6 + 8 + 3] = std::byte{1};
    expect_data_error([&] { static_cast<void>(n3d::parse_demo_file("fixture.bin", bad_pad)); },
                      "fixture.bin", 6 + 8 + 3);

    auto bad_tick = bytes;
    put_le32(bad_tick, 6 + 8 + 4, 9);
    expect_data_error([&] { static_cast<void>(n3d::parse_demo_file("fixture.bin", bad_tick)); },
                      "fixture.bin", 6 + 8 + 4);

    auto bad_header_word = bytes;
    put_le16(bad_header_word, 0, 11);
    expect_data_error([&] { static_cast<void>(n3d::parse_demo_file("fixture.bin", bad_header_word)); },
                      "fixture.bin", 0);

    auto short_header = bytes;
    short_header.resize(5);
    expect_data_error([&] { static_cast<void>(n3d::parse_demo_file("fixture.bin", short_header)); },
                      "fixture.bin", 5);

    auto partial_record = bytes;
    partial_record.push_back(std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_demo_file("fixture.bin", partial_record)); },
                      "fixture.bin", partial_record.size());
}

void bsf_reset_key_and_bounds() {
    const auto bytes = make_bsf();
    const auto archive = n3d::parse_bsf("fixture.bin", bytes);
    check(archive.header.word_0 == 0x681e && archive.header.map_check_byte == 0xd2 &&
          archive.header.flag_byte == 1, "BSF opaque header fields survive decryption");
    check(archive.header.encrypted_header_checksum == 0 &&
          archive.header.block_offsets == std::array<std::uint32_t, 3>{54, 56, 58},
          "BSF ciphertext check and block offsets are preserved");
    check(archive.blocks[0].size() == 2 && archive.blocks[1].size() == 2 &&
          archive.blocks[2].size() == 2 && archive.blocks[0][0] == std::byte{0x41} &&
          archive.blocks[1][0] == std::byte{0x42} && archive.blocks[2][0] == std::byte{0x10},
          "each BSF text block restarts the XOR key at byte zero");

    auto bad_checksum = bytes;
    bad_checksum[0] ^= std::byte{1};
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", bad_checksum)); },
                      "fixture.bin", 0);

    auto bad_offset = bytes;
    bad_offset[0x28] ^= std::byte{1}; // Decrypted second offset 56 -> 57.
    bad_offset[35] ^= std::byte{1};   // Preserve the ciphertext XOR check.
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", bad_offset)); },
                      "fixture.bin", 0x28);

    auto truncated = bytes;
    truncated.pop_back();
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", truncated)); },
                      "fixture.bin", 0x2c);
}

void bsf_reader_rejects_provenance_and_truncation() {
    const auto bytes = make_bsf();

    auto short_header = bytes;
    short_header.resize(53);
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", short_header)); },
                      "fixture.bin", 53);

    auto bad_word = bytes;
    bad_word[0] ^= std::byte{1};  // Decrypted word 0 becomes 0x681f.
    bad_word[35] ^= std::byte{1}; // Preserve the ciphertext XOR check.
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", bad_word)); },
                      "fixture.bin", 0);

    auto bad_flag = bytes;
    bad_flag[3] ^= std::byte{1};  // Decrypted flag byte becomes 0.
    bad_flag[35] ^= std::byte{1}; // Preserve the ciphertext XOR check.
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", bad_flag)); },
                      "fixture.bin", 3);

    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_bsf("fixture.bin", trailing)); },
                      "fixture.bin", 0x36);
}

} // namespace

int run_recordings_tests() {
    demo_round_trip_and_corruption();
    bsf_reset_key_and_bounds();
    bsf_reader_rejects_provenance_and_truncation();
    if (failures != 0) {
        std::cerr << failures << " recording checks failed\n";
        return failures;
    }
    std::cout << "n3d recordings checks passed\n";
    return 0;
}
