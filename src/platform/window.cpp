#include "window.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace n3d::platform {
namespace {
constexpr wchar_t kClassName[] = L"Nitemare3D.Native.IndexedWindow";

std::wstring win32_error(const wchar_t* action, DWORD code) {
    return std::wstring(action) + L" failed (Win32 " + std::to_wstring(code) + L")";
}

DWORD gdi_error() noexcept {
    const DWORD code = GetLastError();
    return code ? code : ERROR_GEN_FAILURE;
}

// Fit a physical screen rectangle to the target monitor's work area.
RECT constrain_rect(RECT rectangle, const RECT& work) noexcept {
    const auto work_width = work.right - work.left;
    const auto work_height = work.bottom - work.top;
    const auto width = std::clamp(rectangle.right - rectangle.left, 1L, work_width);
    const auto height = std::clamp(rectangle.bottom - rectangle.top, 1L, work_height);
    rectangle.left = std::clamp(rectangle.left, work.left, work.right - width);
    rectangle.top = std::clamp(rectangle.top, work.top, work.bottom - height);
    rectangle.right = rectangle.left + width;
    rectangle.bottom = rectangle.top + height;
    return rectangle;
}

bool set_window_style(HWND window, int index, LONG_PTR value) noexcept {
    SetLastError(ERROR_SUCCESS);
    return SetWindowLongPtrW(window, index, value) != 0 ||
           GetLastError() == ERROR_SUCCESS;
}
} // namespace

Window::Window() noexcept {
    dib_.header.biSize = sizeof(BITMAPINFOHEADER);
    dib_.header.biWidth = static_cast<LONG>(IndexedFrame::kWidth);
    dib_.header.biHeight = -static_cast<LONG>(IndexedFrame::kHeight);
    dib_.header.biPlanes = 1;
    dib_.header.biBitCount = 8;
    dib_.header.biCompression = BI_RGB;
    dib_.header.biSizeImage = static_cast<DWORD>(IndexedFrame::kPixelCount);
    dib_.header.biClrUsed = 256;
    dib_.header.biClrImportant = 256;
}

Window::~Window() { destroy(); }

bool Window::create(HINSTANCE instance, const wchar_t* title,
                    std::wstring& error) {
    error.clear();
    if (window_) {
        error = L"The native window is already created";
        return false;
    }
    if (!instance || !title) {
        error = L"A Win32 instance and title are required";
        return false;
    }
    pending_error_.clear();
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    cls.lpszClassName = kClassName;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = win32_error(L"RegisterClassExW", GetLastError());
        return false;
    }
    RECT bounds{0, 0, 960, 720};
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    if (!AdjustWindowRectEx(&bounds, style, FALSE, 0)) {
        error = win32_error(L"AdjustWindowRectEx", GetLastError());
        return false;
    }
    HWND handle = CreateWindowExW(0, kClassName, title, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  bounds.right - bounds.left,
                                  bounds.bottom - bounds.top,
                                  nullptr, nullptr, instance, this);
    if (!handle) {
        error = win32_error(L"CreateWindowExW", GetLastError());
        return false;
    }
    // NCCREATE initialized window_; clamp the initial placement on small screens.
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST),
                         &monitor)) {
        error = win32_error(L"GetMonitorInfoW(initial)", GetLastError());
        destroy();
        return false;
    }
    RECT actual{};
    if (!GetWindowRect(handle, &actual)) {
        error = win32_error(L"GetWindowRect(initial)", GetLastError());
        destroy();
        return false;
    }
    const RECT fitted = constrain_rect(actual, monitor.rcWork);
    if (!SetWindowPos(handle, nullptr, fitted.left, fitted.top,
                      fitted.right - fitted.left, fitted.bottom - fitted.top,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)) {
        error = win32_error(L"SetWindowPos(initial)", GetLastError());
        destroy();
        return false;
    }
    created_ = true;
    return true;
}

void Window::destroy() noexcept {
    if (window_) DestroyWindow(window_);
    input_.detach();
}

void Window::show(int command) noexcept {
    if (window_) {
        ShowWindow(window_, command);
        UpdateWindow(window_);
    }
}

bool Window::pump_messages(std::wstring& error) {
    error.clear();
    if (!pending_error_.empty()) {
        error.swap(pending_error_);
        return false;
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (!pending_error_.empty()) {
            error.swap(pending_error_);
            return false;
        }
    }
    return true;
}

RECT Window::image_rect() const noexcept {
    RECT client{};
    if (!window_ || !GetClientRect(window_, &client)) return client;
    return image_rect(client);
}

