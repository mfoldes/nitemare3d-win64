#include "n3d/scene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
int failures = 0;

void check(bool ok, std::string_view description) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}

void add_frame(n3d::EpisodeAssets& episode, unsigned id, unsigned width, unsigned height,
               std::byte pixel) {
    auto& sequence = episode.images.sequences[id];
    sequence.offset = 1;
    sequence.first_record_index = episode.images.records.size();
    sequence.frame_count = 1;
    const auto start = episode.image_bytes.size();
    episode.image_bytes.insert(episode.image_bytes.end(), width * height, pixel);
    episode.images.records.push_back({start, static_cast<std::uint8_t>(width),
                                      static_cast<std::uint8_t>(height), {}, start,
                                      width * height});
}

std::unique_ptr<n3d::Assets> make_assets() {
    auto assets = std::make_unique<n3d::Assets>();
    auto& episode = assets->episodes[0];
    add_frame(episode, 0, 1, 1, std::byte{1});
    add_frame(episode, 257, 24, 58, std::byte{0x40});
    const auto sprite_start = episode.images.records.back().pixels_offset;
    episode.image_bytes[sprite_start + 21 * 58] = std::byte{0x71};

    auto& background = assets->uif.images[2];
    background.directory_index = 5;
    background.image.width = 320;
    background.image.height = 200;
    background.image.pixels.assign(n3d::IndexedFrame::kPixelCount, std::byte{2});
    return assets;
}

n3d::SceneSnapshot make_scene(const n3d::Assets& assets) {
    n3d::SceneSnapshot scene{};
    scene.ready = true;
    scene.view.viewport_width = 304;
    scene.view.palette = &assets.palette.palette;
    scene.viewport_left = 8;
    scene.viewport_right = 311;
    scene.viewport_top = 4;
    scene.viewport_bottom = 155;
    return scene;
}

n3d::SceneSpriteSlot sprite_slot(std::uint8_t flags, std::int16_t left,
                                 std::int16_t right) {
    n3d::SceneSpriteSlot slot{};
    slot.object_index = 0;
    slot.image_id = 1;
    slot.flags = flags;
    slot.left = left;
    slot.right = right;
    slot.top = 76;
    slot.base_y = 86;
    slot.y16 = 1387;
    slot.used = true;
    return slot;
}

void fully_texture_clipped_hidden_sprite_preserves_background() {
    auto assets = make_assets();
    const auto& episode = assets->episodes[0];
    n3d::LevelState level{};
    level.object_count = 1;
    auto baseline_scene = make_scene(*assets);
    auto sprite_scene = baseline_scene;
    sprite_scene.sprite_slots[0] = sprite_slot(0x23, 3, 8);
    sprite_scene.wall_y16[8] = 1480;
    n3d::IndexedFrame baseline{}, painted{};

    const auto baseline_error = n3d::paint_scene(baseline_scene, level, episode, *assets, baseline);
    const auto sprite_error = n3d::paint_scene(sprite_scene, level, episode, *assets, painted);
    check(baseline_error == n3d::RenderError::none,
          "synthetic scene without a sprite paints successfully");
    check(sprite_error == n3d::RenderError::none && painted.pixels == baseline.pixels,
          "a fully texture-clipped hidden sprite cannot read or alter the frame");
}

void fully_texture_clipped_equal_edge_preserves_background() {
    auto assets = make_assets();
    add_frame(assets->episodes[0], 257, 27, 61, std::byte{0x40});
    const auto& episode = assets->episodes[0];
    n3d::LevelState level{};
    level.object_count = 1;
    auto baseline_scene = make_scene(*assets);
    auto clipped_scene = baseline_scene;
    auto slot = sprite_slot(0x0b, -17, 8);
    slot.top = 54;
    slot.base_y = 109;
    slot.y16 = 1749;
    clipped_scene.sprite_slots[0] = slot;
    clipped_scene.wall_y16[8] = 1629; // Visible column, but no bitmap texel reaches it.
    n3d::IndexedFrame baseline{}, painted{};
    const auto baseline_error = n3d::paint_scene(baseline_scene, level, episode, *assets, baseline);
    const auto clipped_error = n3d::paint_scene(clipped_scene, level, episode, *assets, painted);
    // floor(25 * floor(61*65536/56) / 65536) == 27, exactly the bitmap width.
    check(baseline_error == n3d::RenderError::none && clipped_error == n3d::RenderError::none &&
              painted.pixels == baseline.pixels,
          "rounded viewport-edge contact with no valid texels leaves the frame unchanged");
}

