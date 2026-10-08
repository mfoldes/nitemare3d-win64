#pragma once

#include "n3d/assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace n3d {

inline constexpr std::size_t kMapWidth = 64;
inline constexpr std::size_t kMapHeight = 64;
inline constexpr std::size_t kMapCellCount = kMapWidth * kMapHeight;
inline constexpr std::size_t kMaxVectors = 1000;
inline constexpr std::size_t kMaxVectorsPerDirection = 333;
inline constexpr std::size_t kMaxObjects = 350;
inline constexpr std::size_t kMaxGuards = 100;
struct CellSnapshot final {
    std::uint8_t wall_id = 0;
    std::uint8_t object_id = 0;
};

struct VectorRecord final {
    std::uint8_t image_id = 0;
    std::uint8_t subtype = 0;
    std::uint8_t cached_image_id = 0; // 1010:4c8a cache; zero means unchanged image_id.
    std::uint8_t flags = 0;
    std::uint8_t class_id = 0;
    std::uint8_t direction = 0;
    std::uint8_t frame_index = 0;
    std::uint32_t deadline = 0;
    std::int16_t x0 = 0;
    std::int16_t y0 = 0;
    std::int16_t x1 = 0;
    std::int16_t y1 = 0;
    std::int16_t px0 = 0;
    std::int16_t py0 = 0;
    std::int16_t px1 = 0;
    std::int16_t py1 = 0;
};

struct ObjectRecord final {
    std::uint8_t image_id = 0;
    std::uint8_t subtype = 0;
    std::uint8_t bank = 0; // Original record +2; timed extended descriptors select this bank.
    std::uint8_t flags = 0;
    std::uint8_t class_id = 0;
    std::uint8_t frame_index = 0;
    std::uint32_t deadline_ms = 0; // Original record +8, unsigned absolute millisecond clock.
    std::uint8_t enemy_index = 0;
    std::uint16_t cell_x = 0;
    std::uint16_t cell_y = 0;
    std::int16_t world_x = 0;
    std::int16_t world_y = 0;
    std::uint16_t x_order_index = 0;
    std::uint16_t y_order_index = 0;
    std::int16_t last_base_y = 0;
    std::int16_t lift = 0;
};

struct GuardRecord final {
    std::uint16_t animation_word = 0;
    std::uint32_t seen_tick = 0;
    std::int16_t timer = 0;
    std::uint16_t object_slot = 0;
    std::uint8_t strategy = 0;
    std::uint8_t state = 0;
    std::uint8_t next_state = 0;
    std::uint8_t hidden_object_id = 0;
    std::uint8_t room_id = 0;
    std::uint8_t orientation_aware = 0;
    std::uint8_t hit_points = 0;
    std::uint8_t facing = 0;
    std::uint8_t resolved_octant = 0;
    std::int8_t step_x = 0;
    std::int8_t step_y = 0;
    std::int8_t bob_speed = 0;
    std::uint8_t attack_kind = 0;
    std::uint8_t line_of_sight = 0;
    std::uint8_t adjacent = 0;
    std::uint8_t reserved = 0;
};

struct StartPose final {
    std::uint16_t cell_x = 0;
    std::uint16_t cell_y = 0;
    std::int32_t world_x = 0;
    std::int32_t world_y = 0;
    std::uint16_t heading = 0;
};

struct LevelState final {
    std::array<CellSnapshot, kMapCellCount> cells{};
    std::array<std::uint8_t, 256> wall_class_by_id{};
    std::array<std::uint8_t, 256> object_class_by_id{};
    std::array<std::uint8_t, 256> wall_flags{};
    std::array<std::uint8_t, 256> object_flags{};

    std::array<VectorRecord, kMaxVectors> vectors{};
    std::array<ObjectRecord, kMaxObjects> objects{};
    std::array<GuardRecord, kMaxGuards> guards{};
    std::array<std::uint16_t, kMaxVectorsPerDirection> vector_list_by_direction[4]{};
    std::array<std::uint16_t, kMaxObjects> object_list_x{};
    std::array<std::uint16_t, kMaxObjects> object_list_y{};
    std::array<std::size_t, 4> vector_count_by_direction{};

    std::size_t vector_count = 0;
    std::size_t object_count = 0;
    std::size_t guard_count = 0;

    StartPose start{};
    std::uint16_t level_index = 0;
};

void initialize_level(LevelState& state, const EpisodeAssets& assets,
                      std::uint16_t level_index, std::string_view map_path = "MAP");

} // namespace n3d
