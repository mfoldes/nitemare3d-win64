#include "n3d/game.hpp"
#include "../src/game/internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;

void check(bool ok, std::string_view description) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

n3d::InputFrame key(std::uint8_t value) {
    n3d::InputFrame input{};
    input.key = value;
    return input;
}

void handle(n3d::GameState& state, const n3d::Assets& assets, std::uint8_t value) {
    n3d::detail::handle_gameplay_input(state, assets, key(value));
}

n3d::Assets minimal_hud_assets() {
    n3d::Assets assets{};
    assets.uif_bytes.assign(1, std::byte{0});
    for (auto& font : assets.uif.fonts)
        for (auto& glyph : font.glyphs)
            glyph = n3d::UifGlyph{1, 1, 0, 1};

    auto& episode = assets.episodes[0];
    auto& sequence = episode.images.sequences[511];
    sequence.offset = 1;
    sequence.frame_count = 24;
    sequence.first_record_index = 0;
    episode.image_bytes.assign(24, std::byte{1});
    episode.images.records.reserve(24);
    for (std::size_t i = 0; i < 24; ++i)
        episode.images.records.push_back({i, 1, 1, {}, i, 1});
    return assets;
}

void draw_hud(n3d::GameState& state, const n3d::Assets& assets,
              n3d::IndexedFrame& frame) {
    frame.pixels.fill(11); // UIF5 empty-panel base index.
    n3d::detail::draw_hud(state, assets, frame);
}

std::uint8_t pixel(const n3d::IndexedFrame& frame, int x, int y) {
    return frame.pixels[static_cast<std::size_t>(y) * n3d::IndexedFrame::kWidth +
                        static_cast<std::size_t>(x)];
}

void sensor_input_toggles_independent_gauges_and_clear_panel() {
    n3d::GameState state{};
    n3d::Assets assets{};
    state.screen = n3d::Screen::gameplay;
    state.player_life_state = 1;
    state.player.map_meter = 37;
    state.player.enemy_meter = 82;
    state.automap[9 * 64 + 10] = 0x5a;

    handle(state, assets, 0x78); // F9: map sensor only.
    check(state.player.map_sensor == 1 && state.player.enemy_sensor == 0 &&
              state.player.map_meter == 37 && state.player.enemy_meter == 82,
          "F9 toggles only the map sensor and preserves both independent meters");
    check(!state.sensor_panel_cleared && state.automap[9 * 64 + 10] == 0x5a,
          "turning a sensor on neither clears the panel nor erases the discovered grid");

    handle(state, assets, 0x79); // F10: enemy sensor only.
    check(state.player.map_sensor == 1 && state.player.enemy_sensor == 1 &&
              state.player.map_meter == 37 && state.player.enemy_meter == 82,
          "F10 toggles only the enemy sensor without changing either battery");

    state.sensor_panel_cleared = false;
    handle(state, assets, 0x78);
    check(state.player.map_sensor == 0 && state.player.enemy_sensor == 1 &&
              state.sensor_panel_cleared,
          "turning F9 off requests the original cleared-panel draw while F10 stays active");
    check(state.automap[9 * 64 + 10] == 0x5a,
          "turning F9 off preserves the persistent automap grid");

    state.sensor_panel_cleared = false;
    handle(state, assets, 0x79);
    check(state.player.map_sensor == 0 && state.player.enemy_sensor == 0 &&
              state.sensor_panel_cleared,
          "turning F10 off also requests the original cleared-panel draw");
    check(state.audio.count == 0, "sensor toggles do not invent an audio cue");
}

void empty_sensor_batteries_gate_activation() {
    n3d::GameState state{};
    n3d::Assets assets{};
    state.screen = n3d::Screen::gameplay;
    state.player_life_state = 1;
    state.player.map_meter = 0;
    state.player.enemy_meter = 0;

    handle(state, assets, 0x78);
    handle(state, assets, 0x79);
    check(state.player.map_sensor == 0 && state.player.enemy_sensor == 0 &&
              !state.sensor_panel_cleared,
          "F9 and F10 cannot enable sensors whose own meter is exhausted");

    state.player.enemy_meter = 1;
    handle(state, assets, 0x78);
    handle(state, assets, 0x79);
    check(state.player.map_sensor == 0 && state.player.enemy_sensor == 1,
          "F9 remains gated by map battery while F10 independently uses enemy battery");
    state.player.map_meter = 1;
    state.player.enemy_meter = 0;
    handle(state, assets, 0x78);
    handle(state, assets, 0x79);
    check(state.player.map_sensor == 1 && state.player.enemy_sensor == 1,
          "F10 with zero enemy battery is a no-op and does not block F9");
}

