#pragma once

#include "n3d/game.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace n3d::platform {

// Consumes Win32 events on the owning window's thread. A frame is sampled once
// per logical update; repeated keydown messages update the one-shot key too.
class InputTranslator final {
public:
    InputTranslator() = default;
    InputTranslator(const InputTranslator&) = delete;
    InputTranslator& operator=(const InputTranslator&) = delete;
    ~InputTranslator();

    void attach(HWND window) noexcept;
    void detach() noexcept;
    void set_wasd(bool enabled) noexcept;
    bool set_relative_mouse(bool enabled, std::wstring& error);
    [[nodiscard]] bool relative_mouse() const noexcept { return relative_mouse_; }
    // WinMM's JOYSTICKID1, centred on the current stick sample at enable.
    // Sensitivity is the original 5..100 slider magnitude (50 by default).
    [[nodiscard]] bool set_joystick(bool enabled, std::uint8_t sensitivity,
                                    std::wstring& error);
    // Sample once per due logical update, after consume(). The uncapped axes
    // reproduce 1010:8f86/96f6; the core applies the 2*m / 4*t caps. X with
    // button 2 held is strafe rather than turn (positive means right).
    [[nodiscard]] bool sample_joystick(InputFrame& frame, std::wstring& error);
    [[nodiscard]] bool sample_menu_joystick(std::uint32_t logical_ms,
                                            std::uint8_t& key,
                                            std::wstring& error);

    void focus(bool active) noexcept;
    void message(UINT message, WPARAM wparam, LPARAM lparam,
                 const RECT& image_rect) noexcept;
    [[nodiscard]] InputFrame consume(bool original_help_text = false) noexcept;
    [[nodiscard]] bool physical_buttons_released() const noexcept;

    void reset() noexcept;
    [[nodiscard]] static std::uint8_t translate_help_key(
        std::uint8_t key, std::uint16_t scan_code, std::uint8_t modifiers) noexcept;
    void refresh_capture() noexcept;
    void key_event(WPARAM key, LPARAM detail, bool down) noexcept;
    void mouse_position(LPARAM detail, const RECT& image_rect) noexcept;
    void raw_mouse(HRAWINPUT input) noexcept;
    [[nodiscard]] std::uint16_t held_keys() const noexcept;
    struct JoystickSample final {
        std::int16_t x = 0;
        std::int16_t y = 0;
        bool button1 = false;
        bool button2 = false;
    };
    [[nodiscard]] bool read_joystick(JoystickSample& sample,
                                     std::wstring& error);

    HWND window_ = nullptr;
    std::array<bool, 256> pressed_{};
    bool focused_ = false;
    bool wasd_ = false;
    bool relative_mouse_ = false;
    bool raw_mouse_registered_ = false;
    bool joystick_enabled_ = false;
    bool joystick_needs_calibration_ = false;
    DWORD joystick_center_x_ = 0;
    DWORD joystick_center_y_ = 0;
    std::uint16_t joystick_scale_ = 1;
    std::uint32_t menu_last_joystick_ms_ = 0;
    bool menu_previous_button1_ = false;
    bool menu_previous_button2_ = true;
    bool left_held_ = false;
    bool right_held_ = false;
    bool left_inside_ = false;
    bool mouse_moved_ = false;
    bool left_released_ = false;
    bool right_pressed_ = false;
    std::uint8_t key_ = 0;
    std::uint16_t help_scan_code_ = 0;
    std::uint8_t help_modifiers_ = 0;
    std::int16_t cursor_x_ = 0;
    std::int16_t cursor_y_ = 0;
    bool cursor_valid_ = false;
    std::int32_t relative_turn_ = 0;
    std::int32_t relative_forward_ = 0;
};

} // namespace n3d::platform
