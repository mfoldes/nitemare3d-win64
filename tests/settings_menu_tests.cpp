#include "n3d/settings_menu.hpp"

#include "n3d/assets.hpp"
#include "n3d/game.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

int failures = 0;
void check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void enter_hardware(n3d::SettingsMenu& menu, n3d::Config& config) {
    menu.open();
    check(menu.selected_action() == 0x26, "Configure initially selects Hardware action 0x26");
    menu.handle_key(config, 0x0d, true);
    check(menu.screen() == n3d::SettingsScreen::hardware && menu.selected_action() == 0x0d,
          "Hardware opens UIF10 on the Mouse slider");
}

void hardware_actions_and_raw_bytes() {
    n3d::SettingsMenu menu;
    n3d::Config config;
    config.unknown_bytes = {0xa5u, 0x5au};
    config.output_width = 640;
    config.output_height = 400;
    enter_hardware(menu, config);

    auto event = menu.handle_key(config, 0x27, true);
    check(config.mouse_sensitivity == 55 && config.mouse_enabled == 1,
          "right adjusts disabled slider from absolute value and enables it");
    check(event.reconcile_audio && !event.play_effect_2e && !event.set_music_volume,
          "hardware callback reconciles audio even when volume is unchanged");
    event = menu.handle_key(config, 0x20, true);
    check(config.mouse_sensitivity == 55 && config.mouse_enabled == 0,
          "space negates slider without changing its magnitude");
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x0e, "second row is action 0x0e joystick");
    event = menu.handle_key(config, 0x27, true);
    check(config.joystick_divisor == 55 && config.joystick_enabled == 1 &&
              event.recalibrate_joystick, "enabled joystick recalibrates after each callback");
    event = menu.handle_key(config, 0x28, true);
    check(event.recalibrate_joystick, "cursor motion recalibrates enabled joystick");
    check(menu.selected_action() == 0x10, "third row is Music action 0x10");
    event = menu.handle_key(config, 0x27, true);
    check(config.music_volume == 65 && event.set_music_volume && !event.play_effect_2e,
          "music volume change requests MIDI volume update only");
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x0f, "fourth row is Sound FX action 0x0f");
    event = menu.handle_key(config, 0x27, true);
    check(config.effects_volume == 75 && event.play_effect_2e && !event.set_music_volume,
          "effects volume change requests original 0x2e feedback");
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x27, "last row is Save action 0x27");
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x27, "down never wraps at the last row");
    event = menu.handle_key(config, 0x0d, true);
    check(event.save_config && menu.screen() == n3d::SettingsScreen::configure,
          "Save emits persistence command and pops to Configure");
    menu.handle_key(config, 0x0d, true);
    check(menu.selected_action() == 0x27, "hardware row cursor persists on reopening");
    event = menu.handle_key(config, 0x1b, true);
    check(!event.save_config && menu.screen() == n3d::SettingsScreen::configure,
          "Escape pops hardware without saving its already-live settings");
    menu.handle_key(config, 0x26, true);
    check(menu.selected_action() == 0x26, "up never wraps above Configure first row");
    event = menu.handle_key(config, 0x1b, true);
    check(event.returned_to_root && menu.screen() == n3d::SettingsScreen::closed,
          "Configure Escape returns to root without saving");
}

