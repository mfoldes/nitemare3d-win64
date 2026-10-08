#include "n3d/scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstdlib>

namespace n3d {
namespace {

// The tables are the signed words at 1048:025e and 1048:0314 in the owned
// Windows executable, not values rounded from the host's trigonometry library.
constexpr std::array<std::int16_t, 91> kSine{
    0,17,35,53,71,89,107,124,142,160,177,195,212,230,247,265,282,299,
    316,333,350,366,383,400,416,432,448,464,480,496,512,527,542,557,
    572,587,601,616,630,644,658,671,685,698,711,724,736,748,760,772,
    784,795,806,817,828,838,848,858,868,877,886,895,904,912,920,928,
    935,942,949,955,962,968,973,979,984,989,993,997,1001,1005,1008,
    1011,1014,1016,1018,1020,1021,1022,1023,1023,1024
};
constexpr std::array<std::int16_t, 91> kCosine{
    1024,1023,1023,1022,1021,1020,1018,1016,1014,1011,1008,1005,
    1001,997,993,989,984,979,973,968,962,955,949,942,935,928,920,912,
    904,895,886,877,868,858,848,838,828,817,806,795,784,772,760,748,
    736,724,711,698,685,671,658,644,630,616,601,587,572,557,542,527,
    511,496,480,464,448,432,416,400,383,366,350,333,316,299,282,265,
    247,230,212,195,177,160,142,124,107,89,71,53,35,17,0
};
constexpr std::array<std::array<bool, 8>, 4> kEnabled{{
    {{false,true,true,true,true,true,true,false}},
    {{true,true,true,false,false,true,true,true}},
    {{true,false,false,true,true,true,true,true}},
    {{true,true,true,true,true,false,false,true}}
}};

std::int16_t word(std::int64_t n) noexcept {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(n));
}
std::int32_t dword(std::int64_t n) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(n));
}
std::uint32_t bits(std::int32_t n) noexcept { return std::bit_cast<std::uint32_t>(n); }
std::int16_t high_word(std::int32_t n) noexcept {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(bits(n) >> 16));
}
RenderError divide(std::int32_t n, std::int32_t by, std::int32_t& result) noexcept {
    if (!by || (n == std::numeric_limits<std::int32_t>::min() && by == -1))
        return RenderError::division_overflow;
    result = n / by; // IDIV truncates toward zero.
    return RenderError::none;
}
std::int16_t clamp_screen(std::int32_t n) noexcept {
    return static_cast<std::int16_t>(std::clamp(n, -16383, 16383));
}
std::int16_t vector_key(const VectorRecord& v) noexcept {
    return v.x0 == v.x1 ? v.x0 : v.y0;
}

std::int16_t sine(std::uint16_t angle) noexcept {
    if (angle < 90) return kSine[angle];
    if (angle < 180) return kSine[180-angle];
    if (angle < 270) return -kSine[angle-180];
    return -kSine[360-angle];
}
std::int16_t cosine(std::uint16_t angle) noexcept {
    if (angle < 90) return kCosine[angle];
    if (angle < 180) return -kCosine[180-angle];
    if (angle < 270) return -kCosine[angle-180];
    return kCosine[360-angle];
}

RenderError project_vector(ProjectedVector& out, const VectorRecord& v,
                           const SceneSnapshot& scene) noexcept {
    const auto& view = scene.view;
    std::int16_t x0 = word(static_cast<std::int64_t>(v.x0) - view.world_x);
    std::int16_t x1 = word(static_cast<std::int64_t>(v.x1) - view.world_x);
    std::int16_t y0 = word(static_cast<std::int64_t>(view.world_y) - v.y0);
    std::int16_t y1 = word(static_cast<std::int64_t>(view.world_y) - v.y1);
    const std::int32_t m4 = scene.sine * x0, m28 = scene.cosine * y0;
    const std::int32_t m18 = y0 == y1 ? scene.sine * x1 : m4;
    const std::int32_t m24 = y0 == y1 ? m28 : scene.cosine * y1;
    const std::int32_t depth0 = std::max<std::int32_t>(0x4000, dword(static_cast<std::int64_t>(m4) + m28));
    const std::int32_t depth1 = std::max<std::int32_t>(0x4000, dword(static_cast<std::int64_t>(m18) + m24));
    std::int32_t q0 = 0, q1 = 0;
    if (auto e = divide(scene.height_scale, depth0, q0); e != RenderError::none) return e;
    if (auto e = divide(scene.height_scale, depth1, q1); e != RenderError::none) return e;
    out.y0 = word(static_cast<std::int64_t>(q0) + scene.projection_center16);
    out.y1 = word(static_cast<std::int64_t>(q1) + scene.projection_center16);
    if (out.y0 > (scene.viewport_bottom + 180) * 16 &&
        out.y1 > (scene.viewport_bottom + 180) * 16) {
        out.visible = false;
        return RenderError::none;
    }
    // 1010:e798 clips each endpoint against D=0x4000 along the vector's
    // constant world axis. Its second endpoint tests the *modified* first X.
    if (depth0 == 0x4000) {
        if (x0 == x1) {
            if (view.heading == 90 || view.heading == 270) y0 = 0x4000;
            else {
                if (auto e = divide(dword(0x4000LL - m4), scene.cosine, q0); e != RenderError::none) return e;
                y0 = word(q0);
            }
        } else {
            if (view.heading == 0 || view.heading == 180) x0 = 0x4000;
            else {
                if (auto e = divide(dword(0x4000LL - m28), scene.sine, q0); e != RenderError::none) return e;
                x0 = word(q0);
            }
        }
    }
    const auto lateral0 = dword(static_cast<std::int64_t>(scene.cosine) * x0 -
                                static_cast<std::int64_t>(scene.sine) * y0);
    if (auto e = divide(dword(static_cast<std::int64_t>(lateral0) * scene.focal), depth0, q0);
        e != RenderError::none) return e;
    out.x0 = clamp_screen(dword(static_cast<std::int64_t>(scene.viewport_center_x) + q0));
    if (depth1 == 0x4000) {
        if (x0 == x1) {
            if (view.heading == 90 || view.heading == 270) y1 = 0x4000;
            else {
                if (auto e = divide(dword(0x4000LL - m18), scene.cosine, q1); e != RenderError::none) return e;
                y1 = word(q1);
            }
        } else {
            if (view.heading == 0 || view.heading == 180) x1 = 0x4000;
            else {
                if (auto e = divide(dword(0x4000LL - m24), scene.sine, q1); e != RenderError::none) return e;
                x1 = word(q1);
            }
        }
    }
    const auto lateral1 = dword(static_cast<std::int64_t>(scene.cosine) * x1 -
                                static_cast<std::int64_t>(scene.sine) * y1);
    if (auto e = divide(dword(static_cast<std::int64_t>(lateral1) * scene.focal), depth1, q1);
        e != RenderError::none) return e;
    out.x1 = clamp_screen(dword(static_cast<std::int64_t>(scene.viewport_center_x) + q1));
    if (out.x0 > out.x1) {
        std::swap(out.x0, out.x1);
        std::swap(out.y0, out.y1);
    }
    out.visible = out.x0 <= scene.viewport_right && out.x1 >= scene.viewport_left;
    return RenderError::none;
}

