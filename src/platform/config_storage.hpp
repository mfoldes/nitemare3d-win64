#pragma once

#include "n3d/config.hpp"

#include <filesystem>

namespace n3d::platform {

// The caller owns the user directory and therefore the exact on-disk config path.
// This layer persists only config.sav in that directory and never touches the
// purchased asset directory or any source-data path.
[[nodiscard]] Config load_user_config(const std::filesystem::path& user_directory);
void save_user_config(const std::filesystem::path& user_directory, const Config& config);

} // namespace n3d::platform
