#include "n3d/level.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::size_t cell(unsigned x, unsigned y) { return y * 64u + x; }

void set_wall(n3d::EpisodeAssets& episode, unsigned x, unsigned y, std::uint8_t id) {
    episode.map_bytes[2 * cell(x, y)] = std::byte{id};
}

void neighbor_class_search_starts_at_source_id_and_wraps() {
    n3d::EpisodeAssets episode{};
    episode.map.level_count = 1;
    episode.map.level_size = 8192;
    episode.map_bytes.assign(8192, std::byte{0});
    episode.map.object_class_by_id[1] = 2;
    episode.map.wall_class_by_id[20] = 1;
    episode.map.wall_class_by_id[163] = 0x31; // Flag 8, non-thin for direction 0.
    episode.map.wall_class_by_id[250] = 0x31;
    episode.map.wall_class_by_id[7] = 0x30;
    episode.map.wall_class_by_id[120] = 0x30;
    episode.map.wall_class_by_id[167] = 0x30;
    set_wall(episode, 10, 10, 20);
    set_wall(episode, 10, 9, 163);
    set_wall(episode, 20, 20, 20);
    set_wall(episode, 20, 19, 250);
    episode.map_bytes[2 * cell(30, 30) + 1] = std::byte{1};

    auto level = std::make_unique<n3d::LevelState>();
    n3d::initialize_level(*level, episode, 0, "synthetic MAP");
    bool found_163 = false, found_250 = false;
    for (std::size_t i = 0; i < level->vector_count; ++i) {
        const auto& v = level->vectors[i];
        if (v.direction != 0 || v.class_id != 1) continue;
        if (v.x0 == 10 * 64 && v.y0 == 10 * 64) {
            check(v.image_id == 167, "neighbor 163 scans past earlier class 0x30 IDs to 167");
            found_163 = true;
        }
        if (v.x0 == 20 * 64 && v.y0 == 20 * 64) {
            check(v.image_id == 7, "neighbor 250 wraps to earliest class 0x30 ID 7");
            found_250 = true;
        }
    }
    check(found_163 && found_250, "both north-facing source wall vectors exist");
}

} // namespace

int run_level_remap_tests() {
    neighbor_class_search_starts_at_source_id_and_wraps();
    return failures;
}
