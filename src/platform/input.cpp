#include "input.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <windowsx.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace n3d::platform {
namespace {
constexpr std::uint16_t kEscape = 0x0001;
constexpr std::uint16_t kUp = 0x0002;
constexpr std::uint16_t kDown = 0x0004;
constexpr std::uint16_t kLeft = 0x0008;
constexpr std::uint16_t kRight = 0x0010;
constexpr std::uint16_t kRightShift = 0x0020;
constexpr std::uint16_t kLeftShift = 0x0040;
constexpr std::uint16_t kControl = 0x0080;
constexpr std::uint16_t kAlt = 0x0100;
constexpr std::uint16_t kSpace = 0x0200;

std::uint8_t translated_key(WPARAM key, bool wasd) noexcept {
    if (wasd) {
        switch (key) {
        case 'W': return VK_UP;
        case 'S': return VK_DOWN;
        case 'A': return VK_LEFT;
        case 'D': return VK_RIGHT;
        default: break;
        }
    }
    return static_cast<std::uint8_t>(key);
}

std::wstring joystick_error(const wchar_t* action, MMRESULT result) {
    const wchar_t* cause = result == JOYERR_UNPLUGGED ? L"joystick unplugged" :
                           (result == MMSYSERR_NODRIVER ||
                            result == MMSYSERR_BADDEVICEID) ? L"no joystick driver" :
                           L"WinMM joystick failure";
    return std::wstring(action) + L": " + cause + L" (WinMM " +
           std::to_wstring(result) + L")";
}
} // namespace

InputTranslator::~InputTranslator() { detach(); }

void InputTranslator::attach(HWND window) noexcept {
    detach();
    window_ = window;
}

void InputTranslator::detach() noexcept {
    focus(false);
    if (raw_mouse_registered_) {
        RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&device, 1, sizeof(device));
        raw_mouse_registered_ = false;
    }
    relative_mouse_ = false;
    window_ = nullptr;
}

void InputTranslator::set_wasd(bool enabled) noexcept {
    wasd_ = enabled;
    // Do not reinterpret an already-held physical key when the profile changes.
    reset();
    refresh_capture();
}

bool InputTranslator::set_relative_mouse(bool enabled, std::wstring& error) {
    error.clear();
    if (relative_mouse_ == enabled) return true;
    if (!window_) {
        error = L"Relative mouse requires a created window";
        return false;
    }
    RAWINPUTDEVICE device{0x01, 0x02, enabled ? 0u : RIDEV_REMOVE,
                          enabled ? window_ : nullptr};
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        error = L"RegisterRawInputDevices(mouse) failed (Win32 " +
                std::to_wstring(GetLastError()) + L")";
        return false;
    }
    raw_mouse_registered_ = enabled;
    relative_mouse_ = enabled;
    relative_turn_ = 0;
    relative_forward_ = 0;
    refresh_capture();
    return true;
}

bool InputTranslator::set_joystick(bool enabled, std::uint8_t sensitivity,
                                   std::wstring& error) {
    error.clear();
    if (!enabled) {
        joystick_enabled_ = false;
        joystick_needs_calibration_ = false;
        return true;
    }
    if (!sensitivity) {
        error = L"Joystick sensitivity cannot be zero";
        return false;
    }
    const bool was_enabled = joystick_enabled_;
    JOYCAPSW caps{};
    const MMRESULT caps_result = joyGetDevCapsW(JOYSTICKID1, &caps, sizeof(caps));
    if (caps_result != JOYERR_NOERROR) {
        joystick_enabled_ = false;
        error = joystick_error(L"joyGetDevCapsW", caps_result);
        return false;
    }
    // 1010:8fdb-8ff7 uses a 16-bit unsigned X range and the same quotient
    // for both axes. Center comes from the calibration sample, not JOYCAPS.
    const auto range = static_cast<std::uint16_t>(caps.wXmax - caps.wXmin);
    const auto scale = static_cast<std::uint16_t>(range / sensitivity);
    if (scale == 0u || scale >= 0x8000u) {
        joystick_enabled_ = false;
        error = L"Joystick range and sensitivity do not yield a valid signed divisor";
        return false;
    }
    joystick_scale_ = scale;
    joystick_needs_calibration_ = true;
    joystick_enabled_ = true;
    if (!was_enabled) {
        menu_last_joystick_ms_ = 0;
        menu_previous_button1_ = false;
        menu_previous_button2_ = true;
    }
    return true;
}

