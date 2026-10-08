#pragma once

#include "n3d/game.hpp"

#include <cstddef>
#include <cstdint>
#include <array>

namespace n3d::detail {

// Original MSVC 32-bit LCG (1008:6ec8). Every sound/guard draw is retained even
// when an audio device is muted or absent; only callers decide when to draw.
inline std::uint16_t random15(GameState& state) noexcept {
    state.rng_seed = state.rng_seed * 0x343fdu + 0x269ec3u;
    return static_cast<std::uint16_t>((state.rng_seed >> 16u) & 0x7fffu);
}

void start_level(GameState& state, const Assets& assets, std::uint8_t episode,
                 std::uint16_t level_index);
void apply_cheats(GameState& state); // 1010:b128, on new-game init and Cheat Done.
void end_tally(GameState& state); // 1018:0ab0(10,0), E1/E2/E3 terminal tally.
void set_heading(GameState& state, int degrees);
void service_doors(GameState& state);
void advance_doors_and_panels(GameState& state);
bool vector_contains(const VectorRecord& vector, std::size_t cell) noexcept;
void initialize_pushwalls(GameState& state);
void activate_pushwall(GameState& state, std::uint16_t cell_index);
void initialize_lightning(GameState& state);
void reveal_visible_automap(GameState& state); // 1010:6266 -> b1a4(0), after prepare_scene.
void service_world_a(GameState& state, const Assets& assets, std::uint32_t a_count);
void service_world_b(GameState& state, const Assets& assets);
void advance_chalkboard(GameState& state, const Assets& assets);
void advance_destructible_walls(GameState& state, const Assets& assets);
void initialize_guard_frames(GameState& state, const Assets& assets);
void wake_room_guards(GameState& state, std::uint8_t room_id);
void service_guards(GameState& state, const Assets& assets);
void start_guard_dance(GameState& state, const Assets& assets); // 1010:ae56 radio conversion.
void restore_level_music(GameState& state); // 1010:c63e after radio dance.
bool popup_row_enabled(const GameState& state, std::uint8_t row);
bool guard_open_door(GameState& state, std::uint16_t cell_index,
                     std::uint8_t facing_octant);
std::array<bool, 8> service_projectiles(GameState& state, const Assets& assets);
void handle_gameplay_input(GameState& state, const Assets& assets,
                           const InputFrame& input);
void damage_guard(GameState& state, const Assets& assets,
                  std::uint16_t guard_index);
void draw_hud(const GameState& state, const Assets& assets, IndexedFrame& frame);
void draw_overlay(const GameState& state, const Assets& assets, IndexedFrame& frame);

} // namespace n3d::detail
