#include "../game/internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace n3d::detail {
namespace {

// 1018:0374 fills the entire field before right-aligning the glyphs. The
// original does not clip text at the left edge of the field (notably 100%).
constexpr std::array<int, 3> kFontHeight{5, 7, 8};
constexpr int kWidth = static_cast<int>(IndexedFrame::kWidth);
constexpr int kHeight = static_cast<int>(IndexedFrame::kHeight);

void fill(IndexedFrame& frame, int x, int y, int width, int height, std::uint8_t colour) {
    if (x < 0 || y < 0 || width < 0 || height < 0 ||
        x > kWidth || width > kWidth - x || y > kHeight || height > kHeight - y)
        throw std::out_of_range("HUD rectangle exceeds indexed frame");
    for (int row = 0; row < height; ++row)
        std::fill_n(frame.pixels.data() + static_cast<std::size_t>(y + row) * kWidth + x,
                    width, colour);
}

std::uint8_t ega(const GameState& state, unsigned index) {
    return state.ega_map[index];
}

void field(const Assets& assets, IndexedFrame& frame, const GameState& state,
           int x, int y, int width, std::uint8_t font, std::string_view text) {
    if (font >= kFontHeight.size()) throw std::invalid_argument("HUD UIF font index invalid");
    fill(frame, x, y, width, kFontHeight[font], ega(state, 0));
    const auto text_width = indexed_text_width(assets, font, text);
    const int left = x + width - static_cast<int>(text_width);
    if (left < 0) throw std::out_of_range("HUD field text exceeds indexed frame");
    draw_indexed_text(assets, frame, font, text, left, y, ega(state, 11));
}

char* number(char* begin, char* end, std::uint32_t value) {
    const auto result = std::to_chars(begin, end, value);
    if (result.ec != std::errc{}) throw std::out_of_range("HUD number buffer exhausted");
    return result.ptr;
}

char* signed_number(char* begin, char* end, int value) {
    const auto result = std::to_chars(begin, end, value);
    if (result.ec != std::errc{}) throw std::out_of_range("HUD number buffer exhausted");
    return result.ptr;
}

const ImgRecordMetadata& hud_image(const Assets& assets, const GameState& state,
                                   unsigned frame_index) {
    if (state.episode == 0 || state.episode > assets.episodes.size())
        throw std::out_of_range("HUD episode outside IMG archive range");
    const auto& episode = assets.episodes[state.episode - 1];
    const auto& sequence = episode.images.sequences[511];
    if (!sequence.offset || frame_index >= sequence.frame_count ||
        sequence.first_record_index >= episode.images.records.size() ||
        frame_index >= episode.images.records.size() - sequence.first_record_index)
        throw DataError("IMG", sequence.descriptor_offset, "HUD IMG ID 511 frame exists");
    const auto& image = episode.images.records[sequence.first_record_index + frame_index];
    if (!image.width || !image.height ||
        image.pixels_size != static_cast<std::size_t>(image.width) * image.height ||
        image.pixels_offset > episode.image_bytes.size() ||
        image.pixels_size > episode.image_bytes.size() - image.pixels_offset)
        throw DataError("IMG", image.offset, "HUD IMG frame pixel span is valid");
    return image;
}

// IMG pixels are x-major, with each column containing image.height bytes.
// A negative key is the original opaque mode. A positive key tests the source
// palette index before writing; it does not remap that index.
void blit(IndexedFrame& frame, const Assets& assets, const GameState& state,
          unsigned frame_index, int x, int y, int key = -1, int bottom = kHeight - 1,
          bool skip_bottom_touch = false) {
    const auto& image = hud_image(assets, state, frame_index);
    // 1018:0428 rejects opaque equipment frames when y + height >= 200.
    if (skip_bottom_touch && y + image.height >= kHeight) return;
    const int rows = std::min<int>(image.height, bottom - y + 1);
    if (rows <= 0) return;
    if (x < 0 || y < 0 || x + image.width > kWidth || y + rows > kHeight)
        throw std::out_of_range("HUD IMG frame exceeds indexed frame");
    const auto& pixels = assets.episodes[state.episode - 1].image_bytes;
    for (int col = 0; col < image.width; ++col) {
        const auto start = image.pixels_offset + static_cast<std::size_t>(col) * image.height;
        for (int row = 0; row < rows; ++row) {
            const auto colour = std::to_integer<std::uint8_t>(pixels[start + row]);
            if (key >= 0 && colour == key) continue;
            frame.pixels[static_cast<std::size_t>(y + row) * kWidth + x + col] = colour;
        }
    }
}

void shadow_text(const Assets& assets, IndexedFrame& frame, const GameState& state,
                 std::uint8_t font, std::string_view text, int x, int y,
                 std::uint8_t colour) {
    draw_indexed_text(assets, frame, font, text, x + 1, y + 1, ega(state, 0));
    draw_indexed_text(assets, frame, font, text, x, y, colour);
}

void sensor_panel(const GameState& state, IndexedFrame& frame) {
    const auto& player = state.player;
    // bbca decrements the feedback before painting: 3,2,1,0 are red,
    // while the pass reaching -1 clears the panel. UIF5 is preserved until
    // any sensor or damage operation first touches the panel.
    if (player.damage_feedback >= 0) {
        fill(frame, 256, 162, 62, 36, ega(state, 12));
        return;
    }
    if (!player.map_sensor && !player.enemy_sensor) {
        if (state.sensor_panel_cleared) fill(frame, 256, 162, 62, 36, 0);
        return;
    }
    const int origin_x = std::clamp(static_cast<int>(player.cell_x) - 31, 0, 2);
    const int origin_y = std::clamp(static_cast<int>(player.cell_y) - 18, 0, 28);
    if (player.map_sensor) {
        // GameState::automap follows the original segment-6 x*64+y order;
        // MAP cells, in contrast, are y*64+x. Do not transpose the window.
        for (int x = 0; x < 62; ++x)
            for (int y = 0; y < 36; ++y)
                frame.pixels[static_cast<std::size_t>(162 + y) * kWidth + 256 + x] =
                    state.automap[static_cast<std::size_t>(origin_x + x) * 64 + origin_y + y];
    } else {
        fill(frame, 256, 162, 62, 36, 0);
    }
    // 1010:bbca paints live guards over either map or the cleared panel only
    // if the enemy meter is >15 or odd. a0c6 excludes these four classes.
    if (player.enemy_sensor && (player.enemy_meter > 15 || (player.enemy_meter & 1u))) {
        if (state.level.guard_count > state.level.guards.size())
            throw std::out_of_range("HUD guard count exceeds level capacity");
        for (std::size_t i = 0; i < state.level.guard_count; ++i) {
            const auto& guard = state.level.guards[i];
            if (guard.object_slot >= state.level.object_count ||
                guard.object_slot >= state.level.objects.size())
                throw std::out_of_range("HUD guard object slot outside level");
            const auto& object = state.level.objects[guard.object_slot];
            const auto cls = object.class_id;
            if (guard.state == 10 || cls == 0x15 || cls == 0x16 ||
                cls == 0x19 || cls == 0x21) continue;
            const int x = (static_cast<int>(object.world_x) >> 6) - origin_x;
            const int y = (static_cast<int>(object.world_y) >> 6) - origin_y;
            if (x >= 0 && x < 62 && y >= 0 && y < 36)
                frame.pixels[static_cast<std::size_t>(162 + y) * kWidth + 256 + x] = ega(state, 12);
        }
    }
    // bbca paints the single ee26 result before b1a4(7) noise. The B
    // service preserves this point even when speckles overwrite the original
    // scratch origin, so enemy-only stale-origin cases remain exact.
    if (state.automap_heading_valid) {
        const int x = 256 + state.automap_heading_x;
        const int y = 162 + state.automap_heading_y;
        if (x < 0 || x >= kWidth || y < 0 || y >= kHeight)
            throw std::out_of_range("HUD heading pixel exceeds indexed frame");
        frame.pixels[static_cast<std::size_t>(y) * kWidth + x] = ega(state, 14);
    }
    if (state.automap_speckle_count > state.automap_speckles.size())
        throw std::out_of_range("HUD automap speckle count exceeds capacity");
    if (player.map_sensor) {
        for (std::size_t i = 0; i < state.automap_speckle_count; ++i) {
            const auto& speckle = state.automap_speckles[i];
            if (speckle.x >= 62 || speckle.y >= 36)
                throw std::out_of_range("HUD automap speckle outside panel");
            frame.pixels[static_cast<std::size_t>(162 + speckle.y) * kWidth + 256 + speckle.x] =
                ega(state, 15);
        }
    }
}

void draw_popup(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    if (state.warp_row > 2 || state.player.octant >= 8 ||
        state.player.cell_x >= 64 || state.player.cell_y >= 64)
        throw std::out_of_range("HUD warp popup state outside source range");
    constexpr std::array<int, 8> dx{0, 1, 1, 0, 0, -1, -1, 0};
    constexpr std::array<int, 8> dy{-1, 0, 0, 1, 1, 0, 0, -1};
    const int wx = state.player.cell_x + dx[state.player.octant];
    const int wy = state.player.cell_y + dy[state.player.octant];
    if (wx < 0 || wx >= 64 || wy < 0 || wy >= 64)
        throw std::out_of_range("HUD warp popup source outside MAP");
    const auto id = state.level.cells[static_cast<std::size_t>(wy) * 64 + wx].wall_id;
    const auto cls = state.level.wall_class_by_id[id];
    if (cls < 0x0d || cls > 0x14)
        throw std::invalid_argument("HUD warp popup wall class is not a climb wall");
    int first = -1;
    int last = -1;
    for (std::size_t n = 0; n < state.level.wall_class_by_id.size(); ++n)
        if (state.level.wall_class_by_id[n] == cls) { first = static_cast<int>(n); break; }
    for (const auto& cell : state.level.cells)
        if (state.level.wall_class_by_id[cell.wall_id] == cls)
            last = std::max(last, static_cast<int>(cell.wall_id));
    if (first < 0 || last < 0) throw std::invalid_argument("HUD warp popup class has no wall ID");
    // 1018:1ee0/13ac: draw a 4-pixel-offset box shadow, followed by three
    // source rows, each with literal index-0 text shadow.
    fill(frame, 104, 52, 88, 33, ega(state, 0));
    fill(frame, 100, 48, 88, 33, ega(state, 8));
    constexpr std::array<std::string_view, 3> labels{"Climb up", "Climb down", "Cancel"};
    for (int row = 0; row < 3; ++row) {
        const bool disabled = (row == 0 && id == last) || (row == 1 && id == first);
        if (disabled && row == state.warp_row)
            throw std::invalid_argument("HUD warp popup selected row is disabled");
        draw_indexed_text(assets, frame, 2, labels[row], 111, 51 + row * 10, 0);
        draw_indexed_text(assets, frame, 2, labels[row], 110, 50 + row * 10,
                          disabled ? ega(state, 8) : row == state.warp_row ? ega(state, 14) : ega(state, 2));
    }
}

void draw_message(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    if (state.message.empty()) throw std::invalid_argument("HUD modal message has no source text");
    std::size_t lines = 0;
    std::size_t max_width = 0;
    for (std::size_t begin = 0; begin <= state.message.size();) {
        const auto end = state.message.find('\n', begin);
        const auto line = state.message.substr(begin, end == std::string_view::npos
                                                      ? end : end - begin);
        max_width = std::max(max_width, indexed_text_width(assets, 1, line));
        ++lines;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    const int center_x = state.scene.ready ? state.scene.viewport_center_x : 160;
    const int center_y = state.scene.ready ? state.scene.viewport_center_y : 80;
    const int top = center_y - static_cast<int>(lines * 11) / 2 - 8;
    const int left = center_x - static_cast<int>(max_width) / 2 - 8;
    fill(frame, left + 4, top + 4, static_cast<int>(max_width) + 16,
         static_cast<int>(lines * 11) + 16, ega(state, 0));
    fill(frame, left, top, static_cast<int>(max_width) + 16,
         static_cast<int>(lines * 11) + 16, ega(state, 8));
    std::size_t row = 0;
    for (std::size_t begin = 0; begin <= state.message.size();) {
        const auto end = state.message.find('\n', begin);
        const auto line = state.message.substr(begin, end == std::string_view::npos
                                                      ? end : end - begin);
        const int x = center_x - static_cast<int>(indexed_text_width(assets, 1, line)) / 2;
        const int y = top + 8 + static_cast<int>(row * 11);
        shadow_text(assets, frame, state, 1, line, x, y, ega(state, 14));
        ++row;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
}

void draw_intermission(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    // UIF directory index 14, not a palette-bearing replacement for the live
    // master palette in frame. Its embedded PCX trailer is ignored by Win16.
    const auto& art = assets.uif.images[14 - 3];
    if (art.directory_index != 14 || art.image.width != kWidth ||
        art.image.height != kHeight || art.image.pixels.size() != IndexedFrame::kPixelCount)
        throw DataError("uif.dat", art.offset, "UIF14 intermission fills 320x200");
    for (std::size_t i = 0; i < frame.pixels.size(); ++i)
        frame.pixels[i] = std::to_integer<std::uint8_t>(art.image.pixels[i]);
    constexpr std::array<int, 5> ys{60, 80, 100, 120, 140};
    constexpr std::array<std::uint8_t, 5> fonts{2, 2, 2, 2, 1};
    const std::array<std::uint32_t, 5> values{
        static_cast<std::uint32_t>(state.level_index) + 1u,
        state.intermission_hostiles, state.intermission_panels,
        state.intermission_bonus, state.player.score};
    for (std::size_t i = 0; i < values.size(); ++i) {
        char text[16];
        const char* end = number(text, text + sizeof(text), values[i]);
        shadow_text(assets, frame, state, fonts[i], {text, end}, 220, ys[i], ega(state, 14));
    }
}

} // namespace

void draw_hud(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    const auto& player = state.player;
    sensor_panel(state, frame);

    // 1010:a3b6(0): weapon, fields, icons, chrome, gauges and coordinates,
    // in the original order; all computations read source state without writes.
    if (state.draw_weapon != 0xff) {
        if (state.draw_weapon >= 4 || state.draw_weapon_frame >= 10 ||
            (state.draw_weapon_recoil != 0 && state.draw_weapon_recoil != 3 &&
             state.draw_weapon_recoil != 6))
            throw std::out_of_range("HUD hand-sprite snapshot outside source range");
        constexpr std::array<int, 10> weapon_y{170, 170, 160, 150, 140, 130, 120, 110, 110, 110};
        const int y = weapon_y[state.draw_weapon_frame] + state.draw_weapon_recoil;
        const int viewport_bottom = state.scene.ready ? state.scene.viewport_bottom : 155;
        blit(frame, assets, state, 24u + state.draw_weapon, 146, y, 0x29, viewport_bottom);
    }

    char text[64];
    char* end = number(text, text + sizeof(text), state.episode);
    *end++ = ' '; *end++ = ':'; *end++ = ' ';
    end = number(end, text + sizeof(text), static_cast<std::uint32_t>(state.level_index) + 1u);
    field(assets, frame, state, 50, 171, 31, 1, {text, end});
    end = number(text, text + sizeof(text), player.score);
    field(assets, frame, state, 50, 191, 31, 0, {text, end});

    const auto hp = std::min<unsigned>(player.health, 100);
    blit(frame, assets, state, 13u + (hp + 9u) / 10u, 3, 162, -1, kHeight - 1, true);
    end = number(text, text + sizeof(text), hp);
    *end++ = '%';
    field(assets, frame, state, 7, 192, 10, 0, {text, end});

    for (int i = 0; i < 3; ++i) {
        // The first two source bytes are clamped as signed 8-bit integers;
        // 4c44 (third byte) is clamped as unsigned.
        end = i == 2 ? number(text, text + sizeof(text), std::min<unsigned>(player.ammo[i], 100))
                     : signed_number(text, text + sizeof(text),
                                     std::min<int>(static_cast<std::int8_t>(player.ammo[i]), 100));
        field(assets, frame, state, 109, 164 + 13 * i, 20, 1, {text, end});
    }

    if (player.weapon != 0xff)
        blit(frame, assets, state, player.weapon, 135, 162, -1, kHeight - 1, true);
    constexpr std::array<int, 4> key_x{167, 189, 167, 189};
    constexpr std::array<int, 4> key_y{162, 162, 182, 182};
    for (unsigned i = 0; i < 4; ++i)
        if (player.keys & (1u << i))
            blit(frame, assets, state, 4u + i, key_x[i], key_y[i], -1, kHeight - 1, true);
    for (unsigned i = 0; i < 8; ++i)
        if (player.cards & (1u << i))
            blit(frame, assets, state, 8u + i, 211 + 22 * static_cast<int>(i), 162,
                 -1, kHeight - 1, true);

    const int viewport_bottom = state.scene.ready ? state.scene.viewport_bottom : 155;
    blit(frame, assets, state, 10, 8, 4, 0, viewport_bottom);
    blit(frame, assets, state, 11, 290, 4, 0, viewport_bottom);
    blit(frame, assets, state, 12, 139, 4, 0, viewport_bottom);
    if (!state.hud_message.empty()) {
        const int width = static_cast<int>(indexed_text_width(assets, 1, state.hud_message));
        shadow_text(assets, frame, state, 1, state.hud_message,
                    (kWidth - width) >> 1, 23, ega(state, 12));
    }

    const auto enemy = std::min<unsigned>(player.enemy_meter, 100) * 18 / 100;
    fill(frame, 233, 181, static_cast<int>(enemy), 7,
         ega(state, player.enemy_sensor ? 12 : 4));
    fill(frame, 233 + static_cast<int>(enemy), 181, 18 - static_cast<int>(enemy), 7, ega(state, 0));
    const auto map = std::min<unsigned>(player.map_meter, 100) * 18 / 100;
    fill(frame, 211, 181, static_cast<int>(map), 7,
         player.map_sensor ? ega(state, 10) : static_cast<std::uint8_t>(ega(state, 2) - 5));
    fill(frame, 211 + static_cast<int>(map), 181, 18 - static_cast<int>(map), 7, ega(state, 0));
    fill(frame, 219, 194, 2, 2, ega(state, state.panel_nearby ? 15 : 7));
    end = number(text, text + sizeof(text), player.cell_x);
    *end++ = ',';
    end = number(end, text + sizeof(text), player.cell_y);
    field(assets, frame, state, 140, 192, 19, 0, {text, end});
}

void draw_overlay(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    switch (state.screen) {
    case Screen::warp_popup: draw_popup(state, assets, frame); break;
    case Screen::modal_message: draw_message(state, assets, frame); break;
    case Screen::intermission: draw_intermission(state, assets, frame); break;
    default: break;
    }
}

} // namespace n3d::detail
