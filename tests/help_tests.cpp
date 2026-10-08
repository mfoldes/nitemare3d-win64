#include "n3d/help.hpp"
#include "n3d/ui.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

n3d::Assets help_assets(std::string_view first_page) {
    n3d::Assets assets{};
    assets.bsf.header.block_offsets[1] = 149;
    std::string text(first_page);
    for (std::size_t page = 1; page < n3d::HelpPager::kPageCount; ++page) {
        text += "#A\r\n";
    }
    auto& block = assets.bsf.blocks[1];
    for (const char ch : text) block.push_back(static_cast<std::byte>(ch));

    auto& glyph = assets.uif.fonts[1].glyphs[static_cast<std::size_t>('A' - 1)];
    glyph = n3d::UifGlyph{1, 1, 0, 1};
    assets.uif_bytes.push_back(std::byte{0x80});
    auto& art = assets.uif.images[16 - 3];
    art.directory_index = 16;
    art.image.width = 320;
    art.image.height = 200;
    art.image.pixels.assign(n3d::IndexedFrame::kPixelCount, std::byte{0x55});

    auto& episode = assets.episodes[0];
    episode.images.sequences[257].offset = 1;
    episode.images.sequences[257].frame_count = 1;
    episode.images.records.push_back(n3d::ImgRecordMetadata{0, 1, 1, {}, 0, 1});
    episode.image_bytes.push_back(std::byte{0x33});
    return assets;
}

void page_bounds_colours_and_first_image() {
    auto assets = help_assets("A%C9%A%d%A%o1%\r\n");
    n3d::HelpPager pager(assets);
    n3d::IndexedFrame frame{};
    check(!pager.previous(), "first page does not move backward");
    n3d::render_help(pager, assets, 1, frame);
    const auto pixel = [&](std::size_t x, std::size_t y) {
        return frame.pixels[y * n3d::IndexedFrame::kWidth + x];
    };
    const auto default_colour = n3d::compute_ega_palette_map(assets)[8];
    check(pixel(12, 7) == default_colour, "first glyph uses mapped EGA default");
    check(pixel(14, 7) == 9, "%C uses a direct indexed foreground");
    check(pixel(16, 7) == default_colour, "%d restores the mapped default");
    check(pixel(18, 7) == 0x33, "%o blits the first frame without cursor advance");
    check(pixel(10, 5) == 0xBA, "help page body uses the recovered indexed fill");
    n3d::render_help_order_banner(assets, frame);
    check(pixel(69, 67) == 20 && pixel(254, 96) == 10,
          "order banner paints recovered EGA8 box with EGA0 offset shadow");

    for (std::size_t page = 1; page < n3d::HelpPager::kPageCount; ++page) {
        check(pager.next(), "next reaches each subsequent page");
    }
    check(pager.page_index() == 21 && !pager.next(), "last page stays in bounds");
    n3d::render_help(pager, assets, 1, frame);
    check(pixel(12, 7) == default_colour, "last page draws authored text with reset colour");
    check(pixel(18, 7) == 0xBA, "last page does not retain earlier image pixels");
    check(pager.previous() && pager.page_index() == 20, "previous changes stored page index");
}

void malformed_token_does_not_cross_a_page() {
    const auto assets = help_assets("%o1#A\r\n");
    try {
        const n3d::HelpPager pager(assets);
        check(false, "unclosed image token must fail");
    } catch (const n3d::DataError& error) {
        check(error.path() == "nite3d.bsf" && error.offset() == 149,
              "unclosed image token reports its owning BSF byte");
    }
}

} // namespace

int run_help_tests() {
    page_bounds_colours_and_first_image();
    malformed_token_does_not_cross_a_page();
    if (failures != 0) {
        std::cerr << failures << " help checks failed\n";
        return failures;
    }
    std::cout << "n3d help checks passed\n";
    return 0;
}
