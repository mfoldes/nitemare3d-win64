#include "internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>


namespace n3d {
namespace {

constexpr std::array<std::string_view, 6> kRootLabels{
    "New game", "Configure game...", "Load game...", "Instructions", "Demo", "Quit"};
constexpr std::array<std::string_view, 7> kActiveRootLabels{
    "New game", "Configure game...", "Load game...", "Save game...",
    "Instructions", "Return to game", "Quit"};
constexpr std::array<std::string_view, 3> kEpisodeLabels{
    "Episode 1", "Episode 2", "Episode 3"};
constexpr std::array<std::string_view, 3> kDifficultyLabels{
    "Be gentle!", "I'm tough!", "Let's party!"};
constexpr std::array<int, 91> kTangent{
    0,17,35,53,71,89,107,125,143,162,180,199,217,236,255,274,293,313,332,352,
    372,393,413,434,455,477,499,521,544,567,591,615,639,664,690,717,743,771,
    800,829,859,890,922,954,988,1024,1060,1098,1137,1177,1220,1264,1310,1358,
    1409,1462,1518,1576,1638,1704,1773,1847,1925,2009,2099,2195,2299,2412,
    2534,2667,2813,2973,3151,3349,3571,3821,4107,4435,4817,5268,5807,6465,
    7286,8339,9742,11704,14643,19539,29323,32767,32767};

[[noreturn]] void scene_failure(RenderError error) {
    switch (error) {
    case RenderError::unprepared_scene: throw std::runtime_error("unprepared gameplay scene");
    case RenderError::invalid_viewpoint: throw std::runtime_error("invalid gameplay viewpoint");
    case RenderError::invalid_level: throw std::runtime_error("invalid gameplay level");
    case RenderError::missing_palette: throw std::runtime_error("missing gameplay palette");
    case RenderError::missing_background: throw std::runtime_error("missing gameplay background");
    case RenderError::missing_image: throw std::runtime_error("missing gameplay IMG record");
    case RenderError::invalid_frame: throw std::runtime_error("invalid gameplay IMG frame");
    case RenderError::division_overflow: throw std::runtime_error("gameplay projection division overflow");
    case RenderError::uncovered_viewport: throw std::runtime_error("gameplay viewport is uncovered");
    case RenderError::span_capacity: throw std::runtime_error("gameplay span capacity exceeded");
    case RenderError::sprite_capacity: throw std::runtime_error("gameplay sprite capacity exceeded");
    case RenderError::undefined_wall_table: throw std::runtime_error("gameplay wall table is undefined");
    case RenderError::undefined_sprite_memory: throw std::runtime_error("gameplay sprite read is undefined");
    case RenderError::undefined_frame_read: throw std::runtime_error("gameplay framebuffer read is undefined");
    case RenderError::undefined_frame_write: throw std::runtime_error("gameplay framebuffer write is undefined");
    case RenderError::none: break;
    }
    throw std::runtime_error("unrecognized gameplay scene error");
}

bool episode_enabled(const Assets& assets, std::uint8_t row) noexcept {
    return row < assets.episodes.size() && !assets.episodes[row].map_bytes.empty() &&
           (row == 0 || assets.bsf.header.flag_byte != 0);
}
std::uint8_t menu_row(const GameState& state) noexcept {
    return state.screen == Screen::difficulty_menu ? state.difficulty_row : state.menu_row;
}


void restore_pre_menu_state(GameState& state) {
    state.menu_row = state.save_menu_restore_row;
    state.save_menu_quick_confirm = false;
    state.save_menu_restore_row = 0;
    if (state.save_menu_restore_frame_valid) {
        state.composed_frame = state.save_menu_restore_frame;
        state.composed_ready = true;
        state.save_menu_restore_frame_valid = false;
    }
}

void open_save_menu(GameState& state, Screen caller, Screen menu_kind) {
    state.modal_return = caller;
    state.save_menu_pending = false;
    state.save_menu_quick_confirm = false;
    state.save_menu_outcome = {};
    state.save_menu_restore_row = state.menu_row;
    state.save_menu_restore_frame = state.composed_frame;
    state.save_menu_restore_frame_valid = true;
    state.menu_row = 0;
    state.screen = menu_kind;
}

void compose_quick_load_confirmation(GameState& state, const Assets& assets) {
    // 1018:230a -> 0818 -> 054c/032a/026c/0132. The saved gameplay DIB
    // stays frozen beneath this opaque shadow/main box; no world step occurs.
    // Native saves have no authored slot name, so the source-style name is empty.
    constexpr std::array<std::string_view, 3> lines{
        "About to LOAD game:", "\"\"", "Press ESC to abort..."};
    int width = 0;
    for (const auto line : lines)
        width = std::max(width, static_cast<int>(indexed_text_width(assets, 1, line)));
    constexpr int line_height = 11;
    const int height = line_height * static_cast<int>(lines.size());
    const int text_x = 160 - (width >> 1);
    const int text_y = 80 - (height >> 1);
    const int box_x = text_x - 8;
    const int box_y = text_y - 8;
    const int box_width = width + 16;
    const int box_height = height + 16;
    if (box_x < 0 || box_y < 0 || box_x + box_width + 4 > 320 ||
        box_y + box_height + 4 > 200)
        throw std::out_of_range("quick-load dialog exceeds the 320x200 indexed DIB");
    const auto fill = [&](int x, int y, std::uint8_t colour) {
        for (int row = y; row < y + box_height; ++row)
            std::fill_n(state.composed_frame.pixels.begin() +
                            static_cast<std::size_t>(row) * IndexedFrame::kWidth + x,
                        box_width, colour);
    };
    fill(box_x + 4, box_y + 4, state.ega_map[0]); // Offset shadow first.
    fill(box_x, box_y, state.ega_map[8]);         // Main box covers the overlap.
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int y = text_y + line_height * static_cast<int>(index);
        draw_indexed_text(assets, state.composed_frame, 1, lines[index],
                          text_x + 1, y + 1, state.ega_map[0]);
        draw_indexed_text(assets, state.composed_frame, 1, lines[index],
                          text_x, y, state.ega_map[14]);
    }
    state.composed_ready = true;
}

void change_menu_row(GameState& state, std::uint8_t row) noexcept {
    if (state.screen == Screen::difficulty_menu) state.difficulty_row = row;
    else {
        if (state.menu_row != row) state.save_menu_pending = false;
        state.menu_row = row;
    }
}

std::uint8_t row_count(const GameState& state) {
    switch (state.screen) {
    case Screen::root_menu:
        return state.level_loaded && state.player_life_state == 1 ? 7 : 6;
    case Screen::episode_menu: case Screen::difficulty_menu: return 3;
    case Screen::save_menu: case Screen::load_menu: return 10;
    default: throw std::logic_error("menu row request outside a menu screen");
    }
}

bool row_enabled(const GameState& state, const Assets& assets, std::uint8_t row) noexcept {
    if (state.screen == Screen::save_menu) return row < state.save_slots.size();
    if (state.screen == Screen::load_menu)
        return row < state.save_slots.size() && state.save_slots[row].present;
    return state.screen != Screen::episode_menu || episode_enabled(assets, row);
}

std::uint8_t hit_menu(const GameState& state, const Assets& assets,
                      const InputFrame& input) {
    const auto count = row_count(state);
    const bool save_load = state.screen == Screen::save_menu || state.screen == Screen::load_menu;
    const std::uint8_t font_index = save_load ? kSaveLoadSlotFont : std::uint8_t{2};
    const int x = save_load ? kSaveLoadSlotX : 110;
    const int height = save_load ? kSaveLoadHitHeight : 8;
    for (std::uint8_t row = 0; row < count; ++row) {
        if (!row_enabled(state, assets, row)) continue;
        std::string_view label{};
        if (save_load) {
            label = save_menu_slot_label_view(&state.save_slots[row]);
        } else {
            label = state.screen == Screen::root_menu ?
                (state.level_loaded && state.player_life_state == 1 ? kActiveRootLabels[row] :
                                                                     kRootLabels[row]) :
                state.screen == Screen::episode_menu ? kEpisodeLabels[row] : kDifficultyLabels[row];
        }
        const int y = save_load ? kSaveLoadSlotY + static_cast<int>(row) * kSaveLoadSlotRowPitch :
                      state.screen == Screen::root_menu ? 60 + 15 * row : 80 + 20 * row;
        if (input.cursor_x >= x &&
            input.cursor_x <= x + static_cast<int>(indexed_text_width(assets, font_index, label)) &&
            input.cursor_y >= y && input.cursor_y <= y + height) return row;
    }
    return count;
}

void move_menu(GameState& state, const Assets& assets, int direction) {
    const int original = menu_row(state);
    const int count = row_count(state);
    for (int row = original + direction; row >= 0 && row < count; row += direction) {
        if (row_enabled(state, assets, static_cast<std::uint8_t>(row))) {
            change_menu_row(state, static_cast<std::uint8_t>(row));
            return;
        }
    }
}

void select_tune(GameState& state, std::uint8_t tune) {
    state.current_tune = tune;
    if (state.config.music_enabled)
        state.audio.push({AudioCommandKind::music, tune, 0, 0});
}

void enter_help(GameState& state, const Assets& assets, Screen origin) {
    if (origin == Screen::root_menu) select_tune(state, 1); // 1018:2980.
    if (!state.help_pager) state.help_pager.emplace(assets);
    else state.help_pager->reset();
    state.modal_return = origin;
    state.screen = Screen::help;
}
void compose_new_level(GameState& state, const Assets& assets);
void compose_menu_screen(GameState& state, const Assets& assets);

void start_demo(GameState& state, const Assets& assets) {
    // 1018:24bc always picks MAP.1 when the intact trilogy is available.
    // The legacy fallback for inaccessible earlier MAP files is not a valid
    // identity of our required all-episode Assets set; never expose .2/.3.
    if (assets.episodes[0].map_bytes.empty() || assets.demos[0].events.empty())
        throw std::runtime_error("Demo requires intact MAP.1 and DEMO.1");
    state.demo_saved_config = state.config; // daa0 snapshots the whole 20-byte block.
    state.config.mouse_enabled = state.config.joystick_enabled = 0;
    state.config.cheat_omniscient = state.config.cheat_omnipotent = 0;
    state.config.cheat_omnificent = 0;
    state.config.cheat_omnifarious = 1;
    state.demo.emplace(assets.demos[0]); // Mode 3 before 09f2/c63e selects demo tune.
    state.player = {};
    detail::apply_cheats(state); // 1010:ba16 -> b128.
    detail::start_level(state, assets, 1, 10); // E1L11 is zero-based index 10.
    state.rng_seed = 1; // 1010:daa0 seeds *after* the level and its tune load.
    state.clocks.logical_tick = 0;
    state.clocks.calibrated = false;
    state.clocks.calibration_started = false;
    compose_new_level(state, assets);
}

void restore_demo_config(GameState& state) {
    if (!state.demo) throw std::logic_error("closing an inactive demo");
    state.demo.reset(); // Mode 5 closes; preserve the current three dimensions.
    const auto viewport = state.config.viewport_width;
    const auto output_width = state.config.output_width;
    const auto output_height = state.config.output_height;
    state.config = state.demo_saved_config;
    state.config.viewport_width = viewport;
    state.config.output_width = output_width;
    state.config.output_height = output_height;
}

void close_demo(GameState& state, const Assets& assets) {
    restore_demo_config(state);
    state.level_loaded = false;
    state.player_life_state = 0;
    state.screen = Screen::root_menu;
    state.menu_row = 0;
    state.message = {};
    state.queued_message_count = 0;
    compose_menu_screen(state, assets);
    select_tune(state, 12); // 1018:2f6c, no demo-cycle increment.
}
void restart_demo(GameState& state, const Assets& assets) {
    restore_demo_config(state); // 1010:db38 closes and restores before mode 3 init.
    start_demo(state, assets); // The next 09f2 consumes the next cycle-tune byte.
}



void select_menu(GameState& state, const Assets& assets, std::uint32_t now) {
    const auto row = menu_row(state);
    if (row >= row_count(state))
        throw std::logic_error("selected menu row is outside its source table");
    if (!row_enabled(state, assets, row)) return; // Disabled Load rows ignore Enter.
    switch (state.screen) {
    case Screen::root_menu:
        switch (row) {
        case 0: // 1018:27de action 2 -> 1010:ba16 and episode table 1030:1148.
            state.player = {};
            detail::apply_cheats(state); // 1010:ba16 -> b128, after clearing player fields.
            state.pending_episode = 1;
            state.pending_level = 0;
            state.menu_row = 0;
            state.screen = Screen::episode_menu;
            return;
        case 1: // 1018:27de action 3, Configure UIF9.
            state.settings.open();
            state.screen = Screen::settings;
            return;
        case 2:
            open_save_menu(state, Screen::root_menu, Screen::load_menu);
            return;
        case 3:
            if (state.level_loaded && state.player_life_state == 1) {
                open_save_menu(state, Screen::root_menu, Screen::save_menu);
                return;
            }
            enter_help(state, assets, Screen::root_menu);
            return;
        case 4:
            if (state.level_loaded && state.player_life_state == 1) {
                enter_help(state, assets, Screen::root_menu);
                return;
            }
            start_demo(state, assets); // 1018:299a -> 1010:daa0, never an attract timer.
            return;
        case 5:
            if (state.level_loaded && state.player_life_state == 1) {
                detail::restore_level_music(state); // 1018:29b6, before resume 1ed0.
                state.screen = Screen::gameplay;
                state.player_life_state = 1;
                state.clocks.last_milliseconds = now;
                state.clocks.calibrated = false;
                state.clocks.calibration_started = false;
            } else state.quit_requested = true;
            return;
        case 6: state.quit_requested = true; return;
        }
        break;
    case Screen::episode_menu:
        state.pending_episode = static_cast<std::uint8_t>(row + 1);
        state.difficulty_row = 1; // Source 1030:1190 marks its middle row selected.
        state.screen = Screen::difficulty_menu;
        return;
    case Screen::difficulty_menu:
        state.player.difficulty = row;
        state.clocks.last_milliseconds = now;
        detail::start_level(state, assets, state.pending_episode, state.pending_level);
        state.clocks.calibrated = false;
        return;
    case Screen::save_menu:
    case Screen::load_menu:
    {
        if (!state.save_menu_pending) {
            state.save_menu_pending = true;
            return;
        }
        state.save_menu_pending = false;
        const auto request = SaveMenuRequest{
            state.screen == Screen::save_menu ? SaveMenuOperation::save : SaveMenuOperation::load,
            row};
        restore_pre_menu_state(state);
        state.save_menu_outcome = {false, request};
        state.screen = state.modal_return;
        if (state.screen == Screen::save_menu || state.screen == Screen::load_menu)
            state.screen = Screen::root_menu;
        return;
    }
    default: break;
    }
    throw std::logic_error("selected menu action has no source table");
}

void pop_menu(GameState& state) {
    switch (state.screen) {
    case Screen::root_menu: state.quit_requested = true; break;
    case Screen::episode_menu: state.menu_row = 0; state.screen = Screen::root_menu; break;
    case Screen::difficulty_menu: state.screen = Screen::episode_menu; break;
    case Screen::save_menu:
    case Screen::load_menu:
        restore_pre_menu_state(state);
        state.save_menu_pending = false;
        state.save_menu_outcome = {true, std::nullopt};
        state.screen = state.modal_return;
        if (state.screen == Screen::save_menu || state.screen == Screen::load_menu)
            state.screen = Screen::root_menu;
        break;
    default: throw std::logic_error("menu pop outside a menu screen");
    }
}

void step_menu(GameState& state, const Assets& assets, const InputFrame& input,
               std::uint32_t now) {
    // Win16 1018:2e12 handles a keydown; mouse releases/presses are distinct
    // menu events (1010:95ae). Alt modifies neither the menu nor the title.
    if (input.key && !(input.held & 0x100u)) {
        switch (input.key) {
        case 0x26: move_menu(state, assets, -1); return;
        case 0x28: move_menu(state, assets, +1); return;
        case 0x1b: pop_menu(state); return;
        case 0x0d: case 0x20: select_menu(state, assets, now); return;
        default: break;
        }
    }
    const auto hit = (input.cursor_moved || input.menu_left_released)
                         ? hit_menu(state, assets, input) : row_count(state);
    if (input.cursor_moved && hit < row_count(state)) change_menu_row(state, hit);
    if (input.menu_left_released) {
        if (hit == menu_row(state)) select_menu(state, assets, now);
    } else if (input.menu_right_pressed) pop_menu(state);
}

// 1010:d454: the class-0x2c/0x2d frame groups face the player. The
// differences are original signed 16-bit world-coordinate subtractions.
std::uint8_t object_direction(const PlayerState& player, const ObjectRecord& object,
                              bool eight_directions) noexcept {
    const int dx = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(player.world_x) - static_cast<std::uint16_t>(object.world_x)));
    const int dy = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(player.world_y) - static_cast<std::uint16_t>(object.world_y)));
    const int abs_x = dx < 0 ? -dx : dx;
    const int abs_y = dy < 0 ? -dy : dy;
    if (eight_directions) {
        if (2 * abs_y < abs_x) return dx > 0 ? 2 : 6;
        if (2 * abs_x < abs_y) return dy <= 0 ? 0 : 4;
        return dx > 0 ? (dy <= 0 ? 1 : 3) : (dy > 0 ? 5 : 7);
    }
    if (dx >= 0) return dy < 0 ? (abs_y > abs_x ? 0 : 1) : (abs_y <= abs_x ? 2 : 3);
    return dy >= 0 ? (abs_y > abs_x ? 4 : 5) : (abs_y > abs_x ? 7 : 6);
}