bool InputTranslator::read_joystick(JoystickSample& sample,
                                     std::wstring& error) {
    error.clear();
    sample = {};
    if (!focused_ || !joystick_enabled_) return true;
    JOYINFO position{};
    const MMRESULT result = joyGetPos(JOYSTICKID1, &position);
    if (result != JOYERR_NOERROR) {
        joystick_enabled_ = false;
        error = joystick_error(L"joyGetPos", result);
        return false;
    }
    if (joystick_needs_calibration_) {
        joystick_center_x_ = position.wXpos;
        joystick_center_y_ = position.wYpos;
        joystick_needs_calibration_ = false;
    }
    // Original 8f86 stores only the low 16 bits of the signed quotient.
    const auto axis = [this](DWORD value, DWORD center) noexcept {
        const auto quotient = (static_cast<std::int64_t>(value) - center) /
                              joystick_scale_;
        return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(quotient));
    };
    sample.x = axis(position.wXpos, joystick_center_x_);
    sample.y = axis(position.wYpos, joystick_center_y_);
    sample.button1 = (position.wButtons & JOY_BUTTON1) != 0;
    sample.button2 = (position.wButtons & JOY_BUTTON2) != 0;
    return true;
}

bool InputTranslator::sample_joystick(InputFrame& frame, std::wstring& error) {
    frame.joystick_forward = 0;
    frame.joystick_turn = 0;
    frame.joystick_strafe = 0;
    frame.joystick_fire = false;
    frame.joystick_use = false;
    JoystickSample sample{};
    if (!read_joystick(sample, error)) return false;
    if (!focused_ || !joystick_enabled_) return true;
    // 1010:96f6: dead zone 5, Y first, X turns unless button 2 selects strafe.
    if (sample.y < -5)
        frame.joystick_forward = static_cast<std::int16_t>(-5 - sample.y);
    else if (sample.y > 5)
        frame.joystick_forward = static_cast<std::int16_t>(5 - sample.y);
    if (sample.x < -5) {
        if (sample.button2)
            frame.joystick_strafe = static_cast<std::int16_t>(5 + sample.x);
        else
            frame.joystick_turn = static_cast<std::int16_t>(
                -((-5 - sample.x) >> 1));
    } else if (sample.x > 5) {
        if (sample.button2)
            frame.joystick_strafe = static_cast<std::int16_t>(sample.x - 5);
        else
            frame.joystick_turn = static_cast<std::int16_t>(
                (sample.x - 5) >> 1);
    }
    frame.joystick_fire = sample.button1;
    frame.joystick_use = sample.button2;
    return true;
}

bool InputTranslator::sample_menu_joystick(std::uint32_t logical_ms,
                                            std::uint8_t& key,
                                            std::wstring& error) {
    key = 0;
    JoystickSample sample{};
    if (!read_joystick(sample, error)) return false;
    if (!focused_ || !joystick_enabled_) return true;
    // 1010:963e-96f4: button 1 releases to Enter, button 2 presses to Esc;
    // axes use a larger dead zone and an unsigned >200 ms repeat gate.
    if (!sample.button1 && menu_previous_button1_) key = VK_RETURN;
    if (sample.button2 && !menu_previous_button2_) key = VK_ESCAPE;
    if (logical_ms - menu_last_joystick_ms_ > 200) {
        menu_last_joystick_ms_ = logical_ms;
        if (sample.y > 7) key = VK_DOWN;
        else if (sample.y < -7) key = VK_UP;
        else if (sample.x > 7) key = VK_RIGHT;
        else if (sample.x < -7) key = VK_LEFT;
    }
    menu_previous_button2_ = sample.button2 ||
                             (!sample.button1 && menu_previous_button1_);
    menu_previous_button1_ = sample.button1;
    return true;
}

void InputTranslator::focus(bool active) noexcept {
    focused_ = active;
    if (!active) reset();
    refresh_capture();
}

void InputTranslator::reset() noexcept {
    pressed_.fill(false);
    key_ = 0;
    help_scan_code_ = 0;
    help_modifiers_ = 0;
    left_held_ = false;
    right_held_ = false;
    left_inside_ = false;
    mouse_moved_ = false;
    cursor_valid_ = false;
    left_released_ = false;
    right_pressed_ = false;
    relative_turn_ = 0;
    relative_forward_ = 0;
    menu_previous_button1_ = false;
    menu_previous_button2_ = true;
}

