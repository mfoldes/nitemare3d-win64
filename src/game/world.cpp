#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace n3d::detail {
namespace {

// 1048:03ca, the original signed, 1024-scaled tangent table. The last two
// entries are saturated, not host floating-point tangents of 89 and 90 degrees.
constexpr std::array<std::int16_t, 91> kTangent{
    0,17,35,53,71,89,107,125,143,162,180,199,217,236,255,274,293,313,332,352,
    372,393,413,434,455,477,499,521,544,567,591,615,639,664,690,717,743,771,
    800,829,859,890,922,954,988,1024,1060,1098,1137,1177,1220,1264,1310,1358,
    1409,1462,1518,1576,1638,1704,1773,1847,1925,2009,2099,2195,2299,2412,
    2534,2667,2813,2973,3151,3349,3571,3821,4107,4435,4817,5268,5807,6465,
    7286,8339,9742,11704,14643,19539,29323,32767,32767
};
constexpr std::array<std::int8_t, 8> kPushX{0,8,8,0,0,-8,-8,0};
constexpr std::array<std::int8_t, 8> kPushY{-8,0,0,8,8,0,0,-8};
constexpr std::array<std::string_view, 4> kPentagramCaption{
    "Pentagram of Good Health", "Pentagram of Power",
    "Pentagram of Good Sight", "Pentagram of Knowledge"
};

[[noreturn]] void invalid(const char* message) {
    throw std::runtime_error(message);
}

std::size_t cell_at(int world_x, int world_y) {
    if (world_x < 0 || world_x >= 64 * static_cast<int>(kMapWidth) ||
        world_y < 0 || world_y >= 64 * static_cast<int>(kMapHeight))
        invalid("pushwall moved outside MAP grid");
    return static_cast<std::size_t>((world_y >> 6) * 64 + (world_x >> 6));
}

void effect(GameState& state, std::uint8_t id) {
    if (state.config.effects_enabled)
        state.audio.push({AudioCommandKind::effect, id, 0, 2});
}

// 1010:c9a6 uses the SIGN of the movement delta to choose one scan direction.
// Strict comparisons preserve ties in the source object's prior list order.
void reorder_axis(LevelState& level, std::uint16_t slot, bool x_axis, int delta) noexcept {
    auto& order = x_axis ? level.object_list_x : level.object_list_y;
    auto& object = level.objects[slot];
    std::size_t rank = x_axis ? object.x_order_index : object.y_order_index;
    const int coordinate = x_axis ? object.world_x : object.world_y;
    const auto key = [x_axis, &level](std::uint16_t index) noexcept {
        return x_axis ? level.objects[index].world_x : level.objects[index].world_y;
    };
    if (delta < 0) {
        while (rank && coordinate < key(order[rank - 1])) {
            const auto shifted = order[rank - 1];
            order[rank] = shifted;
            (x_axis ? level.objects[shifted].x_order_index : level.objects[shifted].y_order_index) =
                static_cast<std::uint16_t>(rank--);
        }
    } else if (delta > 0) {
        while (rank + 1 < level.object_count && coordinate > key(order[rank + 1])) {
            const auto shifted = order[rank + 1];
            order[rank] = shifted;
            (x_axis ? level.objects[shifted].x_order_index : level.objects[shifted].y_order_index) =
                static_cast<std::uint16_t>(rank++);
        }
    }
    order[rank] = slot;
    (x_axis ? object.x_order_index : object.y_order_index) = static_cast<std::uint16_t>(rank);
}

void drain_sensors(GameState& state, std::uint32_t a_count) {
    if (state.config.cheat_omniscient) return; // 4be4: neither meter nor the HUD is touched.
    auto& player = state.player;
    const auto old_enemy = player.enemy_meter;
    const auto old_map = player.map_meter;
    if (player.map_sensor && !(a_count & 15u)) --player.map_meter;
    if (player.enemy_sensor && !(a_count & 7u)) --player.enemy_meter;
    if (player.map_sensor && !player.map_meter) {
        player.map_sensor = 0;
        state.sensor_panel_cleared = true; // b1a4(3) clears the 62x36 panel.
    }
    if (player.enemy_sensor && !player.enemy_meter) {
        player.enemy_sensor = 0;
        state.sensor_panel_cleared = true;
    }
    // a3b6(0x13/0x14) clamps each changed byte as an unsigned gauge value.
    if (player.enemy_meter != old_enemy)
        player.enemy_meter = std::min<std::uint8_t>(player.enemy_meter, 100);
    if (player.map_meter != old_map)
        player.map_meter = std::min<std::uint8_t>(player.map_meter, 100);
}

void update_panel_indicator(GameState& state) {
    if (!state.player.map_meter) return; // bd2a retains 46ad when the meter is zero.
    const int player_cell = static_cast<int>(state.player.cell_y) * 64 + state.player.cell_x;
    bool nearby = false;
    for (std::size_t i = 0; i < state.panel_count; ++i) {
        const auto& panel = state.panels[i];
        if (panel.state != 1) continue;
        const int cell = panel.cell_index;
        if (player_cell == cell + 1 || player_cell == cell - 1 ||
            player_cell == cell + 64 || player_cell == cell - 64) {
            nearby = true;
            break;
        }
    }
    state.panel_nearby = nearby;
}

void update_forward_caption(GameState& state) {
    // 1010:bddd–bdf4 seeds 46ba:46bc with its authored demo caption when
    // 46b8!=0, otherwise null. A forward class-0x27 pentagram can override it.
    state.hud_message = state.demo ? "Nitemare-3D -- Demo Mode" : std::string_view{};
    const auto& player = state.player;
    if (player.octant >= 8) invalid("forward caption octant outside direction table");
    const int x = static_cast<int>(player.cell_x) + (kPushX[player.octant] > 0) -
                  (kPushX[player.octant] < 0);
    const int y = static_cast<int>(player.cell_y) + (kPushY[player.octant] > 0) -
                  (kPushY[player.octant] < 0);
    if (x < 0 || x >= 64 || y < 0 || y >= 64) return;
    const std::size_t cell_index = static_cast<std::size_t>(y * 64 + x);
    const auto image_id = state.level.cells[cell_index].object_id;
    if (state.level.object_class_by_id[image_id] != 0x27) return;
    // cef0 requires both the current cell pointer and its current image ID.
    for (std::size_t i = 0; i < state.level.object_count; ++i) {
        const auto& object = state.level.objects[i];
        if (object.cell_x != x || object.cell_y != y || object.image_id != image_id)
            continue;
        if (object.frame_index >= 2 && object.frame_index <= 5)
            state.hud_message = kPentagramCaption[object.frame_index - 2];
        return;
    }
}

void step_pushwalls(GameState& state) {
    auto& level = state.level;
    for (std::size_t i = 0; i < state.pushwall_count; ++i) {
        auto& wall = state.pushwalls[i];
        if (!wall.steps_remaining) continue;
        if (wall.object_slot >= level.object_count) invalid("pushwall object slot outside MAP records");
        auto& object = level.objects[wall.object_slot];
        const int x = static_cast<int>(object.world_x) + wall.step_x;
        const int y = static_cast<int>(object.world_y) + wall.step_y;
        const std::size_t next = cell_at(x, y);
        const std::size_t previous = static_cast<std::size_t>(object.cell_y) * 64 + object.cell_x;
        if (next != previous) {
            if (level.object_flags[level.cells[next].object_id] & 6u) continue;
            level.cells[next].object_id = level.cells[previous].object_id;
            level.cells[previous].object_id = 0;
        }
        object.world_x = static_cast<std::int16_t>(x);
        object.world_y = static_cast<std::int16_t>(y);
        object.cell_x = static_cast<std::uint16_t>(next % 64);
        object.cell_y = static_cast<std::uint16_t>(next / 64);
        if (wall.step_x) reorder_axis(level, wall.object_slot, true, wall.step_x);
        if (wall.step_y) reorder_axis(level, wall.object_slot, false, wall.step_y);
        --wall.steps_remaining;
    }
}

void damage_on_floor(GameState& state) {
    if (state.config.cheat_omnipotent) return; // 4be5: god mode suppresses the entire hazard.
    auto& player = state.player;
    const std::size_t cell = static_cast<std::size_t>(player.cell_y) * 64 + player.cell_x;
    const auto id = state.level.cells[cell].object_id;
    if (state.level.object_class_by_id[id] != 7) return;
    std::size_t first = 0;
    while (first < state.level.object_class_by_id.size() &&
           state.level.object_class_by_id[first] != 7) ++first;
    constexpr std::array<std::uint8_t, 3> damage{100, 10, 2};
    if (first == state.level.object_class_by_id.size() ||
        static_cast<std::size_t>(id) - first >= damage.size())
        invalid("MAP floor hazard has no source damage-table entry");
    // 3b7c(7) additionally presents a blocking one-frame red palette flash;
    // it does not leave a persistent colour in the working palette.
    const auto amount = damage[id - first];
    if (amount >= player.health) {
        player.health = 0;
        state.player_life_state = 3; // be62 bypasses the guard-killer camera turn.
        state.death_turn_pending = true;
        effect(state, 0x0a);
    } else player.health = static_cast<std::uint8_t>(player.health - amount);
}

bool lightning_level(const GameState& state) noexcept {
    return state.episode == 1 || (state.episode == 3 && state.level_index >= 9);
}

void tick_lightning(GameState& state) {
    if (!lightning_level(state)) return;
    const auto old = state.lightning_countdown;
    state.lightning_countdown = static_cast<std::int16_t>(static_cast<std::uint16_t>(old) - 1u);
    if (old) return;
    if (!state.lightning_on) {
        state.lightning_on = true;
        state.lightning_countdown = 3;
        state.palette[0xda] = {255, 255, 255};
    } else {
        state.lightning_on = false;
        state.lightning_countdown = static_cast<std::int16_t>(random15(state) % 240u + 160u);
        state.palette[0xda] = {0, 0, 0};
        effect(state, 0x42);
    }
}

int tangent(unsigned heading) {
    if (heading < 90) return kTangent[heading];
    if (heading < 180) return kTangent[180 - heading];
    if (heading < 270) return -kTangent[heading - 180];
    return -kTangent[360 - heading];
}

// x86 SAR on a signed 32-bit product, including negative products not exactly
// divisible by 1024. No implementation-defined signed host shifts are needed.
int floor_div_1024(int value) noexcept {
    return value >= 0 ? value / 1024 : -1 - (-1 - value) / 1024;
}

void heading_pixel(GameState& state) {
    const auto& player = state.player;
    const int h = player.heading;
    if (h >= 360) invalid("automap heading outside original tangent table");
    const int px = player.cell_x, py = player.cell_y;
    const int x0 = state.automap_origin_x, y0 = state.automap_origin_y;
    const int right = x0 + 61, bottom = y0 + 35;
    const int a = h > 180 ? px - x0 : right - px;
    int y = 0;
    int b = py - y0;
    if (h > 90 && h < 270) { y = bottom - y0; b = bottom - py; }
    int x = floor_div_1024(tangent(h) * b) + (px - x0);
    if (x >= right) {
        x = right - x0;
        const int product = (h < 90 ? -tangent(90 - h) : tangent(h - 90)) * a;
        const int shifted = floor_div_1024(product);
        y = static_cast<std::int16_t>(static_cast<std::uint16_t>(shifted + py - y0));
    } else if (x < x0) {
        x = 0;
        const int product = (h < 270 ? tangent(270 - h) : -tangent(h - 270)) * a;
        const int shifted = floor_div_1024(product);
        y = static_cast<std::int16_t>(static_cast<std::uint16_t>(shifted + py - y0));
    }
    state.automap_heading_x = static_cast<std::int16_t>(x);
    state.automap_heading_y = static_cast<std::int16_t>(std::max(0, y));
    state.automap_heading_valid = true;
}

void service_sensors_and_feedback(GameState& state) {
    auto& player = state.player;
    state.automap_heading_valid = false;
    state.automap_speckle_count = 0;
    if (player.damage_feedback >= 0) {
        --player.damage_feedback;
        state.sensor_panel_cleared = true;
        return;
    }
    if (!player.map_sensor && !player.enemy_sensor) return;
    state.sensor_panel_cleared = true;
    if (player.map_sensor) {
        const auto index = static_cast<std::size_t>(player.cell_x) * 64 + player.cell_y;
        state.automap[index] = state.automap_blink ? state.ega_map[15] : 0;
        state.automap_blink = !state.automap_blink;
        state.automap_origin_x = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(player.cell_x) - 31, 0, 2));
        state.automap_origin_y = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(player.cell_y) - 18, 0, 28));
    }
    if (player.enemy_sensor && (player.enemy_meter > 15 || (player.enemy_meter & 1u))) {
        // b1a4(6) refreshes the scratch origin even when the map is off.
        state.automap_origin_x = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(player.cell_x) - 31, 0, 2));
        state.automap_origin_y = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(player.cell_y) - 18, 0, 28));
    }
    // Case 9 runs before case 7. Enemy-only low-meter passes can deliberately
    // use an origin left behind by the previous pass's random speckles.
    heading_pixel(state);
    if (!player.map_sensor || player.map_meter > 7) return;
    if (!player.map_meter) invalid("active automap sensor has zero battery");
    const unsigned meter = player.map_meter;
    const unsigned count = 500u / (meter * meter * meter);
    for (unsigned i = 0; i < count; ++i) {
        const auto x = static_cast<std::uint8_t>(random15(state) % 62u);
        const auto y = static_cast<std::uint8_t>(random15(state) % 36u);
        state.automap_speckles[i] = {x, y};
        state.automap_origin_x = x;
        state.automap_origin_y = y;
    }
    state.automap_speckle_count = count;
}

