#include "internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace n3d::detail {
namespace {

// Windows nite3w.exe SHA-256 12FE5168783446275802E0E947898261B5ECA6B88288F3A895FC1FAA4C544481:
// 1010:7b56/80aa dispatch, 7494/d50a sight, 76fc/71dc/700a movement,
// 80f8/9fa2 hits and 8c0a/a1ea attacks. The level loader deliberately zeros
// the original's unwritten guard bytes on every native level start; this is a
// deterministic safety policy, NOT evidence of the reused Win16 segment's state.
int magnitude(int n) noexcept { return n < 0 ? -n : n; }
int direction(int n) noexcept { return (n > 0) - (n < 0); }
std::int16_t word(int n) noexcept { return static_cast<std::int16_t>(static_cast<std::uint16_t>(n)); }

const EpisodeAssets& episode(const GameState& state, const Assets& assets) {
    if (state.episode < 1 || state.episode > assets.episodes.size())
        throw std::runtime_error("guard service: invalid episode");
    return assets.episodes[state.episode - 1];
}

const std::byte* descriptor(const EpisodeAssets& ep, const ObjectRecord& obj) {
    const auto& sequence = ep.images.sequences[256u + obj.image_id];
    if (!sequence.offset || sequence.descriptor_offset > ep.image_bytes.size() ||
        ep.image_bytes.size() - sequence.descriptor_offset < 90)
        throw std::runtime_error("guard service: missing extended IMG descriptor");
    return ep.image_bytes.data() + sequence.descriptor_offset;
}

std::uint16_t animation(const std::byte* data, unsigned offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(data[offset]) |
                                      (std::to_integer<unsigned>(data[offset + 1]) << 8u));
}

void enter_animation(GuardRecord& guard, ObjectRecord& obj, std::uint16_t frames,
                     std::uint8_t state, std::uint8_t next) noexcept {
    guard.animation_word = frames;
    obj.frame_index = static_cast<std::uint8_t>(frames);
    guard.timer = static_cast<std::int16_t>(static_cast<int>(frames >> 8) - 1);
    guard.state = state;
    guard.next_state = next;
}

void advance_frame(const GuardRecord& guard, ObjectRecord& obj) noexcept {
    const unsigned first = static_cast<std::uint8_t>(guard.animation_word);
    const unsigned count = guard.animation_word >> 8;
    const unsigned next = static_cast<unsigned>(obj.frame_index) + 1;
    obj.frame_index = static_cast<std::uint8_t>(count && next < first + count ? next : first);
}

std::uint8_t player_octant(const PlayerState& player, const ObjectRecord& obj,
                           bool orientation_aware) noexcept {
    const int dx = static_cast<int>(player.world_x) - obj.world_x;
    const int dy = static_cast<int>(player.world_y) - obj.world_y;
    const int ax = magnitude(dx), ay = magnitude(dy);
    if (orientation_aware) { // 1010:d454(mode=1), including strict half-axis ties.
        if (2 * ay < ax) return dx > 0 ? 2 : 6;
        if (2 * ax < ay) return dy > 0 ? 4 : 0;
        if (dx > 0) return dy > 0 ? 3 : 1;
        return dy > 0 ? 5 : 7;
    }
    if (dx >= 0) {
        if (dy < 0) return ay > ax ? 0 : 1;
        return ay <= ax ? 2 : 3;
    }
    if (dy >= 0) return ay > ax ? 4 : 5;
    return ay > ax ? 7 : 6;
}

void orient(GameState& state, const std::byte* image, GuardRecord& guard,
            ObjectRecord& obj, bool force) {
    const unsigned octant = (static_cast<unsigned>(guard.facing) +
        (guard.orientation_aware ? 4u : 3u) -
        player_octant(state.player, obj, guard.orientation_aware != 0)) & 7u;
    if (guard.resolved_octant == octant && !force) return;
    guard.resolved_octant = static_cast<std::uint8_t>(octant);
    const unsigned bank = guard.state == 8 || guard.state == 16 || guard.state == 17 ? 0x14u :
                          guard.state == 6 && guard.strategy != 2 ? 0x24u : 4u;
    const auto frames = animation(image, bank + 2u * octant);
    guard.animation_word = frames;
    obj.frame_index = static_cast<std::uint8_t>(frames);
}

void face_step(GuardRecord& guard, int dx, int dy) noexcept {
    if (!dx && !dy) return;
    if (dx > 0) guard.facing = dy < 0 ? 1 : (dy > 0 ? 3 : 2);
    else if (dx < 0) guard.facing = dy < 0 ? 7 : (dy > 0 ? 5 : 6);
    else guard.facing = dy < 0 ? 0 : 4;
}

std::size_t cell_at(int x, int y) {
    if (x < 0 || x >= static_cast<int>(kMapWidth * 64) ||
        y < 0 || y >= static_cast<int>(kMapHeight * 64))
        throw std::runtime_error("guard probe left the defined MAP grid");
    return static_cast<std::size_t>((y >> 6) * 64 + (x >> 6));
}

DoorState& door_at(GameState& state, std::size_t cell) {
    for (std::size_t i = 0; i < state.door_count; ++i)
        if (state.doors[i].cell_index == cell) return state.doors[i];
    throw std::runtime_error("guard probe: door cell has no door record");
}

