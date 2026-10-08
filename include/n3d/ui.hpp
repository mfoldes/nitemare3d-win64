#pragma once

#include "n3d/assets.hpp"
#include "n3d/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace n3d {

enum class MenuKind : std::uint8_t {
    Root = 6,
    Episode = 7,
    Difficulty = 8,
    Load = 12,
    Save = 13,
};

enum class SaveMenuOperation : std::uint8_t { save, load };

// Host-owned save-slot metadata. The core never guesses labels for occupied
// slots or performs I/O; it only exposes a one-shot request to the caller.
struct SaveMenuSlot final {
    std::uint8_t slot = 0;
    bool present = false;
    std::string label{};
};

struct SaveMenuRequest final {
    SaveMenuOperation operation = SaveMenuOperation::save;
    std::uint8_t slot = 0;
};

struct SaveMenuOutcome final {
    bool dismissed = false;
    std::optional<SaveMenuRequest> request{};
};

struct MenuSelection final {
    MenuKind kind = MenuKind::Root;
    std::uint8_t selected_row = 0;
    bool active_game = false; // 1030:1028 has seven rows and selects Return to game.
    bool highlight = true; // Save/Load begins without a selected slot until first Enter.
};

// 1030:1220/12e6; callers 1018:28c2/296a select font 1. Only absent
// names have the ten-space prefix at 1048:1d2a; hit height is 1048:04a6.
inline constexpr std::uint8_t kSaveLoadSlotFont = 1u;
inline constexpr int kSaveLoadSlotX = 67;
inline constexpr int kSaveLoadSlotY = 47;
inline constexpr int kSaveLoadSlotRowPitch = 13;
inline constexpr int kSaveLoadHitHeight = 7;
inline constexpr std::string_view kSaveLoadEmptySlotText = "          ** Empty slot **";

inline std::string_view save_menu_slot_label_view(const SaveMenuSlot* slot) noexcept {
    if (slot != nullptr && slot->present) {
        return std::string_view{slot->label};
    }
    return kSaveLoadEmptySlotText;
}

std::array<std::uint8_t, 16> compute_ega_palette_map(const Assets& assets);

std::size_t indexed_text_width(const Assets& assets, std::uint8_t font_index,
                               std::string_view text);
void draw_indexed_text(const Assets& assets, IndexedFrame& frame,
                       std::uint8_t font_index, std::string_view text,
                       int x, int y, std::uint8_t colour);

void compose_title(const Assets& assets, IndexedFrame& frame);
void compose_menu(const Assets& assets, IndexedFrame& frame, MenuSelection selection,
                  const std::array<std::uint8_t, 16>& ega_palette_map,
                  std::span<const SaveMenuSlot> slots = {});

} // namespace n3d
