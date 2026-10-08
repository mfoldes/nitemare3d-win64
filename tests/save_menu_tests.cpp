#include "n3d/game.hpp"
#include "n3d/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void seed_font_fixture(n3d::Assets& assets) {
    constexpr std::size_t small_rows = 10;
    constexpr std::size_t large_rows = 12;
    assets.uif_bytes.assign(127 * (small_rows + large_rows), std::byte{0});
    for (std::size_t index = 0; index < 127; ++index) {
        auto& small = assets.uif.fonts[1].glyphs[index];
        small = n3d::UifGlyph{10, 3, index * small_rows, small_rows};
        auto& large = assets.uif.fonts[2].glyphs[index];
        large = n3d::UifGlyph{12, 5, 127 * small_rows + index * large_rows, large_rows};
        if (index == 31) continue; // A blank space still advances four pixels in font 1.
        for (std::size_t row = 0; row < small_rows; ++row)
            assets.uif_bytes[small.bitmap_offset + row] = std::byte{0xa0};
        for (std::size_t row = 0; row < large_rows; ++row)
            assets.uif_bytes[large.bitmap_offset + row] = std::byte{0xf8};
    }
}

void seed_ui_backgrounds(n3d::Assets& assets) {
    for (std::size_t index = 0; index < assets.uif.images.size(); ++index) {
        auto& image = assets.uif.images[index].image;
        image.width = 320;
        image.height = 200;
        image.bytes_per_line = 320;
        image.pixels.assign(320 * 200, std::byte{static_cast<std::uint8_t>(0x43 + index)});
        // Synthetic baked heading and row dividers, independent of the layout helpers.
        for (int y = 20; y < 40; ++y)
            for (int x = 100; x < 240; ++x)
                image.pixels[static_cast<std::size_t>(y * 320 + x)] = std::byte{0x91};
        for (int y = 46; y <= 176; y += 13)
            for (int x = 66; x <= 247; ++x)
                image.pixels[static_cast<std::size_t>(y * 320 + x)] = std::byte{0x92};
    }
}

n3d::InputFrame key(std::uint8_t value) {
    n3d::InputFrame input{};
    input.key = value;
    return input;
}

std::unique_ptr<n3d::GameState> root_state(std::uint8_t row) {
    auto state = std::make_unique<n3d::GameState>();
    state->screen = n3d::Screen::root_menu;
    state->level_loaded = true;
    state->player_life_state = 1;
    state->menu_row = row;
    state->config.music_enabled = 0;
    state->composed_ready = true;
    state->composed_frame.clear(0x91u);
    state->composed_frame.palette[0] = {1u, 2u, 3u};
    state->ega_map[2] = 2u;
    state->ega_map[8] = 8u;
    state->ega_map[14] = 14u;
    return state;
}

constexpr std::size_t pixel(int x, int y) {
    return static_cast<std::size_t>(y) * n3d::IndexedFrame::kWidth + x;
}


void save_load_render_geometry_checks() {
    auto assets = std::make_unique<n3d::Assets>();
    seed_font_fixture(*assets);
    seed_ui_backgrounds(*assets);
    std::array<n3d::SaveMenuSlot, 10> slots{};
    std::array<std::uint8_t, 16> map{};
    map[2] = 2;
    map[8] = 8;
    map[14] = 14;

    for (const auto kind : {n3d::MenuKind::Save, n3d::MenuKind::Load}) {
        const auto& background = assets->uif.images[static_cast<std::uint8_t>(kind) - 3].image.pixels;
        n3d::IndexedFrame frame{};
        n3d::compose_menu(*assets, frame, {kind, 4, false, true}, map, slots);
        bool artwork_preserved = true;
        for (int y = 0; y < 200; ++y) {
            const bool in_row = y >= 47 && y <= 174 && (y - 47) % 13 <= 10;
            for (int x = 0; x < 320; ++x) {
                if (in_row && x >= 107 && x <= 170) continue;
                const auto index = pixel(x, y);
                artwork_preserved &= frame.pixels[index] == std::to_integer<std::uint8_t>(background[index]);
            }
        }
        check(artwork_preserved,
              "slot text preserves the baked title, separators and pixels outside all ten source row bands");
        for (int row = 0; row < 10; ++row) {
            const auto colour = kind == n3d::MenuKind::Load ? map[8] : row == 4 ? map[14] : map[2];
            unsigned foreground = 0, shadow = 0;
            for (int y = 47 + 13 * row; y <= 57 + 13 * row; ++y) {
                for (int x = 0; x < 320; ++x) {
                    foreground += frame.pixels[pixel(x, y)] == colour;
                    shadow += frame.pixels[pixel(x, y)] == 0;
                }
            }
            check(foreground == 260 && shadow == 260,
                  "each slot uses the small font's exact ink footprint and enabled/selected/disabled colour");
        }

        slots[0].present = true;
        slots[0].label = "A";
        slots[1].present = true;
        slots[1].label.clear();
        n3d::compose_menu(*assets, frame, {kind, 0, false, true}, map, slots);
        check(frame.pixels[pixel(67, 47)] == map[14] &&
                  frame.pixels[pixel(69, 47)] == map[14] &&
                  frame.pixels[pixel(68, 48)] == 0 &&
                  frame.pixels[pixel(107, 47)] == std::to_integer<std::uint8_t>(background[pixel(107, 47)]),
              "an occupied name starts at the raw table origin without empty-slot padding");
        bool blank_preserved = true;
        for (int y = 60; y <= 70; ++y)
            for (int x = 0; x < 320; ++x)
                blank_preserved &= frame.pixels[pixel(x, y)] ==
                    std::to_integer<std::uint8_t>(background[pixel(x, y)]);
        check(blank_preserved, "an occupied blank name paints no guessed empty-slot label");
        slots[0] = {};
        slots[1] = {};
    }
}

