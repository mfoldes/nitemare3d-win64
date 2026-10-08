#include "n3d/level.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace n3d {
namespace {

constexpr std::size_t kMapLevelSize = 8192;

std::uint8_t wall_flags(std::uint8_t c) {
    std::uint8_t f = 0;
    if (c >= 1 && c <= 0x30) f |= 0x04;
    if (c >= 0x31 && c <= 0x40) f |= 0x08;
    if (c >= 1 && c <= 0x40) f |= 0x02;
    if (c == 0x2e || c == 0x2f) f |= 0x10;
    if (c == 0x47 || c == 0x48) f |= 0x40;
    if (f & 0x0c) f |= 0x01;
    return f;
}

std::uint8_t object_flags(std::uint8_t c) {
    std::uint8_t f = 0;
    if (c >= 6 && c <= 0x3d) f |= 0x01;
    if (c >= 8 && c <= 0x2d) f |= 0x02;
    if (c >= 0x2f && c <= 0x3d) f |= 0x04;
    if (c >= 8 && c <= 0x25) f |= 0x08;
    if (c == 0x2a) f |= 0x20;
    if (c == 4) f |= 0x40;
    return f;
}

bool thin(std::uint8_t direction, std::uint8_t c) {
    return c >= 0x31 && c <= 0x40 && ((c & 1u) != 0u) == (direction >= 2u);
}

std::uint8_t first_id(const std::array<std::uint16_t, 256>& by_class,
                      std::uint8_t class_id, std::string_view path, std::size_t offset) {
    const auto value = by_class[class_id];
    if (value == 256u) {
        throw DataError(std::string(path), offset, "referenced class has a source image ID");
    }
    return static_cast<std::uint8_t>(value);
}

// 1010:2334 scans source wall IDs from the supplied neighbor, then wraps.
std::uint8_t class_id_from(const std::array<std::uint8_t, 256>& classes,
                           std::uint8_t class_id, std::uint8_t start_wall_id,
                           std::string_view path, std::size_t offset) {
    for (std::size_t step = 0; step < classes.size(); ++step) {
        const auto id = static_cast<std::uint8_t>(start_wall_id + step);
        if (classes[id] == class_id) return id;
    }
    throw DataError(std::string(path), offset, "neighbor wall class has a source image ID");
}

// 1008:6d4e, with each original four-byte far pointer represented by its
// record index. Preserve its byte-offset partition bookkeeping and tie swaps.
template <typename Key>
void msc_sort(std::uint16_t* values, std::size_t count, std::uint16_t base, Key key) {
    if (count < 2) return;
    for (std::size_t i = 1; i < count; ++i) {
        if (key(values[i]) < key(values[i - 1])) goto unsorted;
    }
    return;
unsorted:
    struct Range { std::uint16_t lo, hi; };
    std::array<Range, 64> stack{};
    std::size_t depth = 1;
    stack[0] = {base, static_cast<std::uint16_t>(base + (count - 1) * 4)};
    const auto at = [base, values](std::uint16_t p) -> std::uint16_t& {
        return values[(p - base) / 4];
    };
    while (depth) {
        const auto [lo, hi] = stack[depth - 1];
        if (lo >= hi) { --depth; continue; }
        std::uint16_t jm = hi, mk = lo;
        std::uint16_t limit = static_cast<std::uint16_t>(hi + 4), scan = lo;
        for (;;) {
            do {
                scan = static_cast<std::uint16_t>(scan + 4);
                if (scan == hi) break;
                const auto a = key(at(scan)), pivot = key(at(lo));
                if (a > pivot) break;
                if (a < pivot) mk = scan;
            } while (true);
            const auto left = scan;
            auto right = limit;
            do {
                right = static_cast<std::uint16_t>(right - 4);
                const auto pivot = key(at(lo)), b = key(at(right));
                if (pivot > b) break;
                if (pivot < b) jm = right;
                if (right == lo) break;
            } while (true);
            if (right > left) {
                std::swap(at(left), at(right));
                mk = left;
                jm = right;
                limit = right;
                scan = left;
                continue;
            }
            std::swap(at(lo), at(right));
            break;
        }
        if (static_cast<std::uint16_t>(hi - jm + lo) < mk) {
            stack[depth - 1] = {lo, mk};
            stack[depth++] = {jm, hi};
        } else {
            stack[depth - 1] = {jm, hi};
            stack[depth++] = {lo, mk};
        }
    }
}

std::size_t neighbor(std::size_t row, std::size_t col, std::uint8_t dir) {
    if (dir == 0) return row ? (row - 1) * 64 + col : kMapCellCount;
    if (dir == 1) return row < 63 ? (row + 1) * 64 + col : kMapCellCount;
    if (dir == 2) return col < 63 ? row * 64 + col + 1 : kMapCellCount;
    return col ? row * 64 + col - 1 : kMapCellCount;
}

} // namespace

