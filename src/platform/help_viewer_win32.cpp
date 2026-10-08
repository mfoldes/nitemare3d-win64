#include "help_viewer_win32.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <commdlg.h>
#include <windowsx.h>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "n3d/winhelp.hpp"

namespace n3d::platform {
namespace {

constexpr wchar_t kHelpClass[] = L"Nitemare3DMapsHelp";
constexpr wchar_t kTopicClass[] = L"Nitemare3DMapsHelpTopic";
constexpr wchar_t kSearchClass[] = L"Nitemare3DMapsHelpSearch";
constexpr int kToolbarHeight = 38;
constexpr int kMargin = 8;
constexpr UINT kContents = 201;
constexpr UINT kSearch = 202;
constexpr UINT kBack = 203;
constexpr UINT kPrint = 204;
constexpr UINT kPrevious = 205;
constexpr UINT kNext = 206;
constexpr UINT kGuide = 207;
constexpr UINT kClose = 208;
constexpr UINT kSearchEdit = 301;
constexpr UINT kKeywords = 302;
constexpr UINT kMatches = 303;
constexpr UINT kShowTopics = 304;
constexpr UINT kGoTo = 305;
constexpr UINT kSearchClose = 306;

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(1252, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Cannot decode maps.hlp text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(1252, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

struct Fragment {
    RECT bounds{}; // Document coordinates, not scrolled coordinates.
    std::wstring text;
    std::size_t font = 0;
    winhelp::HotspotKind hotspot = winhelp::HotspotKind::None;
    std::optional<std::size_t> target;
};

struct HelpState;
struct TopicState {
    HelpState* help = nullptr;
    HWND hwnd = nullptr;
    std::size_t topic = 0;
    bool popup = false;
    bool owned_by_window = false;
    int scroll = 0;
    int height = 0;
    std::vector<Fragment> fragments;
};

struct HelpState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND search = nullptr;
    HWND popup = nullptr;
    HINSTANCE instance = nullptr;
    bool owned_by_window = false;
    winhelp::Document document;
    std::vector<HFONT> fonts;
    TopicState body;
    std::vector<std::size_t> history;
    std::size_t current = 0;

    ~HelpState() {
        for (HFONT font : fonts) if (font) DeleteObject(font);
    }
};

struct SearchState {
    HelpState* help = nullptr;
    bool owned_by_window = false;
    HWND edit = nullptr;
    HWND keywords = nullptr;
    HWND topics = nullptr;
};

HFONT make_font(const winhelp::Font& source, int dpi, bool hotspot) {
    // The source's zero-height Arial descriptor inherits the ordinary Help body size.
    const int half_points = source.half_points ? source.half_points : 24;
    const std::wstring face = wide(source.face);
    return CreateFontW(-MulDiv(half_points, dpi, 144), 0, 0, 0,
                       source.attributes & 1 ? FW_BOLD : FW_NORMAL,
                       (source.attributes & 2) != 0,
                       hotspot || (source.attributes & 4), FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face.c_str());
}

COLORREF foreground(const winhelp::Font& font, bool hotspot) {
    if (hotspot) return RGB(0, 128, 0); // WinHelp's standard link color.
    if (font.foreground == std::array<std::uint8_t, 3>{1, 1, 0})
        return GetSysColor(COLOR_WINDOWTEXT);
    return RGB(font.foreground[0], font.foreground[1], font.foreground[2]);
}

std::vector<HFONT> create_fonts(const winhelp::Document& doc, int dpi) {
    std::vector<HFONT> fonts;
    fonts.reserve(doc.fonts.size() * 2);
    for (const auto& descriptor : doc.fonts) {
        HFONT normal = make_font(descriptor, dpi, false);
        HFONT link = make_font(descriptor, dpi, true);
        if (!normal || !link) {
            if (normal) DeleteObject(normal);
            if (link) DeleteObject(link);
            for (HFONT font : fonts) DeleteObject(font);
            throw std::runtime_error("Cannot create Help text fonts");
        }
        fonts.push_back(normal);
        fonts.push_back(link);
    }
    return fonts;
}

void layout_topic(TopicState& body, HDC dc, const std::vector<HFONT>& fonts, int width,
                  int margin) {
    body.fragments.clear();
    int x = margin;
    int y = margin;
    int line_height = MulDiv(19, GetDeviceCaps(dc, LOGPIXELSY), 96);
    const int default_height = line_height;
    const int right = std::max(margin + 1, width - margin);
    const auto& paragraphs = body.help->document.topics[body.topic].paragraphs;
    for (const auto& para : paragraphs) {
        for (const auto& run : para.runs) {
            const bool link = run.hotspot != winhelp::HotspotKind::None;
            HGDIOBJ previous = SelectObject(dc, fonts[run.font_index * 2 + (link ? 1 : 0)]);
            TEXTMETRICW metric{};
            GetTextMetricsW(dc, &metric);
            const int glyph_height = metric.tmHeight;
            std::wstring text = wide(run.text);
            std::size_t start = 0;
            while (start < text.size()) {
                // Keep the source spaces intact. Split into words and whitespace for wrapping.
                const bool space = text[start] == L' ';
                std::size_t end = start + 1;
                while (end < text.size() && (text[end] == L' ') == space) ++end;
                std::wstring part = text.substr(start, end - start);
                SIZE extent{};
                GetTextExtentPoint32W(dc, part.c_str(), static_cast<int>(part.size()), &extent);
                if (!space && x > margin && x + extent.cx > right) {
                    x = margin;
                    y += line_height;
                    line_height = default_height;
                }
                // Long unbroken words are split at character boundaries instead of clipping.
                std::size_t offset = 0;
                while (offset < part.size()) {
                    std::size_t length = part.size() - offset;
                    SIZE fit_extent{};
                    GetTextExtentPoint32W(dc, part.data() + offset, static_cast<int>(length), &fit_extent);
                    while (length > 1 && x + fit_extent.cx > right) {
                        --length;
                        GetTextExtentPoint32W(dc, part.data() + offset, static_cast<int>(length), &fit_extent);
                    }
                    if (x + fit_extent.cx > right && x > margin) {
                        x = margin;
                        y += line_height;
                        line_height = default_height;
                        continue;
                    }
                    Fragment fragment;
                    fragment.bounds = {x, y, x + fit_extent.cx, y + glyph_height};
                    fragment.text = part.substr(offset, length);
                    fragment.font = run.font_index;
                    fragment.hotspot = run.hotspot;
                    fragment.target = run.target_topic;
                    body.fragments.push_back(std::move(fragment));
                    x += fit_extent.cx;
                    line_height = std::max(line_height, glyph_height);
                    offset += length;
                    if (offset < part.size()) {
                        x = margin;
                        y += line_height;
                        line_height = default_height;
                    }
                }
                start = end;
            }
            if (run.tab) {
                const int step = MulDiv(64, GetDeviceCaps(dc, LOGPIXELSX), 96);
                x = margin + ((x - margin) / step + 1) * step;
            }
            SelectObject(dc, previous);
        }
        x = margin;
        y += line_height;
        line_height = default_height;
    }
    body.height = y + margin;
}

void draw_topic(const TopicState& body, HDC dc, const std::vector<HFONT>& fonts,
                int vertical_offset, const RECT& clip) {
    SetBkMode(dc, TRANSPARENT);
    for (const Fragment& fragment : body.fragments) {
        const int y = fragment.bounds.top - vertical_offset;
        if (y + (fragment.bounds.bottom - fragment.bounds.top) < clip.top || y >= clip.bottom) continue;
        const auto& descriptor = body.help->document.fonts[fragment.font];
        const bool link = fragment.hotspot != winhelp::HotspotKind::None;
        HGDIOBJ old = SelectObject(dc, fonts[fragment.font * 2 + (link ? 1 : 0)]);
        const COLORREF old_color = SetTextColor(dc, foreground(descriptor, link));
        TextOutW(dc, fragment.bounds.left, y, fragment.text.c_str(), static_cast<int>(fragment.text.size()));
        SetTextColor(dc, old_color);
        SelectObject(dc, old);
    }
}

void update_scroll(TopicState& body) {
    RECT client{};
    GetClientRect(body.hwnd, &client);
    const int page = std::max(1, static_cast<int>(client.bottom));
    body.scroll = std::clamp(body.scroll, 0, std::max(0, body.height - page));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = std::max(0, body.height - 1);
    info.nPage = static_cast<UINT>(page);
    info.nPos = body.scroll;
    SetScrollInfo(body.hwnd, SB_VERT, &info, TRUE);
}

void relayout(TopicState& body) {
    if (!body.hwnd) return;
    RECT client{};
    GetClientRect(body.hwnd, &client);
    HDC dc = GetDC(body.hwnd);
    if (!dc) return;
    layout_topic(body, dc, body.help->fonts, client.right, kMargin);
    ReleaseDC(body.hwnd, dc);
    update_scroll(body);
    InvalidateRect(body.hwnd, nullptr, TRUE);
}

void refresh_buttons(HelpState& help) {
    if (!help.hwnd) return;
    const auto& topic = help.document.topics[help.current];
    EnableWindow(GetDlgItem(help.hwnd, kBack), !help.history.empty());
    EnableWindow(GetDlgItem(help.hwnd, kPrevious), topic.browse_previous.has_value());
    EnableWindow(GetDlgItem(help.hwnd, kNext), topic.browse_next.has_value());
}

void navigate(HelpState& help, std::size_t target, bool remember = true) {
    if (target >= help.document.topics.size()) return;
    if (help.popup) DestroyWindow(help.popup);
    if (remember && target != help.current) help.history.push_back(help.current);
    help.current = target;
    help.body.topic = target;
    help.body.scroll = 0;
    relayout(help.body);
    refresh_buttons(help);
}

void show_popup(HelpState& help, std::size_t target, POINT screen) {
    if (target >= help.document.topics.size()) return;
    if (help.popup) DestroyWindow(help.popup);
    auto popup = std::make_unique<TopicState>();
    popup->help = &help;
    popup->topic = target;
    popup->popup = true;
    const int width = 510;
    const int height = 330;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    screen.x = std::clamp(screen.x, work.left, std::max(work.left, work.right - width));
    screen.y = std::clamp(screen.y, work.top, std::max(work.top, work.bottom - height));
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTopicClass,
        wide(help.document.topics[target].title).c_str(), WS_POPUP | WS_BORDER | WS_VSCROLL,
        screen.x, screen.y, width, height, help.hwnd, nullptr, help.instance, popup.get());
    if (!hwnd) {
        MessageBoxW(help.hwnd, L"Unable to display this Help popup.", L"Maps Help", MB_OK | MB_ICONERROR);
        return;
    }
    help.popup = hwnd;
    popup->owned_by_window = true;
    popup.release();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void activate_at(TopicState& body, POINT point) {
    POINT screen = point;
    ClientToScreen(body.hwnd, &screen);
    point.y += body.scroll;
    for (const Fragment& fragment : body.fragments) {
        if (fragment.target && PtInRect(&fragment.bounds, point)) {
            HelpState* help = body.help;
            const auto target = *fragment.target;
            const auto kind = fragment.hotspot;
            if (body.popup) DestroyWindow(body.hwnd);
            if (kind == winhelp::HotspotKind::Popup) {
                show_popup(*help, target, screen);
            } else {
                navigate(*help, target);
                SetFocus(help->body.hwnd);
            }
            return;
        }
    }
    if (body.popup) DestroyWindow(body.hwnd);
}

LRESULT CALLBACK topic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* body = static_cast<TopicState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        body->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(body));
        return TRUE;
    }
    auto* body = reinterpret_cast<TopicState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!body) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_SIZE: relayout(*body); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        draw_topic(*body, dc, body->help->fonts, body->scroll, paint.rcPaint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &info);
        switch (LOWORD(wp)) {
        case SB_LINEUP: body->scroll -= 20; break;
        case SB_LINEDOWN: body->scroll += 20; break;
        case SB_PAGEUP: body->scroll -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: body->scroll += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: body->scroll = info.nTrackPos; break;
        default: break;
        }
        update_scroll(*body);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        body->scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 60;
        update_scroll(*body);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp) + body->scroll};
        bool hotspot = false;
        for (const auto& fragment : body->fragments)
            if (fragment.target && PtInRect(&fragment.bounds, point)) { hotspot = true; break; }
        SetCursor(LoadCursorW(nullptr, hotspot ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: activate_at(*body, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(body->popup ? hwnd : body->help->hwnd); return 0; }
        if (wp == VK_F1) { if (!body->popup) navigate(*body->help, body->help->document.contents_topic); return 0; }
        if (wp == VK_BACK) {
            if (body->popup) { DestroyWindow(hwnd); return 0; }
            if (!body->help->history.empty()) {
                const auto target = body->help->history.back();
                body->help->history.pop_back();
                navigate(*body->help, target, false);
            }
            return 0;
        }
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            body->scroll += wp == VK_PRIOR ? -300 : wp == VK_NEXT ? 300 : wp == VK_UP ? -20 : 20;
            update_scroll(*body);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (body->popup && LOWORD(wp) == WA_INACTIVE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_NCDESTROY:
        if (body->popup) {
            if (body->help->popup == hwnd) body->help->popup = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (body->owned_by_window) delete body;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void fill_matches(SearchState& search) {
    SendMessageW(search.topics, LB_RESETCONTENT, 0, 0);
    const LRESULT row = SendMessageW(search.keywords, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return;
    const LRESULT key = SendMessageW(search.keywords, LB_GETITEMDATA, row, 0);
    if (key < 0 || static_cast<std::size_t>(key) >= search.help->document.keywords.size()) return;
    for (std::size_t topic : search.help->document.keywords[static_cast<std::size_t>(key)].topics) {
        const auto& title = search.help->document.topics[topic].title;
        const LRESULT item = SendMessageW(search.topics, LB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(wide(title).c_str()));
        if (item != LB_ERR && item != LB_ERRSPACE) SendMessageW(search.topics, LB_SETITEMDATA, item, topic);
    }
    SendMessageW(search.topics, LB_SETCURSEL, 0, 0);
}

void filter_keywords(SearchState& search) {
    wchar_t typed[256]{};
    GetWindowTextW(search.edit, typed, static_cast<int>(std::size(typed)));
    std::wstring prefix(typed);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    SendMessageW(search.keywords, LB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < search.help->document.keywords.size(); ++index) {
        const auto& keyword = search.help->document.keywords[index];
        std::wstring label = wide(keyword.text);
        std::wstring folded = label;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (folded.compare(0, prefix.size(), prefix) != 0) continue;
        const LRESULT row = SendMessageW(search.keywords, LB_ADDSTRING, 0,
                                          reinterpret_cast<LPARAM>(label.c_str()));
        if (row != LB_ERR && row != LB_ERRSPACE) SendMessageW(search.keywords, LB_SETITEMDATA, row, index);
    }
    SendMessageW(search.keywords, LB_SETCURSEL, 0, 0);
    fill_matches(search);
}

void go_to_match(SearchState& search) {
    const LRESULT row = SendMessageW(search.topics, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return;
    const LRESULT target = SendMessageW(search.topics, LB_GETITEMDATA, row, 0);
    if (target < 0 || static_cast<std::size_t>(target) >= search.help->document.topics.size()) return;
    navigate(*search.help, static_cast<std::size_t>(target));
    SetForegroundWindow(search.help->hwnd);
    SetFocus(search.help->body.hwnd);
}

LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* search = static_cast<SearchState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(search));
        return TRUE;
    }
    auto* search = reinterpret_cast<SearchState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!search) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE: {
        auto control = [hwnd, search](const wchar_t* kind, const wchar_t* title, DWORD style,
                                      int x, int y, int w, int h, int id) {
            return CreateWindowExW(WS_EX_CLIENTEDGE, kind, title, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   search->help->instance, nullptr);
        };
        control(L"STATIC", L"Keyword:", 0, 12, 12, 100, 18, 0);
        search->edit = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 12, 32, 245, 24, kSearchEdit);
        control(L"STATIC", L"Keywords", 0, 12, 66, 120, 18, 0);
        control(L"STATIC", L"Matching topics", 0, 270, 66, 190, 18, 0);
        search->keywords = control(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
                                   12, 86, 245, 230, kKeywords);
        search->topics = control(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
                                 270, 86, 245, 230, kMatches);
        control(L"BUTTON", L"Show Topics", WS_TABSTOP, 270, 32, 114, 26, kShowTopics);
        control(L"BUTTON", L"Go To", WS_TABSTOP, 390, 32, 58, 26, kGoTo);
        control(L"BUTTON", L"Close", WS_TABSTOP, 451, 32, 64, 26, kSearchClose);
        filter_keywords(*search);
        SetFocus(search->edit);
        return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wp);
        if (id == kSearchEdit && HIWORD(wp) == EN_CHANGE) filter_keywords(*search);
        else if (id == kKeywords && (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK)) fill_matches(*search);
        else if (id == kMatches && HIWORD(wp) == LBN_DBLCLK) go_to_match(*search);
        else if (id == kShowTopics) fill_matches(*search);
        else if (id == kGoTo) go_to_match(*search);
        else if (id == kSearchClose) DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; } break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_NCDESTROY:
        if (search->help->search == hwnd) search->help->search = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (search->owned_by_window) delete search;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void open_search(HelpState& help) {
    if (help.search && IsWindow(help.search)) { SetForegroundWindow(help.search); return; }
    auto search = std::make_unique<SearchState>();
    search->help = &help;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kSearchClass, L"Search Maps Help",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        542, 365, help.hwnd, nullptr, help.instance, search.get());
    if (!window) {
        MessageBoxW(help.hwnd, L"Unable to open the Help keyword search.", L"Maps Help", MB_OK | MB_ICONERROR);
        return;
    }
    help.search = window;
    search->owned_by_window = true;
    search.release();
    ShowWindow(window, SW_SHOW);
}