void advance_queued_object(ObjectRecord& object, const ImgSequenceMetadata& sequence,
                           const PlayerState& player, std::uint32_t now) {
    // cc7c:ce25-ce5e already captured the old image header into a scene slot.
    // Class 5's projectile sequence belongs to 9e20, not this renderer pass.
    if (object.class_id == 5) return;
    if (object.class_id == 0x2c || object.class_id == 0x2d) {
        // cb26 dispatches BEFORE the ordinary interval test. No shipped MAP
        // creates these classes, but an unsupported reachable state must not
        // silently take the plain-animation path.
        const bool eight_directions = object.class_id == 0x2d;
        const unsigned divisor = sequence.frame_count >> (eight_directions ? 3 : 2);
        if (!divisor) throw std::runtime_error("1010:cb26 directional object frame-group divisor is zero");
        const bool due = object.deadline_ms <= now;
        if (due) {
            object.frame_index = static_cast<std::uint8_t>(object.frame_index + 1u);
            object.deadline_ms = now + sequence.delay_ms;
        }
        const auto remainder = object.frame_index % divisor;
        const auto direction = object_direction(player, object, eight_directions);
        const unsigned group = (4u - direction) & 7u;
        object.frame_index = static_cast<std::uint8_t>(
            (eight_directions ? group : group >> 1u) * divisor + remainder);
        return;
    }
    if (!sequence.delay_ms) return; // Guards' extended descriptors have interval zero.
    if (sequence.extended_state_flag)
        throw std::runtime_error("1010:cbd4 timed extended object random-bank branch unresolved");
    if (object.deadline_ms > now) return; // Source compares 32-bit deadlines unsigned.
    object.frame_index = static_cast<std::uint8_t>(object.frame_index + 1u);
    if (object.frame_index >= sequence.frame_count) object.frame_index = 0;
    object.deadline_ms = now + sequence.delay_ms; // u32 wrap, no missed-frame catch-up.
}

