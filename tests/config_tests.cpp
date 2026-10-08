#include "n3d/config.hpp"

#include "n3d/binary_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Fn>
void expect_data_error(Fn&& fn, std::string_view path, std::size_t offset) {
    try {
        fn();
        check(false, "expected DataError");
    } catch (const n3d::DataError& error) {
        check(error.path() == path, "error path is preserved");
        check(error.offset() == offset, "error offset is preserved");
    } catch (...) {
        check(false, "wrong exception type");
    }
}

std::array<std::byte, 20> unusual_config_fixture() {
    std::array<std::byte, 20> bytes{};
    bytes[0] = std::byte{0x30};
    bytes[1] = std::byte{0x01};
    bytes[2] = std::byte{0x12};
    bytes[3] = std::byte{0x34};
    bytes[4] = std::byte{0x40};
    bytes[5] = std::byte{0x01};
    bytes[6] = std::byte{0xC8};
    bytes[7] = std::byte{0x00};
    bytes[8] = std::byte{0x7F};
    bytes[9] = std::byte{0x00};
    bytes[10] = std::byte{0xE7};
    bytes[11] = std::byte{0xFF};
    bytes[12] = std::byte{0xA5};
    bytes[13] = std::byte{0x03};
    bytes[14] = std::byte{0x02};
    bytes[15] = std::byte{0x01};
    bytes[16] = std::byte{0x09};
    bytes[17] = std::byte{0x00};
    bytes[18] = std::byte{0x7F};
    bytes[19] = std::byte{0xFF};
    return bytes;
}

void raw_decode_roundtrips_unusual_bytes() {
    const auto fixture = unusual_config_fixture();
    const auto config = n3d::Config::decode(fixture, "config.sav");

    check(config.viewport_width == 304u, "LE viewport width decodes");

    const auto roundtrip = config.encode();
    check(std::equal(roundtrip.begin(), roundtrip.end(), fixture.begin()),
          "encode preserves original byte layout exactly");
}

void rejects_wrong_length_and_runtime_unsafe_fields() {
    std::array<std::byte, 19> short_fixture{};
    expect_data_error([&] { n3d::Config::decode(short_fixture, "config.sav"); }, "config.sav", 19);

    std::array<std::byte, 21> long_fixture{};
    expect_data_error([&] { n3d::Config::decode(long_fixture, "config.sav"); }, "config.sav", 21);

    auto unsafe = unusual_config_fixture();
    unsafe[15] = std::byte{0x01};
    unsafe[9] = std::byte{0x00};
    const auto raw = n3d::Config::decode(unsafe, "config.sav");
    expect_data_error([&] { raw.validate("config.sav", 0); }, "config.sav", 9);

    auto out_of_range = unusual_config_fixture();
    out_of_range[8] = std::byte{50};
    out_of_range[15] = std::byte{0x00};
    out_of_range[9] = std::byte{0x01};
    out_of_range[10] = std::byte{0xFF};
    const auto volume_cfg = n3d::Config::decode(out_of_range, "config.sav");
    expect_data_error([&] { volume_cfg.validate("config.sav", 0); }, "config.sav", 10);
    auto invalid_mouse = unusual_config_fixture();
    invalid_mouse[15] = std::byte{0};
    invalid_mouse[8] = std::byte{101};
    invalid_mouse[10] = std::byte{60};
    invalid_mouse[11] = std::byte{70};
    const auto sensitivity = n3d::Config::decode(invalid_mouse, "config.sav");
    expect_data_error([&] { sensitivity.validate("config.sav", 0); }, "config.sav", 8);


    auto zero_width = unusual_config_fixture();
    zero_width[0] = std::byte{0x00};
    zero_width[1] = std::byte{0x00};
    const auto width_cfg = n3d::Config::decode(zero_width, "config.sav");
    expect_data_error([&] { width_cfg.validate("config.sav", 0); }, "config.sav", 0);
}

} // namespace

int run_config_tests() {
    raw_decode_roundtrips_unusual_bytes();
    rejects_wrong_length_and_runtime_unsafe_fields();
    if (failures != 0) {
        std::cerr << failures << " config checks failed\n";
        return failures;
    }
    std::cout << "n3d config checks passed\n";
    return 0;
}