// 1010:d50a visits the target before testing its wall/object flags. Movement
// and sight callers select whether intermediate solid objects block the ray.
bool ray_clear(const LevelState& level, int x, int y, int dx, int dy,
               unsigned limit, bool solid_objects, const GameState& state) {
    const int ax = magnitude(dx), ay = magnitude(dy);
    const bool y_major = ay >= ax;
    const int major = y_major ? ay : ax;
    const int minor = y_major ? ax : ay;
    const int sx = direction(dx), sy = direction(dy);
    int error = 2 * minor - major;
    const int target_x = x + dx, target_y = y + dy;
    if (!major) return false; // d50a steps away on a zero delta; it never tests the start cell.
    for (unsigned step = 0; step < limit; ++step) {
        if (y_major) y += sy;
        else x += sx;
        if (error >= 0) {
            if (y_major) x += sx;
            else y += sy;
            error += 2 * (minor - major);
        } else error += 2 * minor;
        if (x == target_x && y == target_y) return true;
        if (x < 0 || y < 0 || x >= 64 || y >= 64)
            throw std::runtime_error("guard sight left the defined MAP grid");
        const auto cell = static_cast<std::size_t>(y * 64 + x);
        const auto& tile = level.cells[cell];
        const unsigned wall = level.wall_flags[tile.wall_id];
        if (wall & 2u) {
            if (wall & 4u) return false;
            if ((wall & 8u) && !([&state, cell] {
                for (std::size_t i = 0; i < state.door_count; ++i)
                    if (state.doors[i].cell_index == cell)
                        return state.doors[i].state == 0 || state.doors[i].state == 4;
                throw std::runtime_error("guard sight: door cell has no door record");
            })()) return false;
        }
        if (solid_objects && (level.object_flags[tile.object_id] & 2u) &&
            !(level.object_flags[tile.object_id] & 0x20u)) return false;
    }
    return false;
}

bool sight(const GameState& state, const GuardRecord& guard, const ObjectRecord& obj,
           bool solid_objects, bool skip_cone) {
    const int dx = static_cast<int>(state.player.cell_x) - obj.cell_x;
    const int dy = static_cast<int>(state.player.cell_y) - obj.cell_y;
    if (magnitude(dx) > 8 || magnitude(dy) > 8) return false;
    if (!skip_cone) {
        const unsigned facing = guard.facing & 7u;
        const unsigned cone = (1u << ((facing + 7u) & 7u)) | (1u << facing) |
                              (1u << ((facing + 1u) & 7u));
        unsigned mask = magnitude(dx) >= magnitude(dy) ? 0x66u : 0x99u;
        mask &= cone;
        mask &= dy >= 0 ? 0x3cu : 0xc3u;
        if (!(mask & (dx >= 0 ? 0x0fu : 0xf0u))) return false;
    }
    return ray_clear(state.level, obj.cell_x, obj.cell_y, dx, dy, 8,
                     solid_objects, state);
}

bool attack_ready(GameState& state, GuardRecord& guard, const ObjectRecord& obj) {
    guard.line_of_sight = sight(state, guard, obj, true, true);
    guard.adjacent = magnitude(static_cast<int>(state.player.world_x) - obj.world_x) <= 64 &&
                     magnitude(static_cast<int>(state.player.world_y) - obj.world_y) <= 64;
    if (guard.attack_kind == 0) return guard.adjacent != 0;
    if (guard.attack_kind == 1 || guard.attack_kind == 2) return guard.line_of_sight != 0;
    throw std::runtime_error("guard service: unknown attack kind");
}

void emit_effect(GameState& state, const Assets& assets, std::uint8_t id,
                 std::uint8_t priority) {
    const auto& entry = assets.sound.directory[32u + id];
    if (state.config.effects_enabled && entry.length)
        state.audio.push({AudioCommandKind::effect, id, 0, priority});
}

enum class GuardSound { alert, attack, death };
void guard_sound(GameState& state, const Assets& assets, std::uint8_t class_id,
                 GuardSound kind) {
    unsigned sound = 0;
    switch (class_id) {
    case 8: sound = kind == GuardSound::alert ? 0x22u : kind == GuardSound::death ? 0x23u : 0u; break;
    case 9: case 0x0a:
        sound = kind == GuardSound::alert ? 0x38u + random15(state) % 3u :
                kind == GuardSound::attack ? 0x41u : class_id == 9 ? 8u : 7u;
        break;
    case 0x0b: sound = kind == GuardSound::alert ? 0x3bu : kind == GuardSound::attack ? 0x4eu : 0x24u; break;
    case 0x0c: sound = kind == GuardSound::alert ? 0x14u : kind == GuardSound::attack ? 0x20u : 4u; break;
    case 0x0d: sound = kind == GuardSound::alert ? 0x15u : kind == GuardSound::attack ? 0x20u : 0x13u; break;
    case 0x0e: sound = kind == GuardSound::alert ? 0x10u : kind == GuardSound::attack ? 0x20u : 0x0eu; break;
    case 0x0f: case 0x10:
        sound = kind == GuardSound::alert ? 0x36u + random15(state) % 2u :
                kind == GuardSound::attack ? 0x17u + random15(state) % 3u :
                0x0bu + random15(state) % 3u;
        break;
    case 0x11: sound = kind == GuardSound::alert ? 6u : 0u; break;
    case 0x12: sound = kind == GuardSound::alert ? 0x3fu : kind == GuardSound::attack ? 0x3eu : 7u; break;
    case 0x13: sound = kind == GuardSound::alert ? 0x3cu : kind == GuardSound::attack ? 0x40u : 7u; break;
    case 0x14: sound = kind == GuardSound::death ? 0x23u : 0u; break;
    case 0x15: case 0x20: case 0x21: break;
    case 0x16:
        sound = kind == GuardSound::alert ? 0x12u :
                kind == GuardSound::attack ? 0x17u + random15(state) % 3u : 0u;
        break;
    case 0x17: sound = kind == GuardSound::alert ? 0x48u : kind == GuardSound::attack ? 0x1fu : 0x46u; break;
    case 0x18: sound = kind == GuardSound::alert ? 0x47u : kind == GuardSound::attack ? 0x20u : 0x46u; break;
    case 0x19: sound = kind == GuardSound::attack ? 0x1du : 0u; break;
    case 0x1a: sound = kind == GuardSound::alert ? 0x49u : kind == GuardSound::attack ? 0x4eu : 0x4au; break;
    case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        sound = kind == GuardSound::death ? (class_id == 0x1d ? 9u :
                class_id >= 0x1e ? 0x46u : 2u) :
                kind == GuardSound::alert ? (class_id == 0x1d ? 0x38u :
                class_id >= 0x1e ? 0x3du : 0x0du) : 0x4bu + random15(state) % 4u;
        break;
    default: throw std::runtime_error("guard audio: class outside the Windows sound tables");
    }
    emit_effect(state, assets, static_cast<std::uint8_t>(sound),
                kind == GuardSound::attack ? 1 : 2);
}

