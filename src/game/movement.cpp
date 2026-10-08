#include "internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <utility>
#include <stdexcept>
#include <string_view>

namespace n3d::detail {

namespace {

// 1010:ecac/ecf8, DS:025e/0314. These are truncated 1024-scaled values,
// not host floating-point trig approximations.
constexpr std::array<std::int16_t, 91> kSin{
    0,17,35,53,71,89,107,124,142,160,177,195,212,230,247,265,282,299,316,333,
    350,366,383,400,416,432,448,464,480,496,512,527,542,557,572,587,601,616,
    630,644,658,671,685,698,711,724,736,748,760,772,784,795,806,817,828,838,
    848,858,868,877,886,895,904,912,920,928,935,942,949,955,962,968,973,979,
    984,989,993,997,1001,1005,1008,1011,1014,1016,1018,1020,1021,1022,1023,
    1023,1024
};
constexpr std::array<std::int16_t, 91> kCos{
    1024,1023,1023,1022,1021,1020,1018,1016,1014,1011,1008,1005,1001,997,993,
    989,984,979,973,968,962,955,949,942,935,928,920,912,904,895,886,877,868,
    858,848,838,828,817,806,795,784,772,760,748,736,724,711,698,685,671,658,
    644,630,616,601,587,572,557,542,527,511,496,480,464,448,432,416,400,383,
    366,350,333,316,299,282,265,247,230,212,195,177,160,142,124,107,89,71,
    53,35,17,0
};
constexpr std::array<std::uint8_t, 4> kFireThreshold{2,1,3,1};
// 1048:0204..0221 is one contiguous 30-byte image, not three separately
// bounded authored levels. E1L11/index10 legitimately aliases E2L1/tune13.
constexpr std::array<std::uint8_t, 30> kLevelTunes{
    11,10,8,4,5,6,7,8,12,11,
    13,2,3,4,5,6,7,8,10,11,
    15,2,3,4,5,6,7,8,10,11};
constexpr std::array<std::string_view, 4> kNeedKeys{
    "You need a Red key", "You need a Green key", "You need a Blue key",
    "You need a Yellow key"
};
constexpr std::array<std::string_view, 4> kUseKeys{
    "You use the Red key", "You use the Green key", "You use the Blue key",
    "You use the Yellow key"
};
constexpr std::array<std::string_view, 2> kNeedCards{
    "You need a Red ID card", "You need a Yellow ID card"
};
constexpr std::array<std::string_view, 3> kScrollTexts{
    "The scroll says:\n   \"01532\"",
    "The scroll says:\n   \"080993\"",
    "The scroll says:\n   \"372535\""
};

[[noreturn]] void unsupported(const char* action) {
    throw std::runtime_error(action);
}

void effect(GameState& state, std::uint8_t id, std::uint8_t priority = 0) {
    if (id && state.config.effects_enabled)
        state.audio.push({AudioCommandKind::effect, id, 0, priority});
}

void music(GameState& state, std::uint8_t id) {
    state.current_tune = id;
    if (state.config.music_enabled)
        state.audio.push({AudioCommandKind::music, id, 0, 0});
}

std::size_t cell_at(int world_x, int world_y) {
    if (world_x < 0 || world_x >= 64 * static_cast<int>(kMapWidth) ||
        world_y < 0 || world_y >= 64 * static_cast<int>(kMapHeight))
        unsupported("gameplay position outside MAP grid");
    return static_cast<std::size_t>((world_y >> 6) * 64 + (world_x >> 6));
}

std::size_t neighbor(std::size_t index, int dx, int dy) {
    const int x = static_cast<int>(index % 64) + dx;
    const int y = static_cast<int>(index / 64) + dy;
    return x >= 0 && x < 64 && y >= 0 && y < 64
        ? static_cast<std::size_t>(y * 64 + x) : kMapCellCount;
}

std::size_t forward_cell(const GameState& state) {
    // 1010:1a22, DS:009a; one four-neighbor cell, independent of distance.
    constexpr std::array<int, 8> dx{0,1,1,0,0,-1,-1,0};
    constexpr std::array<int, 8> dy{-1,0,0,1,1,0,0,-1};
    return neighbor(static_cast<std::size_t>(state.player.cell_y) * 64 + state.player.cell_x,
                    dx[state.player.octant], dy[state.player.octant]);
}

void heading(GameState& state, int value) {
    auto& p = state.player;
    value %= 360;
    if (value < 0) value += 360;
    p.heading = static_cast<std::uint16_t>(value);
    p.octant = static_cast<std::uint8_t>(value / 45);
    p.nearest_sector = static_cast<std::uint8_t>(((2 * value / 45 + 1) & 0x0e) >> 1);
    p.octant_mask = static_cast<std::uint16_t>((1u << p.octant) & 0x99u);
    if (value <= 90) {
        p.sine = kSin[value];
        p.cosine = kCos[value];
    } else if (value <= 180) {
        p.sine = kSin[180 - value];
        p.cosine = static_cast<std::int16_t>(-kCos[180 - value]);
    } else if (value <= 270) {
        p.sine = static_cast<std::int16_t>(-kSin[value - 180]);
        p.cosine = static_cast<std::int16_t>(-kCos[value - 180]);
    } else {
        p.sine = static_cast<std::int16_t>(-kSin[360 - value]);
        p.cosine = kCos[360 - value];
    }
    const int s = std::abs(static_cast<int>(p.sine));
    const int c = std::abs(static_cast<int>(p.cosine));
    p.major_axis = static_cast<std::uint16_t>(c < s); // 1 X-major, 0 Y-major
    const int major = p.major_axis ? s : c;
    const int minor = p.major_axis ? c : s;
    p.bresenham_error = static_cast<std::int16_t>(2 * minor - major);
    p.bresenham_minor = static_cast<std::int16_t>(2 * minor);
    p.bresenham_diagonal = static_cast<std::int16_t>(2 * (minor - major));
}

void commit_position(GameState& state, int x, int y) {
    auto& p = state.player;
    const std::size_t old = static_cast<std::size_t>(p.cell_y) * 64 + p.cell_x;
    const auto index = cell_at(x, y);
    state.automap[static_cast<std::size_t>(p.cell_x) * 64 + p.cell_y] = 0;
    // 1010:8a20 -> b1a4(8,0): clear the old COLUMN-major grid byte.
    p.world_x = static_cast<std::int16_t>(x);
    p.world_y = static_cast<std::int16_t>(y);
    p.cell_x = static_cast<std::uint8_t>(index % 64);
    p.cell_y = static_cast<std::uint8_t>(index / 64);
    if (old == index && p.room_id != 0xff) return;
    const auto wall_id = state.level.cells[index].wall_id;
    if (state.level.wall_class_by_id[wall_id] == 0x44) {
        const auto& classes = state.level.wall_class_by_id;
        for (std::size_t id = 0; id < classes.size(); ++id) {
            if (classes[id] == 0x44) {
                p.room_id = static_cast<std::uint8_t>(wall_id - id);
                break;
            }
        }
    }
}

DoorState* door_at(GameState& state, std::size_t cell) {
    for (std::size_t i = 0; i < state.door_count; ++i)
        if (state.doors[i].cell_index == cell) return &state.doors[i];
    return nullptr;
}

bool door_passable(GameState& state, std::size_t cell) {
    const auto* door = door_at(state, cell);
    if (!door) unsupported("MAP door cell has no door record");
    return door->state == 0 || door->state == 4;
}

std::uint8_t subtype_bit(std::uint8_t subtype) {
    if (subtype >= 8) unsupported("MAP pickup subtype exceeds ownership mask");
    return static_cast<std::uint8_t>(1u << subtype);
}

void select_weapon(GameState& state, std::uint8_t weapon) {
    auto& p = state.player;
    if (weapon >= 8 || !(p.weapon_owned & subtype_bit(weapon))) return;
    if (weapon >= kFireThreshold.size()) unsupported("unsupported owned weapon subtype");
    if (p.weapon == 0xff) {
        p.weapon = weapon;
        p.firing_mode = weapon == 2 ? 1 : 2;
        p.weapon_animation_state = 1;
    } else if (p.weapon != weapon && p.weapon_animation_state == 0) {
        p.queued_weapon = weapon;
        p.weapon_animation_state = 2;
    }
}

int mouse_step(std::uint8_t magnitude, int distance, int limit, int scale) {
    // 1010:9268: signed, low-word/low-dword MSVC arithmetic with a quadratic
    // ramp and M*B/50 cap. All distances/denominators are source viewport units.
    const auto b = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(limit));
    const auto numerator = static_cast<std::uint32_t>(magnitude) *
        static_cast<std::uint32_t>(distance) * static_cast<std::uint32_t>(distance) *
        static_cast<std::uint32_t>(b);
    const int divisor = 50 * scale * scale;
    const auto quotient = std::bit_cast<std::int32_t>(numerator) / divisor;
    const auto ramp = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(quotient) + 1u));
    const auto cap_product = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(static_cast<int>(magnitude) * b));
    return std::min<int>(ramp, cap_product / 50);
}
void floor_trigger(GameState& state, std::size_t cell);

void modal(GameState& state, std::string_view message);

