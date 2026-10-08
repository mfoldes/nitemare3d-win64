#pragma once

#include "n3d/assets.hpp"
#include "n3d/demo.hpp"
#include "n3d/config.hpp"
#include "n3d/frame.hpp"
#include "n3d/help.hpp"
#include "n3d/level.hpp"
#include "n3d/scene.hpp"
#include "n3d/settings_menu.hpp"
#include "n3d/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace n3d {

// This is an update input, not a host keyboard state. The platform converts
// physical keys (including optional WASD/mouse) to the original action bits.
struct InputFrame final {
    std::uint8_t key = 0;             // One WM_KEYDOWN/ToAscii-compatible action, or zero.
    std::uint16_t held = 0;           // Original DS:3756, including Alt/Space in its high byte.
    std::int16_t mouse_forward = 0;  // Enabled-device movement in calibrated game units.
    std::int16_t mouse_turn = 0;
    std::int16_t joystick_forward = 0;
    std::int16_t joystick_turn = 0;
    std::int16_t joystick_strafe = 0; // Button 2 + X, distinct from turning.
    std::int16_t cursor_x = 0;       // Unclamped logical client coordinate.
    std::int16_t cursor_y = 0;
    bool cursor_moved = false;
    bool cursor_valid = false;       // An actual host pointer position was observed.
    bool mouse_left_held = false;    // Source 9392 derives viewport eligibility in the core.
    bool mouse_relative = false;     // Explicit --modern-controls raw-input mode.
    bool mouse_left_inside = false;  // Host hit-test, retained for relative mode.
    bool mouse_right_held = false;
    bool menu_left_released = false;
    bool menu_right_pressed = false;
    bool joystick_fire = false;
    bool joystick_use = false;
    bool ending_finished = false;    // Host decoded all FLI frames and waited one last interval.
};
enum class Screen : std::uint8_t {
    title,
    settings,
    root_menu,
    episode_menu,
    difficulty_menu,
    save_menu,
    load_menu,
    gameplay,
    warp_popup,
    floor_popup,
    control_popup,
    combination_prompt,
    modal_message,
    intermission,
    quest_story,
    ending,
    help,
};


// Named canonical fields, not a packed copy of the 94-byte original user.sav
// block. An explicit native save field order is not frozen yet.
struct PlayerState final {
    std::int16_t world_x = 0;
    std::int16_t world_y = 0;
    std::uint8_t cell_x = 0;
    std::uint8_t cell_y = 0;
    std::uint16_t heading = 0;
    std::uint8_t octant = 0;
    std::uint8_t nearest_sector = 0;
    std::uint16_t octant_mask = 0;
    std::int16_t sine = 0;
    std::int16_t cosine = 1024;
    std::uint16_t major_axis = 0;
    std::int16_t bresenham_error = 0;
    std::int16_t bresenham_minor = 0;
    std::int16_t bresenham_diagonal = 0;
    std::int16_t damage_feedback = -1;
    std::uint16_t difficulty = 1;
    std::uint32_t score = 0;
    std::uint8_t health = 100;
    std::uint8_t class35_count = 0; // Original 4c1e; authored name not established.
    std::uint8_t resource_4c21 = 0;
    std::uint8_t resource_4c22 = 0;
    std::array<std::uint8_t, 3> ammo{}; // Legacy 4c1f, 4c20, 4c44 order.
    std::uint8_t weapon = 0xff;
    std::uint8_t queued_weapon = 0xff;
    std::uint16_t firing_mode = 0;
    std::uint8_t weapon_owned = 0;
    std::uint8_t class37_mask = 0;
    std::uint8_t keys = 0;
    std::uint8_t cards = 0;
    std::uint8_t pentagrams = 0;
    std::uint8_t movement_locked = 0; // Original 4be8, set by the E1L10 floor trigger.
    std::uint8_t jammed = 0;
    std::uint8_t enemy_sensor = 0;
    std::uint8_t map_sensor = 0;
    std::uint8_t enemy_meter = 0;
    std::uint8_t map_meter = 0;
    std::uint8_t room_id = 0xff;
    std::uint8_t weapon_animation_state = 0;
    std::uint8_t weapon_animation_frame = 0;
    std::uint8_t muzzle_flash = 0;
};

struct ClockState final {
    std::uint32_t previous_a_quotient = 0;
    std::uint32_t previous_b_quotient = 0;
    std::uint32_t a_count = 0;
    std::uint32_t b_count = 0;
    std::uint32_t a_deadline = 0;
    std::uint32_t b_deadline = 0;
    std::uint32_t logical_tick = 0;
    std::uint32_t last_milliseconds = 0;
    std::uint16_t rate = 0;
    std::uint16_t move_step = 0;
    std::uint16_t turn_step = 0;
    std::uint16_t projectile_step = 0;
    std::uint32_t average_present_ms = 0; // DS:53f2, measured before 40ms rate clamp.
    bool calibration_started = false;
    bool calibrated = false;
};