void bob(GuardRecord& guard, ObjectRecord& obj) noexcept {
    if (obj.class_id != 8 && obj.class_id != 0x14 && obj.class_id != 0x1a) return;
    if (!guard.bob_speed) guard.bob_speed = 1;
    obj.lift = static_cast<std::int16_t>(obj.lift + guard.bob_speed);
    if (obj.lift <= 10) {
        obj.lift = 10;
        guard.bob_speed = static_cast<std::int8_t>(-guard.bob_speed);
    }
    if (obj.lift >= 35) {
        obj.lift = 35;
        guard.bob_speed = static_cast<std::int8_t>(-guard.bob_speed);
    }
}

bool blocked(GameState& state, const GuardRecord& guard, int x, int y) {
    if (magnitude(x - state.player.world_x) < 42 &&
        magnitude(y - state.player.world_y) < 42) return true;
    const auto index = cell_at(x, y);
    const auto& tile = state.level.cells[index];
    if (state.level.object_flags[tile.object_id] & 2u) return true;
    const unsigned wall = state.level.wall_flags[tile.wall_id];
    if (wall & 8u) {
        auto& door = door_at(state, index);
        if (door.state == 1) {
            const auto class_id = state.level.wall_class_by_id[tile.wall_id];
            if (class_id < 0x33 || class_id > 0x3c) {
                if (guard.strategy == 1)
                    throw std::runtime_error("guard strategy 1: unimplemented class-0x43 closed-door turn");
                if (guard.state == 6) door.sound = true;
                guard_open_door(state, static_cast<std::uint16_t>(index), guard.facing);
            }
            return true;
        }
        return door.state != 0 && door.state != 4;
    }
    return (wall & 2u) != 0;
}

void reorder_axis(LevelState& level, std::uint16_t slot, bool x_axis) noexcept {
    auto& order = x_axis ? level.object_list_x : level.object_list_y;
    auto& obj = level.objects[slot];
    std::size_t rank = x_axis ? obj.x_order_index : obj.y_order_index;
    const int coordinate = x_axis ? obj.world_x : obj.world_y;
    const auto key = [x_axis, &level](std::uint16_t index) noexcept {
        return x_axis ? level.objects[index].world_x : level.objects[index].world_y;
    };
    while (rank && coordinate < key(order[rank - 1])) {
        const auto shifted = order[rank - 1];
        order[rank] = shifted;
        (x_axis ? level.objects[shifted].x_order_index : level.objects[shifted].y_order_index) =
            static_cast<std::uint16_t>(rank);
        --rank;
    }
    while (rank + 1 < level.object_count && coordinate > key(order[rank + 1])) {
        const auto shifted = order[rank + 1];
        order[rank] = shifted;
        (x_axis ? level.objects[shifted].x_order_index : level.objects[shifted].y_order_index) =
            static_cast<std::uint16_t>(rank);
        ++rank;
    }
    order[rank] = slot;
    (x_axis ? obj.x_order_index : obj.y_order_index) = static_cast<std::uint16_t>(rank);
}

std::uint8_t first_wall_id(const LevelState& level, std::uint8_t class_id) {
    for (unsigned id = 0; id < level.wall_class_by_id.size(); ++id)
        if (level.wall_class_by_id[id] == class_id) return static_cast<std::uint8_t>(id);
    throw std::runtime_error("guard service: floor class is absent from MAP table");
}

void step_guard(GameState& state, GuardRecord& guard, ObjectRecord& obj) {
    auto& level = state.level;
    const auto old_cell = cell_at(obj.world_x, obj.world_y);
    const auto previous_wall = level.cells[old_cell].wall_id;
    level.cells[old_cell].object_id = guard.hidden_object_id;
    bob(guard, obj);
    const int dx = guard.step_x, dy = guard.step_y;
    const int lead_x = direction(dx) * 16, lead_y = direction(dy) * 16;
    const int dest_x = obj.world_x + dx + lead_x;
    const int dest_y = obj.world_y + dy + lead_y;
    const bool block_x = dx && (blocked(state, guard, dest_x, obj.world_y - 16) ||
                                blocked(state, guard, dest_x, obj.world_y + 16));
    const bool block_y = dy && (blocked(state, guard, obj.world_x - 16, dest_y) ||
                                blocked(state, guard, obj.world_x + 16, dest_y));
    const int move_x = block_x ? 0 : dx, move_y = block_y ? 0 : dy;
    if (guard.state != 8 || (!block_x && !block_y)) {
        if (move_x) {
            obj.world_x = word(static_cast<int>(obj.world_x) + move_x);
            reorder_axis(level, guard.object_slot, true);
        }
        if (move_y) {
            obj.world_y = word(static_cast<int>(obj.world_y) + move_y);
            reorder_axis(level, guard.object_slot, false);
        }
    }
    if (guard.state == 6 && block_x && block_y) {
        if (random15(state) & 1u) guard.step_x = static_cast<std::int8_t>(-guard.step_x);
        else guard.step_y = static_cast<std::int8_t>(-guard.step_y);
    }
    if (move_x || move_y) advance_frame(guard, obj);
    face_step(guard, move_x, move_y);
    const auto current_cell = cell_at(obj.world_x, obj.world_y);
    obj.cell_x = static_cast<std::uint16_t>(current_cell % 64);
    obj.cell_y = static_cast<std::uint16_t>(current_cell / 64);
    guard.hidden_object_id = level.cells[current_cell].object_id;
    level.cells[current_cell].object_id = obj.image_id;
    const auto& tile = level.cells[current_cell];
    if (current_cell != old_cell && level.wall_class_by_id[tile.wall_id] == 0x44) {
        const auto first = level.wall_class_by_id[previous_wall] == 0x44 ?
            static_cast<std::uint8_t>(previous_wall - guard.room_id) : first_wall_id(level, 0x44);
        guard.room_id = static_cast<std::uint8_t>(tile.wall_id - first);
    }
}