void advance_weapon(GameState& state) {
    auto& player = state.player;
    state.draw_weapon = player.weapon;
    state.draw_weapon_recoil = 0;
    if (player.weapon == 0xff) return;
    if (player.weapon >= 4 || player.weapon_animation_frame >= 10)
        invalid("weapon frame outside IMG 511 animation table");
    // a2ce reads the current hand image and its coordinate before mutating
    // 4c3c/4c23. In particular the last lower pass still paints the old gun.
    state.draw_weapon_frame = player.weapon_animation_frame;
    switch (player.weapon_animation_state) {
    case 0:
        if (player.muzzle_flash) {
            state.draw_weapon_recoil = player.firing_mode == 1 ? 6 : 3;
            player.muzzle_flash = 0; // consume source 4c30 in this B pass.
        }
        break;
    case 1:
        if (player.weapon_animation_frame >= 9) invalid("weapon raise exceeded animation table");
        if (++player.weapon_animation_frame == 9) player.weapon_animation_state = 0;
        break;
    case 2:
        if (!player.weapon_animation_frame) invalid("weapon lower underflowed animation table");
        if (--player.weapon_animation_frame == 0) {
            if (player.queued_weapon >= 4) invalid("queued weapon outside source inventory");
            player.weapon_animation_state = 1;
            player.weapon = player.queued_weapon;
            player.firing_mode = player.weapon == 2 ? 1 : 2;
        }
        break;
    default: invalid("weapon animation has unsupported state");
    }
}

} // namespace

