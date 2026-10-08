#include "n3d/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace n3d {
namespace {

constexpr std::size_t kFrameWidth = IndexedFrame::kWidth;
constexpr std::size_t kFrameHeight = IndexedFrame::kHeight;
constexpr std::size_t kFramePixels = IndexedFrame::kPixelCount;

void load_master_palette(IndexedFrame& frame, const Assets& assets) {
    for (std::size_t index = 0; index < frame.palette.size(); ++index) {
        const auto& rgb = assets.palette.palette[index];
        frame.palette[index] = Rgb8{rgb[0], rgb[1], rgb[2]};
    }
}

void copy_pcx_to_frame(const UifImage& source, IndexedFrame& frame) {
    if (source.image.pixels.size() != kFramePixels) {
        throw DataError("uif.dat", source.offset, "UIF image fills the logical 320x200 frame");
    }
    for (std::size_t index = 0; index < kFramePixels; ++index) {
        frame.pixels[index] = std::to_integer<std::uint8_t>(source.image.pixels[index]);
    }
}

std::uint32_t color_distance_squared(const std::array<std::uint8_t, 3>& lhs,
                                     const std::array<std::uint8_t, 3>& rhs) {
    const auto dr = static_cast<std::int32_t>(lhs[0]) - static_cast<std::int32_t>(rhs[0]);
    const auto dg = static_cast<std::int32_t>(lhs[1]) - static_cast<std::int32_t>(rhs[1]);
    const auto db = static_cast<std::int32_t>(lhs[2]) - static_cast<std::int32_t>(rhs[2]);
    return static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
}

std::array<std::array<std::uint8_t, 3>, 16> make_ega_table() {
    return {{{0, 0, 0},
             {0, 0, 42},
             {0, 42, 0},
             {0, 42, 42},
             {42, 0, 0},
             {42, 0, 42},
             {42, 21, 0},
             {42, 42, 42},
             {21, 21, 21},
             {21, 21, 63},
             {21, 63, 21},
             {21, 63, 63},
             {63, 21, 21},
             {63, 21, 63},
             {63, 63, 21},
             {63, 63, 63}}};
}

std::size_t glyph_width(const UifFont& font, std::uint8_t ch) {
    if (ch >= font.glyphs.size()) {
        return 0u;
    }
    return static_cast<std::size_t>(font.glyphs[ch].width);
}

void draw_glyph(const Assets& assets, IndexedFrame& frame, std::size_t x, std::size_t y,
                std::uint8_t ch, std::uint8_t colour, const UifFont& font) {
    if (ch >= font.glyphs.size()) {
        return;
    }
    const auto& glyph = font.glyphs[ch];
    const auto packed_width = (static_cast<std::size_t>(glyph.width) + 7u) / 8u;
    for (std::size_t row = 0; row < glyph.rows; ++row) {
        const auto src_offset = glyph.bitmap_offset + row * packed_width;
        for (std::size_t col = 0; col < glyph.width; ++col) {
            const auto byte_index = col / 8u;
            const auto bit_index = 7u - (col % 8u);
            const auto byte = std::to_integer<std::uint8_t>(assets.uif_bytes[src_offset + byte_index]);
            if ((byte & (1u << bit_index)) == 0u) {
                continue;
            }
            const auto px = x + col;
            const auto py = y + row;
            if (px < kFrameWidth && py < kFrameHeight) {
                frame.pixels[py * kFrameWidth + px] = colour;
            }
        }
    }
}

void draw_text(const Assets& assets, IndexedFrame& frame, std::string_view text,
               std::size_t x, std::size_t y, std::uint8_t colour, const UifFont& font) {
    std::size_t cursor = x;
    for (const auto ch : text) {
        const auto code = static_cast<std::uint8_t>(static_cast<unsigned char>(ch));
        if (code == 0u || code > 127u) {
            throw std::invalid_argument("UIF font only defines character codes 1..127");
        }
        const auto glyph_index = static_cast<std::uint8_t>(code - 1u);
        draw_glyph(assets, frame, cursor, y, glyph_index, colour, font);
        cursor += glyph_width(font, glyph_index) + 1u;
    }
}

bool is_episode_row_enabled(std::size_t row_index, const Assets& assets) {
    if (row_index == 0u) {
        return true;
    }
    return assets.bsf.header.flag_byte != 0u;
}

std::array<std::uint8_t, 16> build_ega_map(const Assets& assets) {
    std::array<std::uint8_t, 16> map{};
    const auto ega = make_ega_table();
    for (std::size_t entry = 0; entry < ega.size(); ++entry) {
        const std::array<std::uint8_t, 3> target = {ega[entry][0], ega[entry][1], ega[entry][2]};
        std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
        std::uint8_t best_index = 10u;
        for (std::size_t palette_index = 10; palette_index <= 245; ++palette_index) {
            const auto& rgb = assets.palette.palette[palette_index];
            const std::array<std::uint8_t, 3> candidate = {
                static_cast<std::uint8_t>(rgb[0] >> 2),
                static_cast<std::uint8_t>(rgb[1] >> 2),
                static_cast<std::uint8_t>(rgb[2] >> 2),
            };
            const auto distance = color_distance_squared(target, candidate);
            if (distance < best_distance) {
                best_distance = distance;
                best_index = static_cast<std::uint8_t>(palette_index);
            }
        }
        map[entry] = best_index;
    }
    return map;
}

void draw_menu_label(const Assets& assets, IndexedFrame& frame,
                     std::string_view label, std::size_t x, std::size_t y,
                     bool selected, bool disabled,
                     const std::array<std::uint8_t, 16>& map, const UifFont& font) {
    draw_text(assets, frame, label, x + 1u, y + 1u, 0u, font);
    const auto colour = disabled ? map[8u] : (selected ? map[14u] : map[2u]);
    draw_text(assets, frame, label, x, y, colour, font);
}

void draw_root_menu(const Assets& assets, IndexedFrame& frame,
                    std::uint8_t selected_row, bool active_game,
                    const std::array<std::uint8_t, 16>& map,
                    const UifFont& font) {
    constexpr std::array<std::string_view, 6> no_game{
        "New game", "Configure game...", "Load game...", "Instructions", "Demo", "Quit"};
    constexpr std::array<std::string_view, 7> active{
        "New game", "Configure game...", "Load game...", "Save game...",
        "Instructions", "Return to game", "Quit"};
    const std::size_t count = active_game ? active.size() : no_game.size();
    for (std::size_t row = 0; row < count; ++row)
        draw_menu_label(assets, frame, active_game ? active[row] : no_game[row],
                        110u, 60u + 15u * row, row == selected_row, false, map, font);
}

void draw_episode_menu(const Assets& assets, IndexedFrame& frame,
                       std::uint8_t selected_row,
                       const std::array<std::uint8_t, 16>& map,
                       const UifFont& font) {
    const std::array<std::pair<std::string_view, std::size_t>, 3> rows = {{
        {"Episode 1", 80u},
        {"Episode 2", 100u},
        {"Episode 3", 120u},
    }};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto& entry = rows[row];
        const auto disabled = !is_episode_row_enabled(row, assets);
        const auto is_selected = row == static_cast<std::size_t>(selected_row);
        draw_menu_label(assets, frame, entry.first, 110u, entry.second, is_selected, disabled, map, font);
    }
}