void InputTranslator::refresh_capture() noexcept {
    if (!window_) return;
    if (focused_ && (left_held_ || right_held_ || relative_mouse_)) {
        if (GetCapture() != window_) SetCapture(window_);
    } else if (GetCapture() == window_) {
        ReleaseCapture();
    }
}

std::uint8_t InputTranslator::translate_help_key(
    std::uint8_t key, std::uint16_t scan_code, std::uint8_t modifiers) noexcept {
    // 1010:8cd2:8e18 forwards the masked scan code instead of ToAscii for this branch.
    if (scan_code & 0x80u) return static_cast<std::uint8_t>(scan_code & 0x7fu);
    std::array<BYTE, 256> keyboard_state{};
    if (!GetKeyboardState(keyboard_state.data())) return key;
    const auto set_modifier = [&keyboard_state](int generic, int left, int right, bool down) {
        const BYTE value = down ? 0x80u : 0;
        keyboard_state[generic] = value;
        keyboard_state[left] = value;
        keyboard_state[right] = value;
    };
    set_modifier(VK_SHIFT, VK_LSHIFT, VK_RSHIFT, (modifiers & 0x01u) != 0);
    set_modifier(VK_CONTROL, VK_LCONTROL, VK_RCONTROL, (modifiers & 0x02u) != 0);
    set_modifier(VK_MENU, VK_LMENU, VK_RMENU, (modifiers & 0x04u) != 0);
    WORD translated = 0;
    const int count = ToAscii(key, scan_code, keyboard_state.data(), &translated, 0);
    return count == 1 ? static_cast<std::uint8_t>(translated) : key;
}

void InputTranslator::key_event(WPARAM key, LPARAM detail, bool down) noexcept {
    if (key > 255 || !focused_) return;
    auto physical = static_cast<std::uint8_t>(key);
    if (key == VK_SHIFT) {
        physical = ((static_cast<unsigned long>(detail) >> 16) & 0xffu) == 0x36
                       ? VK_RSHIFT : VK_LSHIFT;
    } else if (key == VK_CONTROL) {
        physical = (static_cast<unsigned long>(detail) & (1ul << 24))
                       ? VK_RCONTROL : VK_LCONTROL;
    } else if (key == VK_MENU) {
        physical = (static_cast<unsigned long>(detail) & (1ul << 24))
                       ? VK_RMENU : VK_LMENU;
    }
    pressed_[physical] = down;
    if (!down) return;

    // Gameplay keeps its raw VK action; help translates only when consume(true)
    // is called, so ordinary movement never calls the keyboard layout API.
    key_ = translated_key(key, wasd_);
    help_scan_code_ = static_cast<std::uint16_t>((detail >> 16) & 0xffu);
    help_modifiers_ = 0;
    if (pressed_[VK_LSHIFT] || pressed_[VK_RSHIFT]) help_modifiers_ |= 0x01u;
    if (pressed_[VK_LCONTROL] || pressed_[VK_RCONTROL]) help_modifiers_ |= 0x02u;
    if (pressed_[VK_LMENU] || pressed_[VK_RMENU]) help_modifiers_ |= 0x04u;
}

void InputTranslator::mouse_position(LPARAM detail, const RECT& rect) noexcept {
    if (!focused_) return;
    const int x = GET_X_LPARAM(detail);
    const int y = GET_Y_LPARAM(detail);
    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top;
    const bool inside = width > 0 && height > 0 && x >= rect.left &&
                        x < rect.right && y >= rect.top && y < rect.bottom;
    left_inside_ = inside || relative_mouse_;
    if (width <= 0 || height <= 0) return;
    // 1010:930a stores the signed 16-bit quotient without clamping; a point
    // just outside the client can still truncate to a logical edge pixel.
    const auto mapped_x =
        (static_cast<std::int64_t>(x) - rect.left) * IndexedFrame::kWidth /
        (static_cast<std::int64_t>(width) + 1);
    const auto mapped_y =
        (static_cast<std::int64_t>(y) - rect.top) * IndexedFrame::kHeight /
        (static_cast<std::int64_t>(height) + 1);
    const auto new_x = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(mapped_x));
    const auto new_y = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(mapped_y));
    mouse_moved_ |= new_x != cursor_x_ || new_y != cursor_y_;
    cursor_x_ = new_x;
    cursor_y_ = new_y;
    cursor_valid_ = true;
}