void partially_visible_sprite_samples_in_range_source_column() {
    auto assets = make_assets();
    const auto& episode = assets->episodes[0];
    n3d::LevelState level{};
    level.object_count = 1;
    auto scene = make_scene(*assets);
    scene.sprite_slots[0] = sprite_slot(0x10, 4, 9);
    n3d::IndexedFrame frame{};

    const auto error = n3d::paint_scene(scene, level, episode, *assets, frame);
    check(error == n3d::RenderError::none &&
              frame.pixels[76 * n3d::IndexedFrame::kWidth + 8] == 0x71 &&
              frame.pixels[76 * n3d::IndexedFrame::kWidth + 9] == scene.view.ceiling_index,
          "partially visible left-clipped sprite paints only its in-range source column");
}

// --- Projectile pending-queue regression coverage ----------------------------
//
// One cover wall (direction 1) spans the whole 304-column viewport so
// prepare_scene accepts the synthetic level; its solid 64x64 texture paints
// the band rows 39..120 as 0x44 with floor 0x0c below. The queued projectile
// at (2048,1924) against the heading-0 view at (2048,2048) freezes the pose
// left=155 right=165 y16=1923, painting an exact 11x11 rectangle (rows
// base-10..base, cols 155..165): rows 110..120 at lift 0, rows 103..113 at
// lift 5; both park at row slot 40 because rows are chosen from the unlifted
// projected base. View changes between queue and prepare
// must replay that frozen pose, never re-project.

void add_projectile_frames(n3d::EpisodeAssets& episode, unsigned id, unsigned count,
                           std::uint8_t colour) {
    auto& sequence = episode.images.sequences[id];
    sequence.offset = 1;
    sequence.first_record_index = episode.images.records.size();
    sequence.frame_count = static_cast<std::uint8_t>(count);
    for (unsigned frame = 0; frame < count; ++frame) {
        const auto start = episode.image_bytes.size();
        episode.image_bytes.insert(episode.image_bytes.end(), 64, std::byte{colour});
        episode.images.records.push_back({start, 8, 8, {}, start, 64});
        if (frame == 1) episode.image_bytes[start] = std::byte{0x29};
    }
}

std::unique_ptr<n3d::Assets> make_projectile_assets() {
    auto assets = make_assets();
    auto& episode = assets->episodes[0];
    add_frame(episode, 2, 64, 64, std::byte{0x44}); // cover-wall texture
    add_projectile_frames(episode, 259, 2, 0x55);   // sprite image 3
    add_frame(episode, 261, 8, 8, std::byte{0x66}); // sprite image 5 (MAP object)
    return assets;
}

n3d::Viewpoint make_projectile_view(const n3d::Assets& assets) {
    n3d::Viewpoint view{};
    view.world_x = 2048;
    view.world_y = 2048;
    view.heading = 0;
    view.viewport_width = 304;
    view.shade_level = 0;
    view.palette = &assets.palette.palette;
    return view;
}

void add_cover_wall(n3d::LevelState& level) {
    auto& vector = level.vectors[0];
    vector.flags = 1;
    vector.class_id = 1;
    vector.direction = 1;
    vector.image_id = 2;
    vector.x0 = 1935;
    vector.x1 = 2161;
    vector.y0 = 1924;
    vector.y1 = 1924;
    level.vector_count = 1;
    level.vector_count_by_direction[1] = 1;
    level.vector_list_by_direction[1][0] = 0;
}

n3d::LevelState make_cover_level(bool with_object) {
    n3d::LevelState level{};
    add_cover_wall(level);
    if (with_object) {
        level.object_count = 1;
        auto& object = level.objects[0];
        object.image_id = 5;
        object.flags = 1;
        object.world_x = 2048;
        object.world_y = 1924;
        level.object_list_x[0] = 0;
        level.object_list_y[0] = 0;
    }
    return level;
}

