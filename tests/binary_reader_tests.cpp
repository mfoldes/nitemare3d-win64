#include "n3d/binary_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

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
        check(error.path() == path, "error path is preserved");
        check(error.offset() == offset, "error offset is preserved");
    } catch (...) {
        check(false, "wrong exception type");
    }
}

void reads_little_endian_values_and_spans() {
    constexpr std::array<std::byte, 15> fixture{
        std::byte{0x7f},
        std::byte{0x34}, std::byte{0x12},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
        std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    };
    n3d::ByteReader reader("fixture.bin", fixture);

    check(reader.u8() == 0x7f, "u8 value");
    check(reader.u16le() == 0x1234, "u16 little-endian value");
    check(reader.u32le() == 0x12345678, "u32 little-endian value");
    check(reader.u64le() == 0x0102030405060708ULL, "u64 little-endian value");
    check(reader.empty(), "reader is exhausted");
}

void reports_truncation_without_advancing() {
    constexpr std::array<std::byte, 2> fixture{std::byte{0x01}, std::byte{0x02}};
    n3d::ByteReader reader("fixture.bin", fixture);

    expect_data_error([&] { reader.u32le("map header available"); }, "fixture.bin", 0);
    check(reader.offset() == 0, "failed read does not advance");
    reader.require(2, "remaining fixture bytes");
    check(reader.empty(), "require advances only after validation");
}

void reports_invalid_span_request() {
    constexpr std::array<std::byte, 3> fixture{
        std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc},
    };
    n3d::ByteReader reader("fixture.bin", fixture);

    const auto prefix = reader.bytes(2, "record prefix available");
    check(prefix.size() == 2, "span count");
    check(std::to_integer<unsigned>(prefix[0]) == 0xaa, "span contents");
    expect_data_error([&] { reader.bytes(2, "record payload available"); }, "fixture.bin", 2);
    check(reader.offset() == 2, "failed span does not advance");
}

} // namespace

int run_binary_reader_tests() {
    reads_little_endian_values_and_spans();
    reports_truncation_without_advancing();
    reports_invalid_span_request();
    if (failures != 0) {
        std::cerr << failures << " reader checks failed\n";
        return failures;
    }
    std::cout << "n3d reader checks passed\n";
    return 0;
}
