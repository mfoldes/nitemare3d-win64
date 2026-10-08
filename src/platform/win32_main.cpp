#include "input.hpp"
#include "config_storage.hpp"
#include "map_browser_win32.hpp"
#include "save_storage.hpp"
#include "paths.hpp"
#include "window.hpp"

#include "n3d/game.hpp"
#include "n3d/help.hpp"
#include "n3d/savegame.hpp"
#include "n3d/platform_effects.hpp"
#include "n3d/platform_midi.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

class NativeError final {
public:
    explicit NativeError(std::wstring message) : message_(std::move(message)) {}
    [[nodiscard]] const std::wstring& message() const noexcept { return message_; }

private:
    std::wstring message_;
};

[[nodiscard]] std::wstring diagnostic(std::string_view bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return L"(diagnostic exceeds the Windows text limit)";
    const int length = static_cast<int>(bytes.size());
    UINT encoding = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int count = MultiByteToWideChar(encoding, flags, bytes.data(), length, nullptr, 0);
    if (!count) {
        // Filesystem exception paths can be encoded in the active Windows code page.
        encoding = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(encoding, flags, bytes.data(), length, nullptr, 0);
    }
    if (!count) return L"(diagnostic text could not be decoded)";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(encoding, flags, bytes.data(), length,
                             result.data(), count))
        return L"(diagnostic text could not be decoded)";
    return result;
}

[[noreturn]] void window_failure(const std::wstring& reason) {
    throw NativeError(reason);
}

void show_warning(HWND owner, const std::wstring& message) {
    if (!MessageBoxW(owner, message.c_str(), L"Nitemare 3D - device warning",
                     MB_OK | MB_ICONWARNING | MB_TASKMODAL)) {
        throw NativeError(L"Could not show device warning (Win32 " +
                          std::to_wstring(GetLastError()) + L")");
    }
}

void show_save_error(HWND owner, const std::wstring& message) {
    if (!MessageBoxW(owner, message.c_str(), L"Nitemare 3D - save/load error",
                     MB_OK | MB_ICONERROR | MB_TASKMODAL))
        throw NativeError(L"Could not show save/load error (Win32 " +
                          std::to_wstring(GetLastError()) + L")");
}

void show_save_error(HWND owner, const std::exception& problem) {
    show_save_error(owner, diagnostic(problem.what()));
}

[[nodiscard]] std::array<n3d::SaveMenuSlot, 10> native_slots(
    const std::filesystem::path& user_directory) {
    std::array<n3d::SaveMenuSlot, 10> slots{};
    for (std::size_t index = 0; index < slots.size(); ++index) {
        auto& slot = slots[index];
        slot.slot = static_cast<std::uint8_t>(index);
        slot.present = n3d::platform::native_slot_present(user_directory, index);
        if (slot.present)
            slot.label = "Native slot " + std::string(index < 9 ? "0" : "") +
                         std::to_string(index + 1);
    }
    return slots;
}

void midi_warning(HWND owner, const n3d::MidiStreamPlayer& player,
                  std::string_view& last_reason, std::uint32_t& last_code) {
    const auto reason = player.last_error();
    const auto code = player.last_native_error();
    if (reason == last_reason && code == last_code) return;
    last_reason = reason;
    last_code = code;
    std::wstring message = L"MIDI playback is unavailable: " + diagnostic(reason);
    if (code) message += L" (WinMM " + std::to_wstring(code) + L")";
    show_warning(owner, message);
}

void effects_warning(HWND owner, const n3d::EffectsPlayer& player,
                     std::string_view& last_reason, std::uint32_t& last_code) {
    const auto reason = player.last_error();
    const auto code = player.last_native_error();
    if (reason == last_reason && code == last_code) return;
    last_reason = reason;
    last_code = code;
    std::wstring message = L"Sound effects are unavailable: " + diagnostic(reason);
    if (code) {
        wchar_t formatted[11]{};
        swprintf_s(formatted, L"0x%08X", static_cast<unsigned>(code));
        message += L" (HRESULT ";
        message += formatted;
        message += L")";
    }
    show_warning(owner, message);
}

struct AudioOutput final {
    explicit AudioOutput(const n3d::Assets& assets) : effects(assets), midi(assets) {}