// 1010:cf60/a9e0. Weapon pickups SET pools, whereas class-39 ammo ADDS.
void pickup(GameState& state, std::size_t cell) {
    auto& level = state.level;
    auto& p = state.player;
    const auto id = level.cells[cell].object_id;
    const auto cls = level.object_class_by_id[id];
    if (cls < 0x2f || cls > 0x3d) return;
    ObjectRecord* obj = nullptr;
    for (std::size_t i = 0; i < level.object_count; ++i) {
        auto& candidate = level.objects[i];
        if ((candidate.flags & 1) && candidate.image_id == id &&
            static_cast<std::size_t>(candidate.cell_y) * 64 + candidate.cell_x == cell) {
            obj = &candidate;
            break;
        }
    }
    if (!obj) unsupported("pickup MAP cell has no live object record");
    const auto s = obj->subtype;
    bool accepted = true;
    std::uint8_t sound = 0;
    switch (cls) {
    case 0x2f: p.keys |= subtype_bit(s); sound = 0x31; break;
    case 0x30: p.cards |= subtype_bit(s); sound = 0x31; break;
    case 0x31: p.score += 200; break;
    case 0x32:
        accepted = static_cast<std::int8_t>(p.resource_4c22) < 99;
        if (accepted) p.resource_4c22 = static_cast<std::uint8_t>(p.resource_4c22 + subtype_bit(s));
        break;
    case 0x33:
        accepted = p.health < 100;
        if (accepted) p.health = static_cast<std::uint8_t>(p.health + (0x14u >> s));
        sound = 0x2f;
        break;
    case 0x34:
        accepted = p.health < 100;
        if (accepted) {
            p.health = static_cast<std::uint8_t>(p.health + 30u);
            p.score += 250;
        }
        break;
    case 0x35:
        p.health = 100;
        p.ammo[1] = 100;
        p.score += 500;
        ++p.class35_count;
        break;
    case 0x36:
        p.weapon_owned |= subtype_bit(s);
        select_weapon(state, s);
        if (s == 0 || s == 3) p.ammo[1] = 50;
        else if (s == 1) p.ammo[2] = 50;
        else if (s == 2) p.ammo[0] = 50;
        sound = 0x32;
        break;
    case 0x37: p.class37_mask |= subtype_bit(s); break;
    case 0x38:
        accepted = static_cast<std::int8_t>(p.resource_4c21) < 100;
        if (accepted) p.resource_4c21 = static_cast<std::uint8_t>(p.resource_4c21 + 20u);
        break;
    case 0x39: {
        // The legacy add-item subtype order differs from weapon consumption.
        if (s > 2) unsupported("MAP ammo pickup has unsupported subtype");
        const std::size_t pool = s == 0 ? 0u : s == 1 ? 1u : 2u;
        accepted = pool == 2 ? p.ammo[pool] < 100 :
                   static_cast<std::int8_t>(p.ammo[pool]) < 100;
        if (accepted) p.ammo[pool] = static_cast<std::uint8_t>(p.ammo[pool] + 20u);
        sound = 0x34;
        break;
    }
    case 0x3a:
        accepted = p.enemy_meter < 100;
        if (accepted) p.enemy_meter = static_cast<std::uint8_t>(
            std::min(100u, p.enemy_meter + 20u));
        sound = 0x2e;
        break;
    case 0x3b:
        accepted = p.map_meter < 100;
        if (accepted) p.map_meter = static_cast<std::uint8_t>(
            std::min(100u, p.map_meter + 20u));
        sound = 0x33;
        break;
    case 0x3c: p.pentagrams |= subtype_bit(s); sound = 0x31; break;
    case 0x3d:
        if (s >= kScrollTexts.size()) unsupported("MAP scroll subtype has no source text");
        if (state.screen == Screen::modal_message)
            unsupported("simultaneous scrolls require the original queued-message state");
        modal(state, kScrollTexts[s]);
        state.modal_reason = 4; // 1010:c5b6 -> 1018:083e message queue.
        sound = 0x31;
        break;
    default: unsupported("invalid pickup class");
    }
    if (!accepted) return;
    level.cells[cell].object_id = 0;
    obj->flags &= static_cast<std::uint8_t>(~1u);
    effect(state, sound, 1);
}

int probe_step(GameState& state, int x, int y, int sign, bool x_axis, int heading_arg) {
    auto& level = state.level;
    const int xs = heading_arg < 180 ? 1 : -1;
    const int ys = heading_arg > 90 && heading_arg < 270 ? 1 : -1;
    const int forward_x = x + (x_axis ? 28 * sign : 0);
    const int forward_y = y + (x_axis ? 0 : 28 * sign);
    const auto a = cell_at(forward_x + (x_axis ? 0 : -27 * xs),
                           forward_y + (x_axis ? -27 * ys : 0));
    const auto b = cell_at(forward_x + (x_axis ? 0 : 27 * xs),
                           forward_y + (x_axis ? 27 * ys : 0));
    const auto wall_blocked = [&](std::size_t cell) {
        const auto f = level.wall_flags[level.cells[cell].wall_id];
        return (f & 4u) || ((f & 8u) && !door_passable(state, cell));
    };
    // 1010:84f4 tests BOTH walls first, then pickups A/B, then the
    // pre-pickup solid-object flags (a pickup may occur on a blocked step).
    if (wall_blocked(a) || wall_blocked(b)) { effect(state, 1); return 0; }
    const auto af = level.object_flags[level.cells[a].object_id];
    const auto bf = level.object_flags[level.cells[b].object_id];
    if (af & 4u) pickup(state, a);
    if (bf & 4u) pickup(state, b);
    // 1010:84f4 invokes bfd8 for both passable floor-marker probes A/B.
    if (level.wall_flags[level.cells[a].wall_id] & 0x40u) floor_trigger(state, a);
    if (level.wall_flags[level.cells[b].wall_id] & 0x40u) floor_trigger(state, b);
    if ((af | bf) & 2u) return 0;
    return sign;
}

void move(GameState& state, int direction, bool strafe, int units) {
    if (units <= 0) return;
    if (state.player.movement_locked) return; // 1010:8aac, original 4be8.
    auto& p = state.player;
    const int h = (static_cast<int>(p.heading) + direction + 360) % 360;
    const int xs = h < 180 ? 1 : -1;
    const int ys = h > 90 && h < 270 ? 1 : -1;
    const bool x_major = (p.major_axis != 0) != strafe;
    int x = p.world_x, y = p.world_y;
    for (int i = 0; i < units; ++i) {
        if (x_major) x += probe_step(state, x, y, xs, true, h);
        else y += probe_step(state, x, y, ys, false, h);
        if (p.bresenham_error < 0) {
            p.bresenham_error = static_cast<std::int16_t>(p.bresenham_error + p.bresenham_minor);
        } else {
            p.bresenham_error = static_cast<std::int16_t>(p.bresenham_error + p.bresenham_diagonal);
            if (x_major) y += probe_step(state, x, y, ys, false, h);
            else x += probe_step(state, x, y, xs, true, h);
        }
    }
    commit_position(state, x, y);
}

std::pair<std::uint8_t, std::uint8_t> warp_bounds(const LevelState& level,
                                                   std::uint8_t cls) {
    std::uint8_t first = 0xff, last = 0;
    for (std::size_t id = 0; id < level.wall_class_by_id.size(); ++id)
        if (level.wall_class_by_id[id] == cls) { first = static_cast<std::uint8_t>(id); break; }
    if (first == 0xff) unsupported("warp class lacks first wall image ID");
    for (const auto& cell : level.cells)
        if (level.wall_class_by_id[cell.wall_id] == cls)
            last = std::max(last, cell.wall_id);
    return {first, last};
}

void queued_message(GameState& state, std::string_view text) {
    if (state.screen != Screen::modal_message) {
        state.message = text;
        state.modal_reason = 4;
        state.modal_return = Screen::gameplay;
        state.screen = Screen::modal_message;
    } else {
        if (state.queued_message_count >= state.queued_messages.size())
            unsupported("source message queue exceeds eight slots");
        state.queued_messages[state.queued_message_count++] = text;
    }
}

void set_lights(GameState& state, bool fused) {
    if ((state.lights_fused != 0) == fused) return;
    state.lights_fused = static_cast<std::uint8_t>(fused);
    ++state.shade_epoch; // 1010:c5e2 rebuilds the translated shade table.
}

void floor_trigger(GameState& state, std::size_t cell) {
    const auto cls = state.level.wall_class_by_id[state.level.cells[cell].wall_id];
    const auto once = [&](std::string_view text) {
        if (!state.level_latch) {
            state.level_latch = 1;
            queued_message(state, text);
        }
    };
    // Windows 1010:bfd8/bef4..c04c: episode/level gates, no cell-coordinate
    // checks.  Trigger probes can revisit the same wall on successive steps.
    if (state.episode == 1) {
        if (state.level_index == 6 && cls == 0x47 && !state.level_latch) {
            effect(state, 0x42);
            state.level_latch = 1;
            set_lights(state, true);
            queued_message(state, "Oh dear!  The storm seems to have fused the lights!");
        } else if (state.level_index == 8) {
            if (cls == 0x47 && !state.player.jammed) {
                state.player.jammed = 1;
                effect(state, 0x44);
            } else if (cls == 0x48 && state.player.jammed) {
                state.player.jammed = 0;
                effect(state, 0x44);
            }
        } else if (state.level_index == 9) {
            if (cls == 0x47 && !state.level_latch) {
                effect(state, 0x12);
                state.level_latch = 1;
                state.hamerstein_challenge = 1;
                state.player.movement_locked = 1;
                state.config.cheat_omnipotent = 0;
                queued_message(state, "So!  You have discovered me!");
                queued_message(state, "Fool!  Did you really think you could\ndefeat me?  You have no idea of my power!");
                queued_message(state, "Bid farewell my friend! 'Tis the end for you!");
            } else if (cls == 0x48 && !state.penelope_latch) {
                state.penelope_latch = 1;
                queued_message(state, "Look over there!   It's Penelope\nand the evil Dr. Hamerstein!");
            }
        }
    } else if (cls == 0x47 && state.episode == 2 && state.level_index == 9) {
        once("QUICK!  Destroy the plasma core!");
    } else if (cls == 0x47 && state.episode == 3) {
        if (state.level_index == 0)
            once("You recover to witness the defunct\nplasma core's residual radiation\nslowly decaying.  When the plasma core\nimploded into another dimension the\nblast jammed the automatic doors shut\nand scattered your posessions.  There\ndoes not appear to be any way out!");
        else if (state.level_index == 9)
            once("Look!  It's Penelope! Where's Dr. Hamerstein?");
    }
}

