#include "n3d/game.hpp"
#include "n3d/savegame.hpp"
#include "../src/game/internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, std::string_view description) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
constexpr std::size_t cell(unsigned x, unsigned y) { return y * 64u + x; }
void set_cell(n3d::EpisodeAssets& episode, unsigned x, unsigned y,
              std::uint8_t wall, std::uint8_t object = 0) {
    episode.map_bytes[2 * cell(x, y)] = std::byte{wall};
    episode.map_bytes[2 * cell(x, y) + 1] = std::byte{object};
}

void add_frames(n3d::EpisodeAssets& episode, unsigned id, unsigned width, unsigned height,
                unsigned count, unsigned interval, std::uint8_t colour,
                bool extended = false) {
    auto& sequence = episode.images.sequences[id];
    sequence.offset = 1;
    sequence.first_record_index = episode.images.records.size();
    sequence.frame_count = static_cast<std::uint8_t>(count);
    sequence.delay_ms = static_cast<std::uint16_t>(interval);
    sequence.extended_state_flag = extended ? 1u : 0u;
    for (unsigned frame = 0; frame < count; ++frame) {
        const auto start = episode.image_bytes.size();
        episode.image_bytes.insert(episode.image_bytes.end(), width * height,
                                   std::byte{static_cast<std::uint8_t>(colour + frame)});
        episode.images.records.push_back({start, static_cast<std::uint8_t>(width),
                                          static_cast<std::uint8_t>(height), {}, start,
                                          width * height});
    }
    if (extended) {
        sequence.descriptor_offset = episode.image_bytes.size();
        episode.image_bytes.insert(episode.image_bytes.end(), 90, std::byte{0});
        // Every orientation word selects the one decoded guard frame.
        for (unsigned bank = 0; bank < 3; ++bank)
            for (unsigned direction = 0; direction < 8; ++direction)
                episode.image_bytes[sequence.descriptor_offset + 5 + 16 * bank + 2 * direction] =
                    std::byte{1};
    }
}

std::unique_ptr<n3d::Assets> make_assets() {
    auto assets = std::make_unique<n3d::Assets>();
    assets->required_content_identity = n3d::compute_required_content_identity();
    auto& episode = assets->episodes[0];
    episode.map.level_count = 1;
    episode.map.levels_offset = 0;
    episode.map.level_size = 8192;
    episode.map_bytes.assign(8192, std::byte{0});
    episode.map.wall_class_by_id[2] = 1;
    episode.map.object_class_by_id[1] = 2;    // Start, facing north.
    episode.map.object_class_by_id[3] = 0x2a; // Animated flame.
    episode.map.object_class_by_id[4] = 8;    // Guard: extended, interval zero.
    episode.map.object_class_by_id[5] = 0x2a; // Mutated to projectile class 5 below.
    episode.map.object_class_by_id[6] = 0x2a; // Culled behind the camera.
    episode.map.object_class_by_id[7] = 0x2c; // Directional four-group path.
    episode.map.object_class_by_id[8] = 0x2d; // Directional eight-group path.
    set_cell(episode, 32, 36, 0, 1);
    for (unsigned x = 29; x <= 35; ++x) set_cell(episode, x, 32, 2);
    set_cell(episode, 31, 33, 0, 4);
    set_cell(episode, 32, 34, 0, 3);
    set_cell(episode, 33, 34, 0, 5);
    set_cell(episode, 31, 34, 0, 8);
    set_cell(episode, 32, 35, 0, 7);
    set_cell(episode, 32, 38, 0, 6);
    add_frames(episode, 2, 64, 64, 1, 0, 10);
    add_frames(episode, 256 + 3, 8, 16, 3, 100, 20);
    add_frames(episode, 256 + 4, 8, 16, 1, 0, 30, true);
    add_frames(episode, 256 + 5, 8, 16, 3, 100, 20);
    add_frames(episode, 256 + 6, 8, 16, 3, 100, 20);
    add_frames(episode, 256 + 7, 8, 16, 8, 0, 60);
    add_frames(episode, 256 + 8, 8, 16, 8, 0, 70);
    add_frames(episode, 511, 1, 1, 28, 0, 50); // HUD bars and health face.
    auto& background = assets->uif.images[5 - 3];
    background.directory_index = 5;
    background.image.width = 320;
    background.image.height = 200;
    background.image.pixels.assign(n3d::IndexedFrame::kPixelCount, std::byte{0});
    assets->uif_bytes.assign(1, std::byte{0x80});
    for (auto& font : assets->uif.fonts)
        for (auto& glyph : font.glyphs) {
            glyph.width = glyph.rows = 1;
            glyph.bitmap_size = 1;
        }
    return assets;
}