void demo_sensor_records_use_gameplay_input_dispatch() {
    n3d::GameState state{};
    n3d::Assets assets{};
    state.screen = n3d::Screen::gameplay;
    state.player_life_state = 1;
    state.player.map_meter = 100;
    state.player.enemy_meter = 100;

    n3d::DemoRecording recording{};
    recording.header = {10, 5, 20, 2};
    recording.events = {{0x78, 0, 0, 363}, {0x79, 0, 0, 371}};
    n3d::DemoPlayback playback(recording);

    std::uint32_t tick = 362;
    const auto early = playback.advance(tick, false);
    n3d::detail::handle_gameplay_input(state, assets, {early.key, early.held});
    check(!early.event_applied && state.player.map_sensor == 0 && state.player.enemy_sensor == 0,
          "recorded detector actions remain unapplied before their logical ticks");

    tick = 363;
    const auto map_event = playback.advance(tick, false);
    n3d::detail::handle_gameplay_input(state, assets, {map_event.key, map_event.held});
    check(map_event.event_applied && map_event.key == 0x78 &&
              state.player.map_sensor == 1 && state.player.enemy_sensor == 0,
          "DEMO.1-style tick-363 event reaches the shared F9 gameplay handler");

    tick = 370;
    const auto enemy_early = playback.advance(tick, false);
    n3d::detail::handle_gameplay_input(state, assets, {enemy_early.key, enemy_early.held});
    check(!enemy_early.event_applied && state.player.map_sensor == 1 &&
              state.player.enemy_sensor == 0,
          "recorded F10 remains off before logical tick 371");

    tick = 371;
    const auto enemy_event = playback.advance(tick, false);
    n3d::detail::handle_gameplay_input(state, assets, {enemy_event.key, enemy_event.held});
    check(enemy_event.event_applied && enemy_event.key == 0x79 &&
              state.player.map_sensor == 1 && state.player.enemy_sensor == 1,
          "DEMO.1-style tick-371 event reaches the shared F10 gameplay handler");
}

void hud_draws_sensor_pixels_and_clears_panel_when_sensors_turn_off() {
    auto assets = minimal_hud_assets();
    n3d::GameState state{};
    state.episode = 1;
    state.screen = n3d::Screen::gameplay;
    state.player_life_state = 1;
    state.player.cell_x = 31;
    state.player.cell_y = 18;
    state.player.map_meter = 100;
    state.player.enemy_meter = 100;
    state.player.weapon = 0xff;
    state.draw_weapon = 0xff;
    for (std::size_t i = 0; i < state.ega_map.size(); ++i)
        state.ega_map[i] = static_cast<std::uint8_t>(30 + i);

    n3d::IndexedFrame frame{};
    draw_hud(state, assets, frame);
    check(pixel(frame, 256, 162) == 11 && pixel(frame, 317, 197) == 11,
          "inactive untouched sensor panel preserves the UIF5 base pixels");

    state.automap[5 * 64 + 7] = state.ega_map[2];
    state.automap[10 * 64 + 11] = state.ega_map[2];
    state.player.map_sensor = 1;
    state.player.enemy_sensor = 1;
    state.level.object_count = 1;
    state.level.guard_count = 1;
    state.level.objects[0].class_id = 8;
    state.level.objects[0].world_x = 10 * 64;
    state.level.objects[0].world_y = 11 * 64;
    state.level.guards[0].object_slot = 0;
    draw_hud(state, assets, frame);
    check(pixel(frame, 261, 169) == state.ega_map[2],
          "active automap panel samples the column-major discovered grid");
    check(pixel(frame, 266, 173) == state.ega_map[12],
          "active enemy detector overlays a live guard dot on the map");
    check(pixel(frame, 211, 181) == state.ega_map[10] &&
              pixel(frame, 233, 181) == state.ega_map[12],
          "active map and enemy batteries use their enabled gauge colours");
    check(pixel(frame, 262, 169) == 0,
          "active map raster clears unrevealed panel cells to palette index zero");

    n3d::Assets no_assets{};
    state.level.guard_count = 0;
    state.sensor_panel_cleared = false;
    handle(state, no_assets, 0x78);
    check(state.player.map_sensor == 0 && state.player.enemy_sensor == 1 &&
              state.sensor_panel_cleared,
          "F9-off keeps the other detector but marks the cleared-panel transition");
    state.sensor_panel_cleared = false;
    handle(state, no_assets, 0x79);
    check(state.player.map_sensor == 0 && state.player.enemy_sensor == 0 &&
              state.sensor_panel_cleared,
          "F10-off independently marks the final panel clear");
    draw_hud(state, assets, frame);
    check(pixel(frame, 256, 162) == 0 && pixel(frame, 317, 197) == 0,
          "after both sensors are off the cleared panel is black, not stale map art");
    check(state.automap[5 * 64 + 7] == state.ega_map[2] &&
              state.automap[10 * 64 + 11] == state.ega_map[2],
          "clearing the panel does not erase discovered wall bytes");
    check(pixel(frame, 211, 181) == static_cast<std::uint8_t>(state.ega_map[2] - 5) &&
              pixel(frame, 233, 181) == state.ega_map[4],
          "inactive sensor gauges return to their source off-colours");
}

