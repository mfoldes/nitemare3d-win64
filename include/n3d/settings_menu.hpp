#pragma once

#include "n3d/config.hpp"
#include "n3d/frame.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace n3d {

struct Assets;
struct InputFrame;

// Configure/Hardware/Cheats values equal their UIF directory indices;
// unavailable_cheats is modal state over the Cheats background, not UIF12.
enum class SettingsScreen : std::uint8_t {
    closed,
    configure = 9,
    hardware = 10,
    cheats = 11,
    unavailable_cheats,
};

enum class SettingsJoystickAxis : std::uint8_t { horizontal, vertical };

// Effects for the caller to perform, in addition to the changes already made to
// Config. This module never accesses the file system or a sound/joystick device.
struct SettingsOutcome final {
    bool save_config = false;                 // Persist all 20 Config bytes.
    bool returned_to_root = false;
    bool reconcile_audio = false;             // Original hardware callback e0c4(2).
    bool set_music_volume = false;            // Original dd4a, when volume changes.
    bool play_effect_2e = false;              // Original e3b0(0x2e,0,2).
    bool recalibrate_joystick = false;        // Original 8f86(0), on every enabled callback.
    bool apply_cheats = false;                // Registered mode, after cheat Done.
    bool show_unavailable_cheats = false;     // Caller owns the modal presentation.
};

class SettingsMenu final {
public:
    // Text from 1048:1eb6. The original modal's geometry/raster is not established.
    static constexpr std::string_view kUnavailableCheatsMessage =
        "Cheat modes are only available when you purchase the complete "
        "trilogy.  Please see 'Instructions'";

    void open() noexcept; // Enter UIF9 from the root; retained row cursors survive visits.
    [[nodiscard]] SettingsScreen screen() const noexcept { return screen_; }
    [[nodiscard]] std::uint8_t selected_row() const noexcept;
    [[nodiscard]] std::uint8_t selected_action() const noexcept;
    [[nodiscard]] std::array<std::uint8_t, 3> retained_cursors() const noexcept {
        return cursors_;
    }
    void restore_cursors(std::array<std::uint8_t, 3> cursors) noexcept {
        cursors_ = cursors;
    }

    // key is the original ToAscii-or-VK action byte; Alt is already filtered by
    // process(). registered_mode is BSF header +3 != 0, NOT an inferred licence.
    SettingsOutcome handle_key(Config& config, std::uint8_t key, bool registered_mode);
    // Mirror 1018:2516 after an enabled joystick is disabled by a driver failure.
    void disable_joystick() noexcept;
    SettingsOutcome process(const Assets& assets, Config& config,
                            const InputFrame& input, bool registered_mode);

    // Feed original 963e's button action (0x0d/0x1b), or a direction already
    // obtained from the joystick driver. An inactive joystick is ignored.
    SettingsOutcome handle_joystick(Config& config, std::uint8_t key,
                                    bool registered_mode);

    // Call once per original 200-ms axis polling interval with the signed
    // distance from the device's calibrated centre. Threshold is strictly +/-7.
    // Separate axes avoid inventing a priority when both deflect simultaneously.
    SettingsOutcome handle_joystick_axis(Config& config, SettingsJoystickAxis axis,
                                         int displacement, bool registered_mode);

    // UIF9/10/11 background, font2 shadow/labels, slider bars and indicators.
    // During the unavailable-cheats modal, paints the underlying UIF11 screen;
    // the caller renders the modal itself rather than fabricating legacy pixels.
    void compose(const Assets& assets, IndexedFrame& frame,
                 const std::array<std::uint8_t, 16>& ega_palette_map) const;

private:
    [[nodiscard]] std::uint8_t& cursor() noexcept;
    [[nodiscard]] const std::uint8_t& cursor() const noexcept;
    SettingsOutcome activate(Config& config, bool registered_mode);
    SettingsOutcome hardware_callback(Config& config) const noexcept;
    void pop(SettingsOutcome& outcome) noexcept;

    SettingsScreen screen_ = SettingsScreen::closed;
    std::array<std::uint8_t, 3> cursors_{}; // 1030:11d8, 13ac, 1418 cursor flags.
    std::array<std::int16_t, 4> sliders_{}; // Mouse, Joystick, Music, Sound FX.
    std::array<std::uint8_t, 4> cheats_{}; // Omniscient, Omnipotent, Omnificent, Omnifarious.
};

} // namespace n3d