void set_arrow_step(GuardRecord& guard) noexcept {
    constexpr std::array<std::int8_t, 8> dx{0, 1, 1, 1, 0, -1, -1, -1};
    constexpr std::array<std::int8_t, 8> dy{-1, -1, 0, 1, 1, 1, 0, -1};
    const unsigned facing = guard.facing & 7u;
    const int factor = guard.strategy == 2 ? 16 : 8;
    guard.step_x = static_cast<std::int8_t>(dx[facing] * factor);
    guard.step_y = static_cast<std::int8_t>(dy[facing] * factor);
}

void patrol_tile(GameState& state, GuardRecord& guard, const ObjectRecord& obj) {
    if ((obj.world_x & 63) != 32 || (obj.world_y & 63) != 32) return;
    const auto& level = state.level;
    const auto wall_id = level.cells[static_cast<std::size_t>(obj.cell_y) * 64 + obj.cell_x].wall_id;
    const auto class_id = level.wall_class_by_id[wall_id];
    if (class_id == 0x41) {
        guard.facing = static_cast<std::uint8_t>(wall_id - first_wall_id(level, 0x41));
        if (guard.facing >= 8) throw std::runtime_error("guard patrol: invalid arrow octant");
        set_arrow_step(guard);
    } else if (class_id == 0x42) {
        if (!guard.step_x && !guard.step_y) {
            const auto facing = static_cast<std::uint8_t>(wall_id - first_wall_id(level, 0x42));
            if (facing == 8) guard.state = 3;
            else {
                if (facing >= 8) throw std::runtime_error("guard patrol: invalid turn octant");
                guard.facing = facing;
                set_arrow_step(guard);
            }
        } else {
            guard.step_x = guard.step_y = 0;
            guard.state = 3;
            guard.facing = static_cast<std::uint8_t>((guard.facing + 4u) & 7u);
        }
    }
}

void choose_chase(GameState& state, const std::byte* image, GuardRecord& guard,
                  ObjectRecord& obj) {
    // 1010:76fc: a wounded type-1 door-seeker is unreachable in all shipped
    // MAP blocks (no class-0x43 wall); fail explicitly if external data adds it.
    if (guard.strategy == 1 && guard.hit_points < 0x7f)
        throw std::runtime_error("guard strategy 1: unimplemented class-0x43 door-seeking path");
    if (guard.strategy == 0 || guard.strategy == 1) {
        const int ex = (static_cast<int>(state.player.world_x) - obj.world_x) / 32;
        const int ey = (static_cast<int>(state.player.world_y) - obj.world_y) / 32;
        const unsigned choice = random15(state) & (guard.line_of_sight ? 7u : 3u);
        if (choice == 0) {
            if (!ex) guard.step_x = 8;
            if (!ey) guard.step_y = 8;
        } else if (choice == 1) {
            if (!ex) guard.step_x = -8;
            if (!ey) guard.step_y = -8;
        } else {
            guard.step_x = static_cast<std::int8_t>(8 * direction(ex));
            guard.step_y = static_cast<std::int8_t>(8 * direction(ey));
        }
        if (guard.adjacent) guard.timer = 8;
        else if (guard.line_of_sight) {
            guard.timer = static_cast<std::int16_t>(8 + random15(state) % 8u);
            if (state.player.difficulty == 2) guard.timer >>= 1;
            else if (state.player.difficulty == 0) guard.timer <<= 1;
        } else guard.timer = 24;
    } else if (guard.strategy == 2) {
        guard.timer = static_cast<std::int16_t>(8 + random15(state) % 8u);
    } else if (guard.strategy != 3 && guard.strategy != 4) {
        throw std::runtime_error("guard chase: invalid strategy");
    }
    guard.state = 6;
    face_step(guard, guard.step_x, guard.step_y);
    orient(state, image, guard, obj, true);
    step_guard(state, guard, obj);
}

// x86 SAR, including negative base rows before the guard has entered a frame.
int sar(int n, unsigned bits) noexcept {
    const int divisor = 1 << bits;
    return n >= 0 ? n / divisor : -(((-n) + divisor - 1) / divisor);
}