void prepare(GameState& state, const Assets& assets) {
    if (!state.level_loaded || state.episode == 0 || state.episode > assets.episodes.size())
        throw std::logic_error("gameplay scene requires a loaded episode and level");
    const Viewpoint view{
        state.player.world_x, state.player.world_y, state.player.heading,
        state.config.viewport_width, state.lights_fused ? std::uint8_t{0} : std::uint8_t{0x11},
        state.lights_fused ? std::uint8_t{0} : std::uint8_t{0x0c},
        state.lights_fused ? std::uint8_t{6} : std::uint8_t{2},
        false, &state.palette, state.shade_epoch};
    const auto error = prepare_scene(state.scene, state.level,
                                     assets.episodes[state.episode - 1], view);
    if (error != RenderError::none) scene_failure(error);
    detail::reveal_visible_automap(state); // 1010:6266 records visible wall vectors before painting.
    // cc7c stamps only queued objects, then cbd4/cb26 updates their NEXT
    // phase. SceneSpriteSlot retains the prior frame for the current paint.
    const auto& episode = assets.episodes[state.episode - 1];
    for (std::size_t i = 0; i < state.level.object_count; ++i) {
        if (!state.scene.object_queued[i]) continue;
        auto& object = state.level.objects[i];
        object.last_base_y = state.scene.object_base_y[i];
        advance_queued_object(object, episode.images.sequences[256u + object.image_id],
                              state.player, state.clocks.last_milliseconds);
    }
    for (std::size_t i = 0; i < state.level.guard_count; ++i)
        if (state.scene.guard_seen[i]) state.level.guards[i].seen_tick = state.clocks.logical_tick;
}