void print_topic(HelpState& help) {
    PRINTDLGW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = help.hwnd;
    dialog.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
    if (!PrintDlgW(&dialog)) {
        if (CommDlgExtendedError() != 0)
            MessageBoxW(help.hwnd, L"No available printer or the Windows print dialog failed. No page was printed.",
                        L"Maps Help printing", MB_OK | MB_ICONWARNING);
        return; // User canceled the system print dialog.
    }
    HDC printer = dialog.hDC;
    if (dialog.hDevMode) GlobalFree(dialog.hDevMode);
    if (dialog.hDevNames) GlobalFree(dialog.hDevNames);
    if (!printer) {
        MessageBoxW(help.hwnd, L"No printer device was selected. No page was printed.",
                    L"Maps Help printing", MB_OK | MB_ICONWARNING);
        return;
    }
    std::vector<HFONT> fonts;
    try {
        fonts = create_fonts(help.document, GetDeviceCaps(printer, LOGPIXELSY));
        TopicState page;
        page.help = &help;
        page.topic = help.current;
        const int x_margin = GetDeviceCaps(printer, LOGPIXELSX) / 2;
        const int y_margin = GetDeviceCaps(printer, LOGPIXELSY) / 2;
        const int page_height = std::max(1, GetDeviceCaps(printer, VERTRES) - y_margin * 2);
        const int page_width = GetDeviceCaps(printer, HORZRES);
        layout_topic(page, printer, fonts, page_width - x_margin, x_margin);
        DOCINFOW info{};
        info.cbSize = sizeof(info);
        const std::wstring name = wide(help.document.topics[help.current].title);
        info.lpszDocName = name.c_str();
        if (StartDocW(printer, &info) <= 0) throw std::runtime_error("The printer could not start the Help document");
        bool failed = false;
        for (int offset = 0; offset < page.height; offset += page_height) {
            if (StartPage(printer) <= 0) { failed = true; break; }
            const int saved = SaveDC(printer);
            IntersectClipRect(printer, 0, y_margin, page_width, page_height + y_margin);
            SetViewportOrgEx(printer, 0, y_margin, nullptr);
            RECT clipping{0, 0, page_width, page_height};
            draw_topic(page, printer, fonts, offset + x_margin, clipping);
            RestoreDC(printer, saved);
            if (EndPage(printer) <= 0) { failed = true; break; }
        }
        if (failed) {
            AbortDoc(printer);
            throw std::runtime_error("The printer could not complete the Help page");
        }
        if (EndDoc(printer) <= 0) throw std::runtime_error("The printer could not finish the Help document");
    } catch (const std::exception& error) {
        MessageBoxW(help.hwnd, wide(error.what()).c_str(), L"Maps Help printing", MB_OK | MB_ICONERROR);
    }
    for (HFONT font : fonts) DeleteObject(font);
    DeleteDC(printer);
}

