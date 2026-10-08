#include "n3d/game.hpp"
#include "../src/game/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void screen_art(n3d::Assets& assets, std::size_t directory_index) {
    auto& art = assets.uif.images[directory_index - 3];
    art.directory_index = directory_index;
    art.image.width = 320;
    art.image.height = 200;
    art.image.pixels.assign(n3d::IndexedFrame::kPixelCount, std::byte{9});
}

void font(n3d::Assets& assets) {
    assets.uif_bytes.assign(1, std::byte{0x80});
    for (auto& face : assets.uif.fonts)
        for (auto& glyph : face.glyphs) {
            glyph.rows = 1;
            glyph.width = 1;
            glyph.bitmap_size = 1;
        }
}

n3d::InputFrame key(std::uint8_t vk) {
    n3d::InputFrame input{};
    input.key = vk;
    return input;
}

void demo_abort_restores_config_and_root() {
    auto state = std::make_unique<n3d::GameState>();
    auto assets = std::make_unique<n3d::Assets>();
    font(*assets);
    screen_art(*assets, 6);
    n3d::DemoRecording recording{};
    recording.events.push_back({0x26, 2, 0, 1});
    state->demo.emplace(recording);
    state->screen = n3d::Screen::gameplay;
    state->level_loaded = true;
    state->player_life_state = 1;
    state->demo_saved_config.cheat_omniscient = 1;
    state->demo_saved_config.cheat_omnificent = 1;
    state->demo_saved_config.mouse_enabled = 1;
    state->config = state->demo_saved_config;
    state->config.cheat_omniscient = state->config.cheat_omnificent = 0;
    state->config.cheat_omnifarious = 1;
    state->config.mouse_enabled = 0;
    state->config.viewport_width = 296;
    state->config.output_width = 640;
    state->config.output_height = 400;
    n3d::abort_demo(*state, *assets);
    check(!state->demo && state->screen == n3d::Screen::root_menu &&
              !state->level_loaded && state->menu_row == 0 && state->current_tune == 12,
          "live demo abort restores no-game root and menu tune");
    check(state->config.cheat_omniscient == 1 && state->config.cheat_omnificent == 1 &&
              state->config.cheat_omnifarious == 0 && state->config.mouse_enabled == 1 &&
              state->config.viewport_width == 296 && state->config.output_width == 640 &&
              state->config.output_height == 400,
          "demo close restores saved config fields while retaining current dimensions");
}

void playback_uses_header_after_local_calibration() {
    auto state = std::make_unique<n3d::GameState>();
    n3d::DemoRecording recording{};
    recording.header = {10, 5, 20, 2};
    recording.events.push_back({0x26, 2, 0, 19}); // DEMO.1 +0x06: Up at tick 19.
    recording.events.push_back({0x25, 8, 0, 1157});
    state->demo.emplace(recording);
    state->screen = n3d::Screen::gameplay;
    state->level_loaded = state->composed_ready = true;
    state->player_life_state = 1;
    n3d::begin_calibration(*state);
    n3d::calibrate(*state, 100);
    check(state->clocks.rate == 10 && state->clocks.average_present_ms == 100 &&
              state->clocks.move_step == 10 && state->clocks.turn_step == 5 &&
              state->clocks.projectile_step == 20 && state->clocks.logical_tick == 0,
          "demo keeps local measured B rate but overwrites all three step words from header");
    state->clocks.logical_tick = 18;
    const auto early = state->demo->advance(state->clocks.logical_tick, false);
    state->clocks.logical_tick = 19;
    const auto first = state->demo->advance(state->clocks.logical_tick, false);
    check(!early.event_applied && first.event_applied && first.key == 0x26 && first.held == 2,
          "DEMO.1's first Up event is withheld until source logical tick 19");
    state->clocks.logical_tick = 1157;
    const auto last = state->demo->advance(state->clocks.logical_tick, false);
    const auto eof = state->demo->advance(state->clocks.logical_tick, false);
    check(last.event_applied && eof.event_applied && last.key == 0x25 &&
              eof.key == 0x25 && eof.held == 8,
          "last authored tick remains sticky at EOF for both key and held flag");
}