void save_menu_semantics_checks() {
    auto assets = std::make_unique<n3d::Assets>();
    seed_font_fixture(*assets);
    seed_ui_backgrounds(*assets);
    auto state = root_state(3);
    const auto original_pixels = state->composed_frame.pixels;
    n3d::step(*state, *assets, key(0x0d), 1000);
    check(state->screen == n3d::Screen::save_menu && state->composed_frame.pixels[0] == 0x4d,
          "active root Save opens UIF13");
    check(state->save_menu_restore_frame_valid &&
              state->save_menu_restore_frame.pixels == original_pixels && state->save_menu_restore_row == 3,
          "Save retains the caller framebuffer and cursor");
    n3d::step(*state, *assets, key(0x0d), 1001);
    check(state->screen == n3d::Screen::save_menu && state->save_menu_pending &&
              !state->save_menu_outcome.request,
          "first Enter selects a Save slot without requesting storage I/O");
    n3d::step(*state, *assets, key(0x0d), 1002);
    check(state->screen == n3d::Screen::root_menu && state->menu_row == 3 &&
              state->save_menu_outcome.request &&
              state->save_menu_outcome.request->operation == n3d::SaveMenuOperation::save &&
              state->save_menu_outcome.request->slot == 0 &&
              state->composed_frame.pixels == original_pixels && !state->save_menu_restore_frame_valid,
          "second Enter requests slot zero and restores the caller framebuffer");
    check(state->composed_frame.palette[0].r == 1 && state->composed_frame.palette[0].g == 2 &&
              state->composed_frame.palette[0].b == 3,
          "Save restores the caller palette as well as pixels");
    n3d::step(*state, *assets, {}, 1003);
    check(!state->save_menu_outcome.request, "a Save request is consumed exactly once");
}

void load_menu_semantics_checks() {
    auto assets = std::make_unique<n3d::Assets>();
    seed_font_fixture(*assets);
    seed_ui_backgrounds(*assets);
    auto state = root_state(2);
    const auto original_pixels = state->composed_frame.pixels;
    n3d::step(*state, *assets, key(0x0d), 2000);
    check(state->screen == n3d::Screen::load_menu && state->composed_frame.pixels[0] == 0x4c,
          "root Load opens UIF12");
    n3d::step(*state, *assets, key(0x0d), 2001);
    n3d::step(*state, *assets, key(0x28), 2002);
    check(state->screen == n3d::Screen::load_menu && state->menu_row == 0 &&
              !state->save_menu_pending && !state->save_menu_outcome.request,
          "Enter and Down cannot select absent Load slots");
    state->save_slots[1].present = true;
    state->save_slots[1].label = "A";
    n3d::refresh_save_menu(*state, *assets);
    check(state->save_menu_restore_frame.pixels == original_pixels && state->save_menu_restore_row == 2,
          "metadata refresh does not replace the caller framebuffer or cursor");
    n3d::step(*state, *assets, key(0x28), 2003);
    check(state->menu_row == 1 && !state->save_menu_pending,
          "Down skips absent slots and reaches the occupied slot");
    n3d::step(*state, *assets, key(0x0d), 2004);
    check(state->save_menu_pending && !state->save_menu_outcome.request,
          "first Enter selects the occupied Load slot without requesting I/O");
    n3d::step(*state, *assets, key(0x0d), 2005);
    check(state->screen == n3d::Screen::root_menu && state->menu_row == 2 &&
              state->save_menu_outcome.request &&
              state->save_menu_outcome.request->operation == n3d::SaveMenuOperation::load &&
              state->save_menu_outcome.request->slot == 1 && state->composed_frame.pixels == original_pixels,
          "occupied Load requests the correct slot and restores the caller framebuffer");
    n3d::step(*state, *assets, key(0x0d), 2006);
    n3d::step(*state, *assets, key(0x1b), 2007);
    check(state->screen == n3d::Screen::root_menu && state->menu_row == 2 &&
              state->save_menu_outcome.dismissed && !state->save_menu_outcome.request &&
              state->composed_frame.pixels == original_pixels,
          "Escape cancels Load without changing the caller framebuffer");
}