void InputTranslator::raw_mouse(HRAWINPUT handle) noexcept {
    if (!focused_ || !relative_mouse_) return;
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1) ||
        size > sizeof(input) || input.header.dwType != RIM_TYPEMOUSE ||
        (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
    relative_turn_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(relative_turn_) + input.data.mouse.lLastX,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
    relative_forward_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(relative_forward_) - input.data.mouse.lLastY,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

void InputTranslator::message(UINT message, WPARAM wparam, LPARAM lparam,
                              const RECT& image_rect) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        key_event(wparam, lparam, true);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        key_event(wparam, lparam, false);
        break;
    case WM_MOUSEMOVE: {
        mouse_position(lparam, image_rect);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
        TrackMouseEvent(&tracking);
        break;
    }
    case WM_MOUSELEAVE:
        left_inside_ = relative_mouse_ && focused_;
        cursor_valid_ = false;
        break;
    case WM_LBUTTONDOWN:
        mouse_position(lparam, image_rect);
        left_held_ = true;
        refresh_capture();
        break;
    case WM_LBUTTONUP:
        mouse_position(lparam, image_rect);
        left_released_ |= left_held_;
        left_held_ = false;
        refresh_capture();
        break;
    case WM_RBUTTONDOWN:
        mouse_position(lparam, image_rect);
        right_pressed_ |= !right_held_;
        right_held_ = true;
        refresh_capture();
        break;
    case WM_RBUTTONUP:
        mouse_position(lparam, image_rect);
        right_held_ = false;
        refresh_capture();
        break;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lparam) != window_) {
            left_held_ = false;
            right_held_ = false;
            left_inside_ = false;
        }
        break;
    case WM_INPUT:
        raw_mouse(reinterpret_cast<HRAWINPUT>(lparam));
        break;
    default:
        break;
    }
}

std::uint16_t InputTranslator::held_keys() const noexcept {
    auto pressed = [this](int key) { return pressed_[key]; };
    std::uint16_t held = 0;
    if (pressed(VK_ESCAPE)) held |= kEscape;
    if (pressed(VK_UP) || (wasd_ && pressed('W'))) held |= kUp;
    if (pressed(VK_DOWN) || (wasd_ && pressed('S'))) held |= kDown;
    if (pressed(VK_LEFT) || (wasd_ && pressed('A'))) held |= kLeft;
    if (pressed(VK_RIGHT) || (wasd_ && pressed('D'))) held |= kRight;
    if (pressed(VK_RSHIFT)) held |= kRightShift;
    if (pressed(VK_LSHIFT)) held |= kLeftShift;
    if (pressed(VK_LCONTROL) || pressed(VK_RCONTROL)) held |= kControl;
    if (pressed(VK_LMENU) || pressed(VK_RMENU)) held |= kAlt;
    if (pressed(VK_SPACE)) held |= kSpace;
    return held;
}

InputFrame InputTranslator::consume(bool original_help_text) noexcept {
    InputFrame frame{};
    if (!focused_) return frame;
    frame.key = original_help_text && key_
        ? translate_help_key(key_, help_scan_code_, help_modifiers_) : key_;
    frame.held = held_keys();
    frame.mouse_turn = static_cast<std::int16_t>(relative_turn_);
    frame.mouse_forward = static_cast<std::int16_t>(relative_forward_);
    frame.cursor_x = cursor_x_;
    frame.cursor_y = cursor_y_;
    frame.cursor_moved = mouse_moved_;
    frame.cursor_valid = cursor_valid_;
    frame.mouse_left_held = left_held_;
    frame.mouse_relative = relative_mouse_;
    frame.mouse_left_inside = left_held_ && left_inside_;
    frame.mouse_right_held = right_held_;
    frame.menu_left_released = left_released_;
    frame.menu_right_pressed = right_pressed_;
    key_ = 0;
    help_scan_code_ = 0;
    help_modifiers_ = 0;
    relative_turn_ = 0;
    relative_forward_ = 0;
    mouse_moved_ = false;
    left_released_ = false;
    right_pressed_ = false;
    return frame;
}

bool InputTranslator::physical_buttons_released() const noexcept {
    // Focus changes clear the queued translator state but cannot release a
    // physical key. Recheck the OS state before enabling a restored clock.
    if (left_held_ || right_held_ ||
        std::any_of(pressed_.begin(), pressed_.end(), [](bool down) { return down; }))
        return false;
    for (int key = VK_LBUTTON; key <= VK_OEM_CLEAR; ++key) {
        if (GetAsyncKeyState(key) & 0x8000) return false;
    }
    return true;
}


} // namespace n3d::platform
