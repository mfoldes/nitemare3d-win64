#include "n3d/binary_reader.hpp"
#include "n3d/winhelp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

template <typename Action>
void expects_error(Action action, std::size_t offset) {
    try {
        action();
        check(false, "expected WinHelp DataError");
    } catch (const n3d::DataError& error) {
        check(error.path() == "synthetic.hlp", "WinHelp error preserves input path");
        check(error.offset() == offset, "WinHelp error names absolute file offset");
    } catch (...) {
        check(false, "WinHelp used non-DataError exception");
    }
}

void invalid_lz_distance_and_size() {
    constexpr std::array<std::byte, 3> bad_distance{std::byte{1}, std::byte{0}, std::byte{0}};
    expects_error([&] { (void)n3d::winhelp::detail::decode_lz77("synthetic.hlp", bad_distance, 200, 64); },
                  201);
    constexpr std::array<std::byte, 2> literal{std::byte{0}, std::byte{'X'}};
    expects_error([&] { (void)n3d::winhelp::detail::decode_lz77("synthetic.hlp", literal, 300, 0); },
                  301);
    constexpr std::array<std::byte, 2> incomplete{std::byte{1}, std::byte{0}};
    expects_error([&] { (void)n3d::winhelp::detail::decode_lz77("synthetic.hlp", incomplete, 400, 64); },
                  401);
    constexpr std::array<std::byte, 4> overlap{std::byte{2}, std::byte{'A'},
                                                std::byte{0}, std::byte{0}};
    const auto decoded = n3d::winhelp::detail::decode_lz77("synthetic.hlp", overlap, 500, 4);
    check(decoded == std::vector<std::byte>{std::byte{'A'}, std::byte{'A'}, std::byte{'A'}, std::byte{'A'}},
          "overlapping LZ backreference decodes within bound");
}

void unknown_command_and_dangling_target() {
    // 0x8010 -> eight bytes after the compressed sizes: six known flags, 0x81, 0xff.
    constexpr std::array<std::byte, 11> unknown{
        std::byte{0x10}, std::byte{0x80}, std::byte{6},
        std::byte{0}, std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x81}, std::byte{0xff}};
    constexpr std::array<std::byte, 3> two_strings{std::byte{'A'}, std::byte{0}, std::byte{0}};
    expects_error([&] { (void)n3d::winhelp::detail::parse_paragraph_fixture(
                        "synthetic.hlp", unknown, two_strings, 600, 0x12345678u); },
                  609);

    // E3 begins a jump to a non-existent hash. There are three text strings for E3/89/FF.
    constexpr std::array<std::byte, 16> dangling{
        std::byte{0x1a}, std::byte{0x80}, std::byte{10},
        std::byte{0}, std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0xe3}, std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{0x89}, std::byte{0xff}};
    constexpr std::array<std::byte, 5> three_strings{
        std::byte{0}, std::byte{'G'}, std::byte{'o'}, std::byte{0}, std::byte{0}};
    expects_error([&] { (void)n3d::winhelp::detail::parse_paragraph_fixture(
                        "synthetic.hlp", dangling, three_strings, 700, 0x11223344u); },
                  710);
    const auto paragraphs = n3d::winhelp::detail::parse_paragraph_fixture(
        "synthetic.hlp", dangling, three_strings, 700, 0x04030201u);
    check(paragraphs.size() == 1 && paragraphs.front().runs.size() == 1 &&
          paragraphs.front().runs.front().text == "Go" &&
          paragraphs.front().runs.front().hotspot == n3d::winhelp::HotspotKind::Jump &&
          paragraphs.front().runs.front().target_topic == 0,
          "E3 resolves to owned jump run without borrowed text");
}

void exact_identity_gate() {
    constexpr std::array<std::byte, 16> invalid{};
    expects_error([&] { (void)n3d::winhelp::parse_maps_help("synthetic.hlp", invalid); },
                  invalid.size());
}

} // namespace

int run_winhelp_tests() {
    invalid_lz_distance_and_size();
    unknown_command_and_dangling_target();
    exact_identity_gate();
    if (failures) std::cerr << failures << " WinHelp checks failed\n";
    else std::cout << "n3d WinHelp parser checks passed\n";
    return failures;
}
