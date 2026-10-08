#pragma once

#include "n3d/assets.hpp"
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace n3d::platform {

int run_map_browser(HINSTANCE instance, const n3d::Assets& assets,
                    const std::filesystem::path& data_directory);

} // namespace n3d::platform