int guard_damage(GameState& state, const ObjectRecord& obj) {
    const auto weapon = state.player.weapon;
    int amount = word((static_cast<int>(obj.last_base_y) - state.scene.viewport_center_y) * 8 +
                      random15(state) % 25u);
    switch (obj.class_id) {
    case 8: case 9: case 0x0a: case 0x0b:
        break;
    case 0x0c: case 0x1d: amount = sar(amount, 3); break;
    case 0x0d: amount = sar(amount, weapon == 1 ? 1 : 3); break;
    case 0x0e: case 0x11: case 0x14: amount = sar(amount, weapon == 2 ? 1 : 3); break;
    case 0x0f: case 0x10: amount = sar(amount, weapon == 1 ? 8 : 1); break;
    case 0x12: case 0x13: amount = sar(amount, 2); break;
    case 0x15: case 0x19: amount = 0; break;
    case 0x16: amount = state.episode == 3 ? 3 : 0; break;
    case 0x17: amount = sar(amount, weapon == 1 ? 8 : 2); break;
    case 0x18: amount = sar(amount, weapon == 1 ? 8 : weapon == 2 ? 4 : 3); break;
    case 0x1a: if (weapon == 1) amount = sar(amount, 1); else amount = 0; break;
    case 0x1b: case 0x1c: amount = sar(amount, 1); break;
    case 0x1e: if (weapon != 1) amount = sar(amount, 3); else amount = 0; break;
    case 0x1f: if (weapon != 1) amount = sar(amount, 2); else amount = 0; break;
    default: if (obj.class_id < 0x20) throw std::runtime_error("guard damage: class outside the traced table");
             break; // 1010:9fa2: classes >=0x20 bypass the resistance table.
    }
    if (state.player.difficulty == 2) amount = sar(amount, 1);
    else if (state.player.difficulty == 0) amount = word(amount * 2);
    else if (state.player.difficulty != 1)
        throw std::runtime_error("guard damage: invalid difficulty");
    return amount > 255 ? 255 : amount;
}

std::uint16_t choose_reaction(GameState& state, const std::byte* image, unsigned bank,
                              const GuardRecord& guard) {
    if (guard.resolved_octant == 0 && (animation(image, bank + 14) >> 8) != 0)
        return animation(image, bank + 14);
    bool any = false;
    for (unsigned i = 0; i < 7; ++i) any |= (animation(image, bank + 2 * i) >> 8) != 0;
    if (!any) throw std::runtime_error("guard reaction: zero-frame pain/death bank would hang (class 0x21 or dancing actor)");
    for (;;) {
        const auto index = random15(state) % 7u;
        const auto frames = animation(image, bank + 2u * index);
        if (frames >> 8) return frames;
    }
}

int kill_score(std::uint8_t class_id) {
    switch (class_id) {
    case 8: case 0x1a: return 25;
    case 9: return 75;
    case 0x0a: case 0x20: return 50;
    case 0x0b: case 0x0f: case 0x10: case 0x17: case 0x1b: case 0x1c: return 100;
    case 0x0d: case 0x12: case 0x13: return 150;
    case 0x0e: case 0x14: case 0x18: case 0x1f: return 200;
    case 0x0c: case 0x1d: case 0x1e: return 250;
    case 0x15: return -1000;
    case 0x16: return 1000;
    case 0x11: case 0x19: return 0;
    default:
        if (class_id >= 0x21) return 0; // 1010:9f10 table ends at class 0x20.
        throw std::runtime_error("guard score: class outside the traced table");
    }
}

int rounded_cell_distance(int dx, int dy) noexcept {
    const int square = dx * dx + dy * dy;
    if (square <= 1) return square;
    int root = 1;
    while ((root + 1) * (root + 1) <= square) ++root;
    return root + (square - root * root >= root - 1);
}

std::uint8_t attack_amount(GameState& state, const ObjectRecord& obj) {
    const auto scale = [&state, &obj] {
        const int dx = static_cast<int>(state.player.cell_x) - obj.cell_x;
        const int dy = static_cast<int>(state.player.cell_y) - obj.cell_y;
        const int dist = rounded_cell_distance(dx, dy);
        return dist ? 100 / dist : 100;
    };
    unsigned amount = 0;
    switch (obj.class_id) {
    case 8: amount = random15(state) & 7u; break;
    case 9: case 0x0a: amount = random15(state) & 15u; break;
    case 0x0b: amount = static_cast<unsigned>(scale() >> 2); break;
    case 0x0c: case 0x1d: case 0x1e: amount = static_cast<unsigned>(scale()); break;
    case 0x19: amount = 100; break;
    case 0x11: case 0x12: case 0x13: case 0x14: amount = random15(state) & 31u; break;
    case 0x16: amount = state.episode == 3 || state.level_latch ? 100u : 0x21u; break;
    case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x15: case 0x17:
    case 0x18: case 0x1a: case 0x1b: case 0x1c: case 0x1f: case 0x20: case 0x21:
        amount = static_cast<unsigned>(scale() >> 1); break;
    default:
        if (obj.class_id >= 0x1f) amount = static_cast<unsigned>(scale() >> 1);
        else throw std::runtime_error("guard attack: class outside the traced table");
        break;
    }
    if (state.player.difficulty == 2) amount *= 2;
    else if (state.player.difficulty == 0) amount /= 2;
    else if (state.player.difficulty != 1)
        throw std::runtime_error("guard attack: invalid difficulty");
    return static_cast<std::uint8_t>(amount);
}

void hit_player(GameState& state, const Assets& assets, GuardRecord& guard,
                const ObjectRecord& obj) {
    const auto amount = attack_amount(state, obj); // consumed even when invulnerable/dead
    if (amount) state.player.damage_feedback = 3;
    if (state.config.cheat_omnipotent || state.player.health == 0) return;
    if (amount >= state.player.health) {
        state.player.health = 0;
        state.player_life_state = 2;
        state.death_turn_pending = true;
        state.killer_object_slot = guard.object_slot;
        guard.state = 11; // 80ea: the killer does not enter another attack cycle.
        emit_effect(state, assets, 0x0a, 2);
    } else {
        state.player.health = static_cast<std::uint8_t>(state.player.health - amount);
    }
}