std::size_t object_index(const n3d::LevelState& level, std::uint8_t image_id) {
    for (std::size_t i = 0; i < level.object_count; ++i)
        if (level.objects[i].image_id == image_id) return i;
    check(false, "authored synthetic object has a live level record");
    return 0;
}
const n3d::SceneSpriteSlot* queued_slot(const n3d::SceneSnapshot& scene, std::size_t object) {
    for (const auto& slot : scene.sprite_slots)
        if (slot.used && slot.object_index == object) return &slot;
    return nullptr;
}
std::uint8_t painted_colour(const n3d::GameState& state, const n3d::SceneSpriteSlot& slot) {
    const auto x = static_cast<std::size_t>((slot.left + slot.right) / 2);
    const auto y = static_cast<std::size_t>(slot.top + 1);
    return state.composed_frame.pixels[y * n3d::IndexedFrame::kWidth + x];
}
void crc_update(std::vector<std::byte>& bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 56; i < bytes.size(); ++i) {
        crc ^= std::to_integer<std::uint8_t>(bytes[i]);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i)
        bytes[52 + i] = std::byte{static_cast<std::uint8_t>(crc >> (8u * i))};
}

void animations_follow_queued_compositions() {
    auto assets = make_assets();
    auto source = std::make_unique<n3d::GameState>();
    n3d::detail::start_level(*source, *assets, 1, 0);
    source->audio.clear();
    source->clocks.calibrated = true;
    source->clocks.rate = 25;
    source->clocks.a_deadline = std::numeric_limits<std::uint32_t>::max();
    source->clocks.b_deadline = 1;
    const auto flame = object_index(source->level, 3);
    const auto guard = object_index(source->level, 4);
    const auto projectile = object_index(source->level, 5);
    const auto culled = object_index(source->level, 6);
    const auto facing_four = object_index(source->level, 7);
    const auto facing_eight = object_index(source->level, 8);
    source->level.objects[projectile].class_id = 5; // cc7c skips this source class.

    n3d::step(*source, *assets, {}, 100);
    const auto* first = queued_slot(source->scene, flame);
    check(first && first->frame_index == 0 && painted_colour(*source, *first) == 20 &&
              source->level.objects[flame].frame_index == 1 &&
              source->level.objects[flame].deadline_ms == 200 &&
              source->level.objects[flame].last_base_y == source->scene.object_base_y[flame],
          "first queued composition paints old frame zero, then stores frame one and last base");
    if (!first) return;
    check(queued_slot(source->scene, facing_four) &&
              queued_slot(source->scene, facing_eight) &&
              source->level.objects[facing_four].frame_index == 1 &&
              source->level.objects[facing_eight].frame_index == 1 &&
              source->level.objects[facing_four].deadline_ms == 100 &&
              source->level.objects[facing_eight].deadline_ms == 100,
          "1010:cb26 directional classes dispatch even with authored interval zero");
    check(queued_slot(source->scene, guard) && queued_slot(source->scene, projectile) &&
              !source->scene.object_queued[culled] &&
              source->level.objects[guard].frame_index == 0 &&
              source->level.objects[guard].deadline_ms == 0 &&
              source->level.objects[projectile].frame_index == 0 &&
              source->level.objects[projectile].deadline_ms == 0 &&
              source->level.objects[culled].frame_index == 0 &&
              source->level.objects[culled].deadline_ms == 0,
          "queued zero-interval guard and class-five projectile, and culled actor, never autoadvance");
    n3d::IndexedFrame first_paint{}, repaint{};
    n3d::render(*source, *assets, first_paint);
    n3d::render(*source, *assets, repaint);
    check(repaint.pixels == first_paint.pixels && source->level.objects[flame].frame_index == 1,
          "presentation repaint never advances simulation");
    n3d::step(*source, *assets, {}, 101); // The B quotient has not changed.
    check(source->level.objects[flame].frame_index == 1 &&
              source->level.objects[flame].deadline_ms == 200,
          "a pump without a due B pass does not compose or animate");
    source->screen = n3d::Screen::root_menu;
    n3d::step(*source, *assets, {}, 1000);
    check(source->level.objects[flame].frame_index == 1 &&
              source->level.objects[flame].deadline_ms == 200,
          "paused menu does not service due object animation");
    source->screen = n3d::Screen::gameplay;
    n3d::step(*source, *assets, {}, 150);
    check(queued_slot(source->scene, flame)->frame_index == 1 &&
              source->level.objects[flame].frame_index == 1 &&
              source->level.objects[flame].deadline_ms == 200,
          "strictly future unsigned deadline does not advance on a clock-B pass");

    const auto bytes = n3d::encode_save(*source, *assets, 150);
    constexpr std::size_t object_first = 56 + 79734 + 2 + 2 + 2 + 176 + 2;
    for (const auto field : std::array<std::size_t, 2>{object_first + 4, object_first + 5}) {
        auto corrupt = bytes;
        corrupt[field] = std::byte{1};
        crc_update(corrupt);
        try {
            (void)n3d::decode_save(corrupt, *assets, "broken-animation.sav");
            check(false, "CRC-valid unsupported bank/frame must fail before publishing state");
        } catch (const n3d::DataError& error) {
            check(error.path() == "broken-animation.sav" && error.offset() == field,
                  "corrupt object bank/frame identifies its exact native save field");
        }
    }
    auto decoded = n3d::decode_save(bytes, *assets);
    auto& restored = *decoded.state;
    restored.audio.clear();
    check(decoded.save_anchor_ms == 150 && restored.level.objects[flame].frame_index == 1 &&
              restored.level.objects[flame].bank == 0 &&
              restored.level.objects[flame].deadline_ms == 200,
          "save/load preserves phase, bank, and absolute deadline in the virtual source clock");
    n3d::step(*source, *assets, {}, 200);
    n3d::step(restored, *assets, {}, 200);
    check(queued_slot(restored.scene, flame)->frame_index == 1 &&
              painted_colour(restored, *queued_slot(restored.scene, flame)) == 21 &&
              source->level.objects[flame].frame_index == 2 &&
              restored.level.objects[flame].frame_index == 2 &&
              restored.level.objects[flame].deadline_ms == 300,
          "deadline equality paints the old frame and advances identically after restore");
    n3d::step(restored, *assets, {}, 10000);
    check(queued_slot(restored.scene, flame)->frame_index == 2 &&
              restored.level.objects[flame].frame_index == 0 &&
              restored.level.objects[flame].deadline_ms == 10100,
          "late clock-B composition advances once, wraps, and schedules now plus interval");
    n3d::step(restored, *assets, {}, 10100);
    check(queued_slot(restored.scene, flame)->frame_index == 0 &&
              restored.level.objects[flame].frame_index == 1 &&
              restored.level.objects[flame].deadline_ms == 10200,
          "equal deadline is due and the wrapped sequence continues");
    restored.level.objects[flame].deadline_ms = 0xffffffceu;
    n3d::step(restored, *assets, {}, 0xffffffceu);
    check(restored.level.objects[flame].deadline_ms == 50,
          "due deadline plus interval wraps modulo 2^32, without signed comparison");
}
} // namespace

int run_object_animation_tests() {
    animations_follow_queued_compositions();
    return failures;
}