LRESULT CALLBACK help_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* help = static_cast<HelpState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        help->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(help));
        return TRUE;
    }
    auto* help = reinterpret_cast<HelpState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!help) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE: {
        constexpr std::array<std::pair<UINT, const wchar_t*>, 6> buttons{{
            {kContents, L"Contents"}, {kSearch, L"Search"}, {kBack, L"Back"},
            {kPrint, L"Print"}, {kPrevious, L"<<"}, {kNext, L">>"}
        }};
        int x = 6;
        for (const auto& [id, label] : buttons) {
            const int width = id == kPrevious || id == kNext ? 42 : 78;
            CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                x, 5, width, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                help->instance, nullptr);
            x += width + 5;
        }
        help->body.help = help;
        help->body.topic = help->current;
        help->body.hwnd = CreateWindowExW(0, kTopicClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
            0, kToolbarHeight, 100, 100, hwnd, nullptr, help->instance, &help->body);
        if (!help->body.hwnd) return -1;
        refresh_buttons(*help);
        return 0;
    }
    case WM_SIZE:
        if (help->body.hwnd) MoveWindow(help->body.hwnd, 0, kToolbarHeight,
            LOWORD(lp), std::max(1, static_cast<int>(HIWORD(lp)) - kToolbarHeight), TRUE);
        return 0;
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case kContents: navigate(*help, help->document.contents_topic); return 0;
        case kSearch: open_search(*help); return 0;
        case kBack:
            if (!help->history.empty()) {
                const auto target = help->history.back();
                help->history.pop_back();
                navigate(*help, target, false);
            }
            SetFocus(help->body.hwnd);
            return 0;
        case kPrint: print_topic(*help); return 0;
        case kPrevious:
            if (auto target = help->document.topics[help->current].browse_previous) navigate(*help, *target);
            SetFocus(help->body.hwnd);
            return 0;
        case kNext:
            if (auto target = help->document.topics[help->current].browse_next) navigate(*help, *target);
            SetFocus(help->body.hwnd);
            return 0;
        case kGuide: show_maps_help_guide(hwnd); return 0;
        case kClose: DestroyWindow(hwnd); return 0;
        default: break;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wp == VK_F1) { navigate(*help, help->document.contents_topic); return 0; }
        if (wp == VK_BACK && !help->history.empty()) {
            const auto target = help->history.back();
            help->history.pop_back();
            navigate(*help, target, false);
            return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (help->search) DestroyWindow(help->search);
        if (help->popup) DestroyWindow(help->popup);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (help->owner && IsWindow(help->owner))
            SendMessageW(help->owner, kMapsHelpClosed, reinterpret_cast<WPARAM>(hwnd), 0);
        if (help->owned_by_window) delete help;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_window(HINSTANCE instance, const wchar_t* name, WNDPROC proc, HBRUSH brush) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpfnWndProc = proc;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = brush;
    type.lpszClassName = name;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register native Maps Help window");
}

} // namespace