void projectile_queue_parks_in_pending_until_next_prepare() {
    auto assets = make_projectile_assets();
    const auto& episode = assets->episodes[0];
    const auto view = make_projectile_view(*assets);
    const auto level = make_cover_level(false);
    n3d::SceneSnapshot scene{};

    check(n3d::queue_projectile_sprite(scene, level, episode, 2048, 1924, 3, 0, 1, 5) ==
              n3d::RenderError::unprepared_scene,
          "projectile queue refuses an unprepared snapshot");

    check(n3d::prepare_scene(scene, level, episode, view) == n3d::RenderError::none,
          "baseline prepare accepts the cover-wall level");
    n3d::IndexedFrame frame{};
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none,
          "baseline paint without projectiles succeeds");
    n3d::consume_scene_sprite_queue(scene);

    check(n3d::queue_projectile_sprite(scene, level, episode, 2048, 1924, 3, 0, 0, 5) ==
              n3d::RenderError::none && scene.pending_count == 0,
          "projectile without the active flag parks nothing");
    check(n3d::queue_projectile_sprite(scene, level, episode, 2048, 1924, 3, 0, 1, 5) ==
              n3d::RenderError::none && scene.pending_count == 1,
          "tagged projectile parks one pending row with no MAP object index");

    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) == 0,
          "paint without a following prepare never draws the pending projectile");
    n3d::consume_scene_sprite_queue(scene);
    check(scene.pending_count == 1 && !scene.pending_queue_seeded,
          "consume without a fresh prepare marker keeps the pending row");

    // The next frame's viewpoint may have moved; seeding must replay the
    // frozen pose instead of re-projecting against the new view.
    auto moved = view;
    moved.world_y = 2056;
    check(n3d::prepare_scene(scene, level, episode, moved) == n3d::RenderError::none,
          "next prepare seeds the parked projectile");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none &&
              frame.pixels[113 * n3d::IndexedFrame::kWidth + 166] == 0x55 &&
              frame.pixels[114 * n3d::IndexedFrame::kWidth + 160] == 0x44,
          "next view paints the old lifted pose rather than re-projecting the pending sprite");
}

void seeded_projectile_raster_consumes_pending_but_keeps_cache() {
    auto assets = make_projectile_assets();
    const auto& episode = assets->episodes[0];
    const auto view = make_projectile_view(*assets);
    const auto level = make_cover_level(false);
    n3d::SceneSnapshot scene{};
    n3d::IndexedFrame frame{};

    check(n3d::prepare_scene(scene, level, episode, view) == n3d::RenderError::none,
          "baseline prepare");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none,
          "baseline paint");
    n3d::consume_scene_sprite_queue(scene);
    check(n3d::queue_projectile_sprite(scene, level, episode, 2048, 1924, 3, 0, 1, 0) ==
              n3d::RenderError::none,
          "projectile queues for the next prepare");
    check(n3d::prepare_scene(scene, level, episode, view) == n3d::RenderError::none,
          "next prepare seeds the projectile");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none,
          "seeded projectile paints");
    bool solid = true;
    // Original 374e attempts right-left+2 columns. floor(8*65536/11)=47662,
    // so column 11 still samples source column 7: a 12x11 destination rectangle.
    for (int y = 110; y <= 120; ++y)
        for (int x = 155; x <= 166; ++x)
            solid = solid &&
                    frame.pixels[static_cast<std::size_t>(y) * n3d::IndexedFrame::kWidth +
                                 static_cast<std::size_t>(x)] == 0x55;
    check(solid && std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) ==
                       132,
          "projectile paints the source-derived 12x11 rectangle rows 110..120, cols 155..166");
    check(frame.pixels[109 * n3d::IndexedFrame::kWidth + 160] == 0x44 &&
              frame.pixels[121 * n3d::IndexedFrame::kWidth + 160] == 0x0c,
          "projectile rectangle sits over the wall band with floor below");

    n3d::consume_scene_sprite_queue(scene);
    check(scene.pending_count == 0, "rastered pending rows are cleared by consume");
    check(scene.sprite_count == 1 && scene.sprite_slots[40].used &&
              scene.sprite_slots[40].source == n3d::SceneSpriteSource::projectile,
          "consume keeps the prepared cache for modal redraws");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) == 132 &&
              frame.pixels[115 * n3d::IndexedFrame::kWidth + 160] == 0x55,
          "modal repaint still draws the cached projectile");

    check(n3d::prepare_scene(scene, level, episode, view) == n3d::RenderError::none,
          "next prepare with an empty pending queue");
    check(scene.sprite_count == 0 &&
              std::none_of(scene.sprite_slots.begin(), scene.sprite_slots.end(),
                           [](const n3d::SceneSpriteSlot& s) { return s.used; }),
          "consumed pending leaves no stale projectile slot");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) == 0,
          "no stale projectile pixels after the consumed queue");
}