void show_guard_message(GameState& state, std::string_view text) {
    if (state.screen == Screen::modal_message) {
        if (state.queued_message_count == state.queued_messages.size())
            throw std::runtime_error("guard message: original eight-message queue exhausted");
        state.queued_messages[state.queued_message_count++] = text;
    } else {
        state.message = text;
        state.modal_reason = 4;
        state.modal_return = Screen::gameplay;
        state.screen = Screen::modal_message;
    }
}

void begin_gargoyle_phase(GameState& state, GuardRecord& guard) {
    // 1010:7a06: the octant direction is deliberately snapped to one axis.
    constexpr std::array<std::int8_t, 8> dx{0,8,8,0,0,-8,-8,0};
    constexpr std::array<std::int8_t, 8> dy{-8,0,0,8,8,0,0,-8};
    guard.timer = static_cast<std::int16_t>(random15(state) % 0x50u + 8u);
    guard.state = 19;
    guard.step_x = dx[guard.facing & 7u];
    guard.step_y = dy[guard.facing & 7u];
}

void trigger_gargoyle_wall(GameState& state, const GuardRecord& guard,
                            const ObjectRecord& obj) {
    // 1018:3876 selects a class-seven vector at this cell with the facing side.
    constexpr std::array<std::uint8_t, 8> direction_by_facing{0,2,2,1,1,3,3,0};
    const auto dir = direction_by_facing[guard.facing & 7u];
    for (std::size_t i = 0; i < state.level.vector_count; ++i) {
        auto& v = state.level.vectors[i];
        if (v.class_id != 7 || v.direction != dir) continue;
        const bool contains = dir < 2 ?
            (v.y0 >> 6) - (dir == 1) == obj.cell_y &&
            v.x0 <= obj.world_x && obj.world_x < v.x1 :
            (v.x0 >> 6) - (dir == 2) == obj.cell_x &&
            v.y0 <= obj.world_y && obj.world_y < v.y1;
        if (!contains) continue;
        if (!v.frame_index) v.frame_index = 1; // 1018:392c is one-shot.
        return;
    }
    throw std::runtime_error("gargoyle phase: Vector not in map (1018:3876)");
}

void service_gargoyle_phase(GameState& state, GuardRecord& guard, ObjectRecord& obj) {
    const int old = guard.timer;
    guard.timer = word(old - 1);
    if (!old) { guard.strategy = 0; guard.state = 2; return; }
    if (guard.timer == 8) { trigger_gargoyle_wall(state, guard, obj); return; }
    if (guard.timer >= 8) return;
    const int x = obj.world_x + guard.step_x, y = obj.world_y + guard.step_y;
    const auto new_cell = cell_at(x, y);
    const auto old_cell = cell_at(obj.world_x, obj.world_y);
    if (new_cell != old_cell) {
        // 1010:7a44 tests ANY object byte, not the solid-object flag. The
        // wall is ignored; no sorted-list reorder occurs on this phase move.
        if (state.level.cells[new_cell].object_id ||
            new_cell == static_cast<std::size_t>(state.player.cell_y) * 64 + state.player.cell_x)
            return;
        state.level.cells[new_cell].object_id = state.level.cells[old_cell].object_id;
        state.level.cells[old_cell].object_id = 0;
    }
    obj.world_x = word(x);
    obj.world_y = word(y);
    obj.cell_x = static_cast<std::uint16_t>(new_cell % 64);
    obj.cell_y = static_cast<std::uint16_t>(new_cell / 64);
}

void finish_guard_dance(GameState& state, const Assets& assets) {
    emit_effect(state, assets, 0x45, 2);
    restore_level_music(state); // 1010:c63e: restore this level's tune.
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        auto& g = state.level.guards[i];
        if (g.state != 20) continue;
        auto& o = state.level.objects[g.object_slot];
        const auto image_id = state.radio_original_images[i];
        if (state.level.object_class_by_id[image_id] != o.class_id)
            throw std::runtime_error("guard dance: missing original actor image ID");
        o.image_id = image_id;
        g.strategy = 0;
        g.state = 6;
        g.timer = 1;
        g.next_state = 6;
        g.animation_word = animation(descriptor(episode(state, assets), o), 0x24);
        // 1010:ae56 leaves the dancer's current frame untouched.
    }
}

void finish_death(GameState& state, const Assets& assets, GuardRecord& guard,
                  ObjectRecord& obj) {
    // 1010:a0ee common prefix precedes every class-specific death branch.
    obj.flags |= 1u;
    guard.state = 10;
    if (obj.class_id == 0x11) {
        // Dracula keeps type 2, his death word and orientation flag. The later
        // pistol death sound sees the transformed class 0x14, not class 0x11.
        std::uint8_t bat_image = 0;
        bool found = false;
        for (std::size_t i = 0; i < state.level.object_count; ++i)
            if (state.level.objects[i].class_id == 8) {
                bat_image = state.level.objects[i].image_id;
                found = true;
                break;
            }
        if (!found) throw std::runtime_error("Dracula respawn: No objects of class 8 in level");
        obj.image_id = bat_image;
        obj.class_id = 0x14;
        obj.lift = 0x23;
        guard.state = 8;
        guard.next_state = 2;
        guard.timer = 1;
        guard.hit_points = 0xff;
        state.level.cells[cell_at(obj.world_x, obj.world_y)].object_id = obj.image_id;
        emit_effect(state, assets, 0x22, 2);
        return;
    }
    if (obj.class_id == 0x16) {
        emit_effect(state, assets, 0x12, 2);
        show_guard_message(state, "You've won!  You've won!");
        state.player_life_state = 0; // 46b4; idle enters the ending when 51aa is set.
        state.ending_flag = 1;
    }
    if (obj.class_id == 9 || obj.class_id == 0x0a || obj.class_id == 0x12 ||
        obj.class_id == 0x13 || obj.class_id == 0x1a || obj.class_id == 0x1e ||
        obj.class_id == 0x1f) obj.flags &= static_cast<std::uint8_t>(~1u);
}

