#include "n3d/game.hpp"
#include "../src/game/internal.hpp"

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
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::size_t cell(unsigned x, unsigned y) { return y * 64u + x; }

void warp_landing_follows_live_object_flags_and_column_major_id() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->player.cell_x = 10;
    state->player.cell_y = 10;
    state->level.wall_class_by_id[10] = 0x0d;
    state->level.wall_class_by_id[11] = 0x0d;
    state->level.object_class_by_id[2] = 0x28;
    state->level.object_flags[2] = 2;
    state->level.cells[cell(10, 9)].wall_id = 10;
    state->level.cells[cell(20, 20)].wall_id = 11;
    state->level.cells[cell(30, 30)].wall_id = 11;
    constexpr std::array<int, 4> x{20, 21, 20, 19};
    constexpr std::array<int, 4> y{19, 20, 21, 20};
    constexpr std::array<int, 4> heading{0, 90, 180, 270};
    for (unsigned landing = 0; landing < 4; ++landing) {
        state->screen = n3d::Screen::warp_popup;
        state->player.cell_x = 10;
        state->player.cell_y = 10;
        state->player.world_x = 10 * 64 + 32;
        state->player.world_y = 10 * 64 + 32;
        n3d::detail::set_heading(*state, 0);
        state->warp_row = 0;
        for (unsigned direction = 0; direction < 4; ++direction)
            state->level.cells[cell(x[direction], y[direction])].object_id =
                static_cast<std::uint8_t>(direction < landing ? 2u : 0u);
        n3d::InputFrame enter{};
        enter.key = 0x0d;
        n3d::detail::handle_gameplay_input(*state, assets, enter);
        check(state->player.cell_x == x[landing] && state->player.cell_y == y[landing] &&
                  state->player.heading == heading[landing],
              "paired warp checks live objects N/E/S/W and first column-major duplicate");
    }
}

void locked_warp_rejects_player_cell() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::gameplay;
    state->player_life_state = 1;
    state->player.cell_x = 10;
    state->player.cell_y = 10;
    state->player.world_x = 10 * 64 + 32;
    state->player.world_y = 10 * 64 + 32;
    state->player.keys = 1;
    state->level.wall_class_by_id[30] = 0x19;
    state->level.cells[cell(10, 9)].wall_id = 30;
    state->level.object_flags[2] = 2;
    state->level.cells[cell(10, 8)].object_id = 2;
    state->level.cells[cell(11, 9)].object_id = 2;
    n3d::InputFrame use{};
    use.held = 0x200;
    n3d::detail::handle_gameplay_input(*state, assets, use);
    check(state->player.cell_x == 9 && state->player.cell_y == 9 &&
              state->player.heading == 270 && state->screen == n3d::Screen::modal_message,
          "locked warp rejects occupied N/E then own S before choosing W");
}

void safe_requires_its_underlying_wall_code_before_reward() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::gameplay;
    state->player_life_state = 1;
    state->player.cell_x = 10;
    state->player.cell_y = 10;
    state->player.world_x = 10 * 64 + 32;
    state->player.world_y = 10 * 64 + 32;
    state->level.wall_class_by_id[7] = 0x45;
    state->level.object_class_by_id[3] = 0x26;
    state->level.cells[cell(10, 9)] = {7, 3};
    state->level.object_count = 1;
    auto& safe = state->level.objects[0];
    safe.image_id = 3;
    safe.class_id = 0x26;
    safe.flags = 1;
    safe.subtype = 4; // Red card; safe frame 6 maps to reward 10.
    safe.cell_x = 10;
    safe.cell_y = 9;
    n3d::InputFrame use{};
    use.held = 0x200;
    n3d::detail::handle_gameplay_input(*state, assets, use);
    check(state->screen == n3d::Screen::combination_prompt, "unopened safe requests combination");
    for (char digit : std::string_view{"123"}) {
        n3d::InputFrame input{};
        input.key = static_cast<std::uint8_t>(digit);
        n3d::detail::handle_gameplay_input(*state, assets, input);
    }
    n3d::InputFrame enter{};
    enter.key = 0x0d;
    n3d::detail::handle_gameplay_input(*state, assets, enter);
    check(safe.frame_index == 0 && state->screen == n3d::Screen::modal_message,
          "wrong safe code preserves unopened frame");
    n3d::detail::handle_gameplay_input(*state, assets, enter);
    n3d::detail::handle_gameplay_input(*state, assets, {}); // Release use edge.
    n3d::detail::handle_gameplay_input(*state, assets, use);
    for (char digit : std::string_view{"333"}) {
        n3d::InputFrame input{};
        input.key = static_cast<std::uint8_t>(digit);
        n3d::detail::handle_gameplay_input(*state, assets, input);
    }
    n3d::detail::handle_gameplay_input(*state, assets, enter);
    check(safe.frame_index == 6 && state->player.cards == 0,
          "correct code reveals card without collecting it");
    n3d::detail::handle_gameplay_input(*state, assets, {});
    n3d::detail::handle_gameplay_input(*state, assets, use);
    check(safe.frame_index == 1 && state->player.cards == 1,
          "second safe use awards red card and empties safe");
}