bool replace_column(const VectorRecord& old, const VectorRecord& next) noexcept {
    switch (old.direction * 4u + next.direction) {
    case 2: return next.x0 > old.x0 && next.y0 < old.y0;
    case 3: return next.x1 < old.x1 && next.y0 < old.y0;
    case 6: return next.x0 > old.x0 && next.y1 > old.y1;
    case 7: return next.x1 < old.x1 && next.y1 > old.y1;
    case 8: return next.x1 > old.x1 && next.y1 < old.y1;
    case 9: return next.x1 > old.x1 && next.y0 > old.y0;
    case 12: return next.x0 < old.x0 && next.y1 < old.y1;
    case 13: return next.x0 < old.x0 && next.y0 > old.y0;
    default: return false;
    }
}

RenderError consider_vector(SceneSnapshot& scene, const LevelState& level,
                            std::uint16_t index, int& uncovered) noexcept {
    if (index >= level.vector_count) return RenderError::invalid_level;
    const auto& v = level.vectors[index];
    if (!(v.flags & 1)) return RenderError::none;
    auto& projected = scene.projected[index];
    if (auto e = project_vector(projected, v, scene); e != RenderError::none) return e;
    if (!projected.visible) return RenderError::none;
    const auto first = std::max<int>(projected.x0, scene.viewport_left);
    const auto last = std::min<int>(projected.x1, scene.viewport_right);
    for (int x = first; x <= last; ++x) {
        auto& current = scene.visible_columns[static_cast<std::size_t>(x)];
        if (current == SceneSnapshot::kEmptyColumn) { current = static_cast<std::int16_t>(index); --uncovered; }
        else if (replace_column(level.vectors[static_cast<std::size_t>(current)], v))
            current = static_cast<std::int16_t>(index);
    }
    return RenderError::none;
}

void update_vector_cursor(std::uint16_t& cursor, const LevelState& level,
                          unsigned direction, std::int16_t coordinate) noexcept {
    const auto count = level.vector_count_by_direction[direction];
    if (cursor > count) cursor = static_cast<std::uint16_t>(count);
    const auto& list = level.vector_list_by_direction[direction];
    while (cursor < count && vector_key(level.vectors[list[cursor]]) < coordinate) ++cursor;
    while (cursor && vector_key(level.vectors[list[cursor-1]]) > coordinate) --cursor;
}

RenderError build_visibility(SceneSnapshot& scene, const LevelState& level) noexcept {
    scene.visible_columns.fill(SceneSnapshot::kEmptyColumn);
    std::array<std::size_t, 4> cursors{};
    for (unsigned dir = 0; dir != 4; ++dir) {
        update_vector_cursor(scene.vector_cursors[dir], level, dir,
                             dir < 2 ? word(scene.view.world_y) : word(scene.view.world_x));
        cursors[dir] = scene.vector_cursors[dir];
    }
    int uncovered = scene.view.viewport_width;
    for (;;) {
        bool any = false;
        for (unsigned dir = 0; dir != 4; ++dir) {
            if (!kEnabled[dir][scene.octant]) continue;
            const auto count = level.vector_count_by_direction[dir];
            if (dir == 0 || dir == 3) {
                if (cursors[dir] >= count) continue;
            } else if (cursors[dir] == 0) continue;
            any = true;
            const auto index = level.vector_list_by_direction[dir][
                (dir == 0 || dir == 3) ? cursors[dir]++ : --cursors[dir]];
            if (auto e = consider_vector(scene, level, index, uncovered); e != RenderError::none) return e;
            if (uncovered <= 0) break;
        }
        if (uncovered <= 0) break;
        if (!any) return RenderError::uncovered_viewport; // 3940 otherwise loops forever.
    }
    bool first = true;
    std::int16_t previous = SceneSnapshot::kEmptyColumn;
    for (int x = scene.viewport_left; x <= scene.viewport_right; ++x) {
        const auto index = scene.visible_columns[static_cast<std::size_t>(x)];
        if (index == SceneSnapshot::kEmptyColumn) return RenderError::uncovered_viewport;
        if (index == previous) continue;
        const auto& v = level.vectors[static_cast<std::size_t>(index)];
        if (first) {
            scene.visible_min_x = v.x0; scene.visible_max_x = v.x1;
            scene.visible_min_y = v.y0; scene.visible_max_y = v.y1;
            first = false;
        } else {
            scene.visible_min_x = std::min(scene.visible_min_x, v.x0);
            scene.visible_max_x = std::max(scene.visible_max_x, v.x1);
            scene.visible_min_y = std::min(scene.visible_min_y, v.y0);
            scene.visible_max_y = std::max(scene.visible_max_y, v.y1);
        }
        previous = index;
    }
    // Phase two uses the original phase-one extents, not bounds updated after
    // replacing columns in this phase.
    for (unsigned dir = 0; dir != 4; ++dir) {
        if (!kEnabled[dir][scene.octant]) continue;
        const auto count = level.vector_count_by_direction[dir];
        for (;;) {
            if ((dir == 0 || dir == 3) ? cursors[dir] >= count : cursors[dir] == 0) break;
            const auto index = level.vector_list_by_direction[dir][
                (dir == 0 || dir == 3) ? cursors[dir] : cursors[dir]-1];
            const auto& v = level.vectors[index];
            const bool within = dir == 0 ? v.y1 < scene.visible_max_y :
                                dir == 1 ? v.y0 > scene.visible_min_y :
                                dir == 2 ? v.x0 > scene.visible_min_x : v.x1 < scene.visible_max_x;
            if (!within) break;
            if (dir == 0 || dir == 3) ++cursors[dir]; else --cursors[dir];
            if (auto e = consider_vector(scene, level, index, uncovered); e != RenderError::none) return e;
        }
    }
    for (int x = scene.viewport_left; x <= scene.viewport_right; ++x) {
        const auto index = scene.visible_columns[static_cast<std::size_t>(x)];
        scene.column_pointer_offsets[static_cast<std::size_t>(x)] =
            index == SceneSnapshot::kEmptyColumn ? 0u :
                static_cast<std::uint16_t>(6u + 0x1cu * static_cast<std::uint16_t>(index));
    }
    scene.visible_min_x = word(scene.visible_min_x - 1);
    scene.visible_max_x = word(scene.visible_max_x + 1);
    scene.visible_min_y = word(scene.visible_min_y - 1);
    scene.visible_max_y = word(scene.visible_max_y + 1);
    return RenderError::none;
}