struct DoorState final {
    std::uint16_t cell_index = 0;
    std::uint16_t vector_index = 0;
    std::uint16_t partner_vector_index = 0;
    std::uint8_t state = 1; // 0 open, 1 closed, 2 opening, 3 closing, 4 corpse-held open.
    std::int16_t countdown = 0;
    bool sound = false;
};

struct PanelState final {
    std::uint16_t cell_index = 0;
    std::uint8_t state = 1; // Unused panel counts toward the level-complete tally.
    std::array<std::uint16_t, 4> face_vectors{
        static_cast<std::uint16_t>(kMaxVectors), static_cast<std::uint16_t>(kMaxVectors),
        static_cast<std::uint16_t>(kMaxVectors), static_cast<std::uint16_t>(kMaxVectors)};
};

struct ProjectileState final {
    std::uint8_t state = 0; // 0 free, 1 travelling, 2 explosion.
    std::uint8_t image_id = 0;
    std::int16_t world_x = 0;
    std::int16_t world_y = 0;
    std::int8_t sign_x = 0;
    std::int8_t sign_y = 0;
    std::uint16_t major_axis = 0;
    std::int16_t error = 0;
    std::int16_t minor_increment = 0;
    std::int16_t diagonal_increment = 0;
    std::uint8_t frame = 0;
    std::uint32_t frame_deadline = 0;
    std::uint8_t lift = 5; // Original rec+0x1a (9aac/9d30); v1 omitted it, v2 serializes it.
};

struct PushwallState final {
    std::uint16_t object_slot = 0;
    std::int8_t step_x = 0;
    std::int8_t step_y = 0;
    std::uint8_t steps_remaining = 0;
};

enum class AudioCommandKind : std::uint8_t { effect, music };
struct AudioCommand final {
    AudioCommandKind kind = AudioCommandKind::effect;
    std::uint8_t id = 0;
    std::uint8_t mode = 0;
    std::uint8_t priority = 0;
};

struct AudioCommands final {
    std::array<AudioCommand, 256> entries{};
    std::size_t count = 0;

    void push(AudioCommand command) {
        if (count == entries.size()) throw std::overflow_error("audio command capacity exhausted");
        entries[count++] = command;
    }
    [[nodiscard]] std::span<const AudioCommand> pending() const noexcept {
        return {entries.data(), count};
    }
    void clear() noexcept { count = 0; }
};

struct AutomapSpeckle final {
    std::uint8_t x = 0; // Local panel coordinate, 0..61.
    std::uint8_t y = 0; // Local panel coordinate, 0..35.
};

struct GameState final {
    GameState() = default;
    GameState(const GameState&) = delete;
    GameState& operator=(const GameState&) = delete;
    GameState(GameState&&) = delete;
    GameState& operator=(GameState&&) = delete;