void modal(GameState& state, std::string_view message) {
    state.message = message;
    state.modal_reason = 2;
    state.modal_return = Screen::gameplay;
    state.screen = Screen::modal_message;
}
void guard_debug(GameState& state) {
    // 1010:ab66 returns silently on a forward cell without the guard flag.
    const auto cell = forward_cell(state);
    if (cell == kMapCellCount) return;
    const auto id = state.level.cells[cell].object_id;
    if (!(state.level.object_flags[id] & 8u)) return;
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        const auto& guard = state.level.guards[i];
        if (guard.object_slot >= state.level.object_count)
            unsupported("F12 guard slot outside MAP objects");
        const auto& obj = state.level.objects[guard.object_slot];
        if (static_cast<std::size_t>(obj.cell_y) * 64 + obj.cell_x != cell ||
            obj.image_id != id) continue;
        char* cursor = state.guard_debug_text.data();
        char* const end = cursor + state.guard_debug_text.size();
        const auto field = [&](std::string_view label, int value, char separator) {
            if (static_cast<std::size_t>(end - cursor) <= label.size() + 12u)
                unsupported("F12 guard readout exceeds fixed buffer");
            cursor = std::copy(label.begin(), label.end(), cursor);
            const auto result = std::to_chars(cursor, end, value);
            if (result.ec != std::errc{}) unsupported("F12 guard value cannot be formatted");
            cursor = result.ptr;
            if (separator) *cursor++ = separator;
        };
        // Original format string 1048:1810, three lines / eight fields.
        field("class ", obj.class_id, ',');
        field(" strength ", guard.hit_points, ',');
        field(" strategy ", guard.strategy, '\n');
        field("state ", guard.state, ',');
        field(" nextstate ", guard.next_state, ',');
        field(" timer ", guard.timer, '\n');
        field("octant ", guard.facing, ',');
        field(" resoct ", guard.resolved_octant, 0);
        state.message = {state.guard_debug_text.data(),
                         static_cast<std::size_t>(cursor - state.guard_debug_text.data())};
        state.modal_reason = 2;
        state.modal_return = Screen::gameplay;
        state.screen = Screen::modal_message;
        return;
    }
    unsupported("Guard not in map (1010:8308)");
}


// 1010:188a: vecA/vecB are the two faces of ONE door cell. Adjacent door
// records may be copied by a use, but each has its own geometry and A timer.
bool toggle_door(GameState& state, DoorState& door, std::uint8_t octant) {
    if (state.lights_fused) return false; // 1010:188a cannot operate during blackout.
    if (door.state == 4) return false;
    auto& a = state.level.vectors[door.vector_index];
    auto& b = state.level.vectors[door.partner_vector_index];
    bool changed = true;
    if (door.state == 1 || door.state == 3) {
        if ((a.class_id == 0x3d || a.class_id == 0x3e) &&
            (((octant + 1u) & 6u) >> 1u) != a.subtype) changed = false;
        else {
            door.state = 2;
            if (door.sound) effect(state, 0x25);
        }
    } else {
        door.state = 3;
        if (door.sound) effect(state, 0x26);
        door.sound = false;
        a.flags |= 1u;
        b.flags |= 1u;
    }
    if (a.direction != 0 && a.direction != 2)
        unsupported("door first face has unexpected vector direction");
    for (int sign : {-1, 1}) {
        const auto nearby = neighbor(door.cell_index, a.direction == 0 ? sign : 0,
                                    a.direction == 2 ? sign : 0);
        if (nearby == kMapCellCount ||
            !(state.level.wall_flags[state.level.cells[nearby].wall_id] & 8u)) continue;
        auto* other = door_at(state, nearby);
        if (!other) unsupported("adjacent door has no source record");
        other->state = door.state;
        if (door.state == 3) {
            state.level.vectors[other->vector_index].flags |= 1u;
            state.level.vectors[other->partner_vector_index].flags |= 1u;
        }
    }
    return changed;
}

void warp(GameState& state, int delta) {
    if (!delta) return;
    const auto source = forward_cell(state);
    if (source == kMapCellCount) unsupported("warp beyond MAP bounds");
    const auto id = state.level.cells[source].wall_id;
    const auto cls = state.level.wall_class_by_id[id];
    std::size_t base = source;
    if (cls < 0x19 || cls > 0x1c) {
        const auto target = static_cast<std::uint8_t>(id + delta);
        base = kMapCellCount;
        // 1010:2800 scans x first, y second, comparing WALL ID, not class.
        for (std::size_t x = 0; x < 64 && base == kMapCellCount; ++x)
            for (std::size_t y = 0; y < 64; ++y)
                if (state.level.cells[y * 64 + x].wall_id == target) {
                    base = y * 64 + x;
                    break;
                }
        if (base == kMapCellCount) unsupported("paired warp target wall ID absent from MAP");
    }
    constexpr std::array<int, 4> dx{0,1,0,-1};
    constexpr std::array<int, 4> dy{-1,0,1,0};
    constexpr std::array<std::uint16_t, 4> face{0,90,180,270};
    const auto own = static_cast<std::size_t>(state.player.cell_y) * 64 + state.player.cell_x;
    for (std::size_t i = 0; i < dx.size(); ++i) {
        const auto target = neighbor(base, dx[i], dy[i]);
        if (target == kMapCellCount || target == own) continue;
        const auto& cell = state.level.cells[target];
        if ((state.level.wall_flags[cell.wall_id] & 2u) ||
            (state.level.object_flags[cell.object_id] & 2u)) continue;
        commit_position(state, static_cast<int>(target % 64) * 64 + 32,
                        static_cast<int>(target / 64) * 64 + 32);
        heading(state, face[i]);
        return;
    }
    modal(state, "The way appears to be\nblocked by something.");
}

void intermission(GameState& state, std::uint16_t target) {
    // 1018:0ab0/c38c; bonus is applied now, not on dismissal.
    std::uint8_t hostiles = 0;
    for (std::size_t i = 0; i < state.level.guard_count; ++i) {
        const auto& guard = state.level.guards[i];
        const auto cls = state.level.objects[guard.object_slot].class_id;
        if (guard.state != 10 && cls != 0x15 && cls != 0x16 && cls != 0x19 && cls != 0x21)
            ++hostiles;
    }
    std::uint8_t panels = 0;
    for (std::size_t i = 0; i < state.panel_count; ++i)
        if (state.panels[i].state == 1) ++panels;
    const auto bonus = static_cast<std::uint16_t>((hostiles == 0 ? 5000 : 0) +
                                                   (panels == 0 ? 5000 : 0));
    state.intermission_hostiles = hostiles;
    state.intermission_panels = panels;
    state.intermission_bonus = bonus;
    state.player.score += bonus;
    state.pending_episode = state.episode;
    state.pending_level = target;
    music(state, 10);
    state.screen = Screen::intermission;
}

void activate_panel(GameState& state, PanelState& panel) {
    // 1010:1a22/12e8; repeated use during motion re-shifts the faces.
    panel.state = 2;
    effect(state, 0x27, 2);
    for (std::size_t face = 0; face < panel.face_vectors.size(); ++face) {
        const auto index = panel.face_vectors[face];
        if (index == kMaxVectors) continue;
        auto& v = state.level.vectors[index];
        if (face == 0 || face == 2)
            v.y0 = v.y1 = static_cast<std::int16_t>(v.y0 + (face == 0 ? 1 : -1));
        else v.x0 = v.x1 = static_cast<std::int16_t>(v.x0 + (face == 1 ? -1 : 1));
    }
}

ObjectRecord& object_at(GameState& state, std::size_t cell) {
    const auto id = state.level.cells[cell].object_id;
    for (std::size_t i = 0; i < state.level.object_count; ++i) {
        auto& candidate = state.level.objects[i];
        if ((candidate.flags & 1u) && candidate.image_id == id &&
            static_cast<std::size_t>(candidate.cell_y) * 64 + candidate.cell_x == cell)
            return candidate;
    }
    unsupported("usable MAP object has no live source record");
}

void reward(GameState& state, std::uint8_t value) {
    // 1010:abfc: frame 2..5 rewards, 6..9 keys, 10..11 cards.
    auto& p = state.player;
    switch (value) {
    case 2: p.health = 100; p.score += 150; break;
    case 3:
        p.ammo[1] = 100;
        if (p.weapon_owned & 4u) p.ammo[0] = 100;
        if (p.weapon_owned & 2u) p.ammo[2] = 100;
        p.score += 100;
        break;
    case 4: p.map_meter = 100; p.score += 250; break;
    case 5: p.enemy_meter = 100; p.score += 250; break;
    default:
        if (value >= 6 && value <= 9) p.keys |= subtype_bit(value - 6);
        else if (value == 10 || value == 11) p.cards |= subtype_bit(value - 10);
        else unsupported("safe or trunk frame has no source reward");
        effect(state, 0x31, 2);
        return;
    }
    effect(state, 0x30, 2);
}

void use_container(GameState& state, std::size_t cell, std::uint8_t cls) {
    auto& obj = object_at(state, cell);
    if (obj.frame_index == 0) {
        if (cls == 0x26) {
            state.combination_cell = static_cast<std::uint16_t>(cell);
            state.combination_length = 0;
            state.combination_digits.fill(0);
            state.modal_reason = 8;
            state.screen = Screen::combination_prompt; // 1010:ad9e -> 1018:7a2, reason 8.
        } else {
            obj.frame_index = static_cast<std::uint8_t>(obj.subtype + 2u);
            effect(state, 0x32, 2);
        }
        return;
    }
    if (obj.frame_index == 1) { modal(state, "It's empty!"); return; }
    reward(state, static_cast<std::uint8_t>(obj.frame_index + (cls == 0x26 ? 4 : 0)));
    obj.frame_index = 1;
}

void open_floor_popup(GameState& state, std::uint8_t id, std::uint8_t cls) {
    const auto [first, last] = warp_bounds(state.level, cls);
    const auto count = static_cast<unsigned>(last) - first + 1u;
    if (count > 10) unsupported("Bad m_level menu: more than ten floors");
    state.popup_first = first;
    state.popup_count = static_cast<std::uint8_t>(count);
    state.popup_class = cls;
    state.popup_wall_id = id;
    state.popup_row = static_cast<std::uint8_t>(id - first);
    state.screen = Screen::floor_popup; // 1018:1f6a, source row is current ID.
}