HWND open_maps_help(HWND owner, HINSTANCE instance, const std::filesystem::path& help_path, HWND existing) {
    if (existing && IsWindow(existing)) {
        ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        auto* help = reinterpret_cast<HelpState*>(GetWindowLongPtrW(existing, GWLP_USERDATA));
        if (help) navigate(*help, help->document.contents_topic);
        return existing;
    }
    auto state = std::make_unique<HelpState>();
    state->document = winhelp::load_maps_help(help_path);
    state->current = state->document.contents_topic;
    state->owner = owner;
    state->instance = instance;
    HDC dc = GetDC(owner);
    if (!dc) throw std::runtime_error("Cannot obtain Help display context");
    try { state->fonts = create_fonts(state->document, GetDeviceCaps(dc, LOGPIXELSY)); }
    catch (...) { ReleaseDC(owner, dc); throw; }
    ReleaseDC(owner, dc);
    register_window(instance, kHelpClass, help_proc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    register_window(instance, kTopicClass, topic_proc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    register_window(instance, kSearchClass, search_proc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU help_menu = CreatePopupMenu();
    if (menu && file && help_menu) {
        AppendMenuW(file, MF_STRING, kPrint, L"Print...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, kClose, L"Close");
        AppendMenuW(help_menu, MF_STRING, kContents, L"Contents");
        AppendMenuW(help_menu, MF_STRING, kGuide, L"Using Help");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"Help");
    } else {
        if (file) DestroyMenu(file);
        if (help_menu) DestroyMenu(help_menu);
        if (menu) DestroyMenu(menu);
        menu = nullptr;
    }
    HWND window = CreateWindowExW(0, kHelpClass, wide(state->document.title).c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 710,
        owner, menu, instance, state.get());
    if (!window) {
        if (menu) DestroyMenu(menu);
        throw std::runtime_error("Cannot create native Maps Help window");
    }
    state->owned_by_window = true;
    state.release();
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    auto* help = reinterpret_cast<HelpState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    refresh_buttons(*help);
    SetFocus(help->body.hwnd);
    return window;
}

void show_maps_help_guide(HWND owner) {
    MessageBoxW(owner,
        L"Maps Help uses the maps.hlp file from your owned Windows release.\n\n"
        L"Select an underlined link to open a topic. Some links open a temporary popup; "
        L"click outside it or press Esc to dismiss it. Contents returns to Main Index; "
        L"Back or Backspace returns to the previous topic. << and >> follow the "
        L"author's browse sequence. Search lists the file's keywords and matching topics. "
        L"Scroll with the wheel, arrows or scrollbar. F1 opens Main Index; Esc closes Help. "
        L"Print opens the system printer dialog for the current topic.",
        L"Using Maps Help", MB_OK | MB_ICONINFORMATION);
}

bool handle_maps_help_key(HWND help_window, const MSG& message) {
    if (!help_window || !IsWindow(help_window) || message.message != WM_KEYDOWN) return false;
    auto* help = reinterpret_cast<HelpState*>(GetWindowLongPtrW(help_window, GWLP_USERDATA));
    if (!help) return false;
    const HWND root = GetAncestor(message.hwnd, GA_ROOT);
    if (help->search && root == help->search) {
        if (message.wParam == VK_ESCAPE) { DestroyWindow(root); return true; }
        if (message.wParam == VK_RETURN) {
            auto* search = reinterpret_cast<SearchState*>(GetWindowLongPtrW(root, GWLP_USERDATA));
            if (search) go_to_match(*search);
            return true;
        }
        if (message.wParam == VK_F1) { navigate(*help, help->document.contents_topic); return true; }
        return false; // Backspace belongs to the keyword edit control.
    }
    if (root != help_window || message.hwnd == help_window || message.hwnd == help->body.hwnd)
        return false; // The topic and popup window procedures handle their own keys.
    if (message.wParam == VK_F1 || message.wParam == VK_ESCAPE || message.wParam == VK_BACK) {
        SendMessageW(help_window, WM_KEYDOWN, message.wParam, message.lParam);
        return true;
    }
    return false;
}

} // namespace n3d::platform