void quick_load_confirmation_checks() {
    auto assets = std::make_unique<n3d::Assets>();
    seed_font_fixture(*assets);
    seed_ui_backgrounds(*assets);
    auto state = root_state(5u);
    state->screen = n3d::Screen::load_menu;
    state->modal_return = n3d::Screen::gameplay;
    state->save_slots[0].present = true;
    state->save_slots[0].label = "A";
    state->save_menu_pending = true;
    state->save_menu_quick_confirm = true;
    state->save_menu_restore_frame = state->composed_frame;
    state->save_menu_restore_frame_valid = true;
    state->composed_frame.clear(0x42u);
    n3d::refresh_save_menu(*state, *assets);
    check(state->composed_frame.pixels[0] == 0x91u &&
              state->save_menu_restore_frame.pixels[0] == 0x91u,
          "quick-confirm displays over gameplay but preserves the caller frame");

    n3d::step(*state, *assets, key(0x1b), 3000);
    check(state->screen == n3d::Screen::gameplay &&
              state->save_menu_outcome.dismissed && !state->save_menu_outcome.request &&
              state->composed_frame.pixels[0] == 0x91u,
          "Escape dismisses the quick-load confirmation and returns to gameplay");

    state->screen = n3d::Screen::load_menu;
    state->save_menu_pending = true;
    state->save_menu_quick_confirm = true;
    state->save_menu_restore_frame = state->composed_frame;
    state->save_menu_restore_frame_valid = true;
    state->composed_frame.clear(0x42u);
    n3d::step(*state, *assets, key(0x0d), 3001);
    check(state->screen == n3d::Screen::gameplay &&
              state->save_menu_outcome.request &&
              state->save_menu_outcome.request->operation == n3d::SaveMenuOperation::load &&
              state->save_menu_outcome.request->slot == 0u &&
              state->composed_frame.pixels[0] == 0x91u,
          "Enter confirms the quick-load request without re-composing a new screen");
}

void last_row_hover_boundary_checks() {
    auto assets = std::make_unique<n3d::Assets>();
    seed_font_fixture(*assets);
    seed_ui_backgrounds(*assets);
    struct Probe { int x, y; bool hits; };
    constexpr std::array probes{
        Probe{67, 164, true}, Probe{71, 171, true}, Probe{66, 164, false},
        Probe{72, 171, false}, Probe{67, 163, false}, Probe{71, 172, false},
        Probe{67, 174, false}, Probe{107, 164, false}};
    for (const bool load : {false, true}) {
        auto state = root_state(load ? 2 : 3);
        state->save_slots[9].slot = 9;
        state->save_slots[9].present = true;
        state->save_slots[9].label = "A";
        n3d::step(*state, *assets, key(0x0d), 4000);
        std::uint64_t now = 4001;
        for (const auto probe : probes) {
            state->menu_row = 0;
            state->save_menu_pending = false;
            n3d::refresh_save_menu(*state, *assets);
            n3d::InputFrame input{};
            input.cursor_moved = true;
            input.cursor_x = static_cast<std::int16_t>(probe.x);
            input.cursor_y = static_cast<std::int16_t>(probe.y);
            n3d::step(*state, *assets, input, now++);
            check(state->menu_row == (probe.hits ? 9 : 0),
                  "Save/Load last-row hover respects the inclusive source rectangle and rejects each outer edge");
        }
        state->menu_row = 0;
        state->save_menu_pending = false;
        n3d::InputFrame click{};
        click.cursor_moved = true;
        click.cursor_x = 71;
        click.cursor_y = 171;
        click.menu_left_released = true;
        n3d::step(*state, *assets, click, now++);
        check(state->menu_row == 9 && state->save_menu_pending && !state->save_menu_outcome.request,
              "clicking the last-row hit edge selects that slot without premature storage I/O");
        if (load) {
            state->menu_row = 9;
            click.cursor_x = 107;
            click.cursor_y = 47;
            n3d::step(*state, *assets, click, now);
            check(state->menu_row == 9 && !state->save_menu_outcome.request,
                  "clicking an absent Load row cannot submit the already selected occupied slot");
        }
    }
}

} // namespace

int run_save_menu_tests() {
    save_load_render_geometry_checks();
    save_menu_semantics_checks();
    load_menu_semantics_checks();
    quick_load_confirmation_checks();
    last_row_hover_boundary_checks();
    if (failures != 0) {
        std::cerr << failures << " save-menu checks failed\n";
        return failures;
    }
    std::cout << "n3d save-menu checks passed\n";
    return 0;
}