void use_control(GameState& state, std::uint8_t id) {
    const auto [first, last] = warp_bounds(state.level, 3);
    static_cast<void>(last);
    const unsigned subtype = static_cast<unsigned>(id - first);
    if (subtype >= 8) unsupported("control panel subtype exceeds ID-card mask");
    if (!(state.player.cards & (1u << subtype))) {
        if (subtype >= kNeedCards.size()) unsupported("panel ID card has no source caption");
        modal(state, kNeedCards[subtype]); // 1018:21d8 -> 1010:bcea.
        return;
    }
    state.popup_class = 3;
    state.popup_wall_id = id;
    state.popup_first = static_cast<std::uint8_t>(subtype);
    state.popup_row = (state.remote_open_mask & (1u << subtype)) ? 1 : 0;
    state.screen = Screen::control_popup;
}

void use_special(GameState& state, std::uint8_t id) {
    // 1010:c0a2: no class-8 behavior outside these episode/level gates.
    if (state.episode != 1) return;
    if (state.level_index == 1) {
        state.chalkboard_image = id;
        state.chalkboard_active = true;
        effect(state, 0x44, 2);
    } else if (state.level_index == 6) {
        if (state.level_latch != 1) {
            modal(state, "You already fixed it!");
        } else {
            state.level_latch = 2;
            set_lights(state, false);
            effect(state, 0x32, 2);
            modal(state, "Well done!  You fixed the power!");
        }
    }
}

void use_portal(GameState& state, std::uint8_t id, std::uint8_t cls) {
    if (cls == 0x16) { modal(state, "The mirror crack'd from side to side!"); return; }
    if (cls != 0x15) return; // Classes 0x17/0x18 have no use arm.
    if (state.player.pentagrams == 0x0f) {
        const auto [mirror, last] = warp_bounds(state.level, 0x16);
        static_cast<void>(last);
        effect(state, 0x44, 2); // 1010:c126: blue flash/restore is presentation-only.
        warp(state, static_cast<int>(mirror) - id);
        return;
    }
    constexpr std::string_view prefix =
        "This is the portal to the\n\"Other Side\".   You cannot pass\nthrough until you have collected\nall 4 pentagrams!  You need to find :-";
    constexpr std::array<std::string_view, 4> names{
        " The Red Pentagram", " The Green Pentagram",
        " The Blue Pentagram", " The Yellow Pentagram"};
    std::size_t length = 0;
    for (char ch : prefix) state.portal_text[length++] = ch;
    for (unsigned n = 0; n < names.size(); ++n)
        if (!(state.player.pentagrams & (1u << n))) {
            state.portal_text[length++] = '\n';
            for (char ch : names[n]) state.portal_text[length++] = ch;
        }
    effect(state, 0x42, 2);
    modal(state, {state.portal_text.data(), length});
}

void use_radio(GameState& state, const Assets& assets) {
    if (state.episode != 1 || state.level_index != 8) return;
    modal(state, "Let's find some dance music!"); // 1010:b010/ae56.
    effect(state, 0x45, 2);
    music(state, 3);
    start_guard_dance(state, assets);
}

bool puzzle_row_enabled(const GameState& state, std::uint8_t row) {
    if (state.screen == Screen::floor_popup) {
        if (row >= state.popup_count) return false;
        if (state.popup_class >= 0x25) return true; // 1018:2076 single "Go down" row.
        return state.config.cheat_omnifarious ||
            !(((state.player.cards ^ state.available_cards) >> row) & 1u);
    }
    if (state.screen != Screen::control_popup) return false;
    const bool open = (state.remote_open_mask & (1u << state.popup_first)) != 0;
    switch (row) {
    case 0: return !open;
    case 1: return open;
    case 2: return !state.cannons_enabled;
    case 3: return state.cannons_enabled != 0;
    case 4: return true; // Cancel.
    default: return false;
    }
}

void choose_control(GameState& state, std::uint8_t row) {
    state.screen = Screen::gameplay;
    if (row == 4) return;
    if (row >= 2) {
        state.cannons_enabled ^= 1u; // 1018:2b4a, shared by all control panels.
        return;
    }
    const auto bit = static_cast<std::uint8_t>(1u << state.popup_first);
    // 1018:2a7c matches both 3b/3c by subtype. Closing ignores occupants,
    // and neither operation runs 188a when the lights are fused.
    for (std::size_t i = 0; i < state.door_count; ++i) {
        auto& door = state.doors[i];
        const auto& v = state.level.vectors[door.vector_index];
        if ((v.class_id != 0x3b && v.class_id != 0x3c) ||
            v.subtype != state.popup_first) continue;
        if ((row == 0 && (door.state == 1 || door.state == 3)) ||
            (row == 1 && (door.state == 0 || door.state == 2))) {
            door.sound = true;
            toggle_door(state, door, 0);
        }
    }
    state.remote_open_mask ^= bit;
}

void submit_combination(GameState& state) {
    const auto cell = state.combination_cell;
    if (cell >= kMapCellCount) unsupported("safe combination cell outside MAP");
    auto& obj = object_at(state, cell);
    if (obj.class_id != 0x26 || obj.frame_index != 0)
        unsupported("safe combination no longer targets unopened safe");
    const auto id = state.level.cells[cell].wall_id;
    const auto [first, last] = warp_bounds(state.level, 0x45);
    static_cast<void>(last);
    constexpr std::array<std::string_view, 4> codes{"333", "01532", "080993", "372535"};
    const auto index = static_cast<unsigned>(id - first);
    if (state.level.wall_class_by_id[id] != 0x45 || index >= codes.size())
        unsupported("safe is not on a valid combination spot");
    state.screen = Screen::gameplay;
    if (std::string_view{state.combination_digits.data(), state.combination_length} != codes[index]) {
        modal(state, "I'm sorry, that is not the correct combination.");
        return;
    }
    obj.frame_index = static_cast<std::uint8_t>(obj.subtype + 2u);
    effect(state, 0x32, 2);
}

void use(GameState& state, const Assets& assets) {
    const auto cell = forward_cell(state);
    if (cell == kMapCellCount) unsupported("forward use outside MAP bounds");
    const auto& tile = state.level.cells[cell];
    const auto cls = state.level.wall_class_by_id[tile.wall_id];
    const auto object_cls = state.level.object_class_by_id[tile.object_id];
    if (state.level.wall_flags[tile.wall_id] & 8u) {
        auto* door = door_at(state, cell);
        if (!door) unsupported("usable door cell has no door record");
        const auto& v = state.level.vectors[door->vector_index];
        const auto s = v.subtype;
        if (v.class_id >= 0x33 && v.class_id <= 0x38 &&
            !(state.player.keys & subtype_bit(s))) return;
        if (v.class_id >= 0x39 && v.class_id <= 0x3a &&
            !(state.player.cards & subtype_bit(s))) {
            if (s >= kNeedCards.size()) unsupported("card door subtype is not red/yellow");
            modal(state, kNeedCards[s]);
            return;
        }
        if (v.class_id == 0x3b || v.class_id == 0x3c) return;
        door->sound = true;
        toggle_door(state, *door, state.player.octant);
        return;
    }
    if (cls == 9 || cls == 10) {
        effect(state, 0x44, 2);
        intermission(state, static_cast<std::uint16_t>(state.level_index + (cls == 9 ? 1 : 2)));
        return;
    }
    if (cls >= 0x0d && cls <= 0x14) {
        const auto id = tile.wall_id;
        const auto [first, last] = warp_bounds(state.level, cls);
        if ((state.warp_row == 0 && id == last) ||
            (state.warp_row == 1 && id == first)) {
            while (state.warp_row < 2 &&
                   ((state.warp_row == 0 && id == last) ||
                    (state.warp_row == 1 && id == first))) ++state.warp_row;
        }
        state.screen = Screen::warp_popup;
        return;
    }
    if (cls >= 0x19 && cls <= 0x1c) {
        const auto key = static_cast<std::uint8_t>(cls - 0x19);
        const bool owned = (state.player.keys & (1u << key)) != 0;
        if (owned) { effect(state, 0x32, 2); warp(state, 1); }
        // 1018:20ce performs the warp before displaying the modal box.
        if (state.screen != Screen::modal_message) modal(state, owned ? kUseKeys[key] : kNeedKeys[key]);
        return;
    }
    if (cls >= 0x15 && cls <= 0x18) { use_portal(state, tile.wall_id, cls); return; }
    if (cls >= 0x1d && cls <= 0x24) { open_floor_popup(state, tile.wall_id, cls); return; }
    if (cls >= 0x25 && cls <= 0x2c) {
        // 1018:2076 shows only a down action for a non-first image ID.
        const auto [first, last] = warp_bounds(state.level, cls);
        static_cast<void>(last);
        if (tile.wall_id != first) {
            state.popup_class = cls;
            state.popup_wall_id = tile.wall_id;
            state.popup_count = 1;
            state.popup_row = 0;
            state.screen = Screen::floor_popup;
        }
        return;
    }
    if (object_cls == 3) {
        for (std::size_t i = 0; i < state.panel_count; ++i)
            if (state.panels[i].cell_index == cell) {
                activate_panel(state, state.panels[i]);
                return;
            }
        unsupported("MAP secret panel has no panel record");
    }
    if (cls == 4) { effect(state, 0x2b, 2); state.player.ammo[1] = 100; return; }
    if (cls == 5 || cls == 6) {
        auto& p = state.player;
        if (!p.resource_4c22) return;
        if (cls == 5 && p.health < 100)
            p.health = static_cast<std::uint8_t>(std::min(100u, p.health + 20u));
        else if (cls == 6 && static_cast<std::int8_t>(p.resource_4c21) < 100)
            p.resource_4c21 = static_cast<std::uint8_t>(p.resource_4c21 + 20u);
        else return;
        --p.resource_4c22;
        effect(state, 0x2c, 2);
        return;
    }
    if (cls == 3) { use_control(state, tile.wall_id); return; }
    if (cls == 8) { use_special(state, tile.wall_id); return; }
    if (state.level.object_flags[tile.object_id] & 8u) {
        const auto& obj = object_at(state, cell);
        if (state.level.guards[obj.enemy_index].state == 12)
            modal(state, "I've nothing left!"); // 1010:ab3e; no static state-12 writer.
        return;
    }
    if (object_cls == 0x26 || object_cls == 0x27) {
        use_container(state, cell, object_cls);
        return;
    }

    if (object_cls == 0x28) {
        activate_pushwall(state, static_cast<std::uint16_t>(cell));
        return;
    }
    if (object_cls == 0x29) use_radio(state, assets);
}

