#include "map_browser_win32.hpp"
#include "help_viewer_win32.hpp"

#include <algorithm>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <windows.h>
#include <windowsx.h>

namespace n3d::platform {
namespace {

constexpr int kWidth = 456;
constexpr int kColumns = 64;
constexpr int kRows = 64;
constexpr int kOrigin = 4;
constexpr int kCellWidth = 7;
constexpr UINT kEpisodeMenu = 100;
constexpr UINT kLevelMenu = 110;
constexpr UINT kWallsMenu = 130;
constexpr UINT kObjectsMenu = 131;
constexpr UINT kFloorsMenu = 132;
constexpr UINT kExitMenu = 140;
constexpr UINT kHelpIndexMenu = 141;
constexpr UINT kHelpUsingMenu = 142;
constexpr UINT kAboutMenu = 143;

// Source DS:006a and DS:0072/00a4 in maps.exe SHA-256
// 1F6A143A6E68A5F8D3ADBE32F96B3B0CBE0EF23AED4DD56FB140FCDFF3A0A7A1.
constexpr std::array<std::uint8_t, 8> kPattern{0xcc, 0x33, 0xcc, 0x33, 0xcc, 0x33, 0xcc, 0x33};
constexpr std::array<std::uint8_t, 49> kBothDisc{
    0,0,1,1,1,0,0, 0,1,1,1,1,1,0, 1,1,2,2,2,1,1,
    1,1,2,2,2,1,1, 1,1,2,2,2,1,1, 0,1,1,1,1,1,0, 0,0,1,1,1,0,0
};
constexpr std::array<std::uint8_t, 49> kOneDisc{
    0,0,1,1,1,0,0, 0,1,2,2,2,1,0, 1,2,2,2,2,2,1,
    1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,1,1,0,0
};
constexpr std::array<COLORREF, 16> kRgb{
    RGB(0,0,0), RGB(0,0,168), RGB(0,168,0), RGB(0,168,168),
    RGB(168,0,0), RGB(168,0,168), RGB(255,128,0), RGB(168,168,168),
    RGB(128,128,128), RGB(0,0,255), RGB(0,255,0), RGB(0,255,255),
    RGB(255,0,0), RGB(255,0,255), RGB(255,255,0), RGB(255,255,255)
};

struct BitmapInfo256 {
    BITMAPINFOHEADER header{};
    RGBQUAD colors[256]{};
};
struct Palette256 {
    WORD version = 0x300;
    WORD count = 256;
    PALETTEENTRY entries[256]{};
};
struct CatalogEntry {
    std::string_view code = "F010";
    const std::string* description = nullptr;
};
struct CatalogLookup {
    std::array<CatalogEntry, 256> walls{};
    std::array<CatalogEntry, 256> objects{};
    std::uint8_t first_floor = 0;
    std::uint8_t last_floor = 0;
};

struct BrowserState {
    const Assets& assets;
    std::array<CatalogLookup, 3> catalogs{};
    std::size_t episode = 0;
    std::size_t level = 0;
    int zoom = 7;
    int client_width = kWidth;
    int client_height = 67 * 7 + 8;
    int hover_col = 0; // The source starts its last-cell cache at (0,0).
    int hover_row = 0;
    std::uint8_t floor_first = 0;
    std::uint8_t floor_last = 0;
    bool walls = true;
    bool objects = true;
    bool floors = false;
    BitmapInfo256 info{};
    std::filesystem::path data_directory;
    HWND help = nullptr;
    std::array<std::uint8_t, 16> draw_colors{};
    std::array<UINT, 16> text_colors{};
    HPALETTE palette = nullptr;
    HDC memory_dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous_bitmap = nullptr;
    HFONT glyph_font = nullptr;
    HFONT label_font = nullptr;
    std::uint8_t* pixels = nullptr;
    const std::byte* cells = nullptr;