RenderError build_spans(SceneSnapshot& scene) noexcept {
    scene.span_count = 0;
    for (int x = scene.viewport_left; x <= scene.viewport_right;) {
        const auto index = scene.visible_columns[static_cast<std::size_t>(x)];
        if (index == SceneSnapshot::kEmptyColumn) { ++x; continue; }
        if (scene.span_count+1 == scene.spans.size()) return RenderError::span_capacity;
        auto& span = scene.spans[scene.span_count++];
        span.vector_index = static_cast<std::uint16_t>(index);
        span.first_x = static_cast<std::int16_t>(x);
        do { ++x; } while (x <= scene.viewport_right &&
                           scene.visible_columns[static_cast<std::size_t>(x)] == index);
        span.last_x = static_cast<std::int16_t>(x-1);
        const auto& projected = scene.projected[static_cast<std::size_t>(index)];
        const std::int16_t dx = word(projected.x1 - projected.x0);
        const std::int16_t dy = word(projected.y1 - projected.y0);
        std::int32_t quotient = 0;
        if (dx) {
            if (auto e = divide(dword(static_cast<std::int64_t>(dy) * 65536), dx, quotient);
                e != RenderError::none) return e;
            span.slope = quotient;
        } else span.slope = 0;
        if (dy == 0 || dx == 0) {
            span.first_y16 = projected.y0;
            span.last_y16 = dx == 0 ? projected.y1 : projected.y0;
        } else {
            if (auto e = divide(dword(static_cast<std::int64_t>(span.first_x-projected.x0) * dy), dx, quotient);
                e != RenderError::none) return e;
            span.first_y16 = word(projected.y0 + quotient);
            if (auto e = divide(dword(static_cast<std::int64_t>(span.last_x-projected.x0) * dy), dx, quotient);
                e != RenderError::none) return e;
            span.last_y16 = word(projected.y0 + quotient);
        }
        span.accumulator = dword(static_cast<std::int64_t>(projected.y0 - scene.projection_center16) * 65536 +
                                 static_cast<std::int64_t>(span.slope) * (span.first_x-projected.x0));
    }
    return RenderError::none;
}

const ImgRecordMetadata* record(const EpisodeAssets& episode, unsigned id,
                                 unsigned frame_index) noexcept {
    if (id >= episode.images.sequences.size()) return nullptr;
    const auto& sequence = episode.images.sequences[id];
    if (!sequence.offset || frame_index >= sequence.frame_count ||
        sequence.first_record_index >= episode.images.records.size() ||
        frame_index >= episode.images.records.size()-sequence.first_record_index) return nullptr;
    return &episode.images.records[sequence.first_record_index+frame_index];
}

// 1010:6422 clamps the source coordinate only near a projected endpoint,
// with direction-specific branches; the final bit mask is unconditional.
std::int16_t corner(std::int16_t coordinate, int x, const ProjectedVector& projected,
                    const VectorRecord& v, std::uint8_t width) noexcept {
    const bool near_first = word(x-projected.x0) < 8;
    const bool near_last = word(projected.x1-x) < 8;
    if ((near_first || near_last) && !(v.flags & 8)) {
        const auto length = word(v.direction < 2 ? v.x1-v.x0 : v.y1-v.y0);
        if (v.class_id == 2 && (v.direction == 0 || v.direction == 3)) {
            if (v.direction == 0) {
                if ((near_last && coordinate < 0) || projected.x1 == x) coordinate = 0;
                else if ((near_first && coordinate >= length) || projected.x0 == x)
                    coordinate = word(length-1);
            } else {
                if ((near_first && coordinate >= 0) || projected.x0 == x) coordinate = -1;
                else if ((near_last && word(-length) > coordinate) || projected.x1 == x)
                    coordinate = word(-length);
            }
        } else if (v.direction == 0 || v.direction == 2) {
            if ((near_last && coordinate >= 0) || projected.x1 == x) coordinate = -1;
            else if ((near_first && word(-length) > coordinate) || projected.x0 == x)
                coordinate = word(-length);
        } else {
            if ((near_first && coordinate < 0) || projected.x0 == x) coordinate = 0;
            else if ((near_last && length <= coordinate) || projected.x1 == x)
                coordinate = word(length-1);
        }
    }
    return word(static_cast<std::uint16_t>(coordinate) & (width-1u));
}

