#include "n3d/settings_menu.hpp"

#include "n3d/assets.hpp"
#include "n3d/game.hpp"
#include "n3d/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace n3d {
namespace {

// 1030:11d8, 13ac and 1418; the action IDs are deliberately not field indices.
enum class ItemKind : std::uint8_t { button = 1, checkbox = 2, slider = 3 };
struct Row final {
    std::string_view label;
    std::uint8_t action;
    std::uint8_t y;
    ItemKind kind;
};
constexpr std::array kConfigureRows{
    Row{"Hardware...", 0x26, 80, ItemKind::button},
    Row{"Cheats...", 0x11, 100, ItemKind::button},
    Row{"Done", 0x18, 120, ItemKind::button},
};
constexpr std::array kHardwareRows{
    Row{"Mouse", 0x0d, 70, ItemKind::slider},
    Row{"Joystick", 0x0e, 85, ItemKind::slider},
    Row{"Music", 0x10, 100, ItemKind::slider},
    Row{"Sound FX", 0x0f, 115, ItemKind::slider},
    Row{"Save", 0x27, 130, ItemKind::button},
};
constexpr std::array kCheatRows{
    Row{"Omniscient (all-knowing)", 0x22, 70, ItemKind::checkbox},
    Row{"Omnipotent (all-powerful)", 0x23, 85, ItemKind::checkbox},
    Row{"Omnificent (all-cunning)", 0x24, 100, ItemKind::checkbox},
    Row{"Omnifarious (all things)", 0x25, 115, ItemKind::checkbox},
    Row{"Done", 0x28, 130, ItemKind::button},
    Row{"Cancel", 0x19, 145, ItemKind::button},
};

struct Rows final { const Row* data; std::size_t size; };
Rows rows_for(SettingsScreen screen) noexcept {
    switch (screen) {
    case SettingsScreen::configure: return {kConfigureRows.data(), kConfigureRows.size()};
    case SettingsScreen::hardware: return {kHardwareRows.data(), kHardwareRows.size()};
    case SettingsScreen::cheats:
    case SettingsScreen::unavailable_cheats: return {kCheatRows.data(), kCheatRows.size()};
    case SettingsScreen::closed: return {nullptr, 0};
    }
    return {nullptr, 0};
}

void rect(IndexedFrame& frame, int x, int y, int width, int height,
          std::uint8_t colour) noexcept {
    for (int row = 0; row < height; ++row) {
        const auto offset = static_cast<std::size_t>(y + row) * IndexedFrame::kWidth +
                            static_cast<std::size_t>(x);
        for (int col = 0; col < width; ++col) frame.pixels[offset + col] = colour;
    }
}

int magnitude(std::int16_t value) noexcept { return value < 0 ? -value : value; }

} // namespace

void SettingsMenu::open() noexcept { screen_ = SettingsScreen::configure; }

void SettingsMenu::disable_joystick() noexcept {
    if (screen_ == SettingsScreen::hardware && sliders_[1] > 0)
        sliders_[1] = static_cast<std::int16_t>(-sliders_[1]);
}

std::uint8_t& SettingsMenu::cursor() noexcept {
    const auto index = screen_ == SettingsScreen::configure ? 0u :
                       screen_ == SettingsScreen::hardware ? 1u : 2u;
    return cursors_[index];
}

const std::uint8_t& SettingsMenu::cursor() const noexcept {
    const auto index = screen_ == SettingsScreen::configure ? 0u :
                       screen_ == SettingsScreen::hardware ? 1u : 2u;
    return cursors_[index];
}

std::uint8_t SettingsMenu::selected_row() const noexcept { return cursor(); }

std::uint8_t SettingsMenu::selected_action() const noexcept {
    const auto rows = rows_for(screen_);
    return rows.data && cursor() < rows.size ? rows.data[cursor()].action : 0;
}

void SettingsMenu::pop(SettingsOutcome& outcome) noexcept {
    switch (screen_) {
    case SettingsScreen::configure:
        screen_ = SettingsScreen::closed;
        outcome.returned_to_root = true;
        break;
    case SettingsScreen::hardware:
    case SettingsScreen::cheats:
    case SettingsScreen::unavailable_cheats:
        screen_ = SettingsScreen::configure;
        break;
    case SettingsScreen::closed: break;
    }
}

SettingsOutcome SettingsMenu::hardware_callback(Config& config) const noexcept {
    // 1018:2596 writes all four magnitude/enable pairs on EVERY routed event,
    // including cursor motion and keys that did not change a slider.
    SettingsOutcome outcome{};
    outcome.reconcile_audio = true;
    outcome.set_music_volume = config.music_volume != magnitude(sliders_[2]);
    outcome.play_effect_2e = config.effects_volume != magnitude(sliders_[3]);
    config.mouse_sensitivity = static_cast<std::uint8_t>(magnitude(sliders_[0]));
    config.joystick_divisor = static_cast<std::uint8_t>(magnitude(sliders_[1]));
    config.music_volume = static_cast<std::uint8_t>(magnitude(sliders_[2]));
    config.effects_volume = static_cast<std::uint8_t>(magnitude(sliders_[3]));
    config.mouse_enabled = static_cast<std::uint8_t>(sliders_[0] > 0);
    config.joystick_enabled = static_cast<std::uint8_t>(sliders_[1] > 0);
    config.music_enabled = static_cast<std::uint8_t>(sliders_[2] > 0);
    config.effects_enabled = static_cast<std::uint8_t>(sliders_[3] > 0);
    outcome.recalibrate_joystick = config.joystick_enabled != 0;
    return outcome;
}