void draw_difficulty_menu(const Assets& assets, IndexedFrame& frame,
                         std::uint8_t selected_row,
                         const std::array<std::uint8_t, 16>& map,
                         const UifFont& font) {
    const std::array<std::pair<std::string_view, std::size_t>, 3> rows = {{
        {"Be gentle!", 80u},
        {"I'm tough!", 100u},
        {"Let's party!", 120u},
    }};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto& entry = rows[row];
        const auto is_selected = row == static_cast<std::size_t>(selected_row);
        draw_menu_label(assets, frame, entry.first, 110u, entry.second, is_selected, false, map, font);
    }
}

} // namespace

std::array<std::uint8_t, 16> compute_ega_palette_map(const Assets& assets) {
    return build_ega_map(assets);
}

std::size_t indexed_text_width(const Assets& assets, std::uint8_t font_index,
                               std::string_view text) {
    if (font_index >= assets.uif.fonts.size()) throw std::invalid_argument("invalid UIF font");
    const auto& font = assets.uif.fonts[font_index];
    std::size_t width = 0;
    for (const auto ch : text) {
        const auto code = static_cast<std::uint8_t>(static_cast<unsigned char>(ch));
        if (code == 0u || code > 127u) throw std::invalid_argument("UIF character out of range");
        width += glyph_width(font, static_cast<std::uint8_t>(code - 1u)) + 1u;
    }
    return width;
}