void e1l11_tune_reads_source_boundaries() {
    auto state = std::make_unique<n3d::GameState>();
    state->config.music_enabled = 0;
    state->episode = 1;
    state->level_index = 10;
    n3d::detail::restore_level_music(*state);
    check(state->current_tune == 13,
          "normal E1L11 reads the adjacent E2 row byte in the flat 30-byte table");
    n3d::DemoRecording recording{};
    state->demo.emplace(recording);
    n3d::detail::restore_level_music(*state);
    check(state->current_tune == 13 && state->demo_tune_counter == 1,
          "first demo load consumes cycle tune 13 even with music muted");
    n3d::detail::restore_level_music(*state);
    check(state->current_tune == 8 && state->demo_tune_counter == 2,
          "next demo death/restart consumes persistent cycle tune 8");
}

void demo_status_caption_is_reset_each_world_pass() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->episode = 2; // No E1 lightning RNG in this isolated world-A fixture.
    state->config.cheat_omnipotent = 1; // Floor-hazard service is inapplicable here.
    state->player.cell_x = state->player.cell_y = 10;
    state->hud_message = "stale demo caption";
    n3d::detail::service_world_a(*state, assets, 2);
    check(state->hud_message.empty(), "normal play clears the prior demo caption");
}

void developer_keys_follow_source_branches() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::gameplay;
    state->player_life_state = 1;
    state->player.cell_x = 10;
    state->player.cell_y = 10;
    state->player.health = 255;
    auto input = key(0x6b); // Left Shift plus numpad +, not the main keyboard +.
    input.held = 0x40;
    n3d::detail::handle_gameplay_input(*state, assets, input);
    check(state->player.health == 0, "developer plus wraps the byte rather than capping health");
    input.key = 0x6d;
    n3d::detail::handle_gameplay_input(*state, assets, input);
    check(state->player.health == 255, "developer minus wraps underflow without a lower clamp");
    n3d::detail::handle_gameplay_input(*state, assets, key(0x77)); // F8's source bare retf.
    check(state->screen == n3d::Screen::gameplay, "source F8 stub does not open an invented UI");
    n3d::detail::handle_gameplay_input(*state, assets, key(0x7b));
    check(state->screen == n3d::Screen::gameplay,
          "F12 silently returns when the forward MAP cell lacks a guard flag");

    constexpr std::size_t forward = 9u * 64u + 10u;
    state->level.cells[forward].object_id = 7;
    state->level.object_flags[7] = 8;
    state->level.object_count = state->level.guard_count = 1;
    auto& obj = state->level.objects[0];
    obj.image_id = 7;
    obj.class_id = 8;
    obj.cell_x = 10;
    obj.cell_y = 9;
    auto& guard = state->level.guards[0];
    guard.object_slot = 0;
    guard.hit_points = 99;
    guard.state = 7;
    guard.next_state = 2;
    n3d::detail::handle_gameplay_input(*state, assets, key(0x7b));
    check(state->screen == n3d::Screen::modal_message && state->modal_reason == 2 &&
              state->message.find("class 8, strength 99, strategy 0\nstate 7, nextstate 2") == 0,
          "F12 reads and displays the forward guard's original eight-field diagnostic");
}

void reason2_dismissal_applies_held_turn_and_fire_edge_exactly_once() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::modal_message;
    state->modal_reason = 2;
    state->modal_return = n3d::Screen::gameplay;
    state->message = "Sound on";
    state->player_life_state = 1;
    state->player.heading = 0;
    state->player.room_id = 0; // wake_room_guards fixture assumption: room zero never broadcasts.
    state->player.weapon = 2; // Subtype 2 hitscan: firing_mode 1, pool ammo[0], threshold 3.
    state->player.firing_mode = 1;
    state->player.ammo[0] = 4;
    state->fire_delay = 3; // Exactly kFireThreshold[2]: eligible to fire on a genuine edge.
    state->previous_fire = 0;
    state->clocks.turn_step = 5;
    n3d::InputFrame input{};
    input.key = 0x0d;
    input.held = 0x08 | 0x80; // Held turn-left plus held fire across the dismiss tick.
    n3d::step(*state, assets, input, 500);
    check(state->screen == n3d::Screen::gameplay && state->modal_reason == 0 &&
              state->message.empty(),
          "reason-2 Enter dismissal returns to gameplay and clears the modal");
    check(state->player.heading == 355,
          "held turn is applied by exactly one continuation pass, not zero or twice");
    check(state->player.ammo[0] == 3 && state->player.muzzle_flash == 1 &&
              state->previous_fire == 1 && state->fire_delay == 0,
          "the single continuation pass fires the held hitscan exactly once");
    // Second dismissal tick with fire still held and the delay eligible again:
    // previous_fire remains 1, so the continuation must not synthesize a new edge.
    state->screen = n3d::Screen::modal_message;
    state->modal_reason = 2;
    state->modal_return = n3d::Screen::gameplay;
    state->message = "Music on";
    state->fire_delay = 3;
    n3d::InputFrame refire{};
    refire.key = 0x0d;
    refire.held = 0x80; // Fire still held; turn released on this dismissal tick.
    n3d::step(*state, assets, refire, 501);
    check(state->screen == n3d::Screen::gameplay && state->player.heading == 355 &&
              state->player.ammo[0] == 3 && state->fire_delay == 3 &&
              state->previous_fire == 1,
          "a still-held fire across the next dismissal spends no duplicate shot");
}