bool line_of_sight(GameState& state, std::size_t from, std::size_t to) {
    int x = static_cast<int>(from % 64), y = static_cast<int>(from / 64);
    const int dx = static_cast<int>(to % 64) - x;
    const int dy = static_cast<int>(to / 64) - y;
    const int ax = std::abs(dx), ay = std::abs(dy);
    const bool xmajor = ax > ay;
    const int major = xmajor ? ax : ay, minor = xmajor ? ay : ax;
    if (major > 16) return false;
    int error = 2 * minor - major;
    for (int i = 0; i < major; ++i) {
        if (xmajor) x += dx < 0 ? -1 : 1;
        else y += dy < 0 ? -1 : 1;
        if (error < 0) error += 2 * minor;
        else {
            error += 2 * (minor - major);
            if (xmajor) y += dy < 0 ? -1 : 1;
            else x += dx < 0 ? -1 : 1;
        }
        const auto index = neighbor(0, x, y);
        if (index == kMapCellCount) return false;
        if (index == to) return true; // 1010:d50a excludes target cell.
        const auto& cell = state.level.cells[index];
        const auto wf = state.level.wall_flags[cell.wall_id];
        const auto of = state.level.object_flags[cell.object_id];
        if ((wf & 4u) || ((wf & 8u) && !door_passable(state, index)) ||
            ((of & 2u) && !(of & 0x20u))) return false;
    }
    return false; // d50a never tests the starting cell, even when dx=dy=0.
}

std::uint8_t first_projectile_image(const LevelState& level) {
    for (std::size_t id = 0; id < level.object_class_by_id.size(); ++id)
        if (level.object_class_by_id[id] == 5) return static_cast<std::uint8_t>(id);
    unsupported("MAP has no class-5 projectile images");
}

void fire(GameState& state, const Assets& assets) {
    auto& p = state.player;
    if (p.weapon == 0xff) return;
    if (p.weapon >= kFireThreshold.size()) unsupported("weapon index outside firing table");
    if (p.jammed) {
        modal(state, "Your weapon appears to be jammed!");
        state.modal_reason = 4; // 1018:2508 -> 083e queue -> 0818(reason 4).
        return;
    }
    ProjectileState* slot = nullptr;
    if (p.firing_mode == 2) {
        for (auto& candidate : state.projectiles)
            if (candidate.state == 0) { slot = &candidate; break; }
        if (!slot) return; // 1010:8b06/9aac: no free slot, no ammo spend.
    } else if (p.firing_mode != 1) {
        unsupported("weapon firing mode is neither projectile nor hitscan");
    }
    const std::size_t pool = p.weapon == 2 ? 0u : p.weapon == 1 ? 2u : 1u;
    if (!state.config.cheat_omnipotent) {
        if (!p.ammo[pool]) return;
        --p.ammo[pool];
    }
    if (slot) {
        *slot = {};
        slot->state = 1;
        slot->lift = 5; // 1010:9aac:9b5b initializes record +0x1a.
        const auto first = first_projectile_image(state.level);
        if (first > 252) unsupported("MAP class-5 projectile sequence exceeds object IDs");
        slot->image_id = static_cast<std::uint8_t>(first + (p.weapon == 1 ? 2 : 0));
        slot->world_x = p.world_x;
        slot->world_y = p.world_y;
        slot->sign_x = static_cast<std::int8_t>(p.heading < 180 ? 1 : -1);
        slot->sign_y = static_cast<std::int8_t>(p.heading > 90 && p.heading < 270 ? 1 : -1);
        slot->major_axis = p.major_axis;
        slot->error = p.bresenham_error;
        slot->minor_increment = p.bresenham_minor;
        slot->diagonal_increment = p.bresenham_diagonal;
        slot->frame_deadline = 0; // 1010:9aac initializes +8 to zero.
    } else {
        // cc7c stamps only same-pass, center-column queued guards.
        const auto from = static_cast<std::size_t>(p.cell_y) * 64 + p.cell_x;
        for (std::size_t i = 0; i < state.level.guard_count; ++i) {
            const auto& g = state.level.guards[i];
            if (!state.scene.guard_seen[i] || g.seen_tick != state.clocks.logical_tick ||
                g.state == 0 || g.state == 9 || g.state == 10) continue;
            const auto& obj = state.level.objects[g.object_slot];
            const auto target = cell_at(obj.world_x, obj.world_y);
            if (line_of_sight(state, from, target)) damage_guard(state, assets, static_cast<std::uint16_t>(i));
        }
    }
    wake_room_guards(state, p.room_id);
    effect(state, p.weapon == 1 ? 0x1a : p.weapon == 2 ? 0x1b : 0x21,
           p.weapon == 2 ? 2 : 0);
    p.muzzle_flash = 1;
}

// 1010:1e00, clock B: two face vectors retract toward the opposite
// endpoint by two world units per pass, without changing the MAP wall byte.
void animate_doors(GameState& state) {
    for (std::size_t i = 0; i < state.door_count; ++i) {
        auto& door = state.doors[i];
        if (door.state != 2 && door.state != 3) continue;
        auto& a = state.level.vectors[door.vector_index];
        auto& b = state.level.vectors[door.partner_vector_index];
        const bool far_end = (a.flags & 0x20u) != 0;
        if (a.direction != 0 && a.direction != 2)
            unsupported("animating door has unexpected vector direction");
        auto& endpoint_a = a.direction == 0 ? (far_end ? a.x1 : a.x0) :
                                              (far_end ? a.y1 : a.y0);
        auto& endpoint_b = a.direction == 0 ? (far_end ? b.x1 : b.x0) :
                                              (far_end ? b.y1 : b.y0);
        const int target = door.state == 2
            ? (a.direction == 0 ? (far_end ? a.x0 : a.x1) : (far_end ? a.y0 : a.y1))
            : static_cast<int>(a.direction == 0 ? door.cell_index % 64 : door.cell_index / 64) * 64 +
              (far_end ? 64 : 0);
        if (endpoint_a != target) {
            const int step = (door.state == 2 ? 2 : -2) * (far_end ? -1 : 1);
            endpoint_a = static_cast<std::int16_t>(endpoint_a + step);
            endpoint_b = static_cast<std::int16_t>(endpoint_b + step);
        }
        if (endpoint_a != target) continue;
        const bool opened = door.state == 2;
        door.state = opened ? 0 : 1;
        door.countdown = 32;
        const auto grid = static_cast<std::size_t>(door.cell_index % 64) * 64 +
                          door.cell_index / 64;
        if (opened) {
            a.flags &= static_cast<std::uint8_t>(~1u);
            b.flags &= static_cast<std::uint8_t>(~1u);
            state.automap[grid] = 0;
        } else state.automap[grid] = state.ega_map[9]; // 1010:b1a4(0), cached 2aac(9).
    }
}

void animate_panels(GameState& state) {
    // 1010:1e00: slide toward each face vector's own end coordinate; a
    // completed panel removes every panel sharing a same-slot face pointer.
    for (std::size_t i = 0; i < state.panel_count; ++i) {
        auto& panel = state.panels[i];
        if (panel.state != 2) continue;
        panel.state = 0;
        for (std::size_t face = 0; face < panel.face_vectors.size(); ++face) {
            const auto index = panel.face_vectors[face];
            if (index == kMaxVectors) continue;
            auto& v = state.level.vectors[index];
            auto& current = (face == 0 || face == 2) ? v.x0 : v.y0;
            const auto target = (face == 0 || face == 2) ? v.x1 : v.y1;
            if (current < target) {
                current = static_cast<std::int16_t>(current + 2);
                panel.state = 2;
            }
        }
        if (panel.state == 2) continue;
        for (std::size_t j = 0; j < state.panel_count; ++j) {
            auto& other = state.panels[j];
            bool shared = false;
            for (std::size_t face = 0; face < panel.face_vectors.size(); ++face)
                if (panel.face_vectors[face] != kMaxVectors &&
                    panel.face_vectors[face] == other.face_vectors[face]) shared = true;
            if (!shared) continue;
            for (const auto index : other.face_vectors)
                if (index != kMaxVectors)
                    state.level.vectors[index].flags &= static_cast<std::uint8_t>(~1u);
            state.level.cells[other.cell_index] = {};
            other.state = 0;
        }
    }
}

} // namespace