RenderError build_walls(SceneSnapshot& scene, const LevelState& level,
                        const EpisodeAssets& episode) noexcept {
    for (std::size_t i = 0; i < scene.span_count; ++i) {
        const auto& span = scene.spans[i];
        const auto& v = level.vectors[span.vector_index];
        const auto* image = record(episode, v.cached_image_id ? v.cached_image_id : v.image_id,
                                   v.frame_index);
        if (!image) return RenderError::missing_image;
        if ((image->width != 64 && image->width != 128) || image->height != 64 ||
            image->pixels_offset > episode.image_bytes.size() ||
            image->pixels_size > episode.image_bytes.size()-image->pixels_offset ||
            image->pixels_size < static_cast<std::size_t>(image->width)*64)
            return RenderError::invalid_frame;
        std::int32_t accumulator = span.accumulator;
        for (int x = span.first_x; x <= span.last_x; ++x) {
            // 66b0 reads the high word *before* adding the slope.
            const auto rel = high_word(accumulator);
            accumulator = dword(static_cast<std::int64_t>(accumulator) + span.slope);
            const auto unsigned_rel = static_cast<std::uint16_t>(rel);
            if (!unsigned_rel) return RenderError::division_overflow;
            const int bottom = (unsigned_rel >> 4) + scene.viewport_center_y;
            scene.wall_y16[static_cast<std::size_t>(x)] =
                static_cast<std::uint16_t>(rel + scene.projection_center16);
            const int height = 2*(bottom-scene.viewport_center_y)+1;
            if (height < 1 || height >= 512) return RenderError::undefined_wall_table;
            auto& column = scene.wall_columns[static_cast<std::size_t>(x)];
            column.vector_index = span.vector_index;
            column.image_id = v.cached_image_id ? v.cached_image_id : v.image_id;
            column.frame_index = v.frame_index;
            column.shaded = scene.view.shade_level && !scene.view.monochrome && v.direction >= 2;
            const bool fs = ((v.flags & 0x20) != 0) != (v.class_id == 0x3f || v.class_id == 0x40);
            const bool flip = v.class_id != 2 && (v.direction == 0 || v.direction == 3);
            const auto m1e = v.direction < 2 ?
                (fs ? scene.view.world_x-v.x1+128 : scene.view.world_x-v.x0) :
                (fs ? v.y1-scene.view.world_y-128 : v.y0-scene.view.world_y);
            const auto m2 = v.direction < 2 ? scene.view.world_y-v.y0 : v.x0-scene.view.world_x;
            const auto a = dword(static_cast<std::int64_t>(scene.axis_flag ? scene.sine : scene.cosine) * word(m2));
            const auto p = x-scene.viewport_center_x;
            std::int32_t result = 0, t = 0;
            if (scene.axis_flag) {
                if (v.direction >= 2) {
                    if (auto e = divide(scene.height_scale, unsigned_rel, t); e != RenderError::none) return e;
                    if (auto e = divide(dword(static_cast<std::int64_t>(t)-a), scene.cosine, result);
                        e != RenderError::none) return e;
                } else {
                    if (auto e = divide(dword(static_cast<std::int64_t>(p)*scene.inverse_scale), unsigned_rel, t);
                        e != RenderError::none) return e;
                    if (auto e = divide(dword(static_cast<std::int64_t>(t)+a), scene.cosine, result);
                        e != RenderError::none) return e;
                }
            } else {
                if (v.direction >= 2) {
                    if (auto e = divide(dword(static_cast<std::int64_t>(p)*scene.inverse_scale), unsigned_rel, t);
                        e != RenderError::none) return e;
                    if (auto e = divide(dword(static_cast<std::int64_t>(a)-t), scene.sine, result);
                        e != RenderError::none) return e;
                } else {
                    if (auto e = divide(scene.height_scale, unsigned_rel, t); e != RenderError::none) return e;
                    if (auto e = divide(dword(static_cast<std::int64_t>(t)-a), scene.sine, result);
                        e != RenderError::none) return e;
                }
            }
            auto source_x = word(static_cast<std::int64_t>(result) + word(m1e));
            if (flip) source_x = word(-static_cast<int>(source_x));
            source_x = corner(source_x, x, scene.projected[span.vector_index], v, image->width);
            column.texture_x = static_cast<std::uint16_t>(source_x);
            column.step = static_cast<std::uint32_t>((64u<<16)/static_cast<unsigned>(height));
            if (bottom < scene.viewport_bottom) {
                column.top = word(bottom-height);
                column.row_count = static_cast<std::uint16_t>(height+1);
                column.source_row = 0;
                column.fraction = 0;
            } else {
                column.top = scene.viewport_top;
                column.row_count = static_cast<std::uint16_t>(scene.view.viewport_width/2);
                // 1010:2960: the crop table divides first, then halves; using
                // clip * (truncated step) would lose low fractional bits.
                const int excess = std::max(0, height-static_cast<int>(scene.view.viewport_width/2));
                const auto pos = static_cast<std::uint32_t>(
                    (static_cast<std::int64_t>(excess)*64*65536/height) >> 1);
                column.source_row = static_cast<std::uint16_t>((pos>>16)&0xff);
                column.fraction = static_cast<std::uint16_t>(pos);
            }
        }
    }
    return RenderError::none;
}

void update_object_cursor(std::uint16_t& cursor, const LevelState& level, bool x_axis,
                          std::int16_t coordinate) noexcept {
    const auto count = level.object_count;
    if (cursor > count) cursor = static_cast<std::uint16_t>(count);
    const auto& list = x_axis ? level.object_list_x : level.object_list_y;
    const auto key = [x_axis, &level](std::uint16_t index) noexcept {
        return x_axis ? level.objects[index].world_x : level.objects[index].world_y;
    };
    while (cursor < count && key(list[cursor]) < coordinate) ++cursor;
    while (cursor && key(list[cursor-1]) > coordinate) --cursor;
}