SettingsOutcome SettingsMenu::activate(Config& config, bool registered_mode) {
    SettingsOutcome outcome{};
    switch (screen_) {
    case SettingsScreen::configure:
        switch (cursor()) {
        case 0:
            if (config.mouse_sensitivity > 100 || config.joystick_divisor > 100 ||
                config.music_volume > 100 || config.effects_volume > 100 ||
                (config.joystick_enabled && !config.joystick_divisor)) {
                throw std::invalid_argument("invalid original hardware slider configuration");
            }
            // 1018:2516 stores signed magnitudes, enabled iff positive. Zero is
            // disabled irrespective of the raw enable flag.
            sliders_ = {{
                static_cast<std::int16_t>(config.mouse_enabled ? config.mouse_sensitivity :
                                                                   -config.mouse_sensitivity),
                static_cast<std::int16_t>(config.joystick_enabled ? config.joystick_divisor :
                                                                      -config.joystick_divisor),
                static_cast<std::int16_t>(config.music_enabled ? config.music_volume :
                                                                   -config.music_volume),
                static_cast<std::int16_t>(config.effects_enabled ? config.effects_volume :
                                                                     -config.effects_volume),
            }};
            screen_ = SettingsScreen::hardware;
            break;
        case 1:
            // 1018:26f0 reads cheat bytes in TABLE order, not file order.
            cheats_ = {{config.cheat_omniscient, config.cheat_omnipotent,
                        config.cheat_omnificent, config.cheat_omnifarious}};
            screen_ = SettingsScreen::cheats;
            break;
        case 2: pop(outcome); break;
        }
        break;
    case SettingsScreen::hardware:
        if (cursor() < sliders_.size()) {
            sliders_[cursor()] = static_cast<std::int16_t>(-sliders_[cursor()]);
        } else {
            outcome.save_config = true; // 1018:27de action 0x27, then pop.
            pop(outcome);
        }
        break;
    case SettingsScreen::cheats:
        if (cursor() < cheats_.size()) {
            cheats_[cursor()] ^= 1u; // 1018:2e12, xor the table value word with 1.
        } else if (cursor() == 4) {
            // 1018:2746 copies table to raw 4be4/4be5/4be7/4be6.
            config.cheat_omniscient = cheats_[0];
            config.cheat_omnipotent = cheats_[1];
            config.cheat_omnificent = cheats_[2];
            config.cheat_omnifarious = cheats_[3];
            if (registered_mode) {
                outcome.apply_cheats = true;
                pop(outcome);
            } else {
                config.cheat_omniscient = config.cheat_omnipotent =
                    config.cheat_omnificent = config.cheat_omnifarious = 0;
                screen_ = SettingsScreen::unavailable_cheats;
                outcome.show_unavailable_cheats = true;
            }
            outcome.save_config = true;
        } else {
            pop(outcome); // Cancel discards only the staged cheat table.
        }
        break;
    case SettingsScreen::closed:
    case SettingsScreen::unavailable_cheats: break;
    }
    return outcome;
}

SettingsOutcome SettingsMenu::handle_key(Config& config, std::uint8_t key,
                                         bool registered_mode) {
    SettingsOutcome outcome{};
    if (screen_ == SettingsScreen::closed || key == 0) return outcome;
    if (screen_ == SettingsScreen::unavailable_cheats) {
        if (key == 0x0d || key == 0x1b || key == 0x20) pop(outcome);
        return outcome;
    }
    const bool hardware = screen_ == SettingsScreen::hardware;
    switch (key) {
    case 0x26: // Up, no wrap.
        if (cursor() > 0) --cursor();
        break;
    case 0x28: // Down, no wrap.
        if (static_cast<std::size_t>(cursor()) + 1 < rows_for(screen_).size) ++cursor();
        break;
    case 0x25:
    case 0x27:
        if (hardware && cursor() < sliders_.size()) {
            const int next = magnitude(sliders_[cursor()]) + (key == 0x25 ? -5 : 5);
            sliders_[cursor()] = static_cast<std::int16_t>(next < 0 ? 0 : next > 100 ? 100 : next);
        }
        break;
    case 0x0d: case 0x20: outcome = activate(config, registered_mode); break;
    case 0x1b: pop(outcome); break;
    default: break;
    }
    if (hardware) {
        const auto callback = hardware_callback(config);
        outcome.reconcile_audio = callback.reconcile_audio;
        outcome.set_music_volume = callback.set_music_volume;
        outcome.play_effect_2e = callback.play_effect_2e;
        outcome.recalibrate_joystick = callback.recalibrate_joystick;
    }
    return outcome;
}