void pending_projectile_seeds_before_map_object_slot_collision() {
    auto assets = make_projectile_assets();
    const auto& episode = assets->episodes[0];
    const auto view = make_projectile_view(*assets);
    n3d::SceneSnapshot scene{};
    n3d::IndexedFrame frame{};

    check(n3d::prepare_scene(scene, make_cover_level(false), episode, view) ==
              n3d::RenderError::none,
          "baseline prepare");
    check(n3d::paint_scene(scene, make_cover_level(false), episode, *assets, frame) ==
              n3d::RenderError::none,
          "baseline paint");
    n3d::consume_scene_sprite_queue(scene);
    check(n3d::queue_projectile_sprite(scene, make_cover_level(false), episode,
                                       2048, 1924, 3, 0, 1, 0) == n3d::RenderError::none,
          "projectile parks in pending");
    check(n3d::prepare_scene(scene, make_cover_level(true), episode, view) ==
              n3d::RenderError::none,
          "prepare seeds pending before the MAP object queue");

    const auto& projectile = scene.sprite_slots[40];
    const auto& object = scene.sprite_slots[39];
    check(scene.sprite_count == 2, "pending seed and MAP object both join the cache");
    check(projectile.used && projectile.source == n3d::SceneSpriteSource::projectile &&
              projectile.image_id == 3 && projectile.top == 110 &&
              projectile.base_y == 120,
          "projectile keeps its preferred seeded row slot 40");
    check(object.used && object.source == n3d::SceneSpriteSource::map_object &&
              object.object_index == 0 && object.image_id == 5,
          "colliding MAP object displaced to the free neighbouring row 39");
    check(scene.object_base_y[0] == 120 && scene.object_queued[0],
          "MAP object queueing still records its unlifted base");

    check(n3d::paint_scene(scene, make_cover_level(true), episode, *assets, frame) ==
              n3d::RenderError::none,
          "seeded projectile and object paint together");
    check(frame.pixels[115 * n3d::IndexedFrame::kWidth + 160] == 0x55 &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x66}) == 0,
          "projectile rastered after the colliding object wins the shared rectangle");
    check(frame.pixels[109 * n3d::IndexedFrame::kWidth + 160] == 0x44 &&
              frame.pixels[121 * n3d::IndexedFrame::kWidth + 160] == 0x0c,
          "shared rectangle sits over the wall band with floor below");
}

void projectile_transparent_source_pixel_reveals_background() {
    auto assets = make_projectile_assets();
    const auto& episode = assets->episodes[0];
    const auto view = make_projectile_view(*assets);
    const auto level = make_cover_level(false);
    n3d::SceneSnapshot scene{};
    n3d::IndexedFrame frame{};

    check(n3d::prepare_scene(scene, level, episode, view) == n3d::RenderError::none,
          "baseline prepare");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none,
          "baseline paint");
    n3d::consume_scene_sprite_queue(scene);
    // Frame 1 carries the single transparent source pixel 0x29 at (0,0); with
    // this pose it maps to rows 110..111 x cols 155..156 (image_x stays 0 for
    // the first two painted columns, source y 0 for the first two rows).
    check(n3d::queue_projectile_sprite(scene, level, episode, 2048, 1924, 3, 1, 1, 0) ==
              n3d::RenderError::none,
          "transparent projectile frame queues");
    check(n3d::prepare_scene(scene, level, episode, view) == n3d::RenderError::none,
          "prepare seeds the transparent projectile");
    check(n3d::paint_scene(scene, level, episode, *assets, frame) == n3d::RenderError::none,
          "transparent projectile paints");
    const bool transparent = frame.pixels[110 * n3d::IndexedFrame::kWidth + 155] == 0x44 &&
                             frame.pixels[110 * n3d::IndexedFrame::kWidth + 156] == 0x44 &&
                             frame.pixels[111 * n3d::IndexedFrame::kWidth + 155] == 0x44 &&
                             frame.pixels[111 * n3d::IndexedFrame::kWidth + 156] == 0x44;
    check(transparent, "transparent source pixel keeps the wall band background");
    check(frame.pixels[112 * n3d::IndexedFrame::kWidth + 155] == 0x55 &&
              frame.pixels[110 * n3d::IndexedFrame::kWidth + 157] == 0x55 &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) == 128,
          "the four transparent mapped pixels leave exactly the source-derived opaque area");
}