void service_one(GameState& state, const Assets& assets, const EpisodeAssets& ep,
                 GuardRecord& guard, ObjectRecord& obj) {
    if (obj.class_id < 8 || obj.class_id > 0x21 || guard.strategy > 4)
        throw std::runtime_error("guard service: class or strategy outside Windows dispatcher");
    const auto* image = (guard.state >= 2 && guard.state <= 8) ||
                        (guard.state >= 12 && guard.state <= 16)
        ? descriptor(ep, obj) : nullptr;
    switch (guard.state) {
    case 0:
        advance_frame(guard, obj);
        guard.timer = word(guard.timer - 1);
        if (guard.timer <= 0) guard.state = guard.next_state;
        break;
    case 1: {
        const int old = guard.timer;
        guard.timer = word(old - 1);
        if (!old) guard.state = 2;
        break;
    }
    case 2:
        guard_sound(state, assets, obj.class_id, GuardSound::alert);
        enter_animation(guard, obj, animation(image, 0x34), 0, 3);
        break;
    case 3:
        if (attack_ready(state, guard, obj))
            enter_animation(guard, obj, animation(image, 0x36), 0, 4);
        else choose_chase(state, image, guard, obj);
        break;
    case 4:
        if (attack_ready(state, guard, obj)) {
            guard_sound(state, assets, obj.class_id, GuardSound::attack);
            hit_player(state, assets, guard, obj);
        }
        if (state.player.health && guard.state != 11)
            enter_animation(guard, obj, animation(image, 0x38), 0, 5);
        break;
    case 5:
        choose_chase(state, image, guard, obj);
        break;
    case 6:
        orient(state, image, guard, obj, false);
        step_guard(state, guard, obj);
        guard.timer = word(guard.timer - 1);
        if (!guard.timer) guard.state = 3;
        break;
    case 7:
        orient(state, image, guard, obj, guard.animation_word == 0);
        if (!state.config.cheat_omnificent && sight(state, guard, obj, false, false)) {
            if (guard.strategy == 3) begin_gargoyle_phase(state, guard);
            else guard.state = 2;
        }
        break;
    case 8:
        patrol_tile(state, guard, obj);
        step_guard(state, guard, obj);
        orient(state, image, guard, obj, guard.animation_word == 0);
        if (guard.next_state == 2 && !state.config.cheat_omnificent &&
            sight(state, guard, obj, false, false)) guard.state = 2;
        break;
    case 9:
        finish_death(state, assets, guard, obj);
        if (state.player.firing_mode == 1)
            guard_sound(state, assets, obj.class_id, GuardSound::death);
        if (obj.flags & 1u) {
            const auto cell = cell_at(obj.world_x, obj.world_y);
            if (state.level.wall_flags[state.level.cells[cell].wall_id] & 8u)
                door_at(state, cell).state = 4;
        }
        break;
    case 10: case 11:
        break;
    case 12: case 13: // 1010:7b56: no setter for either dormant state.
        orient(state, image, guard, obj, false);
        break;
    case 14:
        orient(state, image, guard, obj, false);
        if (state.cannons_enabled) { guard.timer = 0; guard.state = 15; }
        break;
    case 15: {
        orient(state, image, guard, obj, false);
        if (!state.cannons_enabled) { guard.state = 14; break; }
        const int old = guard.timer;
        guard.timer = word(old - 1);
        if (old) break;
        if (state.player.room_id == guard.room_id)
            guard_sound(state, assets, obj.class_id, GuardSound::attack);
        guard.state = 16;
        orient(state, image, guard, obj, true);
        guard.timer = static_cast<std::int16_t>(guard.animation_word >> 8);
        guard.state = 0;
        guard.next_state = 16;
        break;
    }
    case 16: {
        orient(state, image, guard, obj, false);
        const int old = guard.timer;
        guard.timer = word(old - 1);
        if (old) break;
        if (attack_ready(state, guard, obj)) hit_player(state, assets, guard, obj);
        guard.timer = 8;
        guard.state = 15;
        orient(state, image, guard, obj, true);
        break;
    }
    case 17:
        throw std::runtime_error("guard state 17: class-0x43 door-turn unreachable in shipped MAP levels");
    case 18:
        if (static_cast<unsigned>(static_cast<std::uint8_t>(guard.animation_word)) +
            (guard.animation_word >> 8) - 1u > obj.frame_index) ++obj.frame_index;
        obj.lift = static_cast<std::int16_t>(obj.lift > 5 ? obj.lift - 5 : 0);
        if (guard.timer > 0) --guard.timer;
        if (!guard.timer && !obj.lift) guard.state = guard.next_state;
        break;
    case 19:
        service_gargoyle_phase(state, guard, obj);
        break;
    case 20:
        if (guard.timer > 0) {
            --guard.timer;
            if (guard.timer < 0x60) step_guard(state, guard, obj);
        } else finish_guard_dance(state, assets);
        break;
    case 21: {
        const unsigned last = static_cast<std::uint8_t>(guard.animation_word) +
                              (guard.animation_word >> 8) - 1u;
        if (last > obj.frame_index) ++obj.frame_index;
        else guard.state = guard.next_state;
        break;
    }
    default:
        throw std::runtime_error("guard service: state outside 22-entry Windows dispatcher");
    }
}

} // namespace
void start_guard_dance(GameState& state, const Assets& assets) {
    // 1010:ae56(0): class-0x46 floors are the only dancers, independently of
    // their actor classes. Class 0x21 supplies a shared IMG descriptor only.
    bool found = false;
    std::uint8_t dance_image = 0;
    for (std::size_t i = 0; i < state.level.object_count; ++i)
        if (state.level.objects[i].class_id == 0x21) {
            dance_image = state.level.objects[i].image_id;
            found = true;
            break;
        }
    const auto& ep = episode(state, assets);
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        auto& guard = state.level.guards[i];
        auto& obj = state.level.objects[guard.object_slot];
        const auto cell = cell_at(obj.world_x, obj.world_y);
        if (state.level.wall_class_by_id[state.level.cells[cell].wall_id] != 0x46) continue;
        if (!found) throw std::runtime_error("guard dance: No objects of class 0x21 in level");
        state.level.cells[cell].wall_id = 0;
        state.radio_original_images[i] = obj.image_id;
        guard.next_state = obj.image_id;
        obj.image_id = dance_image;
        guard.state = 20;
        guard.timer = 0x70;
        guard.step_x = 3;
        const auto* image = descriptor(ep, obj);
        if (obj.class_id == 9) guard.animation_word = animation(image, 4);
        else if (obj.class_id == 0x0b) guard.animation_word = animation(image, 0x24);
        else if (obj.class_id == 0x0c) guard.animation_word = animation(image, 0x14);
        obj.frame_index = static_cast<std::uint8_t>(guard.animation_word);
    }
}


