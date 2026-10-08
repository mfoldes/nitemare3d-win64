#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace n3d::platform {

// Native save files live under <user_directory>/saves and are named slot-01.n3dsave
// through slot-10.n3dsave, matching the zero-based API slot range 0..9.
[[nodiscard]] std::filesystem::path native_slot_path(const std::filesystem::path& user_directory,
                                                    std::size_t slot);
[[nodiscard]] bool native_slot_present(const std::filesystem::path& user_directory, std::size_t slot);
[[nodiscard]] std::optional<std::vector<std::byte>> load_native_slot(
    const std::filesystem::path& user_directory, std::size_t slot);
void save_native_slot(const std::filesystem::path& user_directory, std::size_t slot,
                      std::span<const std::byte> encoded);

} // namespace n3d::platform
