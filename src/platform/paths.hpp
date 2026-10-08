#pragma once

#include "n3d/assets.hpp"

#include <Windows.h>

#include <filesystem>
#include <optional>

namespace n3d::platform {

// Relative command-line paths are made absolute when arguments are parsed, before
// a folder dialog or another component can change the process working directory.
struct LaunchOptions final {
    std::optional<std::filesystem::path> data_directory;
    std::optional<std::filesystem::path> user_directory;
    bool maps = false;
    bool square_pixels = false;
    bool modern_controls = false;
};

LaunchOptions parse_launch_options(int argc, const wchar_t* const* argv);

// Holds the one fully fingerprint-checked load; the runner must use these assets
// rather than loading the selected directory a second time. Settings and saves
// belong only to user_directory, never to the purchased asset directory.
struct ResolvedPaths final {
    n3d::Assets assets;
    std::filesystem::path user_directory;
    std::filesystem::path saves_directory;
    bool maps = false;
    bool square_pixels = false;
    bool modern_controls = false;

    [[nodiscard]] const std::filesystem::path& data_directory() const noexcept {
        return assets.directory;
    }
};

// An invalid --data or existing adjacent data/ propagates its DataError. A stale
// saved directory is explained to the user before offering a folder picker.
// Cancellation and configuration/OS failures throw rather than selecting data
// from a lower-priority source. Call on a GUI STA thread (the picker uses COM).
ResolvedPaths resolve_paths(const LaunchOptions& options, HWND owner = nullptr);

} // namespace n3d::platform