void impact_flag_bypasses_wall_column_flight_does_not() {
    auto assets = make_projectile_assets();
    const auto& episode = assets->episodes[0];
    const auto view = make_projectile_view(*assets);
    const auto level = make_cover_level(false);

    n3d::SceneSnapshot flight{};
    n3d::IndexedFrame frame{};
    check(n3d::prepare_scene(flight, level, episode, view) == n3d::RenderError::none,
          "flight baseline prepare");
    check(n3d::paint_scene(flight, level, episode, *assets, frame) == n3d::RenderError::none,
          "flight baseline paint");
    n3d::consume_scene_sprite_queue(flight);
    check(n3d::queue_projectile_sprite(flight, level, episode, 2048, 1924, 3, 0, 1, 0) ==
              n3d::RenderError::none,
          "flight projectile queues");
    check(n3d::prepare_scene(flight, level, episode, view) == n3d::RenderError::none,
          "flight projectile seeds");
    flight.wall_y16[160] = 0x7fff; // near wall column overrides the far cover wall
    check(n3d::paint_scene(flight, level, episode, *assets, frame) == n3d::RenderError::none,
          "flight projectile paints behind the wall column");
    check(frame.pixels[115 * n3d::IndexedFrame::kWidth + 160] == 0x44,
          "flight flag 1 skips the occluded column");
    check(frame.pixels[115 * n3d::IndexedFrame::kWidth + 155] == 0x55 &&
              frame.pixels[115 * n3d::IndexedFrame::kWidth + 165] == 0x55 &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) == 121,
          "one occluded column leaves the other eleven source-derived columns visible");

    n3d::SceneSnapshot impact{};
    check(n3d::prepare_scene(impact, level, episode, view) == n3d::RenderError::none,
          "impact baseline prepare");
    check(n3d::paint_scene(impact, level, episode, *assets, frame) == n3d::RenderError::none,
          "impact baseline paint");
    n3d::consume_scene_sprite_queue(impact);
    check(n3d::queue_projectile_sprite(impact, level, episode, 2048, 1924, 3, 0, 0x11, 0) ==
              n3d::RenderError::none,
          "impact explosion queues");
    check(n3d::prepare_scene(impact, level, episode, view) == n3d::RenderError::none,
          "impact explosion seeds");
    impact.wall_y16[160] = 0x7fff;
    check(n3d::paint_scene(impact, level, episode, *assets, frame) == n3d::RenderError::none,
          "impact explosion paints against the wall column");
    check(frame.pixels[115 * n3d::IndexedFrame::kWidth + 160] == 0x55 &&
              std::count(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{0x55}) == 132,
          "impact flag 0x11 bypasses the wall test over the full source-derived rectangle");
}
} // namespace

int run_scene_tests() {
    fully_texture_clipped_hidden_sprite_preserves_background();
    fully_texture_clipped_equal_edge_preserves_background();
    partially_visible_sprite_samples_in_range_source_column();
    projectile_queue_parks_in_pending_until_next_prepare();
    seeded_projectile_raster_consumes_pending_but_keeps_cache();
    pending_projectile_seeds_before_map_object_slot_collision();
    projectile_transparent_source_pixel_reveals_background();
    impact_flag_bypasses_wall_column_flight_does_not();
    return failures;
}
