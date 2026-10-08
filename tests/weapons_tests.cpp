#include "n3d/game.hpp"
#include "../src/game/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

namespace {

int failures = 0;
void check(bool ok, std::string_view message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
constexpr std::size_t cell(unsigned x, unsigned y) { return y * 64u + x; }

void empty_ammo_and_full_slots_do_not_fire() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::gameplay;
    state->player_life_state = 1;
    state->player.weapon = 0;
    state->player.weapon_owned = 1;
    state->player.firing_mode = 2;
    state->fire_delay = 2;
    n3d::InputFrame fire{};
    fire.held = 0x80;
    n3d::detail::handle_gameplay_input(*state, assets, fire);
    check(state->projectiles[0].state == 0 && state->player.ammo[1] == 0 &&
              !state->player.muzzle_flash && !state->audio.count && state->fire_delay == 0,
          "empty ammunition leaves the reserved slot free with no flash or sound");

    state->previous_fire = 0;
    state->fire_delay = 2;
    state->player.ammo[1] = 1;
    for (auto& shot : state->projectiles) shot.state = 2;
    n3d::detail::handle_gameplay_input(*state, assets, fire);
    check(state->player.ammo[1] == 1 && !state->player.muzzle_flash && !state->audio.count,
          "a full eight-slot projectile pool is checked before spending ammunition");
}

void jam_precedes_ammo_and_sound() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::gameplay;
    state->player_life_state = 1;
    state->player.weapon = 0;
    state->player.firing_mode = 2;
    state->player.jammed = 1;
    state->player.ammo[1] = 50;
    state->fire_delay = 2;
    n3d::InputFrame fire{};
    fire.held = 0x80;
    n3d::detail::handle_gameplay_input(*state, assets, fire);
    check(state->screen == n3d::Screen::modal_message &&
              state->player.ammo[1] == 50 && state->projectiles[0].state == 0 &&
              !state->audio.count && !state->player.muzzle_flash,
          "jam reports failure without reserving a slot or spending ammunition");
}

void ammo_pickup_keeps_signed_compare_and_unclamped_add() {
    const n3d::Assets assets{};
    const auto collect = [&assets](std::uint8_t subtype, std::uint8_t starting) {
        auto state = std::make_unique<n3d::GameState>();
        state->screen = n3d::Screen::gameplay;
        state->player_life_state = 1;
        state->player.cell_x = 10;
        state->player.cell_y = 10;
        state->player.world_x = 10 * 64 + 32;
        state->player.world_y = 10 * 64 + 20;
        state->clocks.move_step = 1;
        const auto id = static_cast<std::uint8_t>(41 + subtype);
        state->level.cells[cell(10, 9)].object_id = id;
        state->level.object_class_by_id[id] = 0x39;
        state->level.object_flags[id] = 5;
        state->level.object_count = 1;
        auto& item = state->level.objects[0];
        item.image_id = id;
        item.class_id = 0x39;
        item.flags = 1;
        item.subtype = subtype;
        item.cell_x = 10;
        item.cell_y = 9;
        const unsigned pool = subtype == 0 ? 0u : subtype == 1 ? 1u : 2u;
        state->player.ammo[pool] = starting;
        n3d::InputFrame forward{};
        forward.held = 0x02;
        n3d::detail::handle_gameplay_input(*state, assets, forward);
        return std::pair{state->player.ammo[pool], state->level.cells[cell(10, 9)].object_id};
    };
    const auto [above_100, consumed] = collect(0, 95);
    check(above_100 == 115 && consumed == 0, "signed pool below 100 adds 20 without clamping");
    const auto [wrapped, wrapped_item] = collect(1, 250);
    check(wrapped == 14 && wrapped_item == 0, "negative signed pool adds 20 modulo 256");
    const auto [unsigned_pool, retained] = collect(2, 250);
    check(unsigned_pool == 250 && retained == 43,
          "unsigned spellbook pool rejects pickup above 99 without consuming it");
}

