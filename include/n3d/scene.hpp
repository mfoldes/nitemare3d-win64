#pragma once

#include "n3d/assets.hpp"
#include "n3d/frame.hpp"
#include "n3d/level.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace n3d {

// Coordinates and image phases are supplied by the caller. The palette is the
// live master palette (normally Assets::palette.palette), not a UIF PCX trailer.
struct Viewpoint final {
    std::int32_t world_x = 0;
    std::int32_t world_y = 0;
    std::uint16_t heading = 0;
    std::uint16_t viewport_width = 304;
    std::uint8_t ceiling_index = 0x11;
    std::uint8_t floor_index = 0x0c;
    std::uint8_t shade_level = 2;
    bool monochrome = false;
    const std::array<std::array<std::uint8_t, 3>, 256>* palette = nullptr;
    std::uint32_t shade_epoch = 0; // Changed only by original c5e2-style rebuilds.
};

enum class RenderError : std::uint8_t {
    none,
    unprepared_scene,
    invalid_viewpoint,
    invalid_level,
    missing_palette,
    missing_background,
    missing_image,
    invalid_frame,
    division_overflow,
    uncovered_viewport,
    span_capacity,
    sprite_capacity,
    undefined_wall_table,
    undefined_sprite_memory,
    undefined_frame_read,
    undefined_frame_write,
};

struct ProjectedVector final {
    std::int16_t x0 = 0;
    std::int16_t y0 = 0; // 16 times the original bottom ordinate
    std::int16_t x1 = 0;
    std::int16_t y1 = 0;
    bool visible = false;
};

struct SceneSpan final {
    std::uint16_t vector_index = 0;
    std::int16_t first_x = 0;
    std::int16_t last_x = 0;
    std::int16_t first_y16 = 0;
    std::int16_t last_y16 = 0;
    std::int32_t slope = 0;
    std::int32_t accumulator = 0;
};

struct SceneWallColumn final {
    std::uint16_t vector_index = 0;
    std::uint8_t image_id = 0;
    std::uint8_t frame_index = 0;
    std::uint16_t texture_x = 0;
    std::int16_t top = 0;
    std::uint16_t row_count = 0;
    std::uint16_t source_row = 0;
    std::uint16_t fraction = 0;
    std::uint32_t step = 0;
    bool shaded = false;
};

enum class SceneSpriteSource : std::uint8_t { map_object, projectile };

struct SceneSpriteSlot final {
    std::uint16_t object_index = 0;
    std::uint8_t image_id = 0;
    std::uint8_t frame_index = 0;
    std::uint8_t flags = 0;
    std::int16_t left = 0;
    std::int16_t right = 0;
    std::int16_t top = 0;
    std::int16_t base_y = 0;
    std::int16_t y16 = 0;
    bool used = false;
    SceneSpriteSource source = SceneSpriteSource::map_object;
};

struct SceneSnapshot final {
    static constexpr std::size_t kMaxSpans = 50;
    static constexpr std::size_t kSpriteSlots = 100;
    static constexpr std::size_t kPendingProjectiles = 8;
    static constexpr std::int16_t kEmptyColumn = -1;

    std::array<ProjectedVector, kMaxVectors> projected{};
    std::array<std::int16_t, IndexedFrame::kWidth> visible_columns{};
    // DS:53fe is a 320-entry far-pointer table. 3940 clears only the active
    // viewport columns, so offsets outside it persist across frames.
    std::array<std::uint16_t, IndexedFrame::kWidth> column_pointer_offsets{};
    std::array<SceneSpan, kMaxSpans> spans{};
    std::array<SceneWallColumn, IndexedFrame::kWidth> wall_columns{};
    std::array<std::uint16_t, IndexedFrame::kWidth> wall_y16{};
    std::array<SceneSpriteSlot, kSpriteSlots> sprite_slots{};
    // The prepared raster cache stays live across modal redraws. Any serviced
    // projectile snapshots that have not yet been painted are held in a compact
    // transient queue and seeded into sprite_slots only during prepare.
    std::array<SceneSpriteSlot, kPendingProjectiles> pending_projectiles{};
    std::array<std::uint8_t, kPendingProjectiles> pending_rows{};
    std::uint8_t pending_count = 0;
    bool pending_queue_seeded = false;
    // Caller copies these observations into mutable gameplay state before input.
    std::array<std::int16_t, kMaxObjects> object_base_y{};
    std::array<bool, kMaxObjects> object_queued{};
    std::array<bool, kMaxGuards> guard_seen{};
    std::array<std::uint16_t, 4> vector_cursors{};
    std::uint16_t object_cursor_x = 0;
    std::uint16_t object_cursor_y = 0;
    std::size_t span_count = 0;
    std::size_t sprite_count = 0;
    bool ready = false;
    std::int16_t visible_min_x = 0;
    std::int16_t visible_max_x = 0;
    std::int16_t visible_min_y = 0;
    std::int16_t visible_max_y = 0;
    std::int16_t viewport_left = 0;
    std::int16_t viewport_right = 0;
    std::int16_t viewport_top = 0;
    std::int16_t viewport_bottom = 0;
    std::int16_t viewport_center_x = 0;
    std::int16_t viewport_center_y = 0;
    std::int16_t projection_center16 = 0;
    std::int16_t sine = 0;
    std::int16_t cosine = 0;
    std::int32_t focal = 0;
    std::int32_t height_scale = 0;
    std::int32_t inverse_scale = 0;
    std::uint8_t octant = 0;
    bool axis_flag = false;
    Viewpoint view{};
    std::array<std::uint8_t, 256> shade_map{};
    const std::array<std::array<std::uint8_t, 3>, 256>* shaded_palette = nullptr;
    std::uint8_t cached_shade_level = 0xff;
    std::uint32_t cached_shade_epoch = 0xffffffffu;
};

// Reset the snapshot when initialize_level rebuilds LevelState. Within a level,
// reuse it to preserve the original path-dependent equal-key list cursors.
// Neither function performs I/O, allocates memory, or mutates LevelState.
RenderError prepare_scene(SceneSnapshot& scene, const LevelState& level,
                          const EpisodeAssets& episode, const Viewpoint& view) noexcept;
RenderError paint_scene(const SceneSnapshot& scene, const LevelState& level,
                        const EpisodeAssets& episode, const Assets& assets,
                        IndexedFrame& frame) noexcept;

// After a successful paint/consume, queue a serviced projectile using that view.
// Its pose and row are retained for the NEXT prepare/paint, not a cached redraw.
RenderError queue_projectile_sprite(SceneSnapshot& scene, const LevelState& level,
                                    const EpisodeAssets& episode,
                                    std::int16_t world_x, std::int16_t world_y,
                                    std::uint8_t image_id, std::uint8_t frame_index,
                                    std::uint8_t flags, std::uint8_t lift) noexcept;
// Consume only pending sprites seeded into the successfully painted raster.
// Retain its prepared sprite cache for non-advancing modal redraws.
void consume_scene_sprite_queue(SceneSnapshot& scene) noexcept;

} // namespace n3d