void compose_gameplay(GameState& state, const Assets& assets) {
    if (!state.level_loaded || state.episode == 0 || state.episode > assets.episodes.size())
        throw std::logic_error("gameplay composition has no loaded level");
    if (const auto error = paint_scene(state.scene, state.level,
            assets.episodes[state.episode - 1], assets, state.composed_frame);
        error != RenderError::none) scene_failure(error);
    consume_scene_sprite_queue(state.scene); // 6914 consumes this raster before HUD/service.
    detail::draw_hud(state, assets, state.composed_frame);
    state.composed_ready = true;
}

void queue_serviced_projectiles(GameState& state, const Assets& assets,
                                const std::array<bool, 8>& enqueue) {
    if (state.episode == 0 || state.episode > assets.episodes.size())
        throw std::logic_error("projectile queue has no active IMG episode");
    const auto& episode = assets.episodes[state.episode - 1];
    for (std::size_t i = 0; i < enqueue.size(); ++i) {
        if (!enqueue[i]) continue;
        const auto& shot = state.projectiles[i];
        const auto flags = static_cast<std::uint8_t>(1u | (shot.state == 2 ? 0x10u : 0u));
        if (const auto error = queue_projectile_sprite(
                state.scene, state.level, episode, shot.world_x, shot.world_y,
                shot.image_id, shot.frame, flags, shot.lift);
            error != RenderError::none)
            scene_failure(error);
    }
}

void draw_unavailable_cheats(GameState& state, const Assets& assets) {
    // The original message text and gate are known; its Win16 modal raster is
    // not. Render a bounded native notice over the source UIF11 background.
    constexpr std::array<std::string_view, 3> lines{
        "Cheat modes are only available", "when you purchase the complete",
        "trilogy.  Please see 'Instructions'"};
    int width = 0;
    for (const auto line : lines)
        width = std::max(width, static_cast<int>(indexed_text_width(assets, 1, line)));
    const int left = (320 - width - 20) / 2;
    const int top = 77;
    if (left < 0) throw std::out_of_range("unavailable cheat message exceeds UIF frame");
    for (int y = top; y < top + 46; ++y)
        for (int x = left; x < left + width + 20; ++x)
            state.composed_frame.pixels[static_cast<std::size_t>(y) * 320 + x] =
                (x == left || x == left + width + 19 || y == top || y == top + 45)
                    ? state.ega_map[6] : state.ega_map[8];
    for (std::size_t i = 0; i < lines.size(); ++i)
        draw_indexed_text(assets, state.composed_frame, 1, lines[i], left + 10,
                          top + 7 + static_cast<int>(i) * 12, state.ega_map[14]);
}



void compose_menu_screen(GameState& state, const Assets& assets) {
    if (state.screen == Screen::settings) {
        state.settings.compose(assets, state.composed_frame, state.ega_map);
        if (state.settings.screen() == SettingsScreen::unavailable_cheats)
            draw_unavailable_cheats(state, assets);
        state.composed_ready = true;
        return;
    }
    MenuSelection selection{};
    const auto save_kind = state.screen == Screen::save_menu ? MenuKind::Save : MenuKind::Load;
    switch (state.screen) {
    case Screen::root_menu:
        selection = {MenuKind::Root, state.menu_row,
                     state.level_loaded && state.player_life_state == 1};
        break;
    case Screen::episode_menu:
        selection = {MenuKind::Episode, state.menu_row};
        break;
    case Screen::difficulty_menu:
        selection = {MenuKind::Difficulty, state.difficulty_row};
        break;
    case Screen::save_menu:
    case Screen::load_menu:
        selection = {save_kind, state.menu_row, false, state.save_menu_pending};
        break;
    default: throw std::logic_error("menu composition outside a menu screen");
    }
    compose_menu(assets, state.composed_frame, selection, state.ega_map, state.save_slots);
    state.composed_ready = true;
}

void compose_new_level(GameState& state, const Assets& assets) {
    ++state.clocks.logical_tick; // Provisional d7c0, before ba74(1) and d7d0.
    prepare(state, assets);
    detail::service_world_b(state, assets); // d7c0 includes ba54's first weapon frame.
    compose_gameplay(state, assets);
}
void compose_quest_story(GameState& state, const Assets& assets) {
    // 1018:19a6(16,5,7): the source UIF16 and decrypted BSF block 2, not
    // invented intermission art or a replacement narrative.
    const auto& art = assets.uif.images[16 - 3];
    if (art.directory_index != 16 || art.image.width != 320 || art.image.height != 200 ||
        art.image.pixels.size() != IndexedFrame::kPixelCount)
        throw DataError("uif.dat", art.offset, "end-of-quest UIF16 fills 320x200");
    const auto& text = assets.bsf.blocks[2];
    if (text.empty()) throw DataError("nite3d.bsf", assets.bsf.header.block_offsets[2],
                                     "end-of-quest text block is nonempty");
    auto& frame = state.composed_frame;
    for (std::size_t index = 0; index < frame.pixels.size(); ++index)
        frame.pixels[index] = std::to_integer<std::uint8_t>(art.image.pixels[index]);
    for (std::size_t index = 0; index < frame.palette.size(); ++index) {
        const auto& rgb = assets.palette.palette[index];
        frame.palette[index] = {rgb[0], rgb[1], rgb[2]};
    }
    for (int y = 5; y < 185; ++y)
        std::fill_n(frame.pixels.begin() + y * 320 + 10, 300, std::uint8_t{0xba});
    draw_indexed_text(assets, frame, 1, "1 of 1", 257, 190, state.ega_map[8]);
    int x = 12, y = 7;
    unsigned column = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto ch = std::to_integer<std::uint8_t>(text[index]);
        if (!ch) {
            if (index + 1 != text.size())
                throw DataError("nite3d.bsf", assets.bsf.header.block_offsets[2] + index,
                                "end-of-quest text terminator is final");
            break;
        }
        if (ch == '\r') continue;
        if (ch == '\n') { x = 12; y += 11; column = 0; continue; }
        if (ch == '\t') {
            column += 4 - column % 4;
            x = static_cast<int>((column + 2) * 6);
            continue;
        }
        if (ch == '%' || ch < 0x20 || ch >= 0x7f || y >= 185)
            throw DataError("nite3d.bsf", assets.bsf.header.block_offsets[2] + index,
                            "end-of-quest text fits the authored single page");
        const char glyph = static_cast<char>(ch);
        const auto width = indexed_text_width(assets, 1, {&glyph, 1});
        if (x + static_cast<int>(width) > 310)
            throw DataError("nite3d.bsf", assets.bsf.header.block_offsets[2] + index,
                            "end-of-quest line fits the UIF16 text field");
        draw_indexed_text(assets, frame, 1, {&glyph, 1}, x, y, state.ega_map[8]);
        x += static_cast<int>(width);
        ++column;
    }
    state.composed_ready = true;
}