    BrowserState(const Assets& data, const std::filesystem::path& directory)
        : assets(data), data_directory(directory) {}
    BrowserState(const BrowserState&) = delete;
    BrowserState& operator=(const BrowserState&) = delete;
    ~BrowserState() {
        if (memory_dc && previous_bitmap) SelectObject(memory_dc, previous_bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (memory_dc) DeleteDC(memory_dc);
        if (glyph_font) DeleteObject(glyph_font);
        if (label_font) DeleteObject(label_font);
        if (palette) DeleteObject(palette);
    }
    int height() const { return 67 * zoom + 8; }
};

std::uint8_t hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<std::uint8_t>(ch - 'A' + 10);
    return 0;
}

void index_catalog(const EpisodeAssets& episode, CatalogLookup& result) {
    for (const auto& record : episode.walls) {
        if (record.id >= result.walls.size() || record.code.size() != 4)
            throw std::runtime_error("Invalid wall catalog entry");
        result.walls[record.id] = {record.code, &record.description};
        if (episode.map.wall_class_by_id[record.id] == 0x44) {
            if (result.first_floor == 0) result.first_floor = static_cast<std::uint8_t>(record.id);
            else result.last_floor = static_cast<std::uint8_t>(record.id);
        }
    }
    for (const auto& record : episode.objects) {
        if (record.id >= result.objects.size() || record.code.size() != 4)
            throw std::runtime_error("Invalid object catalog entry");
        result.objects[record.id] = {record.code, &record.description};
    }
}

void load_level(BrowserState& state) {
    const auto& ep = state.assets.episodes[state.episode];
    if (state.level >= ep.map.level_count || ep.map.level_size != 8192)
        throw std::runtime_error("MAP level is unavailable");
    const std::size_t offset = ep.map.levels_offset + state.level * ep.map.level_size;
    if (offset > ep.map_bytes.size() || ep.map.level_size > ep.map_bytes.size() - offset)
        throw std::runtime_error("MAP level extends past its verified file");
    state.cells = ep.map_bytes.data() + offset;
    // DS:3d0c is the first FLOOR entry seen in the session; DS:1cd6 is
    // overwritten by the last FLOOR entry on each subsequent catalog load.
    const auto& catalog = state.catalogs[state.episode];
    if (state.floor_first == 0) state.floor_first = catalog.first_floor;
    state.floor_last = catalog.last_floor;
}

void init_graphics(BrowserState& state, HDC display) {
    const int depth = GetDeviceCaps(display, BITSPIXEL) * GetDeviceCaps(display, PLANES);
    Palette256 logical{};
    GetSystemPaletteEntries(display, 0, 256, logical.entries);
    for (int i = 10; i <= 245; ++i) {
        auto& entry = logical.entries[i];
        if (depth > 8) {
            const auto& rgb = state.assets.palette.palette[static_cast<std::size_t>(i)];
            entry.peRed = rgb[0]; entry.peGreen = rgb[1]; entry.peBlue = rgb[2];
        } else {
            entry.peRed = entry.peGreen = entry.peBlue = 0;
        }
        entry.peFlags = PC_RESERVED;
    }
    for (std::size_t i = 0; i < 256; ++i) {
        const auto& entry = logical.entries[i];
        state.info.colors[i] = {entry.peBlue, entry.peGreen, entry.peRed, 0};
    }
    state.palette = CreatePalette(reinterpret_cast<const LOGPALETTE*>(&logical));
    if (!state.palette) throw std::runtime_error("CreatePalette failed for map viewer");
    auto* stock = reinterpret_cast<HPALETTE>(GetStockObject(DEFAULT_PALETTE));
    if (!stock) throw std::runtime_error("DEFAULT_PALETTE is unavailable");
    for (std::size_t i = 0; i < kRgb.size(); ++i) {
        const UINT drawing = GetNearestPaletteIndex(state.palette, kRgb[i]);
        const UINT text = GetNearestPaletteIndex(stock, kRgb[i]);
        if (drawing == CLR_INVALID || drawing > 255 || text == CLR_INVALID)
            throw std::runtime_error("GDI nearest-palette lookup failed");
        state.draw_colors[i] = static_cast<std::uint8_t>(drawing);
        state.text_colors[i] = text;
    }
    auto& header = state.info.header;
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = kWidth;
    header.biHeight = -state.height();
    header.biPlanes = 1;
    header.biBitCount = 8;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(kWidth * state.height());
    header.biClrUsed = 256;
    header.biClrImportant = 256;
    state.memory_dc = CreateCompatibleDC(display);
    if (!state.memory_dc) throw std::runtime_error("CreateCompatibleDC failed for map viewer");
    void* bits = nullptr;
    state.bitmap = CreateDIBSection(display, reinterpret_cast<const BITMAPINFO*>(&state.info),
                                    DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!state.bitmap || !bits) throw std::runtime_error("CreateDIBSection failed for map viewer");
    state.pixels = static_cast<std::uint8_t*>(bits);
    state.previous_bitmap = SelectObject(state.memory_dc, state.bitmap);
    if (!state.previous_bitmap || state.previous_bitmap == HGDI_ERROR)
        throw std::runtime_error("Cannot select map DIB into its memory DC");
    SelectPalette(state.memory_dc, state.palette, FALSE);
    RealizePalette(state.memory_dc);
    state.glyph_font = CreateFontA(state.zoom == 7 ? -9 : -8, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, "Small Fonts");
    state.label_font = CreateFontA(state.zoom == 7 ? -12 : -11, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, "MS Sans Serif");
    if (!state.glyph_font || !state.label_font)
        throw std::runtime_error("Cannot create original map viewer GDI fonts");
    SetBkMode(state.memory_dc, TRANSPARENT);
}

inline std::uint8_t color(const BrowserState& s, int number) {
    return s.draw_colors[static_cast<std::size_t>(number)];
}
inline void pixel(BrowserState& s, int x, int y, std::uint8_t value) {
    s.pixels[y * kWidth + x] = value;
}
void rectangle(BrowserState& s, int x0, int y0, int x1, int y1, std::uint8_t value,
               bool outline = false, bool patterned = false) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (outline && x != x0 && x != x1 && y != y0 && y != y1) continue;
            if (patterned && !(kPattern[static_cast<std::size_t>(y & 7)] >> (x & 7) & 1)) continue;
            pixel(s, x, y, value);
        }
    }
}
void line(BrowserState& s, int x0, int y0, int x1, int y1, std::uint8_t value) {
    const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    const int sx = x1 > x0 ? 1 : -1;
    const int sy = y1 > y0 ? 1 : -1;
    const bool x_major = dy < dx;
    const int major = x_major ? dx : dy;
    const int minor = x_major ? dy : dx;
    int decision = 2 * minor - major;
    for (int i = 0; i <= major; ++i) {
        pixel(s, x0, y0, value);
        if (decision < 0) decision += 2 * minor;
        else {
            decision += 2 * (minor - major);
            if (x_major) y0 += sy;
            else x0 += sx;
        }
        if (x_major) x0 += sx;
        else y0 += sy;
    }
}
void bevel(BrowserState& s, int x0, int y0, int x1, int y1) {
    rectangle(s, x0, y0, x1, y1, color(s, 8), true);
    line(s, x0 + 1, y1, x1, y1, color(s, 15));
    line(s, x1, y0, x1, y1, color(s, 15));
}
void glyph(BrowserState& s, int x, int y, std::string_view code) {
    if (code == "0000") return;
    const int a = hex_digit(code[0]);
    const int b = hex_digit(code[1]);
    const char shape = code[2];
    const std::uint8_t c1 = color(s, a), c2 = color(s, b);
    const auto fill = [&](int l, int t, int r, int bottom, std::uint8_t ink, bool pattern = false) {
        rectangle(s, x + l, y + t, x + r, y + bottom, ink, false, pattern);
    };
    const auto edge = [&](int l, int t, int r, int bottom, std::uint8_t ink) {
        rectangle(s, x + l, y + t, x + r, y + bottom, ink, true);
    };
    const auto stroke = [&](int l, int t, int r, int bottom) {
        line(s, x + l, y + t, x + r, y + bottom, c1);
    };
    switch (shape) {
    case '0': {
        if (b != 0) fill(0, 0, 6, 6, c2);
        SetTextColor(s.memory_dc, PALETTEINDEX(s.text_colors[static_cast<std::size_t>(a)]));
        TextOutA(s.memory_dc, x, y - 2, &code[3], 1);
        break;
    }
    case '1': fill(0, 0, 6, 6, c1); break;
    case '2': fill(0, 0, 6, 6, c1, true); break;
    case '3': fill(0, 0, 6, 6, c1); fill(2, 2, 4, 4, c2); break;
    case '4': fill(0, 0, 6, 6, c1, true); fill(2, 2, 4, 4, c2); break;
    case '5': {
        const auto& disc = a != 0 && b != 0 ? kBothDisc : kOneDisc;
        const std::uint8_t inks[]{color(s, 0), c1, c2};
        for (int row = 0; row < s.zoom; ++row)
            for (int col = 0; col < 7; ++col)
                pixel(s, x + col, y + row, inks[disc[static_cast<std::size_t>(row * 7 + col)]]);
        break;
    }
    case '6': fill(0, 2, 6, 4, c1); break;
    case '7': fill(2, 0, 4, 6, c1); break;
    case '8': fill(0, 0, 6, 3, c1); fill(0, 4, 6, 6, c2); break;
    case '9': edge(3, 3, 4, 4, c1); break;
    case 'a': case 'A': fill(2, 1, 4, 5, c1); break;
    case 'b': case 'B': fill(2, 2, 4, 4, c1); break;
    case 'c': case 'C': fill(1, 1, 5, 5, c1); break;
    case 'd': case 'D': stroke(1, 1, 5, 5); stroke(5, 1, 1, 5); break;
    case 'e': case 'E': edge(0, 0, 6, 6, c1); break;
    case 'f': case 'F':
        switch (b) {
        case 0: stroke(3,6,3,0); stroke(3,0,0,3); stroke(3,0,6,3); break;
        case 1: stroke(0,6,6,0); stroke(6,0,3,0); stroke(6,0,6,3); break;
        case 2: stroke(0,3,6,3); stroke(6,3,3,0); stroke(6,3,3,6); break;
        case 3: stroke(0,0,6,6); stroke(6,6,3,6); stroke(6,6,6,3); break;
        case 4: stroke(3,0,3,6); stroke(3,6,0,3); stroke(3,6,6,3); break;
        case 5: stroke(6,0,0,6); stroke(0,6,3,6); stroke(0,6,0,3); break;
        case 6: stroke(6,3,0,3); stroke(0,3,3,0); stroke(0,3,3,6); break;
        case 7: stroke(6,6,0,0); stroke(0,0,3,0); stroke(0,0,0,3); break;
        default: break; // The original jump table has no branch for 8..15.
        }
        break;
    default: break;
    }
}