// 1018:3736/37a0 normalizes both ends of direction 1/2 runs unless door-flagged.
bool vector_contains(const VectorRecord& v, std::size_t cell) noexcept {
    const int x = static_cast<int>(cell % 64), y = static_cast<int>(cell / 64);
    int x0 = v.x0 >> 6, y0 = v.y0 >> 6;
    int x1 = v.x1 >> 6, y1 = v.y1 >> 6;
    if (!(v.flags & 8u)) {
        if (v.direction == 2) { --x0; --x1; }
        else if (v.direction == 1) { --y0; --y1; }
    }
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

void end_tally(GameState& state) { // 1018:0ab0(10,0), no next level.
    intermission(state, 10);
}

void apply_cheats(GameState& state) {
    // 1010:b128: items are only granted on new-game init, load, and cheat Done.
    // Turning modes back off does not take them away.
    auto& player = state.player;
    if (state.config.cheat_omniscient) {
        player.enemy_meter = 100;
        player.map_meter = 100;
    }
    if (state.config.cheat_omnipotent) {
        player.ammo.fill(100);
        player.health = 100;
        player.weapon_owned = 0x0f;
        if (player.weapon == 0xff) select_weapon(state, 0);
    }
    if (state.config.cheat_omnifarious) {
        player.cards = 3;
        player.keys = 0x0f;
        player.weapon_owned = 0x0f;
        player.pentagrams = 0x0f;
        player.ammo.fill(100);
        player.health = 100;
        player.enemy_meter = player.map_meter = 100;
        player.resource_4c22 = 0x63;
        if (player.weapon == 0xff) select_weapon(state, 0);
    }
}

bool popup_row_enabled(const GameState& state, std::uint8_t row) {
    return puzzle_row_enabled(state, row);
}

void set_heading(GameState& state, int value) {
    heading(state, value); // 1010:e516; also rebuilds persistent Bresenham inputs.
}

void start_level(GameState& state, const Assets& assets, std::uint8_t episode,
                 std::uint16_t level_index) {
    if (episode < 1 || episode > assets.episodes.size())
        unsupported("episode outside loaded asset set");
    const auto& ep = assets.episodes[episode - 1];
    initialize_level(state.level, ep, level_index);
    state.episode = episode;
    state.level_index = level_index;
    state.level_loaded = false;
    state.has_destructible_walls = false;
    for (std::size_t i = 0; i < state.level.vector_count; ++i) {
        const auto cls = state.level.vectors[i].class_id;
        if (cls >= 0x2d && cls <= 0x2f) {
            state.has_destructible_walls = true;
            break;
        }
    }
    // 1010:ef6 resets 51a4..51ab for every MAP start; jam and pentagrams
    // live in the persistent player block and are deliberately not reset.
    state.remote_open_mask = 0;
    state.cannons_enabled = 1;
    state.level_latch = state.penelope_latch = state.hamerstein_challenge = 0;
    state.plasma_destroyed = state.ending_flag = 0;
    state.core_wait_ticks = 0;
    state.core_wait_started_tick = 0;
    set_lights(state, false);
    state.available_cards = 0;
    for (std::size_t i = 0; i < state.level.object_count; ++i) {
        const auto& obj = state.level.objects[i];
        if (obj.class_id == 0x30) state.available_cards |= subtype_bit(obj.subtype);
        if (obj.class_id == 0x26 && obj.subtype >= 4)
            state.available_cards |= subtype_bit(obj.subtype - 4);
    }
    state.queued_message_count = 0;
    state.message = {};
    state.chalkboard_active = false;
    state.radio_original_images.fill(0);
    state.screen = Screen::gameplay;
    state.player_life_state = 1;
    state.death_turn_pending = false;
    state.panel_nearby = false;
    state.hud_message = {}; // 1018:09f2 -> 0e9e clears 46ad and 46ba.
    state.draw_weapon = 0xff;
    state.draw_weapon_frame = 0;
    state.draw_weapon_recoil = 0;
    state.scene = {};
    state.doors.fill({});
    state.panels.fill({});
    state.projectiles.fill({});
    state.automap.fill(0);
    state.automap_origin_x = state.automap_origin_y = 0;
    state.automap_heading_valid = false;
    state.automap_blink = true;
    state.sensor_panel_cleared = false;
    state.automap_speckle_count = 0;
    state.room_wake.fill(0);
    state.door_count = 0;
    state.panel_count = 0;
    state.previous_use = state.previous_fire = state.fire_delay = 0;
    state.player.world_x = static_cast<std::int16_t>(state.level.start.world_x);
    state.player.world_y = static_cast<std::int16_t>(state.level.start.world_y);
    state.player.cell_x = static_cast<std::uint8_t>(state.level.start.cell_x);
    state.player.cell_y = static_cast<std::uint8_t>(state.level.start.cell_y);
    heading(state, state.level.start.heading);
    state.player.room_id = 0xff;
    commit_position(state, state.player.world_x, state.player.world_y);
    if (!state.config.cheat_omnifarious) state.player.keys = state.player.cards = 0;
    // 1010:14a8/16d6: each door cell owns a record and each class-3
    // panel contributes one unvisited tally record. Vectors are already built.
    for (std::size_t cell = 0; cell < kMapCellCount; ++cell) {
        const auto& tile = state.level.cells[cell];
        if (state.level.wall_flags[tile.wall_id] & 8u) {
            if (state.door_count == state.doors.size()) unsupported("MAP exceeds 64 door records");
            auto& door = state.doors[state.door_count++];
            door.cell_index = static_cast<std::uint16_t>(cell);
            door.state = 1;
            const auto x = static_cast<int>(cell % 64), y = static_cast<int>(cell / 64);
            std::size_t first = state.level.vector_count, second = state.level.vector_count;
            for (std::size_t i = 0; i < state.level.vector_count; ++i) {
                const auto& v = state.level.vectors[i];
                if (!(v.flags & 8u) || (v.x0 >> 6) != x || (v.y0 >> 6) != y) continue;
                if (first == state.level.vector_count) first = i;
                else { second = i; break; }
            }
            if (second == state.level.vector_count) unsupported("MAP door cell lacks two face vectors");
            door.vector_index = static_cast<std::uint16_t>(first);
            door.partner_vector_index = static_cast<std::uint16_t>(second);
            const auto& a = state.level.vectors[first];
            if (a.direction != 0 && a.direction != 2)
                unsupported("door first face has unexpected vector direction");
            const auto next = neighbor(cell, a.direction == 0 ? 1 : 0,
                                      a.direction == 2 ? 1 : 0);
            const bool far_end = next != kMapCellCount &&
                (state.level.wall_flags[state.level.cells[next].wall_id] & 8u);
            for (auto index : {first, second}) {
                auto& v = state.level.vectors[index];
                v.flags = static_cast<std::uint8_t>((v.flags & ~0x20u) |
                                                     (far_end ? 0x20u : 0u));
            }
        }
        if (state.level.object_class_by_id[tile.object_id] == 3) {
            if (state.panel_count == state.panels.size()) unsupported("MAP exceeds 32 panel records");
            auto& panel = state.panels[state.panel_count++];
            panel.cell_index = static_cast<std::uint16_t>(cell);
            panel.state = 1;
            panel.face_vectors.fill(static_cast<std::uint16_t>(kMaxVectors));
            const auto wall_class = state.level.wall_class_by_id[tile.wall_id];
            constexpr std::array<std::size_t, 4> slot_for_direction{0,2,1,3};
            // 1018:37a0/3736: last matching vector owns each face.
            for (std::size_t i = 0; i < state.level.vector_count; ++i) {
                const auto& v = state.level.vectors[i];
                if (v.class_id != wall_class || v.direction >= 4 ||
                    !vector_contains(v, cell)) continue;
                panel.face_vectors[slot_for_direction[v.direction]] =
                    static_cast<std::uint16_t>(i);
            }
        }
    }
    initialize_pushwalls(state);
    initialize_guard_frames(state, assets);
    restore_level_music(state);
    state.level_loaded = true;
}

void restore_level_music(GameState& state) {
    // 1010:c63e indexes the flat 30-byte source table for normal play.  E1L11
    // aliases E2L1's byte 0x0d; the demo instead consumes DS:0222's four-byte
    // cycle and increments the process-lifetime cursor at DS:0226.
    if (state.demo) {
        constexpr std::array<std::uint8_t, 4> demo_tunes{13, 8, 15, 1};
        music(state, demo_tunes[state.demo_tune_counter++ & 3u]);
        return;
    }
    if (state.episode < 1 || state.episode > 3)
        unsupported("level has no source tune-table episode");
    const std::size_t offset = (static_cast<std::size_t>(state.episode) - 1u) * 10u +
                               state.level_index;
    if (offset >= kLevelTunes.size())
        unsupported("level tune reads outside the 30-byte source table");
    music(state, kLevelTunes[offset]); // c63e uses 10*episode + index - 10.
}

bool guard_open_door(GameState& state, std::uint16_t cell_index,
                     std::uint8_t facing_octant) {
    if (cell_index >= kMapCellCount) unsupported("guard door cell outside MAP");
    auto* door = door_at(state, cell_index);
    if (!door) unsupported("guard door probe has no door record");
    const auto cls = state.level.vectors[door->vector_index].class_id;
    if (door->state != 1 || (cls >= 0x33 && cls <= 0x3c)) return false;
    return toggle_door(state, *door, facing_octant);
}

void service_doors(GameState& state) {
    // 1010:1d4e, due clock A: one owner record per cell, no B animation here.
    for (std::size_t i = 0; i < state.door_count; ++i) {
        auto& door = state.doors[i];
        const auto cls = state.level.vectors[door.vector_index].class_id;
        if (door.state != 0 || cls == 0x3b || cls == 0x3c) continue;
        if (--door.countdown != 0) continue;
        const auto own = static_cast<std::size_t>(state.player.cell_y) * 64 + state.player.cell_x;
        if (state.level.cells[door.cell_index].object_id || own == door.cell_index) {
            door.countdown = 4;
            continue;
        }
        if (door.sound) effect(state, 0x26);
        door.sound = false;
        door.state = 3;
        state.level.vectors[door.vector_index].flags |= 1u;
        state.level.vectors[door.partner_vector_index].flags |= 1u;
    }
}

void advance_doors_and_panels(GameState& state) {
    // 1010:1e00, after HUD/weapon service and before 9e20 projectiles.
    animate_doors(state);
    animate_panels(state);
}

namespace {


void strike_destructible(GameState& state, std::size_t cell,
                         const EpisodeAssets& episode) {
    auto& level = state.level;
    const auto cls = level.wall_class_by_id[level.cells[cell].wall_id];
    constexpr std::size_t falling_id = 255; // first class-0x2d wall ID in MAP.1/.2/.3.
    if (level.wall_class_by_id[falling_id] != 0x2d)
        unsupported("MAP has no authored class-0x2d wall ID 255");
    const auto& falling = episode.images.sequences[falling_id];
    if (!falling.offset || !falling.frame_count || !falling.delay_ms)
        unsupported("destructible wall class-0x2d has no animation");
    const auto strike = [&](std::uint8_t kind) {
        bool found = false;
        for (std::size_t i = 0; i < level.vector_count; ++i) {
            auto& v = level.vectors[i];
            if (v.class_id != kind || !vector_contains(v, cell)) continue;
            found = true;
            if (kind == 0x2e || v.cached_image_id == falling_id) {
                v.frame_index = 0;
                v.cached_image_id = static_cast<std::uint8_t>(falling_id);
                v.deadline = state.clocks.last_milliseconds + falling.delay_ms;
            } else {
                // 0x2f keeps its own image and deadline even on repeated hits.
                v.frame_index = 1;
            }
            v.class_id = 0x2d;
        }
        return found;
    };
    if (!strike(cls) && !strike(0x2d))
        throw DataError("MAP", episode.map.levels_offset +
                        static_cast<std::size_t>(state.level_index) * 8192u + cell * 2u,
                        "Exploding wall not in map (1018:3118)");
}

bool strike_plasma_core(GameState& state, const Assets& assets) {
    if (state.episode != 2 || state.level_index != 9) return false;
    ++state.plasma_hits; // DS:0202 is process-global, not a per-level flag.
    if (state.plasma_hits % 20u) return true;
    commit_position(state, state.player.world_x,
                    static_cast<int>(state.player.world_y) + 0x2c0);
    constexpr std::size_t falling_id = 255;
    if (state.level.wall_class_by_id[falling_id] != 0x2d ||
        !state.clocks.average_present_ms)
        unsupported("core blast has no authored wall cache or calibrated tick");
    const auto& seq = assets.episodes[1].images.sequences[falling_id];
    if (!seq.offset || !seq.frame_count || !seq.delay_ms)
        unsupported("core blast wall cache has no animation frames");
    state.core_wait_ticks = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(seq.delay_ms) * seq.frame_count) /
        state.clocks.average_present_ms);
    state.core_wait_started_tick = state.clocks.logical_tick;
    return true;
}

} // namespace