void paint_popup_box(IndexedFrame& frame, const GameState& state,
                     int left, int top, int width, int height) {
    // The action rows and active/grey state follow 1018:1f6a/2146. Their
    // exact Win16 window chrome is not established; keep this native overlay
    // bounded and separate from the source-backed interaction state machine.
    for (int y = top; y < top + height + 4; ++y) {
        for (int x = left; x < left + width + 4; ++x) {
            if (x < 0 || x >= 320 || y < 0 || y >= 200) continue;
            frame.pixels[static_cast<std::size_t>(y) * 320 + x] =
                state.ega_map[(x >= left + width || y >= top + height) ? 0 : 8];
        }
    }
}

void draw_puzzle_popup(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    if (state.screen == Screen::combination_prompt) {
        constexpr std::string_view title = "What's the combination?";
        const int title_width = static_cast<int>(indexed_text_width(assets, 1, title));
        const int left = (320 - std::max(title_width, 92) - 16) / 2;
        paint_popup_box(frame, state, left, 55, std::max(title_width, 92) + 16, 47);
        draw_indexed_text(assets, frame, 1, title, left + 8, 64, state.ega_map[14]);
        char digits[] = "0000000";
        for (std::size_t i = 0; i < state.combination_length; ++i)
            digits[i] = state.combination_digits[i];
        draw_indexed_text(assets, frame, 1, {digits, 7}, left + 8, 83, state.ega_map[2]);
        return;
    }
    const bool control = state.screen == Screen::control_popup;
    if (!control && state.screen != Screen::floor_popup)
        throw std::logic_error("puzzle overlay outside popup screens");
    constexpr std::array<std::string_view, 5> controls{
        "Open remote doors", "Close remote doors", "Enable remote cannons",
        "Disable remote cannons", "Cancel"};
    const auto count = control ? 5u : state.popup_count;
    if (!count || count > 10 || state.popup_row >= count)
        throw std::out_of_range("puzzle menu count or cursor outside source range");
    int width = 88;
    for (std::uint8_t row = 0; row < count; ++row) {
        char floor_label[] = "Floor 10";
        floor_label[6] = row == 9 ? '1' : static_cast<char>('1' + row);
        const auto label = control ? controls[row] : state.popup_class >= 0x25 ?
            std::string_view{"Go down"} : std::string_view{floor_label, row == 9 ? 8u : 7u};
        width = std::max(width, static_cast<int>(indexed_text_width(assets, 2, label)) + 16);
    }
    paint_popup_box(frame, state, 100, 48, width, 10 * static_cast<int>(count) + 7);
    for (std::uint8_t row = 0; row < count; ++row) {
        char floor_label[] = "Floor 10";
        floor_label[6] = row == 9 ? '1' : static_cast<char>('1' + row);
        const auto label = control ? controls[row] : state.popup_class >= 0x25 ?
            std::string_view{"Go down"} : std::string_view{floor_label, row == 9 ? 8u : 7u};
        draw_indexed_text(assets, frame, 2, label, 111, 51 + 10 * row, 0);
        draw_indexed_text(assets, frame, 2, label, 110, 50 + 10 * row,
            !detail::popup_row_enabled(state, row) ? state.ega_map[8] :
            row == state.popup_row ? state.ega_map[14] : state.ega_map[2]);
    }
}

void compose_transition_overlay(GameState& state, const Assets& assets) {
    if (state.screen != Screen::warp_popup && state.screen != Screen::floor_popup &&
        state.screen != Screen::control_popup && state.screen != Screen::combination_prompt &&
        state.screen != Screen::modal_message && state.screen != Screen::intermission) return;
    if (!state.composed_ready)
        throw std::logic_error("overlay requires the previously composed frame");
    if (state.screen == Screen::floor_popup || state.screen == Screen::control_popup ||
        state.screen == Screen::combination_prompt)
        draw_puzzle_popup(state, assets, state.composed_frame);
    else detail::draw_overlay(state, assets, state.composed_frame);
}

void sync_palette(GameState& state) noexcept {
    // ba74(0/1) changes the live DIB colour table without painting its pixels.
    // Other palette flashes require an explicit source-backed presentation API.
    if (state.composed_ready) {
        const auto& rgb = state.palette[0xda];
        state.composed_frame.palette[0xda] = {rgb[0], rgb[1], rgb[2]};
    }
}

// 1010:ed90 -> 6dbc -> d8fc. The original 91-entry integer tangent table
// is used, including the 91-degree out-of-range quirk for near-horizontal
// ratios; this is not atan2 rounded to host floating point.
void turn_death_camera(GameState& state) {
    if (state.player_life_state != 2) return;
    if (state.killer_object_slot >= state.level.object_count)
        throw std::out_of_range("death camera killer object slot outside level");
    const auto& killer = state.level.objects[state.killer_object_slot];
    const int dx = static_cast<std::int16_t>(killer.world_x - state.player.world_x);
    const int dy = static_cast<std::int16_t>(killer.world_y - state.player.world_y);
    int angle = 90;
    if (dy) {
        const auto quotient = static_cast<std::uint32_t>(
            (static_cast<std::int64_t>(dx < 0 ? -dx : dx) * 1024) /
            (dy < 0 ? -dy : dy));
        angle = 91;
        for (int index = 0; index < static_cast<int>(kTangent.size()); ++index) {
            if (static_cast<std::uint32_t>(kTangent[index]) >= quotient) {
                angle = index;
                break;
            }
        }
    }
    const int target = dx >= 0 ? (dy <= 0 ? angle : 180 - angle) :
                       (dy < 0 ? 360 - angle : 180 + angle);
    int delta = target - state.player.heading;
    if (delta > 180) delta -= 360;
    else if (delta < -180) delta += 360;
    if (!delta) { state.player_life_state = 3; state.death_turn_pending = false; return; }
    const int maximum = state.clocks.turn_step;
    if (!maximum) throw std::logic_error("death camera requires measured turn calibration");
    const int step = std::clamp(delta, -maximum, maximum);
    detail::set_heading(state, static_cast<int>(state.player.heading) + step);
}