SettingsOutcome SettingsMenu::process(const Assets& assets, Config& config,
                                      const InputFrame& input, bool registered_mode) {
    if (screen_ == SettingsScreen::closed) return {};
    // 1010:8cd2 does not forward the keyboard event while Alt is held.
    if (input.key && !(input.held & 0x100u))
        return handle_key(config, input.key, registered_mode);
    if (screen_ == SettingsScreen::unavailable_cheats) {
        if (input.menu_right_pressed) return handle_key(config, 0x1b, registered_mode);
        if (input.menu_left_released) return handle_key(config, 0x0d, registered_mode);
        return {};
    }
    if (!input.cursor_moved && !input.menu_left_released && !input.menu_right_pressed)
        return {};

    const auto rows = rows_for(screen_);
    std::size_t hit = rows.size;
    if (input.cursor_moved || input.menu_left_released) {
        for (std::size_t i = 0; i < rows.size; ++i) {
            const auto& row = rows.data[i];
            const auto right = 110 + static_cast<int>(indexed_text_width(assets, 2, row.label));
            if (input.cursor_x >= 110 && input.cursor_x <= right &&
                input.cursor_y >= row.y && input.cursor_y <= row.y + 8) {
                hit = i;
                break;
            }
        }
    }
    if (input.cursor_moved && hit < rows.size) cursor() = static_cast<std::uint8_t>(hit);
    if (input.menu_right_pressed) return handle_key(config, 0x1b, registered_mode);
    if (input.menu_left_released && hit == cursor())
        return handle_key(config, 0x0d, registered_mode);
    if (screen_ == SettingsScreen::hardware) return hardware_callback(config);
    return {};
}

SettingsOutcome SettingsMenu::handle_joystick(Config& config, std::uint8_t key,
                                              bool registered_mode) {
    if (!config.joystick_enabled || screen_ == SettingsScreen::closed) return {};
    // Button 1 is Enter, button 2 is Escape; a direction can also be supplied
    // directly if the host joystick driver already translated its axes.
    return handle_key(config, key, registered_mode);
}

SettingsOutcome SettingsMenu::handle_joystick_axis(Config& config, SettingsJoystickAxis axis,
                                                    int displacement, bool registered_mode) {
    if (!config.joystick_enabled || screen_ == SettingsScreen::closed) return {};
    if (displacement < -7)
        return handle_key(config, axis == SettingsJoystickAxis::horizontal ? 0x25 : 0x26,
                          registered_mode);
    if (displacement > 7)
        return handle_key(config, axis == SettingsJoystickAxis::horizontal ? 0x27 : 0x28,
                          registered_mode);
    return {};
}

void SettingsMenu::compose(const Assets& assets, IndexedFrame& frame,
                           const std::array<std::uint8_t, 16>& map) const {
    if (screen_ == SettingsScreen::closed)
        throw std::logic_error("settings menu has no active background");
    const auto kind = screen_ == SettingsScreen::unavailable_cheats ? 11u :
                      static_cast<std::uint8_t>(screen_);
    const auto& source = assets.uif.images[kind - 3u];
    if (source.directory_index != kind || source.image.pixels.size() != IndexedFrame::kPixelCount)
        throw DataError("uif.dat", source.offset, "settings UIF image fills a 320x200 frame");
    for (std::size_t i = 0; i < frame.palette.size(); ++i) {
        const auto& rgb = assets.palette.palette[i];
        frame.palette[i] = Rgb8{rgb[0], rgb[1], rgb[2]};
    }
    for (std::size_t i = 0; i < frame.pixels.size(); ++i)
        frame.pixels[i] = std::to_integer<std::uint8_t>(source.image.pixels[i]);

    const auto rows = rows_for(screen_);
    for (std::size_t i = 0; i < rows.size; ++i) {
        const auto& row = rows.data[i];
        const bool selected = i == cursor();
        const auto ink = map[selected ? 14u : 2u];
        draw_indexed_text(assets, frame, 2, row.label, 111, row.y + 1, 0);
        draw_indexed_text(assets, frame, 2, row.label, 110, row.y, ink);
        if (row.kind == ItemKind::slider) {
            rect(frame, 190, row.y, 82, 10, map[6]);
            rect(frame, 191, row.y + 1, magnitude(sliders_[i]) * 80 / 100, 8, ink);
        }
        if (row.kind == ItemKind::slider || row.kind == ItemKind::checkbox) {
            const bool enabled = row.kind == ItemKind::slider ? sliders_[i] > 0 : cheats_[i] != 0;
            rect(frame, 98, row.y + 2, 8, 5, map[6]);
            rect(frame, 99, row.y + 3, 6, 3, enabled ? ink : 0);
        }
    }
}

} // namespace n3d