void reveal_visible_automap(GameState& state) {
    const auto& scene = state.scene;
    auto& level = state.level;
    if (scene.span_count > scene.spans.size())
        invalid("visible automap span count exceeds scene capacity");

    for (std::size_t i = 0; i < scene.span_count; ++i) {
        const auto vector_index = scene.spans[i].vector_index;
        if (vector_index >= level.vector_count)
            invalid("visible automap span references a missing wall vector");
        const auto& vector = level.vectors[vector_index];
        int x0 = static_cast<int>(vector.x0) >> 6;
        int y0 = static_cast<int>(vector.y0) >> 6;
        int x1 = static_cast<int>(vector.x1) >> 6;
        int y1 = static_cast<int>(vector.y1) >> 6;

        if (vector.flags & 8u) {
            if (x0 < 0 || x0 >= 64 || y0 < 0 || y0 >= 64)
                invalid("visible automap door cell exceeds source grid");
            state.automap[static_cast<std::size_t>(x0) * 64u +
                          static_cast<std::size_t>(y0)] = state.ega_map[9];
            continue;
        }

        if (vector.direction == 2) {
            x0 = x1 = x0 - 1;
        } else if (vector.direction == 1) {
            y0 = y1 = y0 - 1;
        }

        if (x0 == x1) {
            if (y1 <= y0) continue;
            if (x0 < 0 || x0 >= 64 || y0 < 0 || y1 > 64)
                invalid("visible automap vertical wall span exceeds source grid");
            auto index = static_cast<std::size_t>(x0) * 64u +
                         static_cast<std::size_t>(y0);
            for (int y = y0; y < y1; ++y, ++index)
                state.automap[index] = state.ega_map[2];
        } else {
            if (x1 <= x0) continue;
            if (x0 < 0 || x1 > 64 || y0 < 0 || y0 >= 64)
                invalid("visible automap horizontal wall span exceeds source grid");
            auto index = static_cast<std::size_t>(x0) * 64u +
                         static_cast<std::size_t>(y0);
            for (int x = x0; x < x1; ++x, index += 64u)
                state.automap[index] = state.ega_map[2];
        }
    }
}