void draw_indexed_text(const Assets& assets, IndexedFrame& frame,
                       std::uint8_t font_index, std::string_view text,
                       int x, int y, std::uint8_t colour) {
    if (font_index >= assets.uif.fonts.size() || x < 0 || y < 0) {
        throw std::invalid_argument("invalid UIF font or text position");
    }
    draw_text(assets, frame, text, static_cast<std::size_t>(x),
              static_cast<std::size_t>(y), colour, assets.uif.fonts[font_index]);
}

void compose_title(const Assets& assets, IndexedFrame& frame) {
    load_master_palette(frame, assets);
    copy_pcx_to_frame(assets.uif.images[0], frame); // UIF directory entry 3.
}

void compose_menu(const Assets& assets, IndexedFrame& frame, MenuSelection selection,
                  const std::array<std::uint8_t, 16>& ega_palette_map,
                  std::span<const SaveMenuSlot> slots) {
    const auto kind = static_cast<std::uint8_t>(selection.kind);
    const auto row_limit = selection.kind == MenuKind::Root ?
        (selection.active_game ? 7u : 6u) :
        (selection.kind == MenuKind::Save || selection.kind == MenuKind::Load ? 10u : 3u);
    if ((kind != static_cast<std::uint8_t>(MenuKind::Root) &&
         kind != static_cast<std::uint8_t>(MenuKind::Episode) &&
         kind != static_cast<std::uint8_t>(MenuKind::Difficulty) &&
         kind != static_cast<std::uint8_t>(MenuKind::Save) &&
         kind != static_cast<std::uint8_t>(MenuKind::Load)) ||
        selection.selected_row >= row_limit) {
        throw std::invalid_argument("unsupported menu kind or selected row");
    }
    load_master_palette(frame, assets);
    copy_pcx_to_frame(assets.uif.images[kind - 3u], frame);

    const auto& font = assets.uif.fonts[2];
    switch (selection.kind) {
    case MenuKind::Root:
        draw_root_menu(assets, frame, selection.selected_row, selection.active_game,
                       ega_palette_map, font);
        break;
    case MenuKind::Episode:
        draw_episode_menu(assets, frame, selection.selected_row, ega_palette_map, font);
        break;
    case MenuKind::Difficulty:
        draw_difficulty_menu(assets, frame, selection.selected_row, ega_palette_map, font);
        break;
    case MenuKind::Save:
    case MenuKind::Load: {
        for (std::size_t row = 0; row < 10u; ++row) {
            const SaveMenuSlot* slot = row < slots.size() ? &slots[row] : nullptr;
            const auto label = save_menu_slot_label_view(slot);
            const auto y = static_cast<std::size_t>(kSaveLoadSlotY +
                                                   static_cast<int>(row) * kSaveLoadSlotRowPitch);
            draw_menu_label(assets, frame, label, static_cast<std::size_t>(kSaveLoadSlotX), y,
                            selection.highlight && row == selection.selected_row,
                            selection.kind == MenuKind::Load && (!slot || !slot->present),
                            ega_palette_map, assets.uif.fonts[kSaveLoadSlotFont]);
        }
        break;
    }
    }
}


} // namespace n3d