void pump_gameplay(GameState& state, const Assets& assets, const InputFrame& input,
                   std::uint32_t now) {
    if (!state.clocks.calibrated)
        throw std::logic_error("gameplay clock requires five-present calibration");
    auto& clocks = state.clocks;
    clocks.last_milliseconds = now;
    // 1010:d70a/d74a: 32-bit products wrap BEFORE unsigned divide. Count
    // only a changed quotient, once even if several quanta were missed.
    const auto a_quotient = static_cast<std::uint32_t>(now * 8u) / 1000u;
    if (a_quotient != clocks.previous_a_quotient) {
        clocks.previous_a_quotient = a_quotient;
        ++clocks.a_count;
    }
    if (clocks.a_count >= clocks.a_deadline) {
        const auto count = clocks.a_count;
        if (!state.demo) { // 1010:d9c6 suppresses d974 in demo mode on clock A.
            detail::service_doors(state);
            detail::service_guards(state, assets);
            detail::service_world_a(state, assets, count);
            sync_palette(state);
            if (state.fire_delay != 0xff) ++state.fire_delay;
        }
        clocks.a_deadline = count + 1u;
    }
    const auto b_quotient = static_cast<std::uint32_t>(
        now * static_cast<std::uint32_t>(static_cast<std::int16_t>(clocks.rate))) / 1000u;
    if (b_quotient != clocks.previous_b_quotient) {
        clocks.previous_b_quotient = b_quotient;
        ++clocks.b_count;
    }
    if (clocks.b_count < clocks.b_deadline) return;
    const auto count = clocks.b_count;
    if (state.demo && (clocks.logical_tick & 1u)) {
        // d9c6: alternate d974 uses the live A counter on odd demo B ticks.
        detail::service_doors(state);
        detail::service_guards(state, assets);
        detail::service_world_a(state, assets, clocks.a_count);
        sync_palette(state);
        if (state.fire_delay != 0xff) ++state.fire_delay;
    }
    turn_death_camera(state);
    ++clocks.logical_tick;
    prepare(state, assets); // d78c BEFORE bbca/ba54, door B, projectiles and 9806.
    detail::advance_chalkboard(state, assets); // 1010:65a6 at visible-wall draw.
    detail::advance_destructible_walls(state, assets); // 1010:65a6/1018:3c0c.
    detail::service_world_b(state, assets);
    compose_gameplay(state, assets); // ba54/HUD present BEFORE 1e00, 9e20 and 9806.
    detail::advance_doors_and_panels(state);
    const auto enqueue_projectile = detail::service_projectiles(state, assets);
    queue_serviced_projectiles(state, assets, enqueue_projectile);
    InputFrame replay = input;
    if (state.demo && state.player_life_state == 1) {
        const auto due = state.demo->advance(clocks.logical_tick, false);
        replay = {};
        replay.key = due.key;
        replay.held = due.held; // Persistent until the next due event, including EOF.
        if (replay.key == 0x1b) state.player_life_state = 0;
    }
    const auto shade_before_input = state.shade_epoch;
    if (state.player_life_state == 1 && !state.core_wait_ticks && replay.key == 0x1b) {
        state.screen = Screen::root_menu;
        state.menu_row = 5; // 1030:1028 marks Return to game as the initial cursor.
        compose_menu_screen(state, assets);
        select_tune(state, 12);
    } else if (state.player_life_state == 1 && !state.core_wait_ticks && replay.key == 0x70 &&
               !(replay.held & 0x100u))
        enter_help(state, assets, Screen::gameplay);
    else if (state.player_life_state == 1 && !state.core_wait_ticks &&
             (replay.key == 0x73 || replay.key == 0x74)) {
        // The authenticated F5 witness has exactly one occupied first slot.
        // With multiple occupied rows, no source-backed quick-slot identity is
        // available from the host metadata, so use the explicit Load menu.
        const bool quick_load = replay.key == 0x74 && state.save_slots[0].present &&
            std::none_of(state.save_slots.begin() + 1, state.save_slots.end(),
                         [](const SaveMenuSlot& slot) { return slot.present; });
        open_save_menu(state, Screen::gameplay,
                       replay.key == 0x73 ? Screen::save_menu : Screen::load_menu);
        if (quick_load) {
            state.save_menu_quick_confirm = true;
            state.save_menu_pending = true; // Confirmation Enter performs the one-shot load.
            compose_quick_load_confirmation(state, assets);
        } else compose_menu_screen(state, assets);
    } else if (state.player_life_state == 1 && !state.core_wait_ticks)
        detail::handle_gameplay_input(state, assets, replay);
    if (state.shade_epoch != shade_before_input) {
        // 1010:bfd8/c0a2 call c5e2 then d7c0 immediately, before showing
        // the storm/fuse modal; the normal B-pass scene above predates input.
        prepare(state, assets);
        compose_gameplay(state, assets);
    }
    compose_transition_overlay(state, assets);
    clocks.b_deadline = count + 1u;
    // render() only copies this already-composed DIB, so later actor, door,
    // projectile or player mutations cannot leak into the prior presentation.
}

} // namespace
void refresh_save_menu(GameState& state, const Assets& assets) {
    if (state.screen != Screen::save_menu && state.screen != Screen::load_menu) return;
    if (!state.save_menu_restore_frame_valid)
        throw std::logic_error("save menu refresh requires its caller framebuffer");
    if (state.save_menu_quick_confirm && !state.save_slots[0].present) {
        state.save_menu_quick_confirm = false;
        state.save_menu_pending = false;
    }
    if (state.save_menu_quick_confirm) {
        state.composed_frame = state.save_menu_restore_frame;
        compose_quick_load_confirmation(state, assets);
    } else compose_menu_screen(state, assets);
}