void visible_scene_spans_reveal_only_source_wall_cells() {
    n3d::GameState state{};
    state.ega_map[2] = 244;
    state.ega_map[9] = 201;
    state.level.vector_count = 4;
    state.scene.span_count = 4;

    auto& horizontal = state.level.vectors[0];
    horizontal.x0 = 2 * 64;
    horizontal.y0 = 3 * 64;
    horizontal.x1 = 5 * 64;
    horizontal.y1 = 3 * 64;

    auto& vertical = state.level.vectors[1];
    vertical.direction = 2;
    vertical.x0 = vertical.x1 = 8 * 64;
    vertical.y0 = 6 * 64;
    vertical.y1 = 9 * 64;

    auto& adjusted_horizontal = state.level.vectors[2];
    adjusted_horizontal.direction = 1;
    adjusted_horizontal.x0 = 12 * 64;
    adjusted_horizontal.x1 = 15 * 64;
    adjusted_horizontal.y0 = adjusted_horizontal.y1 = 13 * 64;

    auto& door = state.level.vectors[3];
    door.flags = 8;
    door.x0 = 10 * 64;
    door.y0 = 11 * 64;
    door.x1 = 11 * 64;
    door.y1 = 11 * 64;

    for (std::size_t i = 0; i < state.scene.span_count; ++i)
        state.scene.spans[i].vector_index = static_cast<std::uint16_t>(i);
    n3d::detail::reveal_visible_automap(state);

    std::array<std::uint8_t, n3d::kMapCellCount> expected{};
    expected[2 * 64 + 3] = expected[3 * 64 + 3] = expected[4 * 64 + 3] = 244;
    expected[7 * 64 + 6] = expected[7 * 64 + 7] = expected[7 * 64 + 8] = 244;
    expected[12 * 64 + 12] = expected[13 * 64 + 12] = expected[14 * 64 + 12] = 244;
    expected[10 * 64 + 11] = 201;
    check(state.automap == expected,
          "visible scene runs reveal only exact wall and door cells in column-major order");

    n3d::detail::reveal_visible_automap(state);
    check(state.automap == expected,
          "reprocessing a visible run preserves the same persistent discovery grid");
}
} // namespace

int run_sensor_tests() {
    sensor_input_toggles_independent_gauges_and_clear_panel();
    empty_sensor_batteries_gate_activation();
    demo_sensor_records_use_gameplay_input_dispatch();
    hud_draws_sensor_pixels_and_clears_panel_when_sensors_turn_off();
    visible_scene_spans_reveal_only_source_wall_cells();
    return failures;
}