    Screen screen = Screen::title;
    std::uint8_t episode = 1;
    std::uint16_t level_index = 0;
    std::uint8_t pending_episode = 1;
    std::uint16_t pending_level = 0;
    std::uint8_t menu_row = 0;
    std::array<SaveMenuSlot, 10> save_slots{};
    bool save_menu_pending = false; // First Enter marks a selection; second Enter sends the request.
    bool save_menu_quick_confirm = false; // F5 on occupied quick slot uses gameplay overlay.
    SaveMenuOutcome save_menu_outcome{};
    Config config{}; // The sole mutable source for original config.sav fields.
    SettingsMenu settings{};
    SettingsOutcome settings_outcome{}; // Effects of the most recent input step, for the host.
    std::uint8_t warp_row = 0; // 1030:155c retains this cursor across popups.
    std::uint8_t popup_row = 0;
    std::uint8_t popup_first = 0;
    std::uint8_t popup_count = 0;
    std::uint8_t popup_wall_id = 0;
    std::uint8_t popup_class = 0;
    std::uint8_t available_cards = 0; // Original 8399, including cards hidden in safes.
    std::uint16_t combination_cell = 0;
    std::array<char, 7> combination_digits{};
    std::uint8_t combination_length = 0;
    std::uint8_t difficulty_row = 1;
    std::uint8_t previous_use = 0;
    std::uint8_t previous_fire = 0;
    std::uint8_t fire_delay = 0; // DS:01fa, also the unused episode-zero tune-table byte.
    std::uint8_t intermission_hostiles = 0;
    std::uint8_t intermission_panels = 0;
    std::uint16_t intermission_bonus = 0;
    std::array<char, 160> guard_debug_text{}; // F12 modal uses a stable, allocation-free buffer.
    bool quit_requested = false;
    bool level_loaded = false;
    std::uint8_t player_life_state = 0; // Original 46b4: 1 live, 2 lethal hit, 3 death view.
    std::uint16_t killer_object_slot = 0; // Original 4c1a, set on lethal guard attack.
    bool death_turn_pending = false; // Original 46ac while the death camera turns.
    Screen modal_return = Screen::gameplay;
    std::optional<HelpPager> help_pager{}; // Constructed once from owned Windows BSF/UIF data.
    bool help_order_requested = false; // F10 / translated 'y' reached missing order.txt.
    std::uint8_t modal_reason = 0;
    bool menu_cursor_moved = false;
    bool panel_nearby = false; // Original 46ad, drawn as a 2x2 status indicator.
    std::string_view hud_message{}; // Original 46ba:46bc; distinct from modal text.
    std::string_view message{};
    std::string_view warning{};
    std::array<std::string_view, 8> queued_messages{}; // 1010:83e; last queued is first dismissed.
    std::uint8_t queued_message_count = 0;
    std::array<char, 256> portal_text{};
    std::uint8_t remote_open_mask = 0; // Original 51a4, bit per control-panel subtype.
    std::uint8_t cannons_enabled = 1; // Original 51a5, starts enabled each level.
    std::uint8_t level_latch = 0; // Original 51a6; fuse completion changes 1 to 2.
    std::uint8_t penelope_latch = 0; // Original 51a7.
    std::uint8_t hamerstein_challenge = 0; // Original 51a8.
    std::uint8_t plasma_destroyed = 0; // Original 51a9.
    std::uint8_t ending_flag = 0; // Original 51aa.
    std::uint8_t lights_fused = 0; // Original 51ab.
    std::uint16_t plasma_hits = 0; // Original DS:0202; not reset with the level.
    std::optional<DemoPlayback> demo{}; // Borrowed immutable Assets::demos controller.
    Config demo_saved_config{};
    std::uint8_t demo_tune_counter = 0; // 1048:0226, process-lifetime rotating demo tune.
    std::uint32_t core_wait_started_tick = 0;
    std::uint16_t core_wait_ticks = 0; // 1010:c2b8 pumps B passes after hit 20.
    bool has_destructible_walls = false;
    std::uint8_t chalkboard_image = 0; // Original 51ae cache entry for wall IMG ID.
    bool chalkboard_active = false;
    std::array<std::uint8_t, kMaxGuards> radio_original_images{}; // 1010:ae56 restores image after dance.
    std::uint32_t rng_seed = 1;
    std::int16_t lightning_countdown = 0x50; // Original 38ee, clock-A passes.
    bool lightning_on = false; // Original 38f0; palette entry 0xda flashes.
    std::array<std::array<std::uint8_t, 3>, 256> palette{};
    std::uint32_t shade_epoch = 1; // c5e2 rebuild generation; lightning alone does not change it.
    std::array<std::uint8_t, 16> ega_map{};
    LevelState level{};
    SceneSnapshot scene{};
    IndexedFrame composed_frame{}; // Last original-order DIB; render only copies it.
    IndexedFrame save_menu_restore_frame{}; // exact pre-menu framebuffer before a transient save/load menu opens
    bool save_menu_restore_frame_valid = false;
    std::uint8_t save_menu_restore_row = 0; // caller menu row restored when the host dismisses or confirms
    // Menu-only pixels never belong to a save-game snapshot.
    bool composed_ready = false;
    PlayerState player{};
    std::uint8_t draw_weapon = 0xff; // Hand sprite selected before a2ce changes weapon ID.
    std::uint8_t draw_weapon_frame = 0;
    std::uint8_t draw_weapon_recoil = 0; // Signed Y add is 3 or 6 on this B pass only.
    ClockState clocks{};
    std::array<DoorState, 64> doors{};
    std::size_t door_count = 0;
    std::array<PanelState, 32> panels{};
    std::size_t panel_count = 0;
    std::array<PushwallState, 12> pushwalls{};
    std::size_t pushwall_count = 0;
    std::array<ProjectileState, 8> projectiles{};
    std::array<std::uint8_t, kMapCellCount> automap{};
    std::int16_t automap_origin_x = 0; // 38ec; b1a4(7) can overwrite it with random x.
    std::int16_t automap_origin_y = 0; // 38ea; b1a4(7) can overwrite it with random y.
    std::int16_t automap_heading_x = 0; // ee26 output relative to the 62x36 panel.
    std::int16_t automap_heading_y = 0;
    bool automap_heading_valid = false;
    bool automap_blink = true; // Original 01fb initial phase.
    bool sensor_panel_cleared = false;
    std::array<AutomapSpeckle, 500> automap_speckles{};
    std::size_t automap_speckle_count = 0;
    std::array<std::uint8_t, 64> room_wake{};
    std::uint8_t current_tune = 0; // Last original tune selection; F2 can restart it.
    AudioCommands audio{};

};

void initialize_game(GameState& state, const Assets& assets,
                     Config config = Config{});
void begin_calibration(GameState& state); // After first level frame is presented.
void advance_calibration_frame(GameState& state, const Assets& assets,
                               std::uint32_t present_time_ms); // One 1010:d7c0 before each measured present.
void calibrate(GameState& state, std::uint32_t average_present_ms);
void abort_demo(GameState& state, const Assets& assets); // A live WM_KEYDOWN, not replay input.
void step(GameState& state, const Assets& assets, const InputFrame& input,
          std::uint64_t logical_time);
void render(const GameState& state, const Assets& assets, IndexedFrame& frame);
void refresh_save_menu(GameState& state, const Assets& assets);

} // namespace n3d