void initialize_pushwalls(GameState& state) {
    state.pushwalls.fill({});
    state.pushwall_count = 0;
    for (std::size_t i = 0; i < state.level.object_count; ++i) {
        if (state.level.objects[i].class_id != 0x28) continue;
        if (state.pushwall_count == state.pushwalls.size())
            invalid("MAP exceeds 12 pushwall records");
        state.pushwalls[state.pushwall_count++].object_slot = static_cast<std::uint16_t>(i);
    }
}

void activate_pushwall(GameState& state, std::uint16_t cell_index) {
    if (cell_index >= kMapCellCount) invalid("pushwall use cell outside MAP grid");
    if (state.player.octant >= kPushX.size()) invalid("pushwall direction outside octant table");
    for (std::size_t i = 0; i < state.pushwall_count; ++i) {
        auto& wall = state.pushwalls[i];
        if (wall.object_slot >= state.level.object_count)
            invalid("pushwall object slot outside MAP records");
        const auto& object = state.level.objects[wall.object_slot];
        if (object.cell_y * 64u + object.cell_x != cell_index ||
            object.image_id != state.level.cells[cell_index].object_id) continue;
        if (wall.steps_remaining) return;
        const int dx = kPushX[state.player.octant], dy = kPushY[state.player.octant];
        const int beyond_x = static_cast<int>(cell_index % 64) + (dx > 0) - (dx < 0);
        const int beyond_y = static_cast<int>(cell_index / 64) + (dy > 0) - (dy < 0);
        if (beyond_x < 0 || beyond_x >= 64 || beyond_y < 0 || beyond_y >= 64)
            invalid("pushwall target outside MAP grid");
        const auto beyond = static_cast<std::size_t>(beyond_y * 64 + beyond_x);
        if (state.level.wall_flags[state.level.cells[beyond].wall_id] & 2u) return;
        wall.step_x = static_cast<std::int8_t>(dx);
        wall.step_y = static_cast<std::int8_t>(dy);
        wall.steps_remaining = 8;
        effect(state, 0x43);
        return;
    }
    invalid("usable pushwall has no matching object record");
}