void step_screen(GameState& state, const Assets& assets, const InputFrame& input,
                 std::uint64_t logical_time) {
    const auto now = static_cast<std::uint32_t>(logical_time);
    switch (state.screen) {
    case Screen::title:
        // State 2 of 1018:2fc6 has NO timeout. Keydown, L-button release,
        // R-button press or joystick activity takes this single transition.
        if ((input.key && !(input.held & 0x100u)) || input.menu_left_released ||
            input.menu_right_pressed || input.joystick_fire || input.joystick_use ||
            input.joystick_forward || input.joystick_turn || input.joystick_strafe) {
            state.menu_row = 0;
            state.screen = Screen::root_menu;
            compose_menu_screen(state, assets);
            select_tune(state, 12);
        }
        return;
    case Screen::root_menu: case Screen::episode_menu: case Screen::difficulty_menu:
    case Screen::save_menu: case Screen::load_menu: {
        const auto previous_screen = state.screen;
        const auto previous_row = menu_row(state);
        const auto previous_pending = state.save_menu_pending;
        if (state.screen == Screen::load_menu && state.save_menu_quick_confirm) {
            if (input.key == 0x1b || input.menu_right_pressed) pop_menu(state);
            else if ((input.key == 0x0d || input.key == 0x20) && !(input.held & 0x100u))
                select_menu(state, assets, now);
        } else step_menu(state, assets, input, now);
        if (state.screen == Screen::help || state.save_menu_outcome.dismissed ||
            state.save_menu_outcome.request) return;
        if (state.screen == Screen::gameplay) {
            if (!state.demo) compose_new_level(state, assets); // start_demo already prepared it.
            return;
        }
        if (state.screen != previous_screen || menu_row(state) != previous_row ||
            state.save_menu_pending != previous_pending)
            compose_menu_screen(state, assets);
        return;
    }
    case Screen::settings:
        state.settings_outcome = state.settings.process(
            assets, state.config, input, assets.bsf.header.flag_byte != 0);
        if (state.settings_outcome.apply_cheats) detail::apply_cheats(state);
        if (state.settings_outcome.returned_to_root) state.screen = Screen::root_menu;
        compose_menu_screen(state, assets);
        return;
    case Screen::gameplay:
        if (state.demo && state.player_life_state == 3) {
            restart_demo(state, assets); // 1010:daa0 loops the same recording on death.
            return;
        }
        if (state.demo && state.player_life_state == 0) {
            close_demo(state, assets); // Recorded Esc also exits playback.
            return;
        }
        if (state.player_life_state == 3) {
            select_tune(state, 12); // 1010:dc18/dc22 death dispatcher.
            if (state.episode == 1 && state.level_index == 9 &&
                state.hamerstein_challenge && !state.player.health) {
                state.screen = Screen::quest_story;
                compose_quest_story(state, assets); // BSF block2 and UIF16.
            } else if (state.episode == 2 && state.level_index == 9 &&
                       state.plasma_destroyed) {
                detail::end_tally(state);
                compose_transition_overlay(state, assets);
            } else {
                state.screen = Screen::root_menu; // Ordinary/hazard death dc80.
                state.menu_row = 0;
                compose_menu_screen(state, assets);
            }
            return;
        }
        if (state.player_life_state == 0 && state.episode == 3 &&
            state.level_index == 9 && state.ending_flag) {
            select_tune(state, 12); // dc22, then c67a starts tune 1.
            state.player_life_state = 3;
            select_tune(state, 1);
            state.screen = Screen::ending;
            return;
        }
        pump_gameplay(state, assets, input, now);
        return;
    case Screen::quest_story:
        if (input.held & 0x100u) return;
        if (input.key == 0x79) state.help_order_requested = true;
        if (input.key == 0x1b || input.menu_right_pressed || input.joystick_use) {
            detail::end_tally(state); // 1010:1cfe -> 1018:0ab0(10,0).
            compose_transition_overlay(state, assets);
        }
        return;
    case Screen::ending:
        if (input.ending_finished ||
            (!(input.held & 0x100u) &&
             (input.key == 0x1b || input.key == 0x20 || input.key == 0x0d))) {
            // 1010:6d2c closes the FLI, then shows UIF14 tally.
            detail::end_tally(state);
            compose_transition_overlay(state, assets);
        }
        return;
    case Screen::help:
        if (!state.help_pager) throw std::logic_error("help input requires a validated BSF pager");
        if (input.held & 0x100u) return; // 1010:8cd2 does not route keys while Alt is held.
        if (input.key == 0x1b || input.menu_right_pressed || input.joystick_use) {
            if (state.modal_return != Screen::root_menu && state.modal_return != Screen::gameplay)
                throw std::logic_error("help has no original return screen");
            state.screen = state.modal_return;
            state.clocks.last_milliseconds = now;
            if (state.screen == Screen::root_menu) {
                state.menu_row = state.level_loaded && state.player_life_state == 1 ? 5 : 0;
                compose_menu_screen(state, assets);
                select_tune(state, 12);
            } else {
                // 1018:1ed0→1e7c re-runs the provisional frame and d7d0 calibration.
                state.clocks.calibrated = false;
                state.clocks.calibration_started = false;
                compose_new_level(state, assets);
            }
            return;
        }
        switch (input.key) {
        case 0x21: case 0x26: // PgUp/Up; translated '!'/'&' are the same bytes.
            static_cast<void>(state.help_pager->previous());
            break;
        case 0x22: case 0x28: // PgDn/Down; translated '"'/'(' are the same bytes.
            static_cast<void>(state.help_pager->next());
            break;
        case 0x79: // 1018:1cae: F10 or ToAscii 'y'; purchased set lacks order.txt.
            state.help_order_requested = true;
            break;
        default: break;
        }
        return;
    case Screen::warp_popup: case Screen::floor_popup: case Screen::control_popup:
    case Screen::combination_prompt: case Screen::modal_message: case Screen::intermission:
        // These screens suspend d9c6. Their input is handled immediately,
        // including Enter/Esc/Space modal dismissal and intermission acceptance;
        // at most one A/B pass runs after resuming gameplay.
        const auto previous_screen = state.screen;
        const auto previous_warp_row = state.warp_row;
        const auto previous_popup_row = state.popup_row;
        const auto previous_combination_length = state.combination_length;
        const auto previous_message = state.message;
        InputFrame routed = input;
        if (state.demo) {
            const auto due = state.demo->advance(state.clocks.logical_tick, true);
            if (!due.dispatch_modal_key) return; // 90ce increments tick on future modal event.
            routed = {};
            routed.key = due.key;
            routed.held = due.held;
        }
        const bool accepting_level = state.screen == Screen::intermission &&
            (routed.key == 0x0d || routed.key == 0x1b || routed.key == 0x20 ||
             routed.menu_right_pressed);
        const bool reason2_modal = state.screen == Screen::modal_message && state.modal_reason == 2;
        state.clocks.last_milliseconds = now;
        detail::handle_gameplay_input(state, assets, routed);
        if (reason2_modal && state.screen == Screen::gameplay) {
            // A reason-2 modal is dismissed by the gameplay key itself, so the
            // held mask continues once on the same tick with the dismiss edge
            // consumed. Original Windows continuation parity is unverified.
            InputFrame continued = routed;
            continued.key = 0;
            detail::handle_gameplay_input(state, assets, continued);
        }
        if (accepting_level && state.screen == Screen::gameplay) {
            state.clocks.calibrated = false;
            state.clocks.calibration_started = false;
            compose_new_level(state, assets);
        } else if (accepting_level && state.screen == Screen::root_menu) {
            compose_menu_screen(state, assets);
            select_tune(state, 12);
        } else if (state.screen == previous_screen &&
                   ((state.screen == Screen::warp_popup && state.warp_row != previous_warp_row) ||
                    ((state.screen == Screen::floor_popup || state.screen == Screen::control_popup) &&
                     state.popup_row != previous_popup_row) ||
                    (state.screen == Screen::combination_prompt &&
                     state.combination_length != previous_combination_length) ||
                    (state.screen == Screen::modal_message && state.message != previous_message))) {
            if (state.screen == Screen::modal_message) compose_gameplay(state, assets);
            compose_transition_overlay(state, assets);
        } else if (state.screen != previous_screen && state.screen != Screen::gameplay) {
            compose_transition_overlay(state, assets);
        }
        return;
    }
    throw std::logic_error("unrecognized game screen");
}