void draw_strip(BrowserState& s) {
    const int top = 64 * s.zoom + 8;
    rectangle(s, 0, top, 455, 67 * s.zoom + 7, color(s, 7));
    line(s, 0, top, 455, top, color(s, 15));
    bevel(s, 5, top + 2, 45, 67 * s.zoom + 6);
    bevel(s, 50, top + 2, 248, 67 * s.zoom + 6);
    bevel(s, 253, top + 2, 451, 67 * s.zoom + 6);
}
void draw_frame(BrowserState& s) {
    rectangle(s, 0, 0, 455, s.height() - 1, color(s, 0));
    for (int i = 0; i != 4; ++i)
        rectangle(s, i, i, 455 - i, 64 * s.zoom + 7 - i, color(s, 7), true);
    bevel(s, 3, 3, 452, 64 * s.zoom + 4);
    HGDIOBJ old = SelectObject(s.memory_dc, s.glyph_font);
    const auto& catalog = s.catalogs[s.episode];
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kColumns; ++col) {
            const std::size_t cell = static_cast<std::size_t>((row * kColumns + col) * 2);
            const auto wall = std::to_integer<std::uint8_t>(s.cells[cell]);
            const auto object = std::to_integer<std::uint8_t>(s.cells[cell + 1]);
            const int x = kOrigin + col * kCellWidth, y = kOrigin + row * s.zoom;
            const bool floor = wall >= s.floor_first && wall <= s.floor_last;
            if ((floor && s.floors) || (!floor && s.walls))
                glyph(s, x, y, catalog.walls[wall].code);
            if (s.objects) glyph(s, x, y, catalog.objects[object].code);
        }
    }
    SelectObject(s.memory_dc, old);
    draw_strip(s);
}