RECT Window::image_rect(const RECT& client) const noexcept {
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return RECT{};
    const std::int64_t aspect_x = square_pixels_ ? 8 : 4;
    const std::int64_t aspect_y = square_pixels_ ? 5 : 3;
    LONG image_width = width;
    LONG image_height = height;
    if (static_cast<std::int64_t>(width) * aspect_y >
        static_cast<std::int64_t>(height) * aspect_x) {
        image_width = std::max<LONG>(1, static_cast<LONG>(
            static_cast<std::int64_t>(height) * aspect_x / aspect_y));
    } else {
        image_height = std::max<LONG>(1, static_cast<LONG>(
            static_cast<std::int64_t>(width) * aspect_y / aspect_x));
    }
    const LONG left = (width - image_width) / 2;
    const LONG top = (height - image_height) / 2;
    return RECT{left, top, left + image_width, top + image_height};
}

bool Window::paint(HDC dc) noexcept {
    RECT client{};
    if (!GetClientRect(window_, &client)) {
        paint_error_ = gdi_error();
        return false;
    }
    if (client.right <= client.left || client.bottom <= client.top) {
        paint_error_ = ERROR_SUCCESS;
        return true;
    }
    const auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!has_frame_) {
        if (!FillRect(dc, &client, black)) {
            paint_error_ = gdi_error();
            return false;
        }
        paint_error_ = ERROR_SUCCESS;
        return true;
    }
    const RECT dest = image_rect(client);
    if (dest.right <= dest.left || dest.bottom <= dest.top) {
        paint_error_ = ERROR_SUCCESS;
        return true;
    }
    // Only repaint bars; the DIB fills the image rectangle below.
    const RECT bars[] = {
        {0, 0, client.right, dest.top},
        {0, dest.bottom, client.right, client.bottom},
        {0, dest.top, dest.left, dest.bottom},
        {dest.right, dest.top, client.right, dest.bottom},
    };
    for (const RECT& bar : bars) {
        if (bar.right > bar.left && bar.bottom > bar.top &&
            !FillRect(dc, &bar, black)) {
            paint_error_ = gdi_error();
            return false;
        }
    }
    if (!SetStretchBltMode(dc, COLORONCOLOR)) {
        paint_error_ = gdi_error();
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const int result = StretchDIBits(dc, dest.left, dest.top,
                                     dest.right - dest.left, dest.bottom - dest.top,
                                     0, 0, static_cast<int>(IndexedFrame::kWidth),
                                     static_cast<int>(IndexedFrame::kHeight),
                                     pixels_.data(),
                                     reinterpret_cast<const BITMAPINFO*>(&dib_),
                                     DIB_RGB_COLORS, SRCCOPY);
    if (result == GDI_ERROR || result == 0) {
        paint_error_ = gdi_error();
        return false;
    }
    paint_error_ = ERROR_SUCCESS;
    return true;
}

bool Window::present(const IndexedFrame& frame, std::wstring& error) {
    error.clear();
    if (!window_) {
        error = L"Cannot present without a created native window";
        return false;
    }
    // Keep the original indices, not pre-expanded RGB pixels, for WM_PAINT.
    std::memcpy(pixels_.data(), frame.pixels.data(),
                IndexedFrame::kPixelCount);
    for (std::size_t i = 0; i < frame.palette.size(); ++i) {
        const Rgb8 color = frame.palette[i];
        dib_.colors[i] = RGBQUAD{color.b, color.g, color.r, 0};
    }
    has_frame_ = true;
    HDC dc = GetDC(window_);
    if (!dc) {
        error = win32_error(L"GetDC(present)", GetLastError());
        return false;
    }
    const bool painted = paint(dc);
    ReleaseDC(window_, dc);
    if (!painted) {
        error = win32_error(L"GDI present", paint_error_);
        return false;
    }
    return true;
}

void Window::set_square_pixels(bool enabled) noexcept {
    if (square_pixels_ != enabled) {
        square_pixels_ = enabled;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
    }
}

bool Window::change_style(LONG_PTR style, LONG_PTR ex_style,
                          const RECT& bounds) noexcept {
    if (!set_window_style(window_, GWL_STYLE, style) ||
        !set_window_style(window_, GWL_EXSTYLE, ex_style)) return false;
    return SetWindowPos(window_, nullptr, bounds.left, bounds.top,
                        bounds.right - bounds.left, bounds.bottom - bounds.top,
                        SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER |
                        SWP_NOOWNERZORDER) != 0;
}