RenderError wall_sample(const SceneSnapshot& scene, int x, std::uint16_t& sample) noexcept {
    if (x >= 0 && x < static_cast<int>(scene.wall_y16.size())) {
        sample = scene.wall_y16[static_cast<std::size_t>(x)];
        return RenderError::none;
    }
    // DS:58fe+2*x precedes the wall-height table when x<0. For -640..-1
    // it aliases the OFFSET/SELECTOR words of DS:53fe's 320 far pointers.
    // The original 3940 clears only viewport columns. Fresh unused entries
    // start at zero, while prior wider viewports can leave persistent offsets.
    if (x < 0) {
        const int byte_offset = 0x500 + 2*x;
        if (byte_offset < 0 || byte_offset >= 0x500)
            return RenderError::undefined_sprite_memory;
        const auto offset = scene.column_pointer_offsets[static_cast<std::size_t>(byte_offset / 4)];
        if ((byte_offset & 2) != 0 && offset != 0)
            return RenderError::undefined_sprite_memory; // Runtime selector unknown.
        sample = (byte_offset & 2) != 0 ? 0u : offset;
        return RenderError::none;
    }
    if (!scene.view.palette) return RenderError::undefined_sprite_memory;
    const auto byte_offset = static_cast<std::size_t>(x-320)*2;
    if (byte_offset+1 >= 256*3) return RenderError::undefined_sprite_memory;
    const auto channel = [palette=scene.view.palette](std::size_t i) noexcept {
        return (*palette)[i/3][i%3];
    };
    sample = static_cast<std::uint16_t>(channel(byte_offset) |
                                         (static_cast<unsigned>(channel(byte_offset+1)) << 8));
    return RenderError::none;
}

RenderError queue_sprite(SceneSnapshot& scene, const LevelState& level,
                         const EpisodeAssets& episode, SceneSpriteSource source,
                         std::uint16_t object_index, std::uint8_t image_id,
                         std::uint8_t frame_index, std::uint8_t flags,
                         std::int16_t world_x, std::int16_t world_y,
                         std::int16_t lift) noexcept {
    if (source == SceneSpriteSource::map_object && object_index >= level.object_count)
        return RenderError::invalid_level;
    if (!(flags & 1)) return RenderError::none;
    const auto xrel = word(static_cast<std::int64_t>(world_x)-scene.view.world_x);
    const auto yrel = word(static_cast<std::int64_t>(scene.view.world_y)-world_y);
    auto depth = dword(static_cast<std::int64_t>(scene.cosine)*yrel +
                       static_cast<std::int64_t>(scene.sine)*xrel);
    auto lateral = dword(static_cast<std::int64_t>(scene.cosine)*xrel -
                         static_cast<std::int64_t>(scene.sine)*yrel);
    std::int32_t q = 0;
    if (depth < 0x4000) {
        std::int16_t clipped_x = 0;
        if (scene.view.heading == 0 || scene.view.heading == 180) clipped_x = 0x4000;
        else {
            if (auto e = divide(dword(0x4000LL-static_cast<std::int64_t>(scene.cosine)*yrel),
                                scene.sine, q); e != RenderError::none) return e;
            clipped_x = word(q);
        }
        depth = 0x4000;
        lateral = dword(static_cast<std::int64_t>(clipped_x)*scene.cosine -
                        static_cast<std::int64_t>(yrel)*scene.sine);
    }
    if (auto e = divide(dword(static_cast<std::int64_t>(lateral)*scene.focal), depth, q);
        e != RenderError::none) return e;
    const auto center = clamp_screen(dword(static_cast<std::int64_t>(scene.viewport_center_x)+q));
    if (auto e = divide(scene.height_scale, depth, q); e != RenderError::none) return e;
    const auto y16 = word(static_cast<std::int64_t>(q)+scene.projection_center16);
    const auto original_base = static_cast<std::int16_t>(y16 >> 4);
    if (original_base > scene.viewport_bottom+200) return RenderError::none;
    const auto* image = record(episode, 256u+image_id, frame_index);
    if (!image) return RenderError::missing_image;
    if (!image->width || !image->height || image->pixels_offset > episode.image_bytes.size() ||
        image->pixels_size > episode.image_bytes.size()-image->pixels_offset ||
        image->pixels_size < static_cast<std::size_t>(image->width)*image->height)
        return RenderError::invalid_frame;
    const auto height = static_cast<std::int16_t>(word(
        static_cast<std::int64_t>(image->height)*(original_base-scene.viewport_center_y)) >> 5);
    int base = original_base;
    if (lift > 0) base -= (word(static_cast<std::int64_t>(lift)*
                                      (original_base-scene.viewport_center_y)) >> 5)+1;
    const auto top = base-height;
    if (top > scene.viewport_bottom || base < scene.viewport_top) return RenderError::none;
    const auto scaled_width = static_cast<std::uint16_t>(
        static_cast<std::int64_t>(image->width)*height+image->height-1)/image->height;
    const auto left = center-(scaled_width >> 1);
    const auto right = left+scaled_width;
    if (left > scene.viewport_right || right < scene.viewport_left) return RenderError::none;
    constexpr int kFirstModeledAlias = -640;
    constexpr int kAfterPaletteAlias = 320 + 256 * 3 / 2;
    if (left < kFirstModeledAlias || center < kFirstModeledAlias ||
        right < kFirstModeledAlias || left >= kAfterPaletteAlias ||
        center >= kAfterPaletteAlias || right >= kAfterPaletteAlias)
        return RenderError::undefined_sprite_memory;
    bool unresolved_selector = false;
    bool cull = true;
    for (const int probe_x : std::array<int, 3>{left, center, right}) {
        std::uint16_t height_word = 0;
        const auto error = wall_sample(scene, probe_x, height_word);
        if (error == RenderError::undefined_sprite_memory) {
            unresolved_selector = true;
            continue;
        }
        if (error != RenderError::none) return error;
        if (word(height_word) <= y16) {
            cull = false;
            break;
        }
    }
    if (cull && unresolved_selector) return RenderError::undefined_sprite_memory;
    if (cull) return RenderError::none;

    int slot = std::min<int>(scene.viewport_bottom-1, original_base)-scene.viewport_center_y;
    if (slot >= static_cast<int>(scene.sprite_slots.size()))
        slot = static_cast<int>(scene.sprite_slots.size())-1;
    if (slot < 0) return RenderError::sprite_capacity;

    if (source == SceneSpriteSource::projectile &&
        scene.pending_count >= scene.pending_projectiles.size())
        return RenderError::sprite_capacity;
    const auto occupied = [&scene, source](int row) noexcept {
        if (source == SceneSpriteSource::map_object)
            return scene.sprite_slots[static_cast<std::size_t>(row)].used;
        for (std::size_t i = 0; i < scene.pending_count; ++i)
            if (scene.pending_rows[i] == row) return true;
        return false;
    };
    // Only earlier pending projectiles own rows in the post-paint source queue.
    // Prepared MAP sprites are a separate cache retained for modal redraws.
    if (occupied(slot)) {
        int free = slot;
        while (free >= 0 && occupied(free)) --free;
        if (free >= 0) slot = free;
        else {
            if (original_base == scene.viewport_center_y) return RenderError::undefined_sprite_memory;
            free = std::min<int>(original_base-1, scene.viewport_bottom-1)-scene.viewport_center_y;
            while (free <= scene.viewport_bottom-scene.viewport_center_y &&
                   free < static_cast<int>(scene.sprite_slots.size()) &&
                   (free < 0 || occupied(free))) ++free;
            if (free < 0 || free >= static_cast<int>(scene.sprite_slots.size()) ||
                free > scene.viewport_bottom-scene.viewport_center_y)
                return RenderError::sprite_capacity;
            slot = free;
        }
    }
    if (source == SceneSpriteSource::projectile) {
        scene.pending_projectiles[scene.pending_count] = {
            object_index, image_id, frame_index, flags,
            word(left), word(right), word(top), word(base), y16, true, source};
        scene.pending_rows[scene.pending_count] = static_cast<std::uint8_t>(slot);
        ++scene.pending_count;
        return RenderError::none;
    }

    auto& output = scene.sprite_slots[static_cast<std::size_t>(slot)];
    output = {object_index, image_id, frame_index, flags,
              word(left), word(right), word(top), word(base), y16, true, source};
    if (source == SceneSpriteSource::map_object) {
        const auto& obj = level.objects[object_index];
        scene.object_base_y[object_index] = original_base;
        scene.object_queued[object_index] = true;
        if (obj.flags & 8) {
            if (obj.enemy_index >= level.guard_count) return RenderError::invalid_level;
            if (left-4 < scene.viewport_center_x && right+4 > scene.viewport_center_x)
                scene.guard_seen[obj.enemy_index] = true;
        }
    }
    ++scene.sprite_count;
    return RenderError::none;
}

