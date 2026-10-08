#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace n3d::winhelp {

enum class HotspotKind : std::uint8_t { None, Jump, Popup };

struct Font final {
    std::string face;
    std::uint8_t attributes = 0; // 1=bold, 2=italic, 4=underline
    std::uint8_t half_points = 0;
    std::uint8_t family = 0;
    std::array<std::uint8_t, 3> foreground{};
    std::array<std::uint8_t, 3> background{};
};

struct Run final {
    std::uint16_t font_index = 0;
    std::string text; // Owned source bytes (ASCII in the supported document).
    HotspotKind hotspot = HotspotKind::None;
    std::optional<std::size_t> target_topic;
    bool tab = false; // A tab stop immediately follows text.
};

struct Paragraph final {
    std::vector<Run> runs;
};

struct Topic final {
    std::string title;
    std::uint32_t virtual_offset = 0;
    std::optional<std::size_t> browse_previous;
    std::optional<std::size_t> browse_next;
    std::vector<Paragraph> paragraphs;
};

struct Keyword final {
    std::string text;
    std::vector<std::size_t> topics;
};

struct Document final {
    std::string title;
    std::vector<Font> fonts;
    std::vector<Topic> topics; // The terminal WinHelp sentinel is not a topic.
    std::vector<Keyword> keywords;
    std::size_t contents_topic = 0;
    std::vector<std::size_t> browse_chain;
};

// The supported maps.hlp is checked against its exact size and SHA-256 before parsing.
// All returned strings and vectors own their data; errors are n3d::DataError.
[[nodiscard]] Document parse_maps_help(std::string_view path, std::span<const std::byte> bytes);
[[nodiscard]] Document load_maps_help(const std::filesystem::path& path);

// Structural decoder entry for synthetic corruption tests. Not for shipping unknown files.
namespace detail {
[[nodiscard]] Document parse_maps_help_structure(std::string_view path, std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> decode_lz77(std::string_view path,
                                                    std::span<const std::byte> compressed,
                                                    std::size_t absolute_start, std::size_t max_output);
[[nodiscard]] std::vector<Paragraph> parse_paragraph_fixture(std::string_view path,
    std::span<const std::byte> link_data1, std::span<const std::byte> link_data2,
    std::size_t absolute_start, std::uint32_t known_hash);
}

} // namespace n3d::winhelp
