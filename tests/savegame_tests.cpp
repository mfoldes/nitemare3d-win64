#include "n3d/savegame.hpp"
#include "../src/game/internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, std::string_view label) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
constexpr std::size_t cell(unsigned x, unsigned y) { return y * 64u + x; }
void set_cell(n3d::EpisodeAssets& ep, unsigned x, unsigned y, std::uint8_t wall,
              std::uint8_t object) {
    ep.map_bytes[2 * cell(x, y)] = std::byte{wall};
    ep.map_bytes[2 * cell(x, y) + 1] = std::byte{object};
}
std::unique_ptr<n3d::Assets> source_assets() {
    auto assets = std::make_unique<n3d::Assets>();
    assets->required_content_identity = n3d::compute_required_content_identity();
    auto& ep = assets->episodes[0];
    ep.map.level_count = 1;
    ep.map.levels_offset = 0;
    ep.map.level_size = 8192;
    ep.map_bytes.assign(8192, std::byte{0});
    ep.map.object_class_by_id[1] = 2; // Unique authored start pose.
    ep.map.wall_class_by_id[2] = 0x31; // One two-face sliding door.
    ep.map.object_class_by_id[3] = ep.map.object_class_by_id[4] = 8;
    set_cell(ep, 32, 32, 0, 1);
    set_cell(ep, 32, 31, 2, 0);
    set_cell(ep, 29, 32, 0, 3);
    set_cell(ep, 35, 32, 0, 4);
    ep.images.records.resize(2);
    ep.images.records[0].height = ep.images.records[1].height = 16;
    ep.image_bytes.assign(180, std::byte{0});
    for (unsigned id = 3; id <= 4; ++id) {
        auto& seq = ep.images.sequences[256 + id];
        seq.offset = 1;
        seq.first_record_index = id - 3;
        seq.descriptor_offset = 90 * (id - 3);
        seq.frame_count = 1;
    }
    auto& shot = ep.images.sequences[256 + 251];
    shot.offset = 1;
    shot.frame_count = 2;
    shot.delay_ms = 40;
    return assets;
}
std::unique_ptr<n3d::GameState> active(const n3d::Assets& assets) {
    auto s = std::make_unique<n3d::GameState>();
    s->config.music_enabled = 0x82; // The original Config enable is a raw byte.
    s->config.cheat_omnificent = 0x81;
    n3d::detail::start_level(*s, assets, 1, 0);
    s->audio.clear(); // Completed host dispatch boundary.
    s->clocks.calibrated = true;
    s->clocks.rate = 25;
    s->clocks.move_step = 125;
    s->clocks.projectile_step = 1;
    s->clocks.last_milliseconds = 100;
    s->clocks.previous_a_quotient = 3;
    s->clocks.previous_b_quotient = 7;
    s->clocks.a_count = 11;
    s->clocks.b_count = 20;
    s->clocks.a_deadline = 17;
    s->clocks.b_deadline = 21;
    s->clocks.logical_tick = 180;
    s->composed_ready = true;
    s->composed_frame.pixels[0] = 37;
    s->composed_frame.pixels.back() = 197;
    s->composed_frame.palette[0xda] = {15, 17, 19};
    s->palette[0xda] = {0, 0, 0}; // Live flash palette differs from last displayed DIB.
    s->scene.column_pointer_offsets[319] = 0x1234;
    s->scene.wall_y16[319] = 0xff00;
    s->scene.shade_map[37] = 83;
    s->scene.cached_shade_level = 2;
    s->scene.cached_shade_epoch = 9;
    s->shade_epoch = 9;
    s->settings.restore_cursors({2, 4, 5});
    s->warp_row = 2;
    s->player.health = 254;
    s->player.ammo = {255, 202, 17};
    s->player.score = 0xffffffffu;
    s->player.bresenham_error = 123;
    s->rng_seed = 0x8069a4fdu;
    s->automap_speckle_count = 2;
    s->automap_speckles[0] = {14, 23};
    s->automap_speckles[1] = {33, 7};
    s->automap[32 * 64 + 32] = 0x91;
    s->doors[0].state = 2;
    s->doors[0].countdown = -3;
    s->doors[0].sound = true;
    s->level.vectors[s->doors[0].vector_index].x0 += 2;
    s->level.vectors[s->doors[0].partner_vector_index].x0 += 2;
    s->level.object_list_x[0] = 1;
    s->level.object_list_x[1] = 0;
    s->level.objects[1].x_order_index = 0;
    s->level.objects[0].x_order_index = 1; // Path-dependent order, deliberately unsorted.
    s->level.guards[0].state = 0;
    s->level.guards[0].timer = 3;
    s->level.guards[0].seen_tick = 0x12345678u;
    s->level.guards[1].state = 0;
    s->level.guards[1].timer = -30000; // Signed i16 must not be clamped on load.
    auto& projectile = s->projectiles[0];
    projectile.state = 1;
    projectile.image_id = 251;
    projectile.world_x = 32 * 64 + 32;
    projectile.world_y = 33 * 64 + 32;
    projectile.sign_x = 1;
    projectile.sign_y = -1;
    projectile.major_axis = 1;
    projectile.error = -5;
    projectile.minor_increment = 2;
    projectile.diagonal_increment = -10;
    projectile.frame = 2;
    projectile.frame_deadline = 0; // Source's due-now sentinel.
    projectile.lift = 13;
    return s;
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
        bytes[52 + i] = std::byte{static_cast<std::uint8_t>(crc >> (i * 8))};
}
void rejects(std::span<const std::byte> bytes, const n3d::Assets& assets,
             std::size_t expected_offset, std::string_view description) {
    try {
        (void)n3d::decode_save(bytes, assets, "broken.sav");
        check(false, description);
    } catch (const n3d::DataError& error) {
        check(error.path() == "broken.sav" && error.offset() == expected_offset,
              description);
    }
}
void continuation_and_first_frame() {
    auto assets = source_assets();
    auto source = active(*assets);
    constexpr std::uint32_t anchor = 0xfffffff0u;
    auto bytes = n3d::encode_save(*source, *assets, anchor);
    const std::size_t expected = 56 + 79922 + 2 * 2 + 10 * 1 + 25 * 2 + 27 * 2 +
                                 19 * source->level.vector_count;
    check(bytes.size() == expected, "all sixteen groups obey the frozen variable-length equation");
    check(bytes[56] == std::byte{5},
          "living gameplay uses native wire screen byte 5 independently of C++ menu enums");
    const auto saved = n3d::decode_save(bytes, *assets);
    auto& restored = *saved.state;
    check(saved.save_anchor_ms == anchor && restored.clocks.b_deadline == 21 &&
              restored.clocks.previous_b_quotient == 7 &&
              restored.projectiles[0].frame_deadline == 0,
          "virtual anchor and due-now timers survive without rebasing");
    check(restored.settings.retained_cursors() == std::array<std::uint8_t, 3>{2, 4, 5} &&
              restored.config.music_enabled == 0x82 && restored.config.cheat_omnificent == 0x81 &&
              restored.player.health == 254 && restored.player.ammo[0] == 255 &&
              restored.player.score == 0xffffffffu,
          "retained menu cursors and full-range raw values survive");
    check(restored.scene.column_pointer_offsets[319] == 0x1234 &&
              restored.scene.wall_y16[319] == 0xff00 && restored.scene.shade_map[37] == 83 &&
              restored.scene.shaded_palette == &restored.palette && !restored.scene.ready &&
              restored.palette[0xda][0] == 0 && restored.composed_frame.palette[0xda].r == 15,
          "retained scene history and live versus composed palette survive independently");
    check(restored.projectiles[0].frame == 2 && restored.projectiles[0].sign_y == -1 &&
              restored.projectiles[0].lift == 13,
          "active projectile visual lift and frame tail survive exact roundtrip values");
    n3d::IndexedFrame before{}, after{};
    n3d::render(*source, *assets, before);
    n3d::render(restored, *assets, after);
    check(before.pixels == after.pixels && before.palette[0xda].r == after.palette[0xda].r &&
              after.pixels[0] == 37 && after.pixels.back() == 197,
          "first restored DIB is byte-identical before a simulation pass");
    check(restored.audio.pending().size() == 1 &&
              restored.audio.pending()[0].kind == n3d::AudioCommandKind::music &&
              restored.audio.pending()[0].id == source->current_tune,
          "construction tune was cleared and only live music selection replayed");
    restored.audio.clear(); // Simulate the host draining the one restored tune.
    check(restored.level.object_list_x[0] == 1 &&
              restored.level.objects[1].x_order_index == 0 &&
              restored.level.objects[0].x_order_index == 1,
          "path-dependent unsorted permutation is preserved, never re-sorted");
    n3d::detail::advance_doors_and_panels(*source);
    n3d::detail::advance_doors_and_panels(restored);
    n3d::detail::service_projectiles(*source, *assets);
    n3d::detail::service_projectiles(restored, *assets);
    n3d::detail::service_guards(*source, *assets);
    n3d::detail::service_guards(restored, *assets);
    const auto source_random = n3d::detail::random15(*source);
    const auto restored_random = n3d::detail::random15(restored);
    check(restored.level.vectors[restored.doors[0].vector_index].x0 ==
              source->level.vectors[source->doors[0].vector_index].x0 &&
              restored.doors[0].countdown == source->doors[0].countdown &&
              restored.projectiles[0].world_x == source->projectiles[0].world_x &&
              restored.projectiles[0].error == source->projectiles[0].error &&
              restored.projectiles[0].frame_deadline == source->projectiles[0].frame_deadline &&
              restored.projectiles[0].lift == 14 &&
              restored.projectiles[0].lift == source->projectiles[0].lift &&
              restored.level.guards[0].timer == source->level.guards[0].timer &&
              restored.level.guards[1].timer == source->level.guards[1].timer &&
              restored.level.guards[1].state == source->level.guards[1].state &&
              restored.level.guards[0].seen_tick == source->level.guards[0].seen_tick &&
              source_random == restored_random && restored.rng_seed == source->rng_seed,
          "mid-door, projectile, guard and RNG continue on the next source service pass");
    source->screen = n3d::Screen::root_menu;
    source->audio.clear();
    const auto paused_bytes = n3d::encode_save(*source, *assets, 500);
    check(paused_bytes[56] == std::byte{2}, "active root pause uses frozen native screen byte 2");
    auto paused = n3d::decode_save(paused_bytes, *assets);
    check(paused.state->screen == n3d::Screen::root_menu &&
              paused.state->settings.retained_cursors()[2] == 5 &&
              paused.save_anchor_ms == 500,
          "active seven-row root pause retains screen and virtual clock anchor");
}
void corruption_and_atomic_failure() {
    auto assets = source_assets();
    auto source = active(*assets);
    auto bytes = n3d::encode_save(*source, *assets, 100);
    auto changed = bytes;
    changed[56 + 141] ^= std::byte{1};
    rejects(changed, *assets, 52, "payload bit flip fails CRC before publishing state");
    changed = bytes;
    changed[0] ^= std::byte{1};
    rejects(changed, *assets, 0, "bad native save magic reports its first byte");
    changed = bytes;
    changed[8] = std::byte{1};
    rejects(changed, *assets, 8, "version-1 save header is rejected before publication");
    changed = bytes;
    changed[12] ^= std::byte{1};
    rejects(changed, *assets, 12, "foreign release identity fails before reconstruction");
    changed = bytes;
    changed[44] ^= std::byte{1};
    rejects(changed, *assets, 44, "wrong declared payload length fails before reconstruction");
    rejects(std::span<const std::byte>(bytes).first(bytes.size() - 1), *assets, 44,
            "truncated payload fails before reconstruction");
    changed = bytes;
    changed.push_back(std::byte{0});
    rejects(changed, *assets, 44, "extra trailing payload byte is rejected");
    changed = bytes;
    changed[56] = std::byte{7}; // Transient C++ save/load menu enum, not a v2 wire value.
    crc_update(changed);
    rejects(changed, *assets, 56, "CRC-valid non-persistable screen cannot be loaded");
    changed = bytes;
    changed[56 + 141 + 20] = std::byte{2}; // Actual bool, unlike raw Config bytes.
    crc_update(changed);
    rejects(changed, *assets, 56 + 141 + 20,
            "CRC-valid nonboolean flag fails at its absolute field offset");
    changed = bytes;
    const std::size_t projectiles_at = 56 + 79734 + 2 * source->automap_speckle_count +
        (2 + 10 * source->door_count) + (2 + 11 * source->panel_count) +
        (2 + 5 * source->pushwall_count);
    const std::size_t projectile_lift_offset = projectiles_at + 21;
    changed[projectile_lift_offset] = std::byte{21};
    crc_update(changed);
    rejects(changed, *assets, projectile_lift_offset,
            "active projectile lift>20 fails before publication");
    changed = bytes;
    changed[projectile_lift_offset] = std::byte{0};
    crc_update(changed);
    rejects(changed, *assets, projectile_lift_offset,
            "active projectile lift=0 fails before publication");
    changed = bytes;
    changed[projectiles_at] = std::byte{0};
    changed[projectile_lift_offset] = std::byte{21};
    crc_update(changed);
    rejects(changed, *assets, projectile_lift_offset,
            "free projectile lift outside the source range fails before publication");
    changed = bytes;
    // Guard group follows scene/cells, door/panel/pushwall/projectile/object groups.
    const std::size_t guard_count = projectiles_at + 22 * source->projectiles.size() +
                                    (2 + 23 * source->level.object_count);
    changed[guard_count + 2 + 11] = std::byte{17}; // active guard state (not next_state).
    crc_update(changed);
    try {
        (void)n3d::decode_save(changed, *assets, "broken.sav");
        check(false, "CRC-valid unsupported guard state17 fails atomically");
    } catch (const n3d::DataError& error) {
        check(error.path() == "broken.sav" && error.offset() == guard_count + 2 + 11,
              "guard-state failure identifies absolute state byte");
    }
    changed = bytes;
    // The x-order permutation starts after guards and vectors.
    const std::size_t vectors = guard_count + (2 + 25 * source->level.guard_count);
    const std::size_t x_order = vectors + (2 + 19 * source->level.vector_count);
    changed[x_order] = std::byte{0};
    changed[x_order + 1] = std::byte{0};
    changed[x_order + 2] = std::byte{0};
    changed[x_order + 3] = std::byte{0};
    crc_update(changed);
    try {
        (void)n3d::decode_save(changed, *assets, "broken.sav");
        check(false, "CRC-valid duplicate object order fails atomically");
    } catch (const n3d::DataError& error) {
        check(error.path() == "broken.sav" && error.offset() == x_order + 2,
              "object-order failure identifies duplicate permutation entry");
    }
    // Failed loads did not mutate the owned source state.
    check(source->doors[0].state == 2 && source->projectiles[0].state == 1 &&
              source->level.object_list_x[0] == 1,
          "all corrupt loads left the caller's live state untouched");
}
void capture_boundary() {
    auto assets = source_assets();
    auto source = active(*assets);
    const auto rejected = [&](std::string_view label) {
        try {
            (void)n3d::encode_save(*source, *assets, 77);
            check(false, label);
        } catch (const n3d::DataError& error) {
            check(error.path() == "N3DSAVE", label);
        }
    };
    source->screen = n3d::Screen::settings;
    rejected("settings screen is not an eligible captured continuation");
    source->screen = n3d::Screen::gameplay;
    source->player_life_state = 3;
    rejected("death transition cannot silently resume living gameplay");
    source->player_life_state = 1;
    source->projectiles[0].state = 1;
    source->projectiles[0].lift = 0;
    rejected("active projectile with zero lift is rejected before capture");
    source->projectiles[0].lift = 13;
    source->projectiles[0].state = 0;
    source->projectiles[0].lift = 21;
    rejected("free projectile with lift 21 exceeds legal range before capture");
    source->projectiles[0].state = 1;
    source->projectiles[0].lift = 13;
    source->audio.push({n3d::AudioCommandKind::effect, 1, 0, 0});
    rejected("undrained effects exclude an incomplete host dispatch");
}
} // namespace

int run_savegame_tests() {
    continuation_and_first_frame();
    corruption_and_atomic_failure();
    capture_boundary();
    return failures;
}