void initialize_level(LevelState& state, const EpisodeAssets& assets,
                      std::uint16_t level_index, std::string_view map_path) {
    const auto& map = assets.map;
    if (level_index >= map.level_count)
        throw DataError(std::string(map_path), 0, "level index is within the MAP level count");
    if (map.level_size != kMapLevelSize || map.levels_offset > assets.map_bytes.size() ||
        level_index > (assets.map_bytes.size() - map.levels_offset) / kMapLevelSize ||
        kMapLevelSize > assets.map_bytes.size() - map.levels_offset -
                            static_cast<std::size_t>(level_index) * kMapLevelSize)
        throw DataError(std::string(map_path), map.levels_offset, "complete 8192-byte level block exists");
    const std::size_t base = map.levels_offset + static_cast<std::size_t>(level_index) * kMapLevelSize;
    state = {};
    state.level_index = level_index;
    state.wall_class_by_id = map.wall_class_by_id;
    state.object_class_by_id = map.object_class_by_id;
    std::array<std::uint16_t, 256> first_wall{};
    std::array<std::uint16_t, 256> first_object{};
    first_wall.fill(256u);
    first_object.fill(256u);
    for (std::size_t id = 0; id < 256; ++id) {
        const auto wall_class = map.wall_class_by_id[id];
        const auto object_class = map.object_class_by_id[id];
        state.wall_flags[id] = wall_flags(wall_class);
        state.object_flags[id] = object_flags(object_class);
        if (first_wall[wall_class] == 256u) first_wall[wall_class] = static_cast<std::uint16_t>(id);
        if (first_object[object_class] == 256u) first_object[object_class] = static_cast<std::uint16_t>(id);
    }
    for (std::size_t i = 0; i < kMapCellCount; ++i) {
        state.cells[i].wall_id = std::to_integer<std::uint8_t>(assets.map_bytes[base + 2 * i]);
        state.cells[i].object_id = std::to_integer<std::uint8_t>(assets.map_bytes[base + 2 * i + 1]);
    }
    std::size_t starts = 0;
    for (std::size_t i = 0; i < kMapCellCount; ++i) {
        const auto id = state.cells[i].object_id;
        const auto c = state.object_class_by_id[id];
        if (c == 2) {
            if (++starts != 1) throw DataError(std::string(map_path), base + 2 * i + 1, "unique player start cell");
            state.start.cell_x = static_cast<std::uint16_t>(i % 64);
            state.start.cell_y = static_cast<std::uint16_t>(i / 64);
            state.start.world_x = static_cast<std::int32_t>(state.start.cell_x * 64 + 32);
            state.start.world_y = static_cast<std::int32_t>(state.start.cell_y * 64 + 32);
            state.start.heading = static_cast<std::uint16_t>(
                static_cast<std::uint8_t>(id - first_id(first_object, 2, map_path, base + 2 * i + 1)) * 90u);
        }
        if (!(state.object_flags[id] & 1)) continue;
        if (state.object_count == kMaxObjects)
            throw DataError(std::string(map_path), base + 2 * i + 1, "object capacity of 350");
        const auto slot = state.object_count++;
        auto& obj = state.objects[slot];
        obj.image_id = id;
        obj.subtype = static_cast<std::uint8_t>(id - first_id(first_object, c, map_path, base + 2 * i + 1));
        obj.flags = state.object_flags[id];
        obj.class_id = c;
        obj.cell_x = static_cast<std::uint16_t>(i % 64);
        obj.cell_y = static_cast<std::uint16_t>(i / 64);
        obj.world_x = static_cast<std::int16_t>(obj.cell_x * 64 + 32);
        obj.world_y = static_cast<std::int16_t>(obj.cell_y * 64 + 32);
        const auto& seq = assets.images.sequences[256u + id];
        if (!seq.offset || seq.first_record_index >= assets.images.records.size())
            throw DataError("IMG", seq.descriptor_offset, "object image has a decoded first frame");
        const auto height = assets.images.records[seq.first_record_index].height;
        if (c == 0x19 || c == 0x2e) obj.lift = static_cast<std::int16_t>(64 - height);
        else if (c == 0x3b) obj.lift = static_cast<std::int16_t>((64 - height) >> 1);
        if (!(obj.flags & 8)) continue;
        if (state.guard_count == kMaxGuards)
            throw DataError(std::string(map_path), base + 2 * i + 1, "guard capacity of 100");
        auto& guard = state.guards[state.guard_count];
        obj.enemy_index = static_cast<std::uint8_t>(state.guard_count++); // object +7
        guard.object_slot = static_cast<std::uint16_t>(slot);
        guard.hit_points = 0xff;
        // 847e leaves non-floor room and several other bytes unwritten. Use
        // deterministic fresh-level zero, not guessed reused-segment parity.
        if (seq.descriptor_offset > assets.image_bytes.size() ||
            assets.image_bytes.size() - seq.descriptor_offset < 10)
            throw DataError("IMG", seq.descriptor_offset, "orientation words are available");
        guard.orientation_aware =
            assets.image_bytes[seq.descriptor_offset + 6] != assets.image_bytes[seq.descriptor_offset + 8] ||
            assets.image_bytes[seq.descriptor_offset + 7] != assets.image_bytes[seq.descriptor_offset + 9];
        const auto floor = state.wall_class_by_id[state.cells[i].wall_id];
        if (floor == 0x44)
            guard.room_id = static_cast<std::uint8_t>(state.cells[i].wall_id -
                first_id(first_wall, 0x44, map_path, base + 2 * i));
        guard.facing = static_cast<std::uint8_t>((obj.subtype & 3u) * 2u);
        constexpr std::array<std::int8_t, 8> dx{0, 0, 0, 0, 0, 1, 0, -1};
        constexpr std::array<std::int8_t, 8> dy{0, 0, 0, 0, -1, 0, 1, 0};
        if (obj.subtype >= dx.size())
            throw DataError(std::string(map_path), base + 2 * i + 1, "guard subtype is within direction table");
        guard.step_x = static_cast<std::int8_t>(dx[obj.subtype] * 8);
        guard.step_y = static_cast<std::int8_t>(dy[obj.subtype] * 8);
        guard.state = 7;
        guard.next_state = 2;
        guard.attack_kind = 1;
        if (c == 8 || c == 9 || c == 0x0a || c == 0x11 || c == 0x14 || c == 0x1a)
            guard.attack_kind = 0;
        if (c == 0x12 || c == 0x13) { guard.attack_kind = 0; guard.strategy = 3; }
        if (c == 0x15 || c == 0x16) guard.next_state = 0;
        if (c == 0x21) { guard.state = 0; guard.next_state = 0; }
        if (c == 0x19) { guard.strategy = 4; guard.state = 14; }
        if (guard.step_x || guard.step_y) guard.state = 8;
        if (floor == 0x42 || floor == 0x46) guard.strategy = 2;
        if (floor == 0x43) guard.strategy = 1;
    }
    if (!starts) throw DataError(std::string(map_path), base, "unique player start cell");

    for (std::uint8_t dir = 0; dir < 4; ++dir) {
        std::uint8_t prev_id = 0, prev_neighbor = 0;
        bool prev_panel = false, prev_special = false, emitted = false;
        for (std::size_t outer = 0; outer < 64; ++outer) {
            for (std::size_t inner = 0; inner < 64; ++inner) {
                const std::size_t row = dir < 2 ? outer : inner;
                const std::size_t col = dir < 2 ? inner : outer;
                const std::size_t i = row * 64 + col;
                const std::size_t ni = neighbor(row, col, dir);
                if (ni == kMapCellCount) continue; // original does not reset its run state
                const auto id = state.cells[i].wall_id;
                const auto nb = state.cells[ni].wall_id;
                const auto c = state.wall_class_by_id[id];
                const auto nc = state.wall_class_by_id[nb];
                const bool panel = state.object_class_by_id[state.cells[i].object_id] == 3;
                const bool adjacent_panel = !(panel && nb == id) &&
                    state.object_class_by_id[state.cells[ni].object_id] == 3;
                const bool special = (state.wall_flags[id] & 0x10) != 0;
                const bool adjacent_special = !(special && nb == id) && (state.wall_flags[nb] & 0x10);
                const bool door_change = ((state.wall_flags[prev_neighbor] ^ state.wall_flags[nb]) & 8) != 0;
                const bool solid = (state.wall_flags[id] & 4) != 0;
                bool create = false, is_thin = false;
                if (solid) {
                    if ((state.wall_flags[nb] & 4) && !adjacent_panel && !adjacent_special) {
                        emitted = false;
                    } else if (emitted && prev_id == id && !door_change &&
                               prev_panel == panel && prev_special == special) {
                        auto& last = state.vectors[state.vector_list_by_direction[dir]
                            [state.vector_count_by_direction[dir] - 1]];
                        if (dir < 2) last.x1 += 64;
                        else last.y1 += 64;
                    } else {
                        create = true;
                        emitted = true;
                    }
                } else if (thin(dir, c)) {
                    create = is_thin = true; // does not change the solid run's emitted flag
                } else emitted = false;
                if (create) {
                    if (state.vector_count == kMaxVectors ||
                        state.vector_count_by_direction[dir] == kMaxVectorsPerDirection)
                        throw DataError(std::string(map_path), base + 2 * i, "vector capacity (1000 total, 333 per direction)");
                    auto& v = state.vectors[state.vector_count];
                    v.direction = dir;
                    v.class_id = c;
                    v.flags = state.wall_flags[id];
                    v.image_id = id;
                    if (is_thin) {
                        const auto first = first_id(first_wall, c == 0x3d || c == 0x3e ? 0x3e : c,
                                                    map_path, base + 2 * i);
                        v.subtype = static_cast<std::uint8_t>(id - first);
                        if (c != 0x3d && c != 0x3e) v.subtype >>= 1;
                        const bool h = dir >= 2;
                        v.x0 = static_cast<std::int16_t>(64 * col + (h ? 32 : 0));
                        v.y0 = static_cast<std::int16_t>(64 * row + (h ? 0 : 32));
                        v.x1 = static_cast<std::int16_t>(v.x0 + (h ? 0 : 64));
                        v.y1 = static_cast<std::int16_t>(v.y0 + (h ? 64 : 0));
                    } else {
                        v.subtype = static_cast<std::uint8_t>(id - first_id(first_wall, c, map_path, base + 2 * i));
                        if ((state.wall_flags[nb] & 8) && !thin(dir, nc) && nc != 0x3f && nc != 0x40)
                            v.image_id = class_id_from(state.wall_class_by_id, 0x30, nb,
                                                       map_path, base + 2 * i);
                        v.x0 = static_cast<std::int16_t>(64 * (col + (dir == 2)));
                        v.y0 = static_cast<std::int16_t>(64 * (row + (dir == 1)));
                        v.x1 = static_cast<std::int16_t>(v.x0 + (dir < 2 ? 64 : 0));
                        v.y1 = static_cast<std::int16_t>(v.y0 + (dir >= 2 ? 64 : 0));
                    }
                    state.vector_list_by_direction[dir][state.vector_count_by_direction[dir]++] =
                        static_cast<std::uint16_t>(state.vector_count++);
                }
                prev_id = id;
                prev_neighbor = nb;
                prev_panel = panel;
                prev_special = special;
            }
        }
        msc_sort(state.vector_list_by_direction[dir].data(), state.vector_count_by_direction[dir],
                 static_cast<std::uint16_t>(0x6982 + dir * 0x534),
                 [&state](std::uint16_t idx) {
                     const auto& v = state.vectors[idx];
                     return v.x0 == v.x1 ? v.x0 : v.y0;
                 });
    }
    for (std::size_t i = 0; i < state.object_count; ++i) {
        state.object_list_x[i] = static_cast<std::uint16_t>(i);
        state.object_list_y[i] = static_cast<std::uint16_t>(i);
    }
    msc_sort(state.object_list_x.data(), state.object_count, 0x839e,
             [&state](std::uint16_t idx) { return state.objects[idx].world_x; });
    msc_sort(state.object_list_y.data(), state.object_count, 0x8916,
             [&state](std::uint16_t idx) { return state.objects[idx].world_y; });
    for (std::size_t i = 0; i < state.object_count; ++i) {
        state.objects[state.object_list_x[i]].x_order_index = static_cast<std::uint16_t>(i);
        state.objects[state.object_list_y[i]].y_order_index = static_cast<std::uint16_t>(i);
    }
}

} // namespace n3d