    void initialize(HWND owner, const n3d::Config& config) {
        if (!effects.ready()) effects_warning(owner, effects, effects_warning_shown,
                                             effects_code_shown);
        else if (!effects.set_volume(config.effects_volume))
            effects_warning(owner, effects, effects_warning_shown, effects_code_shown);
        if (!midi.set_volume(config.music_volume))
            midi_warning(owner, midi, midi_warning_shown, midi_code_shown);
        if (effects.ready() && !effects.set_muted(!config.effects_enabled))
            effects_warning(owner, effects, effects_warning_shown, effects_code_shown);
    }

    void reconcile(HWND owner, const n3d::GameState& state) {
        if (state.config.music_enabled) {
            if (state.current_tune && active_tune != state.current_tune) {
                active_tune = 0;
                if (midi.play(state.current_tune)) active_tune = state.current_tune;
                else midi_warning(owner, midi, midi_warning_shown, midi_code_shown);
            }
        } else if (active_tune) {
            if (!midi.stop()) midi_warning(owner, midi, midi_warning_shown, midi_code_shown);
            active_tune = 0;
        }
        if (effects.ready() && !effects.set_muted(!state.config.effects_enabled))
            effects_warning(owner, effects, effects_warning_shown, effects_code_shown);
    }

    void drain(HWND owner, n3d::GameState& state) {
        const auto& event = state.settings_outcome;
        if (event.set_music_volume && !midi.set_volume(state.config.music_volume))
            midi_warning(owner, midi, midi_warning_shown, midi_code_shown);
        if (event.play_effect_2e && effects.ready() &&
            !effects.set_volume(state.config.effects_volume))
            effects_warning(owner, effects, effects_warning_shown, effects_code_shown);
        if (event.reconcile_audio) reconcile(owner, state);
        if (event.play_effect_2e && state.config.effects_enabled &&
            !effects.play(0x2e, 0, 2))
            effects_warning(owner, effects, effects_warning_shown, effects_code_shown);

        for (const auto& command : state.audio.pending()) {
            switch (command.kind) {
            case n3d::AudioCommandKind::effect:
                if (state.config.effects_enabled &&
                    !effects.play(command.id, command.mode, command.priority))
                    effects_warning(owner, effects, effects_warning_shown,
                                    effects_code_shown);
                break;
            case n3d::AudioCommandKind::music:
                if (state.config.music_enabled) {
                    active_tune = 0;
                    if (midi.play(command.id)) active_tune = command.id;
                    else midi_warning(owner, midi, midi_warning_shown, midi_code_shown);
                }
                break;
            default:
                throw std::logic_error("unknown audio command kind");
            }
        }
        state.audio.clear();
    }

    void update(HWND owner) {
        if (!midi.update()) {
            active_tune = 0;
            midi_warning(owner, midi, midi_warning_shown, midi_code_shown);
        }
    }

    n3d::EffectsPlayer effects;
    n3d::MidiStreamPlayer midi;
    std::uint8_t active_tune = 0;  // What the host sequencer is actually playing.
    std::string_view effects_warning_shown{};
    std::uint32_t effects_code_shown = 0;
    std::string_view midi_warning_shown{};
    std::uint32_t midi_code_shown = 0;
};

void present(n3d::platform::Window& window, const n3d::GameState& state,
             const n3d::Assets& assets, n3d::IndexedFrame& frame) {
    n3d::render(state, assets, frame);
    std::wstring error;
    if (!window.present(frame, error)) window_failure(error);
}

[[nodiscard]] bool focused(const n3d::platform::Window& window) noexcept {
    const HWND handle = window.native_handle();
    return handle && GetFocus() == handle && GetForegroundWindow() == handle &&
           !IsIconic(handle);
}

void wait_for_messages(DWORD milliseconds) {
    if (MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
        throw NativeError(L"MsgWaitForMultipleObjectsEx failed (Win32 " +
                          std::to_wstring(GetLastError()) + L")");
    }
}

// The original service samples absolute 32-bit millisecond products. Wake at
// the next quotient boundary (including product wrap) without inventing an FPS.
[[nodiscard]] DWORD milliseconds_to_quotient(std::uint32_t now, std::uint32_t rate) noexcept {
    const std::uint32_t product = now * rate;
    const std::uint64_t until_increment = (1000u - product % 1000u + rate - 1u) / rate;
    const std::uint64_t until_wrap = (0x100000000ull - product + rate - 1u) / rate;
    return static_cast<DWORD>(std::max<std::uint64_t>(1,
                                                     std::min(until_increment, until_wrap)));
}