RenderError queue_object(SceneSnapshot& scene, const LevelState& level,
                         const EpisodeAssets& episode, std::uint16_t index) noexcept {
    if (index >= level.object_count) return RenderError::invalid_level;
    const auto& obj = level.objects[index];
    return queue_sprite(scene, level, episode, SceneSpriteSource::map_object, index,
                        obj.image_id, obj.frame_index, obj.flags, obj.world_x,
                        obj.world_y, obj.lift);
}

RenderError build_sprites(SceneSnapshot& scene, const LevelState& level,
                          const EpisodeAssets& episode) noexcept {
    scene.sprite_count = 0;
    for (auto& slot : scene.sprite_slots) {
        slot.used = false;
        slot.source = SceneSpriteSource::map_object;
    }
    if (scene.pending_count > scene.pending_projectiles.size())
        return RenderError::sprite_capacity;
    for (std::size_t i = 0; i < scene.pending_count; ++i) {
        const auto row = scene.pending_rows[i];
        if (row >= scene.sprite_slots.size() || scene.sprite_slots[row].used)
            return RenderError::sprite_capacity;
        scene.sprite_slots[row] = scene.pending_projectiles[i];
        scene.sprite_slots[row].used = true;
        ++scene.sprite_count;
    }
    scene.object_queued.fill(false);
    scene.guard_seen.fill(false);
    update_object_cursor(scene.object_cursor_x, level, true, word(scene.view.world_x));
    update_object_cursor(scene.object_cursor_y, level, false, word(scene.view.world_y));
    const bool scan_x = scene.visible_max_x-scene.visible_min_x <
                        scene.visible_max_y-scene.visible_min_y;
    const auto& list = scan_x ? level.object_list_x : level.object_list_y;
    const auto count = level.object_count;
    auto cursor = static_cast<std::size_t>(scan_x ? scene.object_cursor_x : scene.object_cursor_y);
    const auto low = scan_x ? scene.visible_min_x : scene.visible_min_y;
    const auto high = scan_x ? scene.visible_max_x : scene.visible_max_y;
    const auto key = [scan_x, &level](std::uint16_t index) noexcept {
        return scan_x ? level.objects[index].world_x : level.objects[index].world_y;
    };
    for (auto i = cursor; i < count && key(list[i]) < high; ++i)
        if (auto e = queue_object(scene, level, episode, list[i]); e != RenderError::none) return e;
    while (cursor && key(list[cursor-1]) > low) {
        --cursor;
        if (auto e = queue_object(scene, level, episode, list[cursor]); e != RenderError::none) return e;
    }
    scene.pending_queue_seeded = true;
    return RenderError::none;
}