void draw_hover(BrowserState& s, int col, int row) {
    const int top = 64 * s.zoom;
    rectangle(s, 8, top + 11, 42, 67 * s.zoom + 4, color(s, 7));
    rectangle(s, 53, top + 11, 245, 67 * s.zoom + 4, color(s, 7));
    rectangle(s, 256, top + 11, 448, 67 * s.zoom + 4, color(s, 7));
    HGDIOBJ old = SelectObject(s.memory_dc, s.label_font);
    SetTextColor(s.memory_dc, PALETTEINDEX(s.text_colors[0]));
    const int text_y = top + 8 + (s.zoom == 7 ? 3 : 2);
    char coordinates[32];
    const int size = std::snprintf(coordinates, sizeof(coordinates), "(%02d,%02d)", col, row);
    TextOutA(s.memory_dc, 8, text_y, coordinates, size);
    if (col >= 0 && col < 64 && row >= 0 && row < 64) {
        const std::size_t cell = static_cast<std::size_t>((row * kColumns + col) * 2);
        const auto wall = std::to_integer<std::uint8_t>(s.cells[cell]);
        const auto object = std::to_integer<std::uint8_t>(s.cells[cell + 1]);
        const auto& catalog = s.catalogs[s.episode];
        const auto describe = [&](int x, const CatalogEntry& entry, unsigned id) {
            char unknown[24];
            if (entry.description) {
                const auto& description = *entry.description;
                TextOutA(s.memory_dc, x, text_y, description.data(), static_cast<int>(description.size()));
            } else {
                const int n = std::snprintf(unknown, sizeof(unknown), "Unknown %04X", id);
                TextOutA(s.memory_dc, x, text_y, unknown, n);
            }
        };
        describe(53, catalog.walls[wall], wall);
        describe(256, catalog.objects[object], object);
    }
    SelectObject(s.memory_dc, old);
}