void reason2_dismissal_via_escape_does_not_reopen_menu() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::modal_message;
    state->modal_reason = 2;
    state->modal_return = n3d::Screen::gameplay;
    state->message = "Sound off";
    state->player_life_state = 1;
    state->menu_row = 3;
    n3d::InputFrame input{};
    input.key = 0x1b; // Esc dismisses the modal; it must not also leak into gameplay as menu-open.
    n3d::step(*state, assets, input, 600);
    check(state->screen == n3d::Screen::gameplay && state->modal_reason == 0,
          "the dismissing Esc is consumed by the modal and never reaches the gameplay menu check");
}

void queued_modal_message_stays_modal_without_running_held_movement() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::modal_message;
    state->modal_reason = 2;
    state->modal_return = n3d::Screen::gameplay;
    state->message = "Same";
    state->queued_messages[0] = "Same"; // Identical text avoids a real-asset recomposition.
    state->queued_message_count = 1;
    state->player_life_state = 1;
    state->player.heading = 10;
    state->clocks.turn_step = 5;
    n3d::InputFrame input{};
    input.key = 0x0d;
    input.held = 0x08;
    n3d::step(*state, assets, input, 700);
    check(state->screen == n3d::Screen::modal_message && state->queued_message_count == 0 &&
              state->message == "Same",
          "dismissing with a queued message advances the queue instead of returning to gameplay");
    check(state->player.heading == 10,
          "a queued transition never runs the dismissed tick's held turn");
}

void modal_return_to_non_gameplay_skips_held_movement() {
    auto state = std::make_unique<n3d::GameState>();
    const n3d::Assets assets{};
    state->screen = n3d::Screen::modal_message;
    state->modal_reason = 2;
    state->modal_return = n3d::Screen::root_menu; // Atypical return; exercises the generic guard.
    state->message = "Diagnostic";
    state->player_life_state = 1;
    state->player.heading = 20;
    state->clocks.turn_step = 7;
    state->previous_fire = 0;
    n3d::InputFrame input{};
    input.key = 0x0d;
    input.held = 0x08 | 0x80;
    n3d::step(*state, assets, input, 800);
    check(state->screen == n3d::Screen::root_menu && state->modal_reason == 0 &&
              state->message.empty(),
          "dismissal to a non-gameplay return screen still clears the modal");
    check(state->player.heading == 20 && state->previous_fire == 0,
          "a return to a non-gameplay screen never runs the dismissed tick's held gameplay input");
}


