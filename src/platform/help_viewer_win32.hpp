#pragma once

#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace n3d::platform {
inline constexpr UINT kMapsHelpClosed = WM_APP + 104;


// Loads the owned WinHelp document as data and opens a modeless GDI window.
// Returns the existing window when already open; parsing failures propagate as DataError.
[[nodiscard]] HWND open_maps_help(HWND owner, HINSTANCE instance,
                                  const std::filesystem::path& help_path, HWND existing);
void show_maps_help_guide(HWND owner);
// Handles Help shortcuts while focus is inside a toolbar or search control.
[[nodiscard]] bool handle_maps_help_key(HWND help_window, const MSG& message);

} // namespace n3d::platform