void present(const BrowserState& s, HDC dc) {
    if (s.client_width <= 0 || s.client_height <= 0) return;
    SelectPalette(dc, s.palette, FALSE);
    RealizePalette(dc);
    if (s.client_width == kWidth && s.client_height == s.height()) {
        BitBlt(dc, 0, 0, kWidth, s.height(), s.memory_dc, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, 0, 0, s.client_width, s.client_height,
                   s.memory_dc, 0, 0, kWidth, s.height(), SRCCOPY);
    }
}
void update_title(HWND hwnd, const BrowserState& s) {
    wchar_t title[96];
    std::swprintf(title, std::size(title), L"Maps - Episode %u, Level %u",
                  static_cast<unsigned>(s.episode + 1), static_cast<unsigned>(s.level + 1));
    SetWindowTextW(hwnd, title);
}
void rebuild_level_menu(HWND hwnd, BrowserState& s) {
    HMENU maps = GetSubMenu(GetMenu(hwnd), 0);
    if (!maps) return;
    HMENU levels = GetSubMenu(maps, 1);
    if (!levels) return;

    const std::size_t count = s.assets.episodes[s.episode].map.level_count;
    for (int index = GetMenuItemCount(levels) - 1; index >= 0; --index)
        RemoveMenu(levels, index, MF_BYPOSITION);

    // Original maps.exe offers only Level 1..10 in its menu, even though episode 1
    // stores an eleventh map reachable by ten Up presses. Expose it separately
    // as an explicitly labelled native convenience without misrepresenting that menu.
    for (std::size_t i = 0; i < count; ++i) {
        wchar_t name[32];
        const bool demo_only = s.episode == 0 && i == 10;
        std::swprintf(name, std::size(name), L"Level %zu%ls",
                      i + 1, demo_only ? L" (demo-only)" : L"");
        if (!AppendMenuW(levels, MF_STRING, kLevelMenu + static_cast<UINT>(i), name))
            throw std::runtime_error("Cannot append level item");
    }
    CheckMenuRadioItem(levels, kLevelMenu, kLevelMenu + static_cast<UINT>(count - 1),
                       kLevelMenu + static_cast<UINT>(s.level), MF_BYCOMMAND);
}
void check_selection_menu(HWND hwnd, const BrowserState& s) {
    HMENU maps = GetSubMenu(GetMenu(hwnd), 0);
    if (!maps) return;
    CheckMenuRadioItem(GetSubMenu(maps, 0), kEpisodeMenu, kEpisodeMenu + 2,
                       kEpisodeMenu + static_cast<UINT>(s.episode), MF_BYCOMMAND);
    HMENU levels = GetSubMenu(maps, 1);
    const UINT last = kLevelMenu + static_cast<UINT>(s.assets.episodes[s.episode].map.level_count - 1);
    CheckMenuRadioItem(levels, kLevelMenu, last,
                       kLevelMenu + static_cast<UINT>(s.level), MF_BYCOMMAND);
}