void slider_clamps_zero_and_joystick_gate() {
    n3d::SettingsMenu menu;
    n3d::Config config;
    config.mouse_sensitivity = 100;
    config.mouse_enabled = 0;
    config.joystick_divisor = 0;
    config.joystick_enabled = 0;
    enter_hardware(menu, config);
    menu.handle_key(config, 0x27, true);
    check(config.mouse_sensitivity == 100 && config.mouse_enabled == 1,
          "right at 100 clamps to 100 and enables");
    for (int i = 0; i < 21; ++i) menu.handle_key(config, 0x25, true);
    check(config.mouse_sensitivity == 0 && config.mouse_enabled == 0,
          "left clamps below zero and zero is disabled");
    menu.handle_key(config, 0x20, true);
    check(config.mouse_sensitivity == 0 && config.mouse_enabled == 0,
          "negating a zero slider cannot enable it");
    menu.handle_key(config, 0x28, true);
    auto event = menu.handle_joystick(config, 0x28, true);
    check(menu.selected_action() == 0x0e && !event.recalibrate_joystick,
          "disabled joystick cannot route menu actions");
    event = menu.handle_key(config, 0x27, true);
    check(config.joystick_divisor == 5 && config.joystick_enabled == 1 &&
              event.recalibrate_joystick, "right from zero makes a safe enabled divisor");
    event = menu.handle_joystick_axis(config, n3d::SettingsJoystickAxis::vertical, 7, true);
    check(menu.selected_action() == 0x0e && !event.recalibrate_joystick,
          "axis threshold is strict at +7");
    event = menu.handle_joystick_axis(config, n3d::SettingsJoystickAxis::vertical, 8, true);
    check(menu.selected_action() == 0x10 && event.recalibrate_joystick,
          "enabled joystick axis routes down above +7");
    menu.handle_joystick(config, 0x1b, true);
    check(menu.screen() == n3d::SettingsScreen::configure,
          "joystick button 2 Escape pops the submenu");
}

void cheats_done_cancel_and_gate() {
    n3d::SettingsMenu menu;
    n3d::Config config;
    menu.open();
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x11, "Configure second row is Cheats action 0x11");
    menu.handle_key(config, 0x0d, true);
    check(menu.screen() == n3d::SettingsScreen::cheats && menu.selected_action() == 0x22,
          "Cheats opens UIF11 on Omniscient 0x22");
    menu.handle_key(config, 0x20, true);
    menu.handle_key(config, 0x28, true);
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x24, "Omnificent third row is action 0x24");
    menu.handle_key(config, 0x0d, true);
    check(config.cheat_omniscient == 0 && config.cheat_omnificent == 0,
          "checkbox changes remain staged until Done");
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x25, "Omnifarious fourth row is action 0x25");
    menu.handle_key(config, 0x28, true);
    menu.handle_key(config, 0x28, true);
    check(menu.selected_action() == 0x19, "Cancel is original action 0x19");
    auto event = menu.handle_key(config, 0x0d, true);
    check(menu.screen() == n3d::SettingsScreen::configure && !event.save_config &&
              config.cheat_omniscient == 0 && config.cheat_omnificent == 0,
          "Cancel drops staged checks and never persists");
    menu.handle_key(config, 0x0d, true);
    check(menu.selected_action() == 0x19, "Cheat row cursor persists across visits");
    menu.handle_key(config, 0x26, true);
    check(menu.selected_action() == 0x28, "Done uses action 0x28");
    menu.handle_key(config, 0x26, true);
    menu.handle_key(config, 0x20, true); // Omnifarious 0x25, raw +0x12.
    menu.handle_key(config, 0x26, true);
    menu.handle_key(config, 0x20, true); // Omnificent 0x24, raw +0x13.
    menu.handle_key(config, 0x28, true);
    menu.handle_key(config, 0x28, true);
    event = menu.handle_key(config, 0x0d, true);
    check(event.save_config && event.apply_cheats && !event.show_unavailable_cheats &&
              menu.screen() == n3d::SettingsScreen::configure,
          "registered Done applies cheats, saves, and pops");
    check(config.cheat_omnifarious == 1 && config.cheat_omnificent == 1 &&
              config.cheat_omniscient == 0 && config.cheat_omnipotent == 0,
          "0x25/0x24 action order maps to original +0x12/+0x13 byte order");

    menu.handle_key(config, 0x0d, false); // Configure cursor remains Cheats.
    event = menu.handle_key(config, 0x0d, false); // Staged Done; original bytes nonzero.
    check(config.cheat_omnifarious == 0 && config.cheat_omnificent == 0,
          "unavailable Done clears even pre-existing selected cheat bytes");
    check(event.save_config && event.show_unavailable_cheats && !event.apply_cheats &&
              menu.screen() == n3d::SettingsScreen::unavailable_cheats,
          "BSF +3 zero saves cleared cheats and requests the modal");
    event = menu.handle_key(config, 0x1b, false);
    check(menu.screen() == n3d::SettingsScreen::configure && !event.save_config,
          "modal Escape returns to Configure without a second save");
}