void build_shade_map(SceneSnapshot& scene) noexcept {
    if (scene.shaded_palette == scene.view.palette &&
        scene.cached_shade_level == scene.view.shade_level &&
        scene.cached_shade_epoch == scene.view.shade_epoch) return;
    scene.shaded_palette = scene.view.palette;
    scene.cached_shade_level = scene.view.shade_level;
    scene.cached_shade_epoch = scene.view.shade_epoch;
    scene.shade_map.fill(0);
    const int darkness[8]{0,4,8,12,16,20,30,40};
    const int amount = darkness[scene.view.shade_level]*4;
    const auto& colors = *scene.view.palette;
    for (std::size_t i = 10; i <= 245; ++i) {
        const int r = std::max(0, static_cast<int>(colors[i][0])-amount);
        const int g = std::max(0, static_cast<int>(colors[i][1])-amount);
        const int b = std::max(0, static_cast<int>(colors[i][2])-amount);
        int best = std::numeric_limits<int>::max();
        for (std::size_t j = 10; j <= 245; ++j) {
            const int score = std::abs(r-colors[j][0]) +
                              std::abs(g-colors[j][1]) + std::abs(b-colors[j][2]);
            if (score < best) { best = score; scene.shade_map[i] = static_cast<std::uint8_t>(j); }
        }
    }
}

RenderError paint_walls(const SceneSnapshot& scene, const EpisodeAssets& episode,
                        IndexedFrame& frame) noexcept {
    for (int x = scene.viewport_left; x <= scene.viewport_right; ++x) {
        const auto& column = scene.wall_columns[static_cast<std::size_t>(x)];
        const auto* image = record(episode, column.image_id, column.frame_index);
        if (!image) return RenderError::missing_image;
        const std::size_t begin = image->pixels_offset;
        const std::size_t end = begin+image->pixels_size;
        const auto column_start = static_cast<std::size_t>(column.texture_x)*64;
        auto row_index = static_cast<std::size_t>(column.source_row);
        std::uint32_t fraction = column.fraction;
        for (unsigned row = 0; row < column.row_count; ++row) {
            const int y = column.top+static_cast<int>(row);
            if (y < 0 || y >= static_cast<int>(IndexedFrame::kHeight))
                return RenderError::undefined_frame_write;
            const auto index = column_start+row_index;
            if (index >= image->pixels_size || begin+index >= end || begin+index >= episode.image_bytes.size())
                return RenderError::undefined_frame_read;
            auto pixel = std::to_integer<std::uint8_t>(episode.image_bytes[begin+index]);
            if (column.shaded) pixel = scene.shade_map[pixel];
            frame.pixels[static_cast<std::size_t>(y)*IndexedFrame::kWidth+static_cast<std::size_t>(x)] = pixel;
            fraction += column.step;
            row_index += fraction >> 16;
            fraction &= 0xffff;
        }
    }
    return RenderError::none;
}

RenderError paint_sprite(const SceneSnapshot& scene, const SceneSpriteSlot& slot,
                         const EpisodeAssets& episode, IndexedFrame& frame) noexcept {
    const auto* image = record(episode, 256u+slot.image_id, slot.frame_index);
    if (!image) return RenderError::missing_image;
    const auto full_rows = static_cast<int>(slot.base_y)-slot.top+1;
    if (full_rows <= 0) return RenderError::undefined_sprite_memory;
    const auto step = static_cast<std::uint32_t>((static_cast<std::uint32_t>(image->height)<<16)/full_rows);
    int rows = full_rows;
    if (slot.base_y > scene.viewport_bottom) rows -= slot.base_y-scene.viewport_bottom;
    int top = slot.top;
    std::uint32_t vertical_start = 0;
    if (top < scene.viewport_top) {
        const int clip = scene.viewport_top-top;
        vertical_start = static_cast<std::uint32_t>(clip)*step;
        rows -= clip;
        top = scene.viewport_top;
    }
    int left = slot.left;
    int cols = slot.right-slot.left+2;
    std::uint32_t horizontal_start = 0;
    if (slot.right >= scene.viewport_right) cols = scene.viewport_right-slot.left+1;
    else if (slot.left < scene.viewport_left) {
        const int clip = scene.viewport_left-slot.left;
        cols = slot.right-scene.viewport_left+2;
        horizontal_start = static_cast<std::uint32_t>(clip)*step;
        left = scene.viewport_left;
    }
    if (cols <= 0 || rows <= 0) return RenderError::none;
    if (left < 0 || left+cols > static_cast<int>(IndexedFrame::kWidth) ||
        top < 0 || top+rows > static_cast<int>(IndexedFrame::kHeight))
        return RenderError::undefined_frame_write;
    // The rounded coarse rectangle can touch the view after every bitmap texel
    // has been clipped away. Do not sample the original's undefined heap edge.
    if ((horizontal_start >> 16) >= image->width) return RenderError::none;
    std::uint32_t x_step = horizontal_start;
    for (int col = 0; col < cols; ++col) {
        const int x = left+col;
        if (!(slot.flags & 0x10) && scene.wall_y16[static_cast<std::size_t>(x)] >
                                    static_cast<std::uint16_t>(slot.y16)) {
            x_step += step;
            continue;
        }
        const auto image_x = (x_step >> 16) & 0xff;
        if (image_x >= image->width) {
            break;
        }
        std::uint32_t y_step = vertical_start;
        for (int row = 0; row < rows; ++row) {
            const auto source = static_cast<std::size_t>(image_x)*image->height+(y_step>>16);
            if (source >= image->pixels_size || image->pixels_offset > episode.image_bytes.size() ||
                source >= episode.image_bytes.size()-image->pixels_offset)
                return RenderError::undefined_frame_read;
            const auto pixel = std::to_integer<std::uint8_t>(episode.image_bytes[image->pixels_offset+source]);
            if (pixel != 0x29)
                frame.pixels[static_cast<std::size_t>(top+row)*IndexedFrame::kWidth+
                             static_cast<std::size_t>(x)] = pixel;
            y_step += step;
        }
        x_step += step;
    }
    return RenderError::none;
}

} // namespace

