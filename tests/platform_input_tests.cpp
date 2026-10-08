#include "../src/platform/input.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;
constexpr RECT image_rect{0, 0, 320, 200};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void enable_wasd(n3d::platform::InputTranslator& input) {
    input.attach(nullptr);
    input.focus(true);
    input.set_wasd(true);
}

struct DirectionCase final {
    WPARAM physical;
    std::uint16_t held;
    std::uint8_t action_key;
    std::string_view held_message;
    std::string_view key_message;
    std::string_view release_message;
};

void wasd_uses_original_direction_actions() {
    n3d::platform::InputTranslator input;
    enable_wasd(input);

    constexpr std::array directions{
        DirectionCase{'W', 0x0002, VK_UP, "W holds original Up", "W emits VK_UP action", "W release stops Up"},
        DirectionCase{'A', 0x0008, VK_LEFT, "A holds original Left", "A emits VK_LEFT action", "A release stops Left"},
        DirectionCase{'S', 0x0004, VK_DOWN, "S holds original Down", "S emits VK_DOWN action", "S release stops Down"},
        DirectionCase{'D', 0x0010, VK_RIGHT, "D holds original Right", "D emits VK_RIGHT action", "D release stops Right"},
    };
    for (const auto& direction : directions) {
        input.message(WM_KEYDOWN, direction.physical, 0, image_rect);
        const auto pressed = input.consume();
        check(pressed.held == direction.held, direction.held_message);
        check(pressed.key == direction.action_key, direction.key_message);

        if (direction.physical == 'W') {
            const auto continued = input.consume();
            check(continued.held == 0x0002, "W remains held on the next update");
            check(continued.key == 0, "W action key is a one-shot edge");
        }

        input.message(WM_KEYUP, direction.physical, 0, image_rect);
        const auto released = input.consume();
        check(released.held == 0 && released.key == 0, direction.release_message);
    }

    input.message(WM_KEYDOWN, 'W', 0, image_rect);
    input.message(WM_KEYUP, 'W', 0, image_rect);
    const auto quick_tap = input.consume();
    check(quick_tap.held == 0, "W released before the update cannot keep movement held");
    check(quick_tap.key == VK_UP, "brief W tap still delivers its original Up action");
}

void help_mode_keeps_raw_vk_for_non_printable_page_keys() {
    n3d::platform::InputTranslator input;
    enable_wasd(input);

    input.message(WM_KEYDOWN, VK_PRIOR, 0, image_rect);
    const auto prior = input.consume(true);
    check(prior.held == 0 && prior.key == VK_PRIOR,
          "PgUp in help mode keeps the original raw VK value");

    input.message(WM_KEYDOWN, VK_NEXT, 0, image_rect);
    const auto next = input.consume(true);
    check(next.held == 0 && next.key == VK_NEXT,
          "PgDn in help mode keeps the original raw VK value");
}

void arrows_still_work_with_wasd_enabled() {
    n3d::platform::InputTranslator input;
    enable_wasd(input);

    constexpr std::array arrows{
        DirectionCase{VK_UP, 0x0002, VK_UP, "Up arrow holds original Up", "Up arrow emits VK_UP", "Up arrow release stops Up"},
        DirectionCase{VK_LEFT, 0x0008, VK_LEFT, "Left arrow holds original Left", "Left arrow emits VK_LEFT", "Left arrow release stops Left"},
        DirectionCase{VK_DOWN, 0x0004, VK_DOWN, "Down arrow holds original Down", "Down arrow emits VK_DOWN", "Down arrow release stops Down"},
        DirectionCase{VK_RIGHT, 0x0010, VK_RIGHT, "Right arrow holds original Right", "Right arrow emits VK_RIGHT", "Right arrow release stops Right"},
    };
    for (const auto& arrow : arrows) {
        input.message(WM_KEYDOWN, arrow.physical, 0, image_rect);
        const auto pressed = input.consume();
        check(pressed.held == arrow.held, arrow.held_message);
        check(pressed.key == arrow.action_key, arrow.key_message);
        input.message(WM_KEYUP, arrow.physical, 0, image_rect);
        const auto released = input.consume();
        check(released.held == 0 && released.key == 0, arrow.release_message);
    }

    input.message(WM_KEYDOWN, 'W', 0, image_rect);
    input.message(WM_KEYDOWN, VK_UP, 0, image_rect);
    const auto overlapping = input.consume();
    check(overlapping.held == 0x0002, "W and Up arrow share the original Up action");
    input.message(WM_KEYUP, 'W', 0, image_rect);
    const auto arrow_still_held = input.consume();
    check(arrow_still_held.held == 0x0002, "releasing W cannot release a held Up arrow");
    input.message(WM_KEYUP, VK_UP, 0, image_rect);
    check(input.consume().held == 0, "releasing the remaining Up arrow stops movement");
}

void focus_loss_clears_motion_modifiers_and_pending_actions() {
    n3d::platform::InputTranslator input;
    enable_wasd(input);

    input.message(WM_KEYDOWN, 'W', 0, image_rect);
    input.message(WM_KEYDOWN, VK_SHIFT, 0x2aL << 16, image_rect);
    input.message(WM_KEYDOWN, VK_SHIFT, 0x36L << 16, image_rect);
    input.message(WM_KEYDOWN, VK_CONTROL, 0, image_rect);
    input.message(WM_SYSKEYDOWN, VK_MENU, 0, image_rect);
    const auto active = input.consume();
    check(active.held == 0x01e2, "W and both Shifts, Ctrl, Alt retain original action bits");
    check(active.key == VK_MENU, "Alt is the last pending action key");

    input.message(WM_SYSKEYUP, VK_MENU, 0, image_rect);
    const auto no_alt = input.consume();
    check(no_alt.held == 0x00e2, "Alt release clears its held bit without stopping W or modifiers");
    check(no_alt.key == 0, "Alt release does not generate a new action key");

    input.message(WM_KEYDOWN, 'A', 0, image_rect);
    input.focus(false);
    const auto unfocused = input.consume();
    check(unfocused.held == 0 && unfocused.key == 0,
          "focus loss clears held motion, modifiers, and pending A action");
    input.message(WM_KEYDOWN, 'D', 0, image_rect);
    input.focus(true);
    const auto refocused = input.consume();
    check(refocused.held == 0 && refocused.key == 0,
          "refocus cannot resurrect held keys or keys pressed while unfocused");

    input.message(WM_KEYDOWN, 'W', 0, image_rect);
    const auto fresh = input.consume();
    check(fresh.held == 0x0002 && fresh.key == VK_UP,
          "new W press after refocus starts the original Up action");
    input.message(WM_KEYUP, 'W', 0, image_rect);
    check(input.consume().held == 0, "releasing W after refocus stops movement");
}
} // namespace

int run_platform_input_tests() {
    wasd_uses_original_direction_actions();
    help_mode_keeps_raw_vk_for_non_printable_page_keys();
    arrows_still_work_with_wasd_enabled();
    focus_loss_clears_motion_modifiers_and_pending_actions();
    if (failures != 0) {
        std::cerr << failures << " platform input checks failed\n";
        return failures;
    }
    std::cout << "n3d platform input checks passed\n";
    return 0;
}