void initialize_guard_frames(GameState& state, const Assets& assets) {
    const auto& ep = episode(state, assets);
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        auto& guard = state.level.guards[i];
        if (guard.object_slot >= state.level.object_count)
            throw std::runtime_error("guard frame initialization: invalid object slot");
        auto& obj = state.level.objects[guard.object_slot];
        orient(state, descriptor(ep, obj), guard, obj, true);
    }
}

void service_guards(GameState& state, const Assets& assets) {
    const auto& ep = episode(state, assets);
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        auto& guard = state.level.guards[i];
        if (guard.object_slot >= state.level.object_count)
            throw std::runtime_error("guard service: invalid object slot");
        service_one(state, assets, ep, guard, state.level.objects[guard.object_slot]);
    }
}

void wake_room_guards(GameState& state, std::uint8_t room_id) {
    if (!room_id) return; // 1010:7664: room zero does not broadcast noise.
    if (room_id >= state.room_wake.size())
        throw std::runtime_error("guard noise: player room outside 64-entry wake table");
    if (state.room_wake[room_id]) return;
    state.room_wake[room_id] = 1;
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        auto& guard = state.level.guards[i];
        if (!guard.strategy && guard.room_id == room_id &&
            (guard.state == 7 || guard.state == 8)) {
            guard.timer = static_cast<std::int16_t>(random15(state) % 8u);
            guard.state = 1;
        }
    }
}

void damage_guard(GameState& state, const Assets& assets, std::uint16_t guard_index) {
    if (guard_index >= state.level.guard_count)
        throw std::runtime_error("guard damage: invalid guard index");
    auto& guard = state.level.guards[guard_index];
    if (guard.object_slot >= state.level.object_count)
        throw std::runtime_error("guard damage: invalid object slot");
    auto& obj = state.level.objects[guard.object_slot];
    // 1010:a0ee/7e4a moves guarded deaths into state 9/10/11 after the first
    // kill, and later passes can re-encounter the same slot from projectiles or
    // hitscan raycasts. Keep the source-defined dead states and zero-HP paths as
    // terminal without re-awarding a score or re-entering the pain/death banks.
    if (guard.state == 9 || guard.state == 10 || guard.state == 11 || guard.hit_points == 0)
        return;
    // 8308 filters next_state==9 for projectiles; 8b06 hitscan instead filters
    // state 0/9/10. Both callers can reach a zero-frame dancer pain bank.
    const auto amount = static_cast<std::uint8_t>(guard_damage(state, obj));
    if (obj.class_id == 0x15)
        show_guard_message(state, "Watch out you clod! You almost hit me!");
    if (!amount && guard.hit_points > amount) return;
    const auto* image = descriptor(episode(state, assets), obj);
    if (guard.hit_points <= amount) {
        guard.hit_points = 0;
        const auto frames = choose_reaction(state, image, 0x4a, guard);
        if (state.player.firing_mode != 1)
            guard_sound(state, assets, obj.class_id, GuardSound::death);
        enter_animation(guard, obj, frames, obj.lift > 0 ? 18 : 0, 9);
        state.player.score += static_cast<std::uint32_t>(kill_score(obj.class_id));
        state.level.cells[cell_at(obj.world_x, obj.world_y)].object_id = guard.hidden_object_id;
        return;
    }
    if (!amount) return;
    if (guard.state == 7) guard_sound(state, assets, obj.class_id, GuardSound::alert);
    guard.hit_points = static_cast<std::uint8_t>(guard.hit_points - amount);
    guard.resolved_octant = 8; // 80f8 forces the next 6ee0 orientation refresh.
    if (guard.strategy == 4) return; // Cannon only; no pain draw/animation.
    const auto frames = choose_reaction(state, image, 0x3a, guard);
    if (guard.strategy == 2) {
        enter_animation(guard, obj, frames, 0, 8); // 80f8: next=8 disables re-sighting.
        return;
    }
    if (guard.state == 3 || guard.state == 4 || guard.state == 11) return;
    if (guard.state == 7 || guard.state == 8 || guard.state == 21) {
        guard.line_of_sight = sight(state, guard, obj, false, true);
        enter_animation(guard, obj, frames, 0, 5);
    } else {
        guard.animation_word = frames;
        obj.frame_index = static_cast<std::uint8_t>(frames);
        if (guard.state != 0) guard.next_state = guard.state;
        guard.state = 21;
    }
}

} // namespace n3d::detail