void mouse_hover_and_error_boundaries() {
    n3d::SettingsMenu menu;
    n3d::Config config;
    n3d::Assets assets{};
    n3d::InputFrame input{};
    menu.open();
    input.cursor_x = 110;
    input.cursor_y = 100; // Cheats in Configure.
    input.menu_left_released = true;
    menu.process(assets, config, input, true);
    check(menu.screen() == n3d::SettingsScreen::configure && menu.selected_action() == 0x26,
          "click outside selected row does not activate it");
    input.menu_left_released = false;
    input.cursor_moved = true;
    menu.process(assets, config, input, true);
    check(menu.selected_action() == 0x11, "mouse hover changes selected row only on motion");
    input.cursor_moved = false;
    input.menu_left_released = true;
    menu.process(assets, config, input, true);
    check(menu.screen() == n3d::SettingsScreen::cheats,
          "release on previously selected text activates it");
    input = {};
    input.key = 0x1b;
    input.held = 0x100u;
    menu.process(assets, config, input, true);
    check(menu.screen() == n3d::SettingsScreen::cheats,
          "Alt suppresses keyboard routing");
    input.held = 0;
    menu.process(assets, config, input, true);
    check(menu.screen() == n3d::SettingsScreen::configure,
          "Escape routes after Alt is released");
    menu.handle_key(config, 0x26, true); // Configure row 1 -> 0.
    config.mouse_sensitivity = 101; // Invalid for the source-backed slider geometry.
    try {
        menu.handle_key(config, 0x0d, true);
        check(false, "invalid slider value must fail closed");
    } catch (const std::invalid_argument&) {
        check(menu.screen() == n3d::SettingsScreen::configure &&
                  config.mouse_sensitivity == 101, "invalid source slider remains untouched");
    }
}

void slider_and_checkbox_click_regions() {
    n3d::SettingsMenu menu;
    n3d::Config config;
    n3d::Assets assets{};
    n3d::InputFrame click{};
    enter_hardware(menu, config);
    click.cursor_x = 200; // The slider bar is intentionally not a hit region.
    click.cursor_y = 70;
    click.menu_left_released = true;
    menu.process(assets, config, click, true);
    check(config.mouse_enabled == 0 && config.mouse_sensitivity == 50,
          "clicking slider bar does not drag or change value");
    click.cursor_x = 110;
    auto event = menu.process(assets, config, click, true);
    check(config.mouse_enabled == 1 && config.mouse_sensitivity == 50 &&
              event.reconcile_audio, "clicking slider label toggles sign and calls callback");

    menu.handle_key(config, 0x1b, true);
    menu.handle_key(config, 0x28, true);
    menu.handle_key(config, 0x0d, true);
    click.cursor_x = 99; // Checkbox indicator is outside the source text hit-test.
    menu.process(assets, config, click, true);
    click.cursor_x = 110;
    menu.process(assets, config, click, true);
    for (int row = 0; row < 4; ++row) menu.handle_key(config, 0x28, true);
    event = menu.handle_key(config, 0x0d, true);
    check(event.save_config && config.cheat_omniscient == 1 &&
              config.cheat_omnipotent == 0,
          "only checkbox text click toggles staged cheat before Done");
}

} // namespace

int run_settings_menu_tests() {
    hardware_actions_and_raw_bytes();
    slider_clamps_zero_and_joystick_gate();
    cheats_done_cancel_and_gate();
    mouse_hover_and_error_boundaries();
    slider_and_checkbox_click_regions();
    if (failures) std::cerr << failures << " settings menu checks failed\n";
    return failures;
}