void authored_story_and_episode_endings() {
    auto state = std::make_unique<n3d::GameState>();
    auto assets = std::make_unique<n3d::Assets>();
    font(*assets);
    screen_art(*assets, 6);
    screen_art(*assets, 14);
    screen_art(*assets, 16);
    constexpr std::string_view authored = "Is this the end of your quest?\r\nYour fate awaits you.\r\n";
    for (char ch : authored) assets->bsf.blocks[2].push_back(std::byte{static_cast<unsigned char>(ch)});
    state->ega_map[8] = 42;
    state->ega_map[14] = 77;
    state->screen = n3d::Screen::gameplay;
    state->level_loaded = state->composed_ready = true;
    state->config.music_enabled = 0;
    state->episode = 1;
    state->level_index = 9;
    state->player_life_state = 3;
    state->hamerstein_challenge = 1;
    state->player.health = 0;
    n3d::step(*state, *assets, {}, 1000);
    n3d::IndexedFrame frame{};
    n3d::render(*state, *assets, frame);
    check(state->screen == n3d::Screen::quest_story && frame.pixels[7 * 320 + 12] == 42,
          "E1L10 scripted death renders supplied BSF end text over UIF16");
    n3d::step(*state, *assets, key(0x1b), 1001);
    check(state->screen == n3d::Screen::intermission && state->pending_level == 10 &&
              state->intermission_bonus == 10000 && state->player.score == 10000,
          "E1L10 story Escape opens the terminal source tally and applies empty-level bonuses");
    n3d::step(*state, *assets, key(0x0d), 1002);
    check(state->screen == n3d::Screen::root_menu && state->player_life_state == 3,
          "E1L10 terminal tally returns to the no-game root, not a fabricated next level");

    state->screen = n3d::Screen::modal_message;
    state->episode = 2;
    state->player_life_state = 3;
    state->plasma_destroyed = 1;
    state->message = "A";
    state->modal_reason = 4;
    state->modal_return = n3d::Screen::gameplay;
    state->queued_messages[0] = "B";
    state->queued_messages[1] = "C";
    state->queued_message_count = 2;
    n3d::detail::handle_gameplay_input(*state, *assets, key(0x20));
    check(state->message == "C", "message queue first dismiss shows the last queued story message");
    n3d::detail::handle_gameplay_input(*state, *assets, key(0x20));
    check(state->message == "B", "message queue second dismiss shows the earlier message");
    n3d::detail::handle_gameplay_input(*state, *assets, key(0x20));
    n3d::step(*state, *assets, {}, 1003);
    check(state->screen == n3d::Screen::intermission && state->pending_level == 10,
          "E2L10 plasma end reaches the source terminal tally after queued messages");
    n3d::step(*state, *assets, key(0x0d), 1004);
    check(state->screen == n3d::Screen::root_menu,
          "E2L10 tally never auto-starts the next episode");

    state->screen = n3d::Screen::gameplay;
    state->episode = 3;
    state->level_index = 9;
    state->player_life_state = 0;
    state->ending_flag = 1;
    n3d::step(*state, *assets, {}, 1005);
    check(state->screen == n3d::Screen::ending && state->current_tune == 1,
          "E3L10 guard victory enters FLI playback after requesting ending tune");
    auto alt_enter = key(0x0d);
    alt_enter.held = 0x100;
    n3d::step(*state, *assets, alt_enter, 1006);
    check(state->screen == n3d::Screen::ending,
          "Alt-held Enter does not close source FLI playback");
    for (const auto close_vk : {std::uint8_t{0x1b}, std::uint8_t{0x20},
                                std::uint8_t{0x0d}}) {
        state->screen = n3d::Screen::ending;
        n3d::step(*state, *assets, key(close_vk), 1006);
        check(state->screen == n3d::Screen::intermission && state->pending_level == 10,
              "Esc, Space and Enter each close FLI to the authored tally");
    }
    state->screen = n3d::Screen::ending;
    n3d::InputFrame finished{};
    finished.ending_finished = true; // Runner waited the 489th 135-ms interval.
    n3d::step(*state, *assets, finished, 1006);
    check(state->screen == n3d::Screen::intermission && state->pending_level == 10,
          "the FLI terminal interval opens the E3L10 tally");
    n3d::step(*state, *assets, key(0x0d), 1007);
    check(state->screen == n3d::Screen::root_menu,
          "E3L10 tally dismisses to root without an invented post-ending screen");
}

} // namespace

int run_demo_story_integration_tests() {
    demo_abort_restores_config_and_root();
    playback_uses_header_after_local_calibration();
    e1l11_tune_reads_source_boundaries();
    demo_status_caption_is_reset_each_world_pass();
    developer_keys_follow_source_branches();
    reason2_dismissal_applies_held_turn_and_fire_edge_exactly_once();
    reason2_dismissal_via_escape_does_not_reopen_menu();
    queued_modal_message_stays_modal_without_running_held_movement();
    modal_return_to_non_gameplay_skips_held_movement();
    authored_story_and_episode_endings();
    if (failures) std::cerr << failures << " demo/story integration checks failed\n";
    return failures;
}