bool Window::toggle_borderless(std::wstring& error) {
    error.clear();
    if (!window_) {
        error = L"Cannot toggle fullscreen without a created native window";
        return false;
    }
    if (!borderless_) {
        restored_placement_.length = sizeof(restored_placement_);
        if (!GetWindowPlacement(window_, &restored_placement_)) {
            error = win32_error(L"GetWindowPlacement", GetLastError());
            return false;
        }
        if (!GetWindowRect(window_, &restored_bounds_)) {
            error = win32_error(L"GetWindowRect", GetLastError());
            return false;
        }
        restored_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
        restored_ex_style_ = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                             &monitor)) {
            error = win32_error(L"GetMonitorInfoW", GetLastError());
            return false;
        }
        const LONG_PTR borderless_style =
            (restored_style_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) |
            WS_POPUP;
        const LONG_PTR borderless_ex_style =
            restored_ex_style_ & ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
        if (!change_style(borderless_style, borderless_ex_style,
                          monitor.rcMonitor)) {
            const DWORD code = GetLastError();
            (void)change_style(restored_style_, restored_ex_style_, restored_bounds_);
            error = win32_error(L"Entering borderless fullscreen", code);
            return false;
        }
        borderless_ = true;
    } else {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                             &monitor)) {
            error = win32_error(L"GetMonitorInfoW", GetLastError());
            return false;
        }
        const bool was_maximized = restored_placement_.showCmd == SW_SHOWMAXIMIZED;
        if (!set_window_style(window_, GWL_STYLE, restored_style_) ||
            !set_window_style(window_, GWL_EXSTYLE, restored_ex_style_) ||
            !SetWindowPlacement(window_, &restored_placement_)) {
            error = win32_error(L"Restoring window placement", GetLastError());
            return false;
        }
        // WINDOWPLACEMENT stores workspace coordinates, not screen coordinates;
        // ask Win32 to restore first, then constrain its physical rectangle.
        ShowWindow(window_, SW_RESTORE);
        RECT normal{};
        if (!GetWindowRect(window_, &normal)) {
            error = win32_error(L"GetWindowRect(restored)", GetLastError());
            return false;
        }
        const RECT fitted = constrain_rect(normal, monitor.rcWork);
        if (!SetWindowPos(window_, nullptr, fitted.left, fitted.top,
                          fitted.right - fitted.left, fitted.bottom - fitted.top,
                          SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER |
                          SWP_NOOWNERZORDER)) {
            error = win32_error(L"SetWindowPos(restored)", GetLastError());
            return false;
        }
        if (was_maximized) ShowWindow(window_, SW_MAXIMIZE);
        borderless_ = false;
    }
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK Window::window_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Window*>(creation->lpCreateParams);
        self->window_ = window;
        self->input_.attach(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->dispatch(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Window::dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        const HDC dc = BeginPaint(window_, &ps);
        if (dc) {
            (void)paint(dc);
            EndPaint(window_, &ps);
        } else {
            paint_error_ = gdi_error();
        }
        if (paint_error_ != ERROR_SUCCESS)
            pending_error_ = win32_error(L"WM_PAINT", paint_error_);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_DISPLAYCHANGE: {
        if (IsIconic(window_)) return 0;
        if (!borderless_ && IsZoomed(window_)) return 0;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                             &monitor)) {
            pending_error_ = win32_error(L"GetMonitorInfoW(display)", GetLastError());
            return 0;
        }
        RECT current{};
        if (!GetWindowRect(window_, &current)) {
            pending_error_ = win32_error(L"GetWindowRect(display)", GetLastError());
            return 0;
        }
        const RECT fitted = borderless_ ? monitor.rcMonitor :
                            constrain_rect(current, monitor.rcWork);
        if (!SetWindowPos(window_, nullptr, fitted.left, fitted.top,
                          fitted.right - fitted.left, fitted.bottom - fitted.top,
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
            pending_error_ = win32_error(L"SetWindowPos(display)", GetLastError());
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST),
                             &monitor)) {
            pending_error_ = win32_error(L"GetMonitorInfoW(DPI)", GetLastError());
            return 0;
        }
        const RECT fitted = borderless_ ? monitor.rcMonitor :
                            constrain_rect(*suggested, monitor.rcWork);
        if (!SetWindowPos(window_, nullptr, fitted.left, fitted.top,
                          fitted.right - fitted.left, fitted.bottom - fitted.top,
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
            pending_error_ = win32_error(L"SetWindowPos(DPI)", GetLastError());
        return 0;
    }
    case WM_SETFOCUS:
        input_.focus(true);
        return 0;
    case WM_KILLFOCUS:
        input_.focus(false);
        return 0;
    case WM_ACTIVATEAPP:
        input_.focus(wparam != 0);
        return 0;
    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN && (lparam & (1LL << 29))) {
            if (!(lparam & (1LL << 30))) {
                std::wstring error;
                if (!toggle_borderless(error)) pending_error_ = std::move(error);
            }
            return 0;
        }
        [[fallthrough]];
    case WM_SYSKEYUP:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_INPUT:
        input_.message(message, wparam, lparam, image_rect());
        if (message == WM_INPUT) break; // DefWindowProcW performs raw-input cleanup.
        return 0;
    case WM_SYSCHAR:
        return 0; // Do not open the system menu or beep for Alt+Enter.
    case WM_DESTROY:
        input_.focus(false);
        if (created_) PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        HWND dead = window_;
        input_.detach();
        window_ = nullptr;
        created_ = false;
        has_frame_ = false;
        borderless_ = false;
        SetWindowLongPtrW(dead, GWLP_USERDATA, 0);
        return DefWindowProcW(dead, message, wparam, lparam);
    }
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

} // namespace n3d::platform