void control_popup_changes_matching_door_and_global_cannon_mask() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->config.effects_enabled = 0;
    state->screen = n3d::Screen::control_popup;
    state->popup_first = 0;
    state->popup_row = 0;
    state->door_count = 1;
    state->doors[0].cell_index = static_cast<std::uint16_t>(cell(20, 20));
    state->doors[0].vector_index = 0;
    state->doors[0].partner_vector_index = 1;
    state->doors[0].state = 1;
    state->level.vectors[0].class_id = 0x3b;
    state->level.vectors[0].subtype = 0;
    state->level.vectors[0].direction = 0;
    n3d::InputFrame enter{};
    enter.key = 0x0d;
    n3d::detail::handle_gameplay_input(*state, assets, enter);
    check(state->doors[0].state == 2 && state->remote_open_mask == 1,
          "remote open toggles matching door record and colour mask");
    state->screen = n3d::Screen::control_popup;
    state->popup_row = 3;
    n3d::detail::handle_gameplay_input(*state, assets, enter);
    check(state->cannons_enabled == 0 && state->remote_open_mask == 1,
          "cannon disable is global and leaves remote-door bit unchanged");
}


void panel_face_bounds_match_original_and_open_shared_panels_in_source_time() {
    auto state = std::make_unique<n3d::GameState>();
    n3d::Assets assets{};
    auto& episode = assets.episodes[0];
    episode.map.level_count = 1;
    episode.map.level_size = 8192;
    episode.map.levels_offset = 0;
    episode.map.wall_class_by_id[1] = 1;
    episode.map.wall_class_by_id[2] = 1;
    episode.map.wall_class_by_id[79] = 1;
    episode.map.wall_class_by_id[187] = 0x44;
    episode.map.object_class_by_id[1] = 2;
    episode.map.object_class_by_id[98] = 3;
    episode.map_bytes.assign(8192, std::byte{0});
    const auto set_cell = [&episode](unsigned x, unsigned y, std::uint8_t wall,
                                     std::uint8_t object) {
        const auto offset = 2 * cell(x, y);
        episode.map_bytes[offset] = std::byte{wall};
        episode.map_bytes[offset + 1] = std::byte{object};
    };
    set_cell(5, 5, 0, 1); // Unique source start position.
    for (unsigned x = 21; x <= 23; ++x) set_cell(x, 41, 1, 0);
    set_cell(21, 42, 187, 0);
    set_cell(22, 42, 187, 0);
    set_cell(23, 42, 79, 98); // Wall class 1 with a class-3 panel.
    set_cell(24, 42, 187, 0);
    set_cell(23, 43, 2, 0); // Solid neighbor keeps a lower dir-1 face absent.

    state->config.effects_enabled = 0;
    state->config.mouse_enabled = 0;
    state->config.joystick_enabled = 0;
    n3d::detail::start_level(*state, assets, 1, 0);
    check(state->panel_count == 1, "synthetic MAP creates one class-3 panel record");
    if (state->panel_count != 1) return;

    constexpr std::size_t panel_cell = cell(23, 42);
    constexpr std::size_t shared_cell = cell(23, 43);
    auto& panel = state->panels[0];
    check(panel.face_vectors[1] < state->level.vector_count &&
              panel.face_vectors[2] == n3d::kMaxVectors &&
              panel.face_vectors[3] < state->level.vector_count,
          "panel assignment keeps the real dir-2/dir-3 faces and excludes the unrelated dir-1 row");
    if (panel.face_vectors[1] >= state->level.vector_count ||
        panel.face_vectors[3] >= state->level.vector_count) return;

    const auto first_face = panel.face_vectors[1];
    const auto second_face = panel.face_vectors[3];
    const auto& first_vector = state->level.vectors[first_face];
    const auto& second_vector = state->level.vectors[second_face];
    check(first_vector.direction == 2 && first_vector.y1 - first_vector.y0 == 64 &&
              second_vector.direction == 3 && second_vector.y1 - second_vector.y0 == 64,
          "the paired panel faces have the original 64-unit travel, not the 192-unit dir-1 run");

    bool found_unrelated_edge = false;
    for (std::size_t i = 0; i < state->level.vector_count; ++i) {
        const auto& vector = state->level.vectors[i];
        if (vector.direction != 1 || vector.class_id != 1 ||
            vector.x0 != 21 * 64 || vector.x1 != 24 * 64 ||
            vector.y0 != 42 * 64 || vector.y1 != 42 * 64) continue;
        found_unrelated_edge = true;
        check(!n3d::detail::vector_contains(vector, panel_cell),
              "non-door direction-1 upper bound excludes the adjacent panel row");
        auto door_boundary = vector;
        door_boundary.flags |= 0x08;
        check(n3d::detail::vector_contains(door_boundary, panel_cell) &&
                  !n3d::detail::vector_contains(door_boundary, cell(23, 41)),
              "door-flagged direction-1 bounds are not shifted");
        break;
    }
    check(found_unrelated_edge, "synthetic MAP contains the unrelated merged direction-1 face");

    // A second MAP panel record shares the pair; completing one source panel
    // removes both cells in the same completion pass, as 1010:1e00 does.
    state->panel_count = 2;
    auto& shared_panel = state->panels[1];
    shared_panel.cell_index = static_cast<std::uint16_t>(shared_cell);
    shared_panel.state = 1;
    shared_panel.face_vectors = panel.face_vectors;
    state->level.cells[shared_cell] = {79, 98};
    state->player.cell_x = 24;
    state->player.cell_y = 42;
    state->player.world_x = static_cast<std::int16_t>(24 * 64 + 32);
    state->player.world_y = static_cast<std::int16_t>(42 * 64 + 32);
    n3d::detail::set_heading(*state, 270);
    n3d::InputFrame use{};
    use.held = 0x200;
    n3d::detail::handle_gameplay_input(*state, assets, use);
    check(panel.state == 2 && shared_panel.state == 1,
          "Use starts the targeted panel without prematurely clearing its shared neighbor");

    for (unsigned pass = 0; pass < 32; ++pass)
        n3d::detail::advance_doors_and_panels(*state);
    check(panel.state == 2 && shared_panel.state == 1 &&
              state->level.vectors[first_face].y0 == state->level.vectors[first_face].y1 &&
              state->level.vectors[second_face].y0 == state->level.vectors[second_face].y1 &&
              state->level.cells[panel_cell].object_id == 98 &&
              state->level.cells[shared_cell].object_id == 98,
          "32 two-unit updates reach the paired face ends but retain the completion state");
    n3d::detail::advance_doors_and_panels(*state);
    check(panel.state == 0 && shared_panel.state == 0 &&
              state->level.cells[panel_cell].wall_id == 0 &&
              state->level.cells[panel_cell].object_id == 0 &&
              state->level.cells[shared_cell].wall_id == 0 &&
              state->level.cells[shared_cell].object_id == 0,
          "the 33rd source-length pass clears every panel cell sharing the paired faces");
}
} // namespace

int run_puzzle_tests() {
    warp_landing_follows_live_object_flags_and_column_major_id();
    locked_warp_rejects_player_cell();
    safe_requires_its_underlying_wall_code_before_reward();
    control_popup_changes_matching_door_and_global_cannon_mask();
    panel_face_bounds_match_original_and_open_shared_panels_in_source_time();
    if (failures) std::cerr << failures << " puzzle checks failed\n";
    return failures;
}