[[nodiscard]] bool has_menu_input(const n3d::InputFrame& input) noexcept {
    return input.key || input.cursor_moved || input.menu_left_released ||
           input.menu_right_pressed || input.joystick_fire || input.joystick_use ||
           input.joystick_forward || input.joystick_turn || input.joystick_strafe;
}

struct Calibration final {
    bool begun = false;
    unsigned measured = 0;
    std::chrono::steady_clock::duration total{};
};

struct RestoredClock final {
    std::uint32_t anchor_ms = 0;
    std::uint64_t host_epoch_ms = 0;
    bool active = false;
    bool waiting_for_release = false;

    [[nodiscard]] std::uint64_t now() const noexcept {
        const auto host_now = GetTickCount64();
        if (!active) return host_now;
        return static_cast<std::uint32_t>(anchor_ms +
            (waiting_for_release ? 0u : static_cast<std::uint32_t>(host_now - host_epoch_ms)));
    }

    void begin(std::uint32_t saved_anchor_ms) noexcept {
        anchor_ms = saved_anchor_ms;
        active = true;
        waiting_for_release = true;
    }

    void resume_after_release() noexcept {
        host_epoch_ms = GetTickCount64();
        waiting_for_release = false;
    }
};

int run(HINSTANCE instance, int show_command) {
    int argc = 0;
    wchar_t** raw_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!raw_argv) {
        throw NativeError(L"CommandLineToArgvW failed (Win32 " +
                          std::to_wstring(GetLastError()) + L")");
    }
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> argv(raw_argv, LocalFree);
    const auto options = n3d::platform::parse_launch_options(argc, argv.get());
    auto paths = n3d::platform::resolve_paths(options);
    if (options.maps) return n3d::platform::run_map_browser(instance, paths.assets,
                                                             paths.data_directory());
    auto state = std::make_unique<n3d::GameState>();
    auto config = n3d::platform::load_user_config(paths.user_directory);
    if (paths.modern_controls) config.mouse_enabled = 1; // Explicit --modern-controls override.
    n3d::initialize_game(*state, paths.assets, config);

    n3d::platform::Window window;
    std::wstring error;
    if (!window.create(instance, L"Nitemare-3D for Windows V1.10", error))
        window_failure(error);
    window.set_square_pixels(paths.square_pixels);
    window.input().set_wasd(paths.modern_controls);
    if (paths.modern_controls && state->config.mouse_enabled &&
        !window.input().set_relative_mouse(true, error))
        show_warning(window.native_handle(), L"Relative mouse is unavailable: " + error +
                     L"\nKeyboard and legacy mouse buttons remain available.");
    n3d::IndexedFrame frame{};
    present(window, *state, paths.assets, frame); // Retained for the first WM_PAINT.
    window.show(show_command);

    AudioOutput audio(paths.assets);
    audio.initialize(window.native_handle(), state->config);
    audio.drain(window.native_handle(), *state); // The initial title tune, once.
    Calibration calibration;
    bool was_focused = false;
    bool joystick_configured = false;
    std::optional<n3d::FliPlayer> ending;
    std::uint64_t last_fli_frame_ms = 0;
    RestoredClock virtual_clock;
    const auto reconcile_relative_mouse = [&]() {
        if (paths.modern_controls &&
            (state->config.mouse_enabled != 0) != window.input().relative_mouse() &&
            !window.input().set_relative_mouse(state->config.mouse_enabled != 0, error))
            show_warning(window.native_handle(), L"Relative mouse is unavailable: " + error);
    };

    for (;;) {
        if (!window.pump_messages(error)) {
            if (!error.empty()) window_failure(error);
            return EXIT_SUCCESS; // WM_CLOSE/WM_DESTROY posted WM_QUIT.
        }
        const HWND owner = window.native_handle();
        const auto disable_joystick = [&](const std::wstring& reason) {
            state->config.joystick_enabled = 0;
            state->settings.disable_joystick();
            joystick_configured = false;
            if (state->screen == n3d::Screen::settings &&
                state->settings.screen() == n3d::SettingsScreen::hardware) {
                state->settings.compose(paths.assets, state->composed_frame, state->ega_map);
                present(window, *state, paths.assets, frame);
            }
            show_warning(owner, reason);
        };
        audio.update(owner);
        const bool is_focused = focused(window);
        if (is_focused != was_focused) {
            window.input().focus(is_focused); // Also discard input across deactivation.
            was_focused = is_focused;
        }

        const bool background_demo = !is_focused && state->demo.has_value();
        if (!is_focused && !background_demo) {
            wait_for_messages(20);
            continue;
        }

        if (state->config.joystick_enabled && !joystick_configured) {
            if (window.input().set_joystick(true, state->config.joystick_divisor, error))
                joystick_configured = true;
            else disable_joystick(L"Joystick is unavailable: " + error);
        } else if (!state->config.joystick_enabled && joystick_configured) {
            (void)window.input().set_joystick(false, state->config.joystick_divisor, error);
            joystick_configured = false;
        }
        if (!focused(window) && !background_demo) continue; // A device warning may have deactivated us.
        if (virtual_clock.waiting_for_release) {
            // The decoded 320x200 DIB was already presented. Do not run a core
            // step or count wall time while keys held at the old process/menu
            // boundary could trigger another shot, use, or menu selection.
            auto discarded = window.input().consume();
            bool released = window.input().physical_buttons_released();
            if (state->config.joystick_enabled) {
                if (!window.input().sample_joystick(discarded, error)) {
                    disable_joystick(L"Joystick input stopped: " + error);
                    released = false;
                } else if (discarded.joystick_forward || discarded.joystick_turn ||
                           discarded.joystick_strafe || discarded.joystick_fire ||
                           discarded.joystick_use) {
                    released = false;
                }
            }
            if (!released || !focused(window)) {
                wait_for_messages(20);
                continue;
            }
            state->previous_use = 0;
            state->previous_fire = 0;
            window.input().reset();
            virtual_clock.resume_after_release();
            continue;
        }
        if (!background_demo && state->demo) {
            // WM_KEYDOWN (including modifiers and repeats) aborts immediately,
            // even during five-frame calibration or a recorded modal wait.
            const auto live = window.input().consume();
            if (live.key) {
                n3d::abort_demo(*state, paths.assets);
                calibration = {};
                reconcile_relative_mouse();
                audio.drain(owner, *state);
                audio.reconcile(owner, *state);
                present(window, *state, paths.assets, frame);
                continue;
            }
        }

        if (state->screen == n3d::Screen::ending) {
            if (!ending) ending.emplace(paths.assets); // Owns only mutable decode state.
            const auto input = background_demo ? n3d::InputFrame{} : window.input().consume();
            const auto now = virtual_clock.now();
            const bool key_closes = !(input.held & 0x100u) &&
                (input.key == VK_ESCAPE || input.key == VK_RETURN || input.key == VK_SPACE);
            const bool frame_due = !ending->decoded_frames() ||
                now - last_fli_frame_ms >= ending->frame_interval_ms();
            if (key_closes || (frame_due && !ending->next_frame())) {
                ending.reset(); // 1010:6d2c explicitly closes before the tally.
                n3d::InputFrame close = input;
                close.ending_finished = !key_closes;
                n3d::step(*state, paths.assets, close, now);
                audio.drain(owner, *state);
                present(window, *state, paths.assets, frame);
            } else if (frame_due) {
                last_fli_frame_ms = now; // At most one decode; no catch-up loop.
                if (!window.present(ending->frame(), error)) window_failure(error);
            }
            wait_for_messages(20); // Speed 9 is tested from the last decode baseline.
            continue;
        }

        if (state->screen == n3d::Screen::gameplay && !state->clocks.calibrated) {
            if (!calibration.begun) {
                present(window, *state, paths.assets, frame);
                if (!focused(window) && !background_demo) continue;
                n3d::begin_calibration(*state); // The provisional frame was presented first.
                calibration.begun = true;
                calibration.measured = 0;
                calibration.total = {};
            } else {
                const auto before = std::chrono::steady_clock::now();
                n3d::advance_calibration_frame(*state, paths.assets,
                                               static_cast<std::uint32_t>(virtual_clock.now()));
                present(window, *state, paths.assets, frame);
                calibration.total += std::chrono::steady_clock::now() - before;
                if (++calibration.measured == 5) {
                    // The high-resolution host measurement can be positive yet
                    // truncate to zero milliseconds. Preserve its nonzero
                    // magnitude at the integer-ms boundary used by DS:53f2;
                    // no invented render period or E2 core-wait divisor.
                    const auto measured_ms = std::chrono::ceil<std::chrono::milliseconds>(
                        calibration.total / calibration.measured).count();
                    if (measured_ms <= 0 ||
                        static_cast<std::uint64_t>(measured_ms) >
                            std::numeric_limits<std::uint32_t>::max())
                        throw std::runtime_error("five-present calibration has no representable positive millisecond average");
                    n3d::calibrate(*state, static_cast<std::uint32_t>(measured_ms));
                    calibration = {};
                }
            }
            continue; // No gameplay step or input consumption during calibration.
        }

        const std::uint64_t absolute_ms = virtual_clock.now();
        const auto now = static_cast<std::uint32_t>(absolute_ms);
        const auto old_screen = state->screen;
        const auto old_menu_row = state->menu_row;
        const auto old_difficulty_row = state->difficulty_row;
        const bool old_save_menu_pending = state->save_menu_pending;
        const auto old_help_page = state->help_pager ? state->help_pager->page_index() : 0;
        const auto old_tick = state->clocks.logical_tick;
        bool step_due = false;
        n3d::InputFrame input{};
        DWORD wait_ms = 20; // Menu joystick and MIDI service without polling the core.

        if (state->screen == n3d::Screen::gameplay &&
            (state->player_life_state == 1 || state->player_life_state == 2)) {
            const auto& clocks = state->clocks;
            if (!clocks.rate) throw std::logic_error("calibrated gameplay rate is zero");
            const bool a_changed = (now * 8u) / 1000u != clocks.previous_a_quotient;
            const bool b_changed = (now * static_cast<std::uint32_t>(clocks.rate)) / 1000u !=
                                   clocks.previous_b_quotient;
            const bool b_pass = clocks.b_count + static_cast<unsigned>(b_changed) >=
                                clocks.b_deadline;
            step_due = a_changed || b_changed || clocks.a_count >= clocks.a_deadline ||
                       b_pass;
            if (step_due && b_pass) {
                if (background_demo) {
                    input = {};
                } else {
                    input = window.input().consume();
                    if (state->config.joystick_enabled &&
                        !window.input().sample_joystick(input, error))
                        disable_joystick(L"Joystick input stopped: " + error);
                }
            }
            wait_ms = std::min(milliseconds_to_quotient(now, 8),
                               milliseconds_to_quotient(now, clocks.rate));
        } else if (state->screen == n3d::Screen::gameplay) {
            step_due = true; // State-1 idle dispatches death, demo loop, or victory.
            wait_ms = 1;
        } else {
            // 1010:8c9a routes one translated ASCII byte to the pager, else the VK.
            // Recorded modal transitions also advance in the background, with no live input.
            if (!background_demo) {
                input = window.input().consume(state->screen == n3d::Screen::help ||
                                               state->screen == n3d::Screen::quest_story);
                if (state->config.joystick_enabled) {
                    std::uint8_t joystick_key = 0;
                    if (!window.input().sample_menu_joystick(now, joystick_key, error))
                        disable_joystick(L"Joystick input stopped: " + error);
                    else if (!input.key) input.key = joystick_key;
                }
            }
            step_due = has_menu_input(input) || state->demo.has_value();
            if (state->demo) wait_ms = 1; // 90ce advances modal demo ticks on idle.
        }
        if (!focused(window) && !background_demo) continue;

        if (step_due) {
            if (old_screen == n3d::Screen::gameplay &&
                (input.key == VK_F4 || input.key == VK_F5)) {
                try {
                    state->save_slots = native_slots(paths.user_directory);
                } catch (const std::exception& problem) {
                    show_save_error(owner, problem);
                    continue; // Do not enter a save menu with unknowable slot state.
                }
            }
            n3d::step(*state, paths.assets, input, absolute_ms);
            if (old_screen != n3d::Screen::gameplay &&
                old_screen != n3d::Screen::save_menu &&
                old_screen != n3d::Screen::load_menu &&
                (state->screen == n3d::Screen::save_menu ||
                 state->screen == n3d::Screen::load_menu)) {
                try {
                    state->save_slots = native_slots(paths.user_directory);
                    n3d::refresh_save_menu(*state, paths.assets);
                } catch (const std::exception& problem) {
                    n3d::InputFrame cancel{};
                    cancel.key = VK_ESCAPE;
                    n3d::step(*state, paths.assets, cancel, absolute_ms);
                    show_save_error(owner, problem);
                }
            }
            const auto& event = state->settings_outcome;
            reconcile_relative_mouse();
            if (event.recalibrate_joystick && state->config.joystick_enabled) {
                if (window.input().set_joystick(true, state->config.joystick_divisor, error))
                    joystick_configured = true;
                else disable_joystick(L"Joystick is unavailable: " + error);
            } else if (event.reconcile_audio && !state->config.joystick_enabled &&
                       joystick_configured) {
                (void)window.input().set_joystick(false, state->config.joystick_divisor, error);
                joystick_configured = false;
            }
            if (event.save_config)
                n3d::platform::save_user_config(paths.user_directory, state->config);
            audio.drain(owner, *state);
            if (const auto request = state->save_menu_outcome.request) {
                if (request->operation == n3d::SaveMenuOperation::save) {
                    try {
                        const auto bytes = n3d::encode_save(*state, paths.assets, now);
                        n3d::platform::save_native_slot(paths.user_directory, request->slot,
                                                        std::span<const std::byte>(bytes.data(), bytes.size()));
                    } catch (const std::exception& problem) {
                        show_save_error(owner, problem); // The old slot and live state remain intact.
                    }
                } else {
                    try {
                        const auto bytes = n3d::platform::load_native_slot(paths.user_directory,
                                                                           request->slot);
                        if (!bytes) {
                            show_save_error(owner, L"Selected native save slot no longer exists.");
                        } else {
                            const auto path = n3d::platform::native_slot_path(paths.user_directory,
                                                                               request->slot);
                            auto loaded = n3d::decode_save(
                                std::span<const std::byte>(bytes->data(), bytes->size()),
                                paths.assets, path.string());
                            // Present the serialized, already-composed DIB before any
                            // calibration or world update and publish only validated state.
                            n3d::render(*loaded.state, paths.assets, frame);
                            if (!window.present(frame, error)) window_failure(error);
                            const auto anchor = loaded.save_anchor_ms;
                            state = std::move(loaded.state);
                            virtual_clock.begin(anchor);
                            calibration = {};
                            ending.reset();
                            last_fli_frame_ms = 0;
                            audio.drain(owner, *state); // One saved tune; no baseline effects.
                            reconcile_relative_mouse();
                            continue; // Next pump waits for real physical release.
                        }
                    } catch (const std::exception& problem) {
                        show_save_error(owner, problem); // No decoded state was published.
                    }
                }
                state->save_menu_outcome = {};
            }
            const bool repaint_help = state->help_order_requested;
            if (repaint_help) {
                state->help_order_requested = false;
                // The original 054c banner is in the indexed DIB while 308e's
                // Warning blocks; 17d8 repaints the same page afterward.
                n3d::render(*state, paths.assets, frame);
                n3d::render_help_order_banner(paths.assets, frame);
                if (!window.present(frame, error)) window_failure(error);
                MessageBeep(MB_ICONEXCLAMATION);
                if (!MessageBoxW(owner, L"Error opening file order.txt", L"Warning",
                                 MB_OK | MB_ICONEXCLAMATION | MB_TASKMODAL))
                    window_failure(L"Could not show missing order.txt warning");
            }
            if (state->quit_requested) return EXIT_SUCCESS;
            const bool changed = old_screen != state->screen ||
                                 old_menu_row != state->menu_row ||
                                 old_difficulty_row != state->difficulty_row ||
                                 old_save_menu_pending != state->save_menu_pending ||
                                 (old_tick != state->clocks.logical_tick &&
                                  state->screen == n3d::Screen::gameplay) || repaint_help ||
                                 (state->help_pager && old_help_page != state->help_pager->page_index());
            if ((changed || state->screen == n3d::Screen::settings) &&
                state->screen != n3d::Screen::ending &&
                !(state->screen == n3d::Screen::gameplay &&
                  !state->clocks.calibrated))
                present(window, *state, paths.assets, frame);
            if (state->screen == n3d::Screen::gameplay && !state->clocks.calibrated)
                continue; // Present the new provisional scene before any next step.
        }
        wait_for_messages(wait_ms);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    std::wstring message;
    try {
        return run(instance, show_command);
    } catch (const NativeError& problem) {
        message = problem.message();
    } catch (const n3d::DataError& problem) {
        message = L"Game data error:\nPath: " + diagnostic(problem.path()) +
                  L"\nByte offset: " + std::to_wstring(problem.offset()) +
                  L"\nReason: " + diagnostic(problem.invariant());
    } catch (const std::exception& problem) {
        message = diagnostic(problem.what());
    } catch (...) {
        message = L"An unknown native failure stopped Nitemare 3D.";
    }
    MessageBoxW(nullptr, message.c_str(), L"Nitemare 3D - cannot continue",
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
    return EXIT_FAILURE;
}