void twentieth_core_hit_teleports_then_transitions() {
    auto state = std::make_unique<n3d::GameState>();
    auto assets = std::make_unique<n3d::Assets>();
    state->screen = n3d::Screen::gameplay;
    state->player_life_state = 1;
    state->episode = 2;
    state->level_index = 9;
    state->player.weapon = 0;
    state->player.world_x = 50 * 64 + 32;
    state->player.world_y = 41 * 64 + 32;
    state->player.cell_x = 50;
    state->player.cell_y = 41;
    state->clocks.projectile_step = 1;
    state->clocks.average_present_ms = 40;
    state->level.cells[cell(50, 37)] = {80, 97};
    state->level.wall_flags[80] = 4;
    state->level.object_flags[97] = 0x40;
    state->level.wall_class_by_id[255] = 0x2d;
    assets->episodes[1].images.sequences[507].offset = 1;
    assets->episodes[1].images.sequences[507].frame_count = 2;
    assets->episodes[1].images.sequences[507].delay_ms = 50;
    assets->episodes[1].images.sequences[508].offset = 1;
    assets->episodes[1].images.sequences[508].frame_count = 3;
    assets->episodes[1].images.sequences[508].delay_ms = 50;
    assets->episodes[1].images.sequences[255].offset = 1;
    assets->episodes[1].images.sequences[255].frame_count = 9;
    assets->episodes[1].images.sequences[255].delay_ms = 100;
    state->plasma_hits = 18;
    const auto hit = [&] {
        auto& shot = state->projectiles[0];
        shot = {};
        shot.state = 1;
        shot.image_id = 251;
        shot.world_x = 50 * 64 + 32;
        shot.world_y = 37 * 64 + 32;
        shot.sign_y = -1;
        n3d::detail::service_projectiles(*state, *assets);
    };
    hit();
    check(state->plasma_hits == 19 && !state->core_wait_ticks &&
              state->player.world_y == 41 * 64 + 32 && !state->audio.count,
          "nineteenth core projectile is consumed before the solid wall without impact sound");
    hit();
    check(state->plasma_hits == 20 && state->player.world_y == 52 * 64 + 32 &&
              state->core_wait_ticks == 22 && !state->plasma_destroyed,
          "global twentieth hit displaces player 704 units and waits 900/40 B passes");
    for (unsigned tick = 1; tick <= 22; ++tick) {
        state->clocks.logical_tick = tick;
        n3d::detail::service_projectiles(*state, *assets);
    }
    check(state->plasma_destroyed && state->player_life_state == 3 &&
              state->screen == n3d::Screen::modal_message,
          "core completion opens its modal only after the calibrated wait");
    n3d::InputFrame enter{};
    enter.key = 0x0d;
    n3d::detail::handle_gameplay_input(*state, *assets, enter);
    check(state->screen == n3d::Screen::modal_message,
          "first dismissal leaves the second core modal open");
    n3d::detail::handle_gameplay_input(*state, *assets, enter);
    check(state->screen == n3d::Screen::gameplay,
          "closing the second core message resumes gameplay before the idle dispatcher");
    // The source dc18/dc5e idle dispatch calls 0ab0(10); the full n3d::step
    // path also paints UIF14, whereas this focused state test has no UI assets.
    n3d::detail::end_tally(*state);
    check(state->screen == n3d::Screen::intermission && state->pending_level == 10,
          "terminal core tally records level ten without loading an absent MAP level");
    n3d::detail::handle_gameplay_input(*state, *assets, enter);
    check(state->screen == n3d::Screen::root_menu,
          "accepting the final E2 tally returns to root rather than automatically starting E3");
}

void dead_guards_cannot_score_or_reenter_pain() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->level.guard_count = 1;
    state->level.object_count = 1;
    auto& guard = state->level.guards[0];
    guard.object_slot = 0;
    guard.state = 9;
    guard.hit_points = 5;
    state->player.score = 100;
    n3d::detail::damage_guard(*state, assets, 0);
    check(guard.state == 9 && guard.hit_points == 5 && state->player.score == 100 &&
              !state->audio.count,
          "a dying guard is terminal even before hit points reach zero");

    guard.state = 2;
    guard.hit_points = 0;
    n3d::detail::damage_guard(*state, assets, 0);
    check(guard.state == 2 && !guard.hit_points && state->player.score == 100 &&
              !state->audio.count,
          "a zero-HP guard cannot be re-killed or award duplicate points");
}

} // namespace

int run_weapons_tests() {
    empty_ammo_and_full_slots_do_not_fire();
    jam_precedes_ammo_and_sound();
    ammo_pickup_keeps_signed_compare_and_unclamped_add();
    twentieth_core_hit_teleports_then_transitions();
    dead_guards_cannot_score_or_reenter_pain();
    if (failures) std::cerr << failures << " weapon checks failed\n";
    return failures;
}