std::array<bool, 8> service_projectiles(GameState& state, const Assets& assets) {
    auto& level = state.level;
    const auto now = state.clocks.last_milliseconds;
    std::array<bool, 8> queue{};
    for (std::size_t index = 0; index < state.projectiles.size(); ++index) {
        auto& shot = state.projectiles[index];
        if (shot.state == 0) continue;
        const auto& seq = assets.episodes[state.episode - 1].images.sequences[256u + shot.image_id];
        if (!seq.offset || !seq.frame_count || !seq.delay_ms)
            unsupported("active projectile lacks IMG animation metadata");
        if (shot.state == 2) {
            if (shot.frame_deadline <= now) {
                ++shot.frame;
                shot.frame_deadline = now + seq.delay_ms;
            }
            if (shot.frame >= seq.frame_count) shot = {};
            else queue[index] = true; // 9e20 queues every surviving impact frame.
            continue;
        }
        if (shot.state != 1) unsupported("projectile state outside free/travel/impact");
        if (shot.frame_deadline <= now) {
            if (++shot.frame >= seq.frame_count) shot.frame = 0;
            shot.frame_deadline = now + seq.delay_ms;
        }
        for (std::size_t n = 0; n < state.clocks.projectile_step; ++n) {
            if (shot.major_axis) shot.world_x += shot.sign_x;
            else shot.world_y += shot.sign_y;
            if (shot.error < 0) shot.error = static_cast<std::int16_t>(shot.error + shot.minor_increment);
            else {
                shot.error = static_cast<std::int16_t>(shot.error + shot.diagonal_increment);
                if (shot.major_axis) shot.world_y += shot.sign_y;
                else shot.world_x += shot.sign_x;
            }
            const auto cell = cell_at(shot.world_x, shot.world_y);
            const auto& tile = level.cells[cell];
            const auto wf = level.wall_flags[tile.wall_id];
            const auto of = level.object_flags[tile.object_id];
            bool impact = false, wall_impact = false;
            if ((of & 0x40u) && strike_plasma_core(state, assets)) impact = true;
            else if (wf & 4u) {
                if (wf & 0x10u) {
                    effect(state, 0x29, 2);
                    strike_destructible(state, cell, assets.episodes[state.episode - 1]);
                    impact = true;
                } else impact = wall_impact = true;
            } else if ((wf & 8u) && !door_passable(state, cell)) impact = wall_impact = true;
            else if (of & 8u) {
                bool found = false;
                for (std::size_t i = 0; i < level.guard_count; ++i) {
                    const auto& g = level.guards[i];
                    const auto& obj = level.objects[g.object_slot];
                    if (g.state == 10 || g.next_state == 9 ||
                        static_cast<std::size_t>(obj.cell_y) * 64 + obj.cell_x != cell ||
                        obj.image_id != tile.object_id) continue;
                    found = true;
                    if (std::abs(static_cast<int>(obj.world_x) - shot.world_x) < 10 &&
                        std::abs(static_cast<int>(obj.world_y) - shot.world_y) < 10) {
                        damage_guard(state, assets, static_cast<std::uint16_t>(i));
                        impact = true;
                    }
                    break;
                }
                if (!found) unsupported("MAP guard object has no live guard record");
            } else if ((of & 2u) && !(of & 0x20u)) impact = wall_impact = true;
            if (!impact) continue;
            // 1010:b5c8 emits this impact only for the current gun types 0/3.
            if (wall_impact && (state.player.weapon == 0 || state.player.weapon == 3))
                effect(state, 0x29);
            const auto impact_image = static_cast<std::uint8_t>(shot.image_id + 1u);
            const auto& impact_sequence = assets.episodes[state.episode - 1].images.sequences[
                256u + impact_image];
            if (!impact_sequence.offset || !impact_sequence.frame_count ||
                !impact_sequence.delay_ms)
                unsupported("impact projectile lacks IMG animation metadata");
            shot.state = 2;
            shot.frame = 0;
            shot.image_id = impact_image; // flying 251/253 -> explosion 252/254.
            // A hit preserves the flying sequence deadline; explosion frame 0
            // lasts until that deadline rather than restarting a 50ms timer.
            break;
        }
        if (shot.lift < 20) ++shot.lift; // 1010:9d30:9e00–9e0b, once per flight service.
        const auto delta_x = std::abs(static_cast<int>(shot.world_x) - state.player.world_x);
        const auto delta_y = std::abs(static_cast<int>(shot.world_y) - state.player.world_y);
        queue[index] = delta_x > 20 || delta_y > 20; // 9e92–9eb9 suppresses only the near square.
    }
    if (state.core_wait_ticks &&
        state.core_wait_started_tick != state.clocks.logical_tick &&
        --state.core_wait_ticks == 0) {
        // 1010:c2b8: messages appear after the calibrated B-pass wait.
        queued_message(state, "CONGRATULATIONS!  You destroyed the plasma core!");
        queued_message(state, "Unfortunately the blast impact overwhelms you...  Let's hope you recover in time for the final showdown in episode three!!!");
        state.player_life_state = 3;
        state.plasma_destroyed = 1;
    }
    return queue;
}