void abort_demo(GameState& state, const Assets& assets) {
    if (!state.demo) return;
    state.demo->abort_on_live_keydown(); // 1010:8cd2 before the next B pass.
    close_demo(state, assets);
}

void step(GameState& state, const Assets& assets, const InputFrame& input,
          std::uint64_t logical_time) {
    state.settings_outcome = {};
    state.save_menu_outcome = {};
    if (state.demo && input.key) {
        abort_demo(state, assets); // Never consume the live key as a recorded action.
        return;
    }
    step_screen(state, assets, input, logical_time);
    std::string_view notice{};
    // 1010:8cd2 routes menus before invoking ddb2/ddfc. The key toggles
    // are global and do not repaint or reinitialize an open hardware slider.
    if (input.key == 0x71) {
        state.config.music_enabled ^= 1u;
        state.settings_outcome.reconcile_audio = true;
        notice = state.config.music_enabled ? "Music on" : "Music off";
    } else if (input.key == 0x72) {
        state.config.effects_enabled ^= 1u;
        state.settings_outcome.reconcile_audio = true;
        notice = state.config.effects_enabled ? "Sound on" : "Sound off";
        if (state.config.effects_enabled && state.screen != Screen::gameplay &&
            state.screen != Screen::help)
            state.audio.push({AudioCommandKind::effect, 0x2e, 0, 2});
    }
    if (!notice.empty() && state.screen == Screen::gameplay &&
        state.player_life_state == 1) {
        state.message = notice; // 1010:ddb2/ddfc -> 1018:0818.
        state.modal_reason = 2;
        state.modal_return = Screen::gameplay;
        state.screen = Screen::modal_message;
        compose_transition_overlay(state, assets);
    }
}

void render(const GameState& state, const Assets& assets, IndexedFrame& frame) {
    if (!state.composed_ready)
        throw std::logic_error("render requires an original-order composed frame");
    if (state.screen == Screen::ending)
        throw std::logic_error("ending pixels must come from the active FliPlayer");
    if (state.screen == Screen::help) {
        if (!state.help_pager) throw std::logic_error("help screen requires a validated BSF pager");
        render_help(*state.help_pager, assets, state.episode, frame);
        return;
    }
    frame = state.composed_frame;
}

void initialize_game(GameState& state, const Assets& assets, Config config) {
    config.validate();
    std::destroy_at(&state);
    std::construct_at(&state);
    state.config = config;
    state.ega_map = compute_ega_palette_map(assets);
    for (std::size_t i = 0; i < state.palette.size(); ++i)
        state.palette[i] = assets.palette.palette[i];
    compose_title(assets, state.composed_frame);
    state.composed_ready = true;
    select_tune(state, 1);
}

void begin_calibration(GameState& state) {
    if (!state.level_loaded || state.screen != Screen::gameplay ||
        state.player_life_state != 1 || !state.composed_ready)
        throw std::logic_error("calibration requires the already-presented provisional frame");
    detail::initialize_lightning(state); // 1018:1e7c, after d7c0 and HUD.
    sync_palette(state);
    state.clocks.calibrated = false;
    state.clocks.calibration_started = true;
}
void advance_calibration_frame(GameState& state, const Assets& assets,
                               std::uint32_t present_time_ms) {
    if (!state.level_loaded || state.screen != Screen::gameplay ||
        state.player_life_state != 1 || !state.clocks.calibration_started)
        throw std::logic_error("calibration frame requires active five-present measurement");
    state.clocks.last_milliseconds = present_time_ms;
    // 1010:d7d0 renders a fresh d7c0 for EACH of its five measured frames,
    // then resets 53dc at the end. This includes scene/HUD work in the
    // measurement instead of timing the same cached DIB five times.
    compose_new_level(state, assets);
}


void calibrate(GameState& state, std::uint32_t average_present_ms) {
    if (!state.level_loaded || state.screen != Screen::gameplay ||
        state.player_life_state != 1 || !state.clocks.calibration_started)
        throw std::logic_error("calibration requires begin_calibration and five measured presents");
    // d7d0 receives the observed five-present average; ba74(1) already ran
    // between the first provisional presentation and these measured frames.
    auto& clocks = state.clocks;
    const auto x = std::max<std::uint32_t>(40, average_present_ms);
    clocks.average_present_ms = average_present_ms; // DS:53f2 is the measured, unclamped value.
    clocks.rate = static_cast<std::uint16_t>((x / 2u + 1000u) / x);
    clocks.move_step = static_cast<std::uint16_t>(std::max<std::uint32_t>(1, (x + 2u) >> 2u));
    clocks.turn_step = static_cast<std::uint16_t>(std::max<std::uint32_t>(
        1, (x * 360u + 0x578u) / 0xaf0u));
    clocks.projectile_step = static_cast<std::uint16_t>(clocks.move_step * 2u);
    if (state.demo) {
        const auto& header = state.demo->header(); // 90ce overwrites steps, NOT local B rate.
        clocks.move_step = header.word_0;
        clocks.turn_step = header.word_1;
        clocks.projectile_step = header.word_2;
    }
    clocks.logical_tick = 0;
    clocks.calibrated = true;
    clocks.calibration_started = false;
}

} // namespace n3d