void initialize_lightning(GameState& state) {
    if (!lightning_level(state)) return;
    state.lightning_on = false;
    state.lightning_countdown = 0x50;
    // The scene shade translation must additionally remap every 0xda output
    // to 0xdb except input 0xda itself; prepare_scene rebuilds that table.
    for (auto& entry : state.scene.shade_map)
        if (entry == 0xda) entry = 0xdb;
    state.scene.shade_map[0xda] = 0xda;
    state.palette[0xda] = {0, 0, 0};
}

void service_world_a(GameState& state, const Assets&, std::uint32_t a_count) {
    // The caller has already run 1d4e (doors) then 80aa (guards). Do not
    // consume lightning's RNG draw until after guard, sensor, and hazard work.
    drain_sensors(state, a_count);      // bb26
    update_panel_indicator(state);      // bd2a
    update_forward_caption(state);      // bdd8
    step_pushwalls(state);              // 2210
    damage_on_floor(state);             // be62
    tick_lightning(state);              // ba74(0)
    // a3b6(0x11) and (A&7 ? nothing : a3b6(0x10)) are proven no-ops.
    // The caller then saturating-increments 01fa (fire_delay) via aa90(1).
}

void advance_chalkboard(GameState& state, const Assets& assets) {
    if (!state.chalkboard_active || !state.scene.ready) return;
    if (state.episode < 1 || state.episode > assets.episodes.size())
        invalid("chalkboard episode outside IMG archives");
    const auto& seq = assets.episodes[state.episode - 1].images.sequences[state.chalkboard_image];
    if (!seq.offset || !seq.frame_count) invalid("chalkboard wall has no IMG frames");
    // 1010:c0a2 changes the IMG-cache delay to 150ms.  1010:65a6 runs
    // while drawing visible walls, not as a level-wide animation timer.
    for (int x = state.scene.viewport_left; x <= state.scene.viewport_right; ++x) {
        auto& column = state.scene.wall_columns[static_cast<std::size_t>(x)];
        if (column.image_id != state.chalkboard_image ||
            column.vector_index >= state.level.vector_count) continue;
        auto& v = state.level.vectors[column.vector_index];
        if (static_cast<std::int32_t>(state.clocks.last_milliseconds - v.deadline) >= 0) {
            v.frame_index = static_cast<std::uint8_t>((v.frame_index + 1u) % seq.frame_count);
            v.deadline = state.clocks.last_milliseconds + 150u;
        }
        column.frame_index = v.frame_index;
    }
}