RenderError queue_projectile_sprite(SceneSnapshot& scene, const LevelState& level,
                                    const EpisodeAssets& episode,
                                    std::int16_t world_x, std::int16_t world_y,
                                    std::uint8_t image_id, std::uint8_t frame_index,
                                    std::uint8_t flags, std::uint8_t lift) noexcept {
    if (!scene.ready) return RenderError::unprepared_scene;
    return queue_sprite(scene, level, episode, SceneSpriteSource::projectile, 0,
                        image_id, frame_index, flags, world_x, world_y, lift);
}

void consume_scene_sprite_queue(SceneSnapshot& scene) noexcept {
    // Repainting a cached scene must not discard future projectile snapshots.
    // Only the next prepared raster that seeded them consumes the source queue.
    if (!scene.pending_queue_seeded) return;
    scene.pending_count = 0;
    scene.pending_queue_seeded = false;
}

RenderError prepare_scene(SceneSnapshot& scene, const LevelState& level,
                          const EpisodeAssets& episode, const Viewpoint& view) noexcept {
    scene.ready = false;
    scene.pending_queue_seeded = false;
    if (view.viewport_width < 8 || view.viewport_width > IndexedFrame::kWidth ||
        (view.viewport_width & 7u) || view.heading >= 360 || view.shade_level >= 8)
        return RenderError::invalid_viewpoint;
    if (!view.palette) return RenderError::missing_palette;
    if (level.vector_count > level.vectors.size() || level.object_count > level.objects.size() ||
        level.guard_count > level.guards.size()) return RenderError::invalid_level;
    for (unsigned dir = 0; dir != 4; ++dir) {
        if (level.vector_count_by_direction[dir] > kMaxVectorsPerDirection)
            return RenderError::invalid_level;
        for (std::size_t i = 0; i < level.vector_count_by_direction[dir]; ++i)
            if (level.vector_list_by_direction[dir][i] >= level.vector_count)
                return RenderError::invalid_level;
    }
    for (std::size_t i = 0; i < level.object_count; ++i)
        if (level.object_list_x[i] >= level.object_count ||
            level.object_list_y[i] >= level.object_count)
            return RenderError::invalid_level;
    scene.view = view;
    const auto width = static_cast<int>(view.viewport_width);
    const auto half = width >> 1;
    scene.viewport_left = static_cast<std::int16_t>((320-width)>>1);
    scene.viewport_right = static_cast<std::int16_t>(scene.viewport_left+width-1);
    scene.viewport_top = static_cast<std::int16_t>(scene.viewport_left>>1);
    scene.viewport_bottom = static_cast<std::int16_t>(scene.viewport_top+half-1);
    scene.viewport_center_x = static_cast<std::int16_t>(scene.viewport_left+half);
    scene.viewport_center_y = static_cast<std::int16_t>(scene.viewport_top+(half>>1));
    scene.projection_center16 = static_cast<std::int16_t>(scene.viewport_center_y<<4);
    scene.focal = 12000*width/20480;
    scene.height_scale = 16*half*33600;
    scene.inverse_scale = scene.height_scale/scene.focal;
    scene.octant = static_cast<std::uint8_t>(view.heading/45);
    scene.axis_flag = ((1u<<scene.octant)&0x99u) != 0;
    scene.sine = sine(view.heading);
    scene.cosine = cosine(view.heading);
    if (auto e = build_visibility(scene, level); e != RenderError::none) return e;
    if (auto e = build_spans(scene); e != RenderError::none) return e;
    if (auto e = build_walls(scene, level, episode); e != RenderError::none) return e;
    if (auto e = build_sprites(scene, level, episode); e != RenderError::none) return e;
    if (view.shade_level && !view.monochrome) build_shade_map(scene);
    scene.ready = true;
    return RenderError::none;
}

RenderError paint_scene(const SceneSnapshot& scene, const LevelState& level,
                        const EpisodeAssets& episode, const Assets& assets,
                        IndexedFrame& frame) noexcept {
    if (!scene.ready) return RenderError::unprepared_scene;
    const auto& background = assets.uif.images[5-3];
    const auto& screen = background.image;
    if (background.directory_index != 5 || screen.width != IndexedFrame::kWidth ||
        screen.height != IndexedFrame::kHeight || screen.pixels.size() != IndexedFrame::kPixelCount)
        return RenderError::missing_background;
    if (!scene.view.palette) return RenderError::missing_palette;
    for (std::size_t i = 0; i < 256; ++i) {
        const auto& color = (*scene.view.palette)[i];
        frame.palette[i] = {color[0], color[1], color[2]};
    }
    for (std::size_t i = 0; i < frame.pixels.size(); ++i)
        frame.pixels[i] = std::to_integer<std::uint8_t>(screen.pixels[i]);
    const auto rows = static_cast<int>(scene.view.viewport_width)/4;
    for (int y = scene.viewport_top; y < scene.viewport_top+rows; ++y)
        std::fill_n(frame.pixels.data()+static_cast<std::size_t>(y)*IndexedFrame::kWidth+
                    scene.viewport_left, scene.view.viewport_width, scene.view.ceiling_index);
    for (int y = scene.viewport_top+rows; y < scene.viewport_top+2*rows; ++y)
        std::fill_n(frame.pixels.data()+static_cast<std::size_t>(y)*IndexedFrame::kWidth+
                    scene.viewport_left, scene.view.viewport_width, scene.view.floor_index);
    if (auto e = paint_walls(scene, episode, frame); e != RenderError::none) return e;
    for (const auto& slot : scene.sprite_slots) {
        if (!slot.used) continue;
        switch (slot.source) {
        case SceneSpriteSource::map_object:
            if (slot.object_index >= level.object_count) return RenderError::invalid_level;
            break;
        case SceneSpriteSource::projectile:
            break;
        default:
            return RenderError::invalid_level;
        }
        if (auto e = paint_sprite(scene, slot, episode, frame); e != RenderError::none) return e;
    }
    return RenderError::none;
}

} // namespace n3d