void handle_gameplay_input(GameState& state, const Assets& assets,
                           const InputFrame& input) {
    if (state.screen == Screen::modal_message) {
        if ((state.modal_reason == 2 && input.key) || input.key == 0x0d ||
            input.key == 0x1b || input.key == 0x20 || input.menu_right_pressed) {
            if (state.queued_message_count) {
                state.message = state.queued_messages[--state.queued_message_count];
                state.queued_messages[state.queued_message_count] = {};
            } else {
                state.screen = state.modal_return;
                state.message = {};
                state.modal_reason = 0;
            }
        }
        return;
    }
    if (state.screen == Screen::intermission) {
        if (input.key == 0x0d || input.key == 0x1b || input.key == 0x20 ||
            input.menu_right_pressed) {
            if (state.pending_level == 10 &&
                ((state.episode == 1 && state.hamerstein_challenge && !state.player.health) ||
                 (state.episode == 2 && state.plasma_destroyed) ||
                 (state.episode == 3 && state.ending_flag))) {
                state.screen = Screen::root_menu; // reason 10 returns to root, never auto-advances.
                state.menu_row = 0;
            } else start_level(state, assets, state.pending_episode, state.pending_level);
        }
        return;
    }
    if (state.screen == Screen::warp_popup) {
        const auto cell = forward_cell(state);
        if (cell == kMapCellCount) unsupported("popup source moved outside MAP");
        const auto cls = state.level.wall_class_by_id[state.level.cells[cell].wall_id];
        const auto id = state.level.cells[cell].wall_id;
        const auto [first, last] = warp_bounds(state.level, cls);
        const auto enabled = [=](std::uint8_t row) {
            return row == 2 || (row == 0 ? id != last : id != first);
        };
        if (input.key == 0x1b || input.menu_right_pressed) { state.screen = Screen::gameplay; return; }
        if (input.key == 0x26 || input.key == 0x28) {
            int row = static_cast<int>(state.warp_row);
            const int direction = input.key == 0x26 ? -1 : 1;
            do { row += direction; } while (row >= 0 && row <= 2 && !enabled(static_cast<std::uint8_t>(row)));
            if (row >= 0 && row <= 2) state.warp_row = static_cast<std::uint8_t>(row);
            return;
        }
        const auto hovered = [&]() -> std::uint8_t {
            static constexpr std::array<std::string_view, 3> names{"Climb up", "Climb down", "Cancel"};
            const auto& font = assets.uif.fonts[2];
            for (std::uint8_t row = 0; row < 3; ++row) {
                int width = 0;
                for (char c : names[row])
                    width += font.glyphs[static_cast<unsigned char>(c) - 1u].width + 1;
                if (input.cursor_x >= 110 && input.cursor_x <= 110 + width &&
                    input.cursor_y >= 50 + 10 * row && input.cursor_y <= 58 + 10 * row)
                    return row;
            }
            return 3;
        };
        const std::uint8_t mouse_row = (input.cursor_moved || input.menu_left_released)
            ? hovered() : std::uint8_t{3};
        if (input.cursor_moved && mouse_row < 3 && enabled(mouse_row)) state.warp_row = mouse_row;
        if (input.key == 0x0d || input.key == 0x20 ||
            (input.menu_left_released && mouse_row == state.warp_row)) {
            if (!enabled(state.warp_row)) return;
            state.screen = Screen::gameplay;
            if (state.warp_row != 2) warp(state, state.warp_row == 0 ? 1 : -1);
        }
        return;
    }
    if (state.screen == Screen::combination_prompt) {
        // 1018:cfe: the shipped combinations are decimal, at most six digits;
        // Enter submits reason 8, Esc closes the editor without changing safe.
        if (input.key == 0x1b || input.menu_right_pressed) {
            state.screen = Screen::gameplay;
        } else if (input.key == 0x0d) {
            submit_combination(state);
        } else if (input.key == 0x08 && state.combination_length) {
            state.combination_digits[--state.combination_length] = 0;
        } else if (input.key >= '0' && input.key <= '9' &&
                   state.combination_length < 6) {
            state.combination_digits[state.combination_length++] = static_cast<char>(input.key);
        }
        return;
    }
    if (state.screen == Screen::floor_popup || state.screen == Screen::control_popup) {
        const auto count = state.screen == Screen::control_popup ? 5u : state.popup_count;
        if (!count || state.popup_row >= count) unsupported("popup cursor exceeds source rows");
        if (input.key == 0x1b || input.menu_right_pressed) {
            state.screen = Screen::gameplay;
            return;
        }
        if (input.key == 0x26 || input.key == 0x28) {
            const int direction = input.key == 0x26 ? -1 : 1;
            for (int row = static_cast<int>(state.popup_row) + direction;
                 row >= 0 && row < static_cast<int>(count); row += direction)
                if (puzzle_row_enabled(state, static_cast<std::uint8_t>(row))) {
                    state.popup_row = static_cast<std::uint8_t>(row);
                    break;
                }
            return;
        }
        bool click_on_row = false;
        if (input.cursor_moved || input.menu_left_released) {
            constexpr std::array<std::string_view, 5> controls{
                "Open remote doors", "Close remote doors", "Enable remote cannons",
                "Disable remote cannons", "Cancel"};
            for (std::uint8_t row = 0; row < count; ++row) {
                char floor_label[] = "Floor 10";
                floor_label[6] = row == 9 ? '1' : static_cast<char>('1' + row);
                const auto label = state.screen == Screen::control_popup ? controls[row] :
                    state.popup_class >= 0x25 ? std::string_view{"Go down"} :
                    std::string_view{floor_label, row == 9 ? 8u : 7u};
                const auto width = static_cast<int>(indexed_text_width(assets, 2, label));
                if (input.cursor_x < 110 || input.cursor_x > 110 + width ||
                    input.cursor_y < 50 + 10 * row || input.cursor_y > 58 + 10 * row)
                    continue;
                if (puzzle_row_enabled(state, row)) {
                    click_on_row = true;
                    state.popup_row = row;
                    break;
                }
            }
        }
        if (input.key == 0x0d || input.key == 0x20 ||
            (input.menu_left_released && click_on_row)) {
            if (!puzzle_row_enabled(state, state.popup_row)) return;
            if (state.screen == Screen::control_popup) {
                choose_control(state, state.popup_row);
            } else {
                state.screen = Screen::gameplay;
                const int delta = state.popup_class >= 0x25 ? -1 :
                    static_cast<int>(state.popup_first + state.popup_row) - state.popup_wall_id;
                warp(state, delta);
            }
        }
        return;
    }
    if (state.screen != Screen::gameplay || state.player_life_state != 1) return;
    if (input.key >= '1' && input.key <= '9') select_weapon(state, static_cast<std::uint8_t>(input.key - '1'));
    if (input.key == 0x1b) {
        state.menu_row = 5;
        state.screen = Screen::root_menu;
        return;
    }
    if ((input.held & 0x40u) && input.key == 0x6b)
        ++state.player.health; // 1010:988c, byte wrap, no clamp.
    else if ((input.held & 0x40u) && input.key == 0x6d)
        --state.player.health; // 1010:98a0, byte wrap, no clamp.
    if (input.key == 0x78 && state.player.map_meter != 0) { // F9, 1010:bc6a.
        state.player.map_sensor ^= 1u;
        if (!state.player.map_sensor) state.sensor_panel_cleared = true; // b1a4(3).
    } else if (input.key == 0x79 && state.player.enemy_meter != 0) { // F10, 1010:bc90.
        state.player.enemy_sensor ^= 1u;
        if (!state.player.enemy_sensor) state.sensor_panel_cleared = true; // b1a4(3).
    }
    if (input.key == 0x7b) guard_debug(state); // F12, 1010:ab66.
    if (input.key == 0x09) {
        // Tab (0x09) is unrelated to the F9/F10 gameplay sensor toggles.
        return;
    }
    if (input.key == 0x73 || input.key == 0x74) {
        // F4/F5 are intercepted in pump_gameplay after source-order scene/HUD
        // composition, before this input handler mutates world or player state.
        return;
    }
    // F8 (0x77) enters the original bare-retf 1010:c868 stub.
    // 1010:9806: devices, Up, Down, Left, Right, fire, use in this order.
    const int m = (input.held & 0x40u) ? 1 :
                  static_cast<int>(state.clocks.move_step) * ((input.held & 0x20u) ? 2 : 1);
    const int t = (input.held & 0x40u) ? 1 :
                  static_cast<int>(state.clocks.turn_step) * ((input.held & 0x20u) ? 2 : 1);
    bool mouse_inside = false;
    if (state.config.mouse_enabled) {
        if (input.mouse_relative) {
            // --modern-controls is an explicit non-original raw-delta mode.
            const int forward = std::clamp(static_cast<int>(input.mouse_forward), -2 * m, 2 * m);
            const int turn = std::clamp(static_cast<int>(input.mouse_turn), -4 * t, 4 * t);
            if (forward > 0) move(state, 0, false, forward);
            else if (forward < 0) move(state, 180, false, -forward);
            if (turn) heading(state, static_cast<int>(state.player.heading) + turn);
        } else {
            // 1010:5208/9392: absolute 320x200 cursor and viewport geometry.
            const int width = state.config.viewport_width;
            const int left = (320 - width) / 2;
            const int top = left / 2;
            const int centre_x = left + width / 2;
            const int centre_y = top + width / 4;
            const int dead_x = width / 8;
            const int dead_y = (width / 2) / 8;
            const int x = input.cursor_x, y = input.cursor_y;
            mouse_inside = input.cursor_valid && x >= 0 && x < 320 && y >= 0 &&
                           y <= top + width / 2 - 1;
            if (mouse_inside) {
                const int dy = y - centre_y;
                if (dy < -dead_y) {
                    const int amount = mouse_step(state.config.mouse_sensitivity,
                        -dy - dead_y, 2 * m, centre_y - 2 * dead_y);
                    if (amount > 0) move(state, 0, false, amount);
                } else if (dy > dead_y) {
                    const int amount = mouse_step(state.config.mouse_sensitivity,
                        dy - dead_y, 2 * m, centre_y - 2 * dead_y);
                    if (amount > 0) move(state, 180, false, amount);
                }
                const int dx = x - centre_x;
                if (dx < -dead_x || dx > dead_x) {
                    const bool strafe = (input.held & (0x100u | 0x200u)) != 0 ||
                                        input.mouse_right_held;
                    const int amount = mouse_step(state.config.mouse_sensitivity,
                        (dx < 0 ? -dx : dx) - dead_x, strafe ? 2 * m : 2 * t,
                        centre_x - 2 * dead_x);
                    if (amount > 0) {
                        if (strafe) move(state, dx < 0 ? 270 : 90, true, amount);
                        else heading(state, static_cast<int>(state.player.heading) +
                                                 (dx < 0 ? -amount : amount));
                    }
                }
            }
        }
    }
    if (state.config.joystick_enabled) {
        // 1010:96f6: forward and button-2 strafe cap at 2*m; turn at 4*t.
        // InputFrame keeps the calibrated, dead-zoned signed axis magnitudes.
        const int forward = std::clamp(static_cast<int>(input.joystick_forward), -2 * m, 2 * m);
        if (forward > 0) move(state, 0, false, forward);
        else if (forward < 0) move(state, 180, false, -forward);
        if (input.joystick_use) {
            const int strafe = std::clamp(static_cast<int>(input.joystick_strafe), -2 * m, 2 * m);
            if (strafe > 0) move(state, 90, true, strafe);
            else if (strafe < 0) move(state, 270, true, -strafe);
        } else {
            const int turn = std::clamp(static_cast<int>(input.joystick_turn), -4 * t, 4 * t);
            if (turn) heading(state, static_cast<int>(state.player.heading) + turn);
        }
    }
    if (input.held & 0x02u) move(state, 0, false, m);
    if (input.held & 0x04u) move(state, 180, false, m);
    if (input.held & 0x08u) {
        if (input.held & 0x100u) move(state, 270, true, m);
        else heading(state, static_cast<int>(state.player.heading) - t);
    }
    if (input.held & 0x10u) {
        if (input.held & 0x100u) move(state, 90, true, m);
        else heading(state, static_cast<int>(state.player.heading) + t);
    }
    const bool firing = (input.held & 0x80u) ||
                        (state.config.mouse_enabled &&
                         (input.mouse_relative ? input.mouse_left_inside :
                                                 input.mouse_left_held && mouse_inside)) ||
                        (state.config.joystick_enabled && input.joystick_fire);
    const bool edge = firing && !state.previous_fire;
    state.previous_fire = static_cast<std::uint8_t>(firing);
    if (firing && state.player.weapon < kFireThreshold.size() &&
        state.fire_delay >= kFireThreshold[state.player.weapon] &&
        (state.player.weapon == 3 || edge)) {
        state.fire_delay = 0;
        fire(state, assets);
    }
    const bool using_now = (input.held & 0x200u) ||
                           (state.config.mouse_enabled && input.mouse_right_held) ||
                           (state.config.joystick_enabled && input.joystick_use);
    const bool use_edge = using_now && !state.previous_use;
    state.previous_use = static_cast<std::uint8_t>(using_now);
    if (use_edge && state.screen == Screen::gameplay) use(state, assets);
}

} // namespace n3d::detail