void advance_destructible_walls(GameState& state, const Assets& assets) {
    if (!state.has_destructible_walls || !state.scene.ready) return;
    auto& level = state.level;
    const auto& images = assets.episodes[state.episode - 1].images;
    for (int x = state.scene.viewport_left; x <= state.scene.viewport_right; ++x) {
        auto& column = state.scene.wall_columns[static_cast<std::size_t>(x)];
        if (column.vector_index >= level.vector_count) continue;
        auto& v = level.vectors[column.vector_index];
        if (v.class_id != 0x2d || !(v.flags & 1u)) continue;
        const auto id = v.cached_image_id ? v.cached_image_id : v.image_id;
        const auto& seq = images.sequences[id];
        if (!seq.offset || !seq.frame_count || !seq.delay_ms)
            invalid("class-0x2d visible wall has no IMG animation");
        if (static_cast<std::int32_t>(state.clocks.last_milliseconds - v.deadline) < 0)
            continue;
        ++v.frame_index;
        v.deadline = state.clocks.last_milliseconds + seq.delay_ms;
        if (v.frame_index >= seq.frame_count) {
            v.frame_index = static_cast<std::uint8_t>(seq.frame_count - 1u);
            // 1018:3c0c finds every class-0x2d vector at this cell and
            // removes its entire merged run, but leaves its class unchanged.
            const int cell_x = (v.x0 >> 6) - (v.direction == 2);
            const int cell_y = (v.y0 >> 6) - (v.direction == 1);
            for (std::size_t i = 0; i < level.vector_count; ++i) {
                auto& other = level.vectors[i];
                if (other.class_id != 0x2d ||
                    cell_x < (other.x0 >> 6) - (other.direction == 2) ||
                    cell_x > (other.x1 >> 6) ||
                    cell_y < (other.y0 >> 6) - (other.direction == 1) ||
                    cell_y > (other.y1 >> 6)) continue;
                other.flags &= static_cast<std::uint8_t>(~1u);
                const int left = (other.x0 >> 6) - (other.direction == 2);
                const int top = (other.y0 >> 6) - (other.direction == 1);
                if (other.direction < 2) {
                    for (int col = left; col < (other.x1 >> 6); ++col)
                        if (col >= 0 && col < 64 && top >= 0 && top < 64)
                            level.cells[static_cast<std::size_t>(top * 64 + col)].wall_id = 0;
                } else {
                    for (int row = top; row < (other.y1 >> 6); ++row)
                        if (left >= 0 && left < 64 && row >= 0 && row < 64)
                            level.cells[static_cast<std::size_t>(row * 64 + left)].wall_id = 0;
                }
            }
        }
        column.frame_index = v.frame_index;
    }
}

void service_world_b(GameState& state, const Assets&) {
    // Called after d78c prepared its same-pass scene; the caller then draws
    // HUD and runs 1e00, 9e20, present and 9806 in that order.
    service_sensors_and_feedback(state); // bbca
    advance_weapon(state);               // ba54 -> a3b6(2) -> a2ce
}

} // namespace n3d::detail
