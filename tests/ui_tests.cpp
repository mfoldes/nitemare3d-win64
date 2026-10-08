#include "n3d/ui.hpp"

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

void root_menu_draws_selected_font_glyph_and_shadow() {
    n3d::Assets assets{};
    auto& font = assets.uif.fonts[2];
    assets.uif_bytes.assign(font.glyphs.size(), std::byte{0});
    for (std::size_t index = 0; index < font.glyphs.size(); ++index) {
        font.glyphs[index] = n3d::UifGlyph{1u, 1u, index, 1u};
    }
    assets.uif_bytes[static_cast<std::size_t>('N' - 1)] = std::byte{0x80};
    // The label's next character maps to a distinct glyph index with a wider
    // two-pixel bitmap, so the cursor advance between characters is observable.
    constexpr std::size_t second_glyph_index = static_cast<std::size_t>('e' - 1);
    font.glyphs[second_glyph_index] = n3d::UifGlyph{1u, 2u, second_glyph_index, 1u};
    assets.uif_bytes[second_glyph_index] = std::byte{0xC0};

    for (std::size_t index = 3; index <= 5; ++index) {
        auto& image = assets.uif.images[index].image;
        image.width = static_cast<std::uint16_t>(n3d::IndexedFrame::kWidth);
        image.height = static_cast<std::uint16_t>(n3d::IndexedFrame::kHeight);
        image.bytes_per_line = image.width;
        image.pixels.assign(n3d::IndexedFrame::kPixelCount, std::byte{0x55});
    }

    std::array<std::uint8_t, 16> ega_map{};
    ega_map.fill(77u);
    ega_map[0] = 42u; // A mapped EGA black is not the literal index-zero shadow.
    ega_map[2] = 2u;
    ega_map[8] = 8u;
    ega_map[14] = 14u;

    n3d::IndexedFrame frame{};
    n3d::compose_menu(assets, frame, n3d::MenuSelection{n3d::MenuKind::Root, 0u}, ega_map);

    constexpr std::size_t x = 110u;
    constexpr std::size_t y = 60u;
    check(frame.pixels[y * n3d::IndexedFrame::kWidth + x] == 14u,
          "root selected first glyph uses EGA 14");
    check(frame.pixels[(y + 1u) * n3d::IndexedFrame::kWidth + (x + 1u)] == 0u,
          "root first glyph shadow uses literal palette index zero");
    check(frame.pixels[y * n3d::IndexedFrame::kWidth + (x + 2u)] == 14u,
          "second glyph follows the single-glyph advance");
    check(frame.pixels[y * n3d::IndexedFrame::kWidth + (x + 3u)] == 14u,
          "second glyph width spans two foreground pixels");
}

} // namespace

int run_ui_tests() {
    root_menu_draws_selected_font_glyph_and_shadow();
    if (failures != 0) {
        std::cerr << failures << " ui checks failed\n";
        return failures;
    }
    std::cout << "n3d ui checks passed\n";
    return 0;
}
