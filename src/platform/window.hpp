#pragma once

#include "input.hpp"
#include "n3d/frame.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace n3d::platform {

// The only owner of the HWND and presentation DIB. All calls must be made on
// the window's creating thread. The caller owns simulation and update timing.
class Window final {
public:
    Window() noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    [[nodiscard]] bool create(HINSTANCE instance, const wchar_t* title,
                              std::wstring& error);
    void destroy() noexcept;
    void show(int command = SW_SHOWNORMAL) noexcept;
    // Returns false on WM_QUIT (empty error) or a window event failure
    // (populated error); dispatches pending messages without blocking.
    [[nodiscard]] bool pump_messages(std::wstring& error);
    [[nodiscard]] bool present(const IndexedFrame& frame, std::wstring& error);
    void set_square_pixels(bool enabled) noexcept;
    [[nodiscard]] bool square_pixels() const noexcept { return square_pixels_; }
    [[nodiscard]] bool toggle_borderless(std::wstring& error);
    [[nodiscard]] bool borderless() const noexcept { return borderless_; }
    [[nodiscard]] DWORD paint_error() const noexcept { return paint_error_; }
    [[nodiscard]] HWND native_handle() const noexcept { return window_; }
    [[nodiscard]] InputTranslator& input() noexcept { return input_; }

private:
    struct DibInfo final {
        BITMAPINFOHEADER header{};
        RGBQUAD colors[256]{};
    } dib_{};

    [[nodiscard]] static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                                        WPARAM wparam, LPARAM lparam);
    [[nodiscard]] LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam);
    [[nodiscard]] RECT image_rect() const noexcept;
    [[nodiscard]] RECT image_rect(const RECT& client) const noexcept;
    [[nodiscard]] bool paint(HDC dc) noexcept;
    [[nodiscard]] bool change_style(LONG_PTR style, LONG_PTR ex_style,
                                     const RECT& bounds) noexcept;

    HWND window_ = nullptr;
    InputTranslator input_{};
    std::array<std::uint8_t, IndexedFrame::kPixelCount> pixels_;
    WINDOWPLACEMENT restored_placement_{sizeof(WINDOWPLACEMENT)};
    RECT restored_bounds_{};
    LONG_PTR restored_style_ = 0;
    LONG_PTR restored_ex_style_ = 0;
    bool created_ = false;
    bool has_frame_ = false;
    bool square_pixels_ = false;
    bool borderless_ = false;
    DWORD paint_error_ = ERROR_SUCCESS;
    std::wstring pending_error_;
};

} // namespace n3d::platform