void check_view_menu(HWND hwnd, const BrowserState& s) {
    HMENU menu = GetSubMenu(GetMenu(hwnd), 1);
    CheckMenuItem(menu, kWallsMenu, MF_BYCOMMAND | (s.walls ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kObjectsMenu, MF_BYCOMMAND | (s.objects ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kFloorsMenu, MF_BYCOMMAND | (s.floors ? MF_CHECKED : MF_UNCHECKED));
}

void change_episode(HWND hwnd, BrowserState& s, std::size_t episode) {
    if (episode >= s.assets.episodes.size()) return;
    s.episode = episode;
    s.level = 0;
    load_level(s);
    rebuild_level_menu(hwnd, s);
    check_selection_menu(hwnd, s);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK browser_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* s = reinterpret_cast<BrowserState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (s->help && IsWindow(s->help)) DestroyWindow(s->help);
        PostQuitMessage(0);
        return 0;
    case kMapsHelpClosed:
        if (s->help == reinterpret_cast<HWND>(wp)) s->help = nullptr;
        return 0;
    case WM_SIZE:
        s->client_width = LOWORD(lp);
        s->client_height = HIWORD(lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_COMMAND: {
        const UINT id = LOWORD(wp);
        if (id >= kEpisodeMenu && id < kLevelMenu) {
            change_episode(hwnd, *s, id - kEpisodeMenu);
            return 0;
        }
        if (id >= kLevelMenu && id < kWallsMenu) {
            const std::size_t level = id - kLevelMenu;
            const std::size_t count = s->assets.episodes[s->episode].map.level_count;
            if (level < count) {
                s->level = level;
                load_level(*s);
                check_selection_menu(hwnd, *s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (id == kExitMenu) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (id == kAboutMenu) {
            MessageBoxW(hwnd, L"Maps Application Version 1.1\nCopyright © 1994, David P. Gray",
                        L"About Maps", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (id == kHelpUsingMenu) { show_maps_help_guide(hwnd); return 0; }
        if (id == kHelpIndexMenu) {
            try {
                s->help = open_maps_help(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                                         s->data_directory / L"maps.hlp", s->help);
            } catch (const std::exception& error) {
                const std::string details = error.what();
                const int count = MultiByteToWideChar(CP_ACP, 0, details.c_str(),
                                                       static_cast<int>(details.size()), nullptr, 0);
                std::wstring message = L"Unable to open Maps Help:\n";
                if (count > 0) {
                    const std::size_t prefix = message.size();
                    message.resize(prefix + static_cast<std::size_t>(count));
                    MultiByteToWideChar(CP_ACP, 0, details.c_str(), static_cast<int>(details.size()),
                                        message.data() + prefix, count);
                }
                MessageBoxW(hwnd, message.c_str(), L"Maps Help", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (id == kWallsMenu) s->walls = !s->walls;
        else if (id == kObjectsMenu) s->objects = !s->objects;
        else if (id == kFloorsMenu) s->floors = !s->floors;
        else return DefWindowProcW(hwnd, msg, wp, lp);
        check_view_menu(hwnd, *s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        const std::size_t count = s->assets.episodes[s->episode].map.level_count;
        switch (wp) {
        case 'Q': case VK_ESCAPE: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case VK_F1: SendMessageW(hwnd, WM_COMMAND, kHelpIndexMenu, 0); return 0;
        case 'W': SendMessageW(hwnd, WM_COMMAND, kWallsMenu, 0); return 0;
        case 'O': SendMessageW(hwnd, WM_COMMAND, kObjectsMenu, 0); return 0;
        case 'F': SendMessageW(hwnd, WM_COMMAND, kFloorsMenu, 0); return 0;
        case VK_PRIOR: case VK_UP: if (s->level + 1 < count) ++s->level; break;
        case VK_NEXT: case VK_DOWN: if (s->level > 0) --s->level; break;
        case VK_HOME: s->level = 0; break;
        case VK_END: s->level = count ? count - 1 : 0; break;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
        }
        load_level(*s);
        rebuild_level_menu(hwnd, *s);
        check_selection_menu(hwnd, *s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (s->client_width <= 0 || s->client_height <= 0) return 0;
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const int col = std::clamp((x - kOrigin) * kWidth / (s->client_width * kCellWidth), 0, 63);
        const int row = std::clamp((y - kOrigin) * (67 * s->zoom + 8) / (s->client_height * s->zoom), 0, 63);
        if (col == s->hover_col && row == s->hover_row) return 0;
        s->hover_col = col;
        s->hover_row = row;
        draw_hover(*s, col, row);
        HDC dc = GetDC(hwnd);
        if (dc) { present(*s, dc); ReleaseDC(hwnd, dc); }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        draw_frame(*s);
        update_title(hwnd, *s);
        present(*s, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HMENU create_browser_menu() {
    HMENU menu = CreateMenu();
    if (!menu) throw std::runtime_error("CreateMenu failed for map viewer");
    try {
        HMENU maps = CreatePopupMenu();
        if (!maps) throw std::runtime_error("CreatePopupMenu failed for Maps");
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(maps), L"Maps")) {
            DestroyMenu(maps);
            throw std::runtime_error("Cannot attach Maps menu");
        }
        HMENU episodes = CreatePopupMenu();
        if (!episodes) throw std::runtime_error("CreatePopupMenu failed for episodes");
        if (!AppendMenuW(maps, MF_POPUP, reinterpret_cast<UINT_PTR>(episodes), L"Episode")) {
            DestroyMenu(episodes);
            throw std::runtime_error("Cannot attach episode menu");
        }
        for (UINT i = 0; i < 3; ++i) {
            wchar_t name[16];
            std::swprintf(name, std::size(name), L"Episode %u", i + 1);
            if (!AppendMenuW(episodes, MF_STRING, kEpisodeMenu + i, name))
                throw std::runtime_error("Cannot append episode item");
        }
        HMENU levels = CreatePopupMenu();
        if (!levels) throw std::runtime_error("CreatePopupMenu failed for levels");
        if (!AppendMenuW(maps, MF_POPUP, reinterpret_cast<UINT_PTR>(levels), L"Level")) {
            DestroyMenu(levels);
            throw std::runtime_error("Cannot attach level menu");
        }
        if (!AppendMenuW(maps, MF_SEPARATOR, 0, nullptr) ||
            !AppendMenuW(maps, MF_STRING, kExitMenu, L"Exit"))
            throw std::runtime_error("Cannot append Maps item");
        HMENU view = CreatePopupMenu();
        if (!view) throw std::runtime_error("CreatePopupMenu failed for view");
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View")) {
            DestroyMenu(view);
            throw std::runtime_error("Cannot attach view menu");
        }
        if (!AppendMenuW(view, MF_STRING | MF_CHECKED, kWallsMenu, L"Walls\tW") ||
            !AppendMenuW(view, MF_STRING | MF_CHECKED, kObjectsMenu, L"Objects\tO") ||
            !AppendMenuW(view, MF_STRING, kFloorsMenu, L"Floors\tF"))
            throw std::runtime_error("Cannot append view item");
        HMENU help = CreatePopupMenu();
        if (!help) throw std::runtime_error("CreatePopupMenu failed for help");
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Help")) {
            DestroyMenu(help);
            throw std::runtime_error("Cannot attach Help menu");
        }
        if (!AppendMenuW(help, MF_STRING, kHelpIndexMenu, L"Index") ||
            !AppendMenuW(help, MF_STRING, kHelpUsingMenu, L"Using Help") ||
            !AppendMenuW(help, MF_SEPARATOR, 0, nullptr) ||
            !AppendMenuW(help, MF_STRING, kAboutMenu, L"About Maps..."))
            throw std::runtime_error("Cannot append Help item");
    } catch (...) {
        DestroyMenu(menu);
        throw;
    }
    return menu;
}

} // namespace

int run_map_browser(HINSTANCE instance, const Assets& assets, const std::filesystem::path& data_directory) {
    if (!instance) throw std::invalid_argument("Map viewer needs an HINSTANCE");
    BrowserState state(assets, data_directory);
    state.zoom = GetSystemMetrics(SM_CXSCREEN) >= 550 ? 7 : 6;
    state.client_height = state.height();
    for (std::size_t ep = 0; ep < state.catalogs.size(); ++ep)
        index_catalog(assets.episodes[ep], state.catalogs[ep]);
    load_level(state);
    HDC display = GetDC(nullptr);
    if (!display) throw std::runtime_error("Cannot acquire display DC for map viewer");
    try { init_graphics(state, display); }
    catch (...) { ReleaseDC(nullptr, display); throw; }
    ReleaseDC(nullptr, display);

    constexpr wchar_t kClass[] = L"Nitemare3DMapBrowser";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = browser_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("RegisterClassExW failed for map viewer");
    HMENU menu = create_browser_menu();
    RECT frame{0, 0, kWidth, state.height()};
    if (!AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, TRUE)) {
        DestroyMenu(menu);
        throw std::runtime_error("AdjustWindowRect failed for map viewer");
    }
    HWND hwnd = CreateWindowExW(0, kClass, L"Maps - Episode 1, Level 1", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 frame.right - frame.left, frame.bottom - frame.top,
                                 nullptr, menu, instance, &state);
    if (!hwnd) {
        DestroyMenu(menu);
        throw std::runtime_error("CreateWindowExW failed for map viewer");
    }
    rebuild_level_menu(hwnd, state);
    check_selection_menu(hwnd, state);
    check_view_menu(hwnd, state);
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (handle_maps_help_key(state.help, message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0) throw std::runtime_error("GetMessageW failed in map viewer");
    return static_cast<int>(message.wParam);
}

} // namespace n3d::platform
