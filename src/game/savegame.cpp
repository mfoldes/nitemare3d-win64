#include "n3d/savegame.hpp"
#include "internal.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace n3d {
namespace {

constexpr std::size_t kProjectileCount = 8u;
constexpr std::size_t kProjectileRecordSize = 22u;
constexpr std::uint32_t kNativeSaveVersion = 2u;
constexpr std::size_t kHeaderSize = 56;
constexpr std::size_t kMinimumPayload = 79922;
constexpr std::size_t kMaximumPayload = 112924;
constexpr std::array<std::uint8_t, 8> kMagic{'N','3','D','S','A','V','E',0};
constexpr std::array<std::string_view, 4> kCaptions{
    "Pentagram of Good Health", "Pentagram of Power",
    "Pentagram of Good Sight", "Pentagram of Knowledge"};

[[noreturn]] void invalid(std::string_view path, std::size_t offset,
                          std::string_view reason) {
    throw DataError(std::string(path), offset, reason);
}
void require(bool ok, std::string_view path, std::size_t offset,
             std::string_view reason) {
    if (!ok) invalid(path, offset, reason);
}

// The same field-order functions drive both codecs; each integer is explicitly
// sized and endian converted, never copied from a C++ object representation.
struct Writer final {
    std::vector<std::byte> bytes;
    [[nodiscard]] std::size_t position() const noexcept { return bytes.size(); }
    template<class T> void u8(const T& value) { bytes.push_back(std::byte{static_cast<std::uint8_t>(value)}); }
    template<class T> void i8(const T& value) { u8(static_cast<std::uint8_t>(value)); }
    template<class T> void u16(const T& value) {
        const auto bits = static_cast<std::uint16_t>(value);
        u8(bits); u8(bits >> 8);
    }
    template<class T> void i16(const T& value) { u16(static_cast<std::uint16_t>(value)); }
    template<class T> void u32(const T& value) {
        const auto bits = static_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) u8(bits >> shift);
    }
    void boolean(bool value) { u8(static_cast<std::uint8_t>(value)); }
};
struct Reader final {
    ByteReader input;
    explicit Reader(std::string_view path, std::span<const std::byte> bytes) : input(path, bytes) {
        input.bytes(kHeaderSize);
    }
    [[nodiscard]] std::size_t position() const noexcept { return input.offset(); }
    void u8(std::uint8_t& value) { value = input.u8(); }
    void i8(std::int8_t& value) { value = std::bit_cast<std::int8_t>(input.u8()); }
    void u16(std::uint16_t& value) { value = input.u16le(); }
    void i16(std::int16_t& value) { value = std::bit_cast<std::int16_t>(input.u16le()); }
    void u32(std::uint32_t& value) { value = input.u32le(); }
    void boolean(bool& value) {
        const auto at = position();
        const auto raw = input.u8();
        require(raw <= 1, input.path(), at, "boolean field is 0 or 1");
        value = raw != 0;
    }
};

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool valid_projectile_lift(const ProjectileState& projectile) noexcept {
    return projectile.state == 0 ?
        (projectile.lift == 0u || (projectile.lift >= 5u && projectile.lift <= 20u)) :
        (projectile.lift >= 5u && projectile.lift <= 20u);
}

void eligible(const GameState& s, const Assets& assets, std::string_view path,
              std::size_t at) {
    require(assets.required_content_identity == compute_required_content_identity(), path, at,
            "required Windows content identity is validated");
    require(s.episode >= 1 && s.episode <= assets.episodes.size() &&
            s.level_index < assets.episodes[s.episode - 1].map.level_count &&
            s.level_loaded && s.level.level_index == s.level_index, path, at,
            "owned, identified MAP level is loaded");
    require(!s.demo && s.player_life_state == 1 && s.clocks.calibrated &&
            !s.clocks.calibration_started && s.composed_ready &&
            (s.screen == Screen::gameplay || s.screen == Screen::root_menu) &&
            s.settings.screen() == SettingsScreen::closed &&
            s.audio.pending().empty() && s.queued_message_count == 0 &&
            s.message.empty() && s.warning.empty() && !s.quit_requested &&
            !s.settings_outcome.save_config && !s.settings_outcome.returned_to_root &&
            !s.settings_outcome.reconcile_audio && !s.settings_outcome.set_music_volume &&
            !s.settings_outcome.play_effect_2e && !s.settings_outcome.recalibrate_joystick &&
            !s.settings_outcome.apply_cheats && !s.settings_outcome.show_unavailable_cheats,
            path, at, "completed living gameplay or active root-pause capture boundary");
    const auto cursors = s.settings.retained_cursors();
    require(s.menu_row < 7 && s.difficulty_row < 3 && s.warp_row < 3 &&
            cursors[0] < 3 && cursors[1] < 5 && cursors[2] < 6,
            path, at, "retained menu cursors index their source tables");
    require(s.automap_speckle_count <= s.automap_speckles.size() &&
            s.door_count <= s.doors.size() && s.panel_count <= s.panels.size() &&
            s.pushwall_count <= s.pushwalls.size() &&
            s.level.object_count <= kMaxObjects && s.level.guard_count <= kMaxGuards &&
            s.level.vector_count <= kMaxVectors, path, at, "record counts fit source capacities");
    s.config.validate(path, kHeaderSize + 15);
}

#define U8(x) io.u8(x)
#define I8(x) io.i8(x)
#define U16(x) io.u16(x)
#define I16(x) io.i16(x)
#define U32(x) io.u32(x)
#define BOOL(x) io.boolean(x)

template<class IO, class S>
void session(IO& io, S& s, std::uint32_t& anchor) {
    // Group 1: 15 session bytes followed by exactly Config::encode's 20 bytes.
    if constexpr (std::is_same_v<IO, Writer>) {
        // Native screen discriminants are wire values, not C++ enum ordinals.
        U8(s.screen == Screen::gameplay ? 5u : 2u);
    } else {
        const auto at = io.position();
        std::uint8_t raw = 0;
        U8(raw);
        require(raw == 2 || raw == 5, io.input.path(), at,
                "native save screen is gameplay 5 or active root pause 2; other values are rejected");
        s.screen = raw == 2 ? Screen::root_menu : Screen::gameplay;
    }
    U8(s.episode); U16(s.level_index); U8(s.pending_episode); U16(s.pending_level);
    U8(s.menu_row); U8(s.difficulty_row); U8(s.warp_row);
    if constexpr (std::is_same_v<IO, Writer>) {
        for (const auto cursor : s.settings.retained_cursors()) U8(cursor);
    } else {
        std::array<std::uint8_t, 3> cursors{};
        for (auto& cursor : cursors) U8(cursor);
        s.settings.restore_cursors(cursors);
    }
    U8(s.current_tune); U8(s.demo_tune_counter);
    if constexpr (std::is_same_v<IO, Writer>) {
        for (const auto byte : s.config.encode()) U8(std::to_integer<std::uint8_t>(byte));
    } else {
        std::array<std::byte, Config::kSerializedSize> bytes{};
        for (auto& byte : bytes) { std::uint8_t raw = 0; U8(raw); byte = std::byte{raw}; }
        s.config = Config::decode(bytes, io.input.path());
        s.config.validate(io.input.path(), kHeaderSize + 15);
    }
    // Group 2: virtual host clock anchor plus all thirteen live clock fields.
    U32(anchor);
    auto& c = s.clocks;
    U32(c.previous_a_quotient); U32(c.previous_b_quotient);
    U32(c.a_count); U32(c.b_count); U32(c.a_deadline); U32(c.b_deadline);
    U32(c.logical_tick); U32(c.last_milliseconds);
    U16(c.rate); U16(c.move_step); U16(c.turn_step); U16(c.projectile_step);
    U32(c.average_present_ms);
}

template<class IO, class S>
void player(IO& io, S& s) {
    auto& p = s.player;
    I16(p.world_x); I16(p.world_y); U8(p.cell_x); U8(p.cell_y);
    U16(p.heading); U8(p.octant); U8(p.nearest_sector); U16(p.octant_mask);
    I16(p.sine); I16(p.cosine); U16(p.major_axis); I16(p.bresenham_error);
    I16(p.bresenham_minor); I16(p.bresenham_diagonal); I16(p.damage_feedback);
    U16(p.difficulty); U32(p.score); U8(p.health); U8(p.class35_count);
    U8(p.resource_4c21); U8(p.resource_4c22);
    for (auto& ammo : p.ammo) U8(ammo);
    U8(p.weapon); U8(p.queued_weapon); U16(p.firing_mode);
    U8(p.weapon_owned); U8(p.class37_mask); U8(p.keys); U8(p.cards);
    U8(p.pentagrams); U8(p.movement_locked); U8(p.jammed);
    U8(p.enemy_sensor); U8(p.map_sensor); U8(p.enemy_meter); U8(p.map_meter);
    U8(p.room_id); U8(p.weapon_animation_state); U8(p.weapon_animation_frame);
    U8(p.muzzle_flash);
}

template<class IO, class S>
void world(IO& io, S& s) {
    U8(s.previous_use); U8(s.previous_fire); U8(s.fire_delay);
    U8(s.available_cards); U8(s.remote_open_mask); U8(s.cannons_enabled);
    U8(s.level_latch); U8(s.penelope_latch); U8(s.hamerstein_challenge);
    U8(s.plasma_destroyed); U8(s.ending_flag); U8(s.lights_fused);
    U16(s.plasma_hits); U32(s.core_wait_started_tick); U16(s.core_wait_ticks);
    BOOL(s.has_destructible_walls); U8(s.chalkboard_image); BOOL(s.chalkboard_active);
    U32(s.rng_seed); I16(s.lightning_countdown); BOOL(s.lightning_on);
    U32(s.shade_epoch); BOOL(s.panel_nearby); U8(s.draw_weapon);
    U8(s.draw_weapon_frame); U8(s.draw_weapon_recoil);
    std::uint8_t caption = 0;
    if constexpr (std::is_same_v<IO, Writer>) {
        for (std::size_t i = 0; i < kCaptions.size(); ++i)
            if (s.hud_message == kCaptions[i]) caption = static_cast<std::uint8_t>(i + 1);
        require(s.hud_message.empty() || caption, "N3DSAVE", io.position(),
                "HUD caption is empty or a pentagram caption");
    }
    U8(caption);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(caption <= kCaptions.size(), io.input.path(), io.position() - 1,
                "HUD caption ID references a pentagram caption");
        s.hud_message = caption ? kCaptions[caption - 1] : std::string_view{};
    }
    for (auto& image : s.radio_original_images) U8(image);
    for (auto& room : s.room_wake) U8(room);
    I16(s.automap_origin_x); I16(s.automap_origin_y);
    I16(s.automap_heading_x); I16(s.automap_heading_y);
    BOOL(s.automap_heading_valid); BOOL(s.automap_blink); BOOL(s.sensor_panel_cleared);
    std::uint16_t count = 0;
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(s.automap_speckle_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count <= s.automap_speckles.size(), io.input.path(), io.position() - 2,
                "automap speckle count within 500 slots");
        s.automap_speckle_count = count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        U8(s.automap_speckles[i].x); U8(s.automap_speckles[i].y);
        if constexpr (std::is_same_v<IO, Reader>)
            require(s.automap_speckles[i].x < 62 && s.automap_speckles[i].y < 36,
                    io.input.path(), io.position() - 2,
                    "automap speckle indexes the 62x36 sensor panel");
    }
    for (auto& pixel : s.automap) U8(pixel); // Original column-major indexing.
    for (auto& rgb : s.palette) for (auto& channel : rgb) U8(channel);
}

template<class IO, class S>
void frame_scene(IO& io, S& s) {
    for (auto& pixel : s.composed_frame.pixels) U8(pixel);
    for (auto& rgb : s.composed_frame.palette) {
        U8(rgb.r); U8(rgb.g); U8(rgb.b);
    }
    auto& scene = s.scene;
    for (auto& cursor : scene.vector_cursors) U16(cursor);
    U16(scene.object_cursor_x); U16(scene.object_cursor_y);
    for (auto& offset : scene.column_pointer_offsets) U16(offset);
    for (auto& height : scene.wall_y16) U16(height);
    for (auto& shade : scene.shade_map) U8(shade);
    U8(scene.cached_shade_level); U32(scene.cached_shade_epoch);
}

template<class IO, class S>
void records(IO& io, S& s, const Assets& assets, const GameState* pristine = nullptr) {
    auto& level = s.level;
    for (auto& cell : level.cells) { U8(cell.wall_id); U8(cell.object_id); }
    std::uint16_t count = 0;
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(s.door_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count == pristine->door_count, io.input.path(), io.position() - 2,
                "door count equals pristine MAP-derived count");
        s.door_count = count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& d = s.doors[i];
        U16(d.cell_index); U16(d.vector_index); U16(d.partner_vector_index);
        U8(d.state); I16(d.countdown); BOOL(d.sound);
    }
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(s.panel_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count == pristine->panel_count, io.input.path(), io.position() - 2,
                "panel count equals pristine MAP-derived count");
        s.panel_count = count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& p = s.panels[i];
        U16(p.cell_index); U8(p.state);
        for (auto& face : p.face_vectors) U16(face);
    }
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(s.pushwall_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count == pristine->pushwall_count, io.input.path(), io.position() - 2,
                "pushwall count equals pristine MAP-derived count");
        s.pushwall_count = count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& w = s.pushwalls[i];
        U16(w.object_slot); I8(w.step_x); I8(w.step_y); U8(w.steps_remaining);
    }
    for (auto& p : s.projectiles) {
        if constexpr (std::is_same_v<IO, Writer>) {
            require(p.state <= 2, "N3DSAVE", io.position(),
                    "projectile state has a dispatcher");
            require(valid_projectile_lift(p), "N3DSAVE", io.position() + 21,
                    "projectile lift is zero for free slots or 5..20 for active slots");
        }
        U8(p.state); U8(p.image_id); I16(p.world_x); I16(p.world_y);
        I8(p.sign_x); I8(p.sign_y); U16(p.major_axis); I16(p.error);
        I16(p.minor_increment); I16(p.diagonal_increment);
        U8(p.frame); U32(p.frame_deadline);
        if constexpr (std::is_same_v<IO, Reader>) {
            const auto at = io.position();
            U8(p.lift);
            require(valid_projectile_lift(p), io.input.path(), at,
                    "projectile lift is zero for free slots or 5..20 for active slots");
        } else {
            U8(p.lift);
        }
    }
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(level.object_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count == pristine->level.object_count, io.input.path(), io.position() - 2,
                "object count equals pristine MAP-derived count");
        level.object_count = count;
    }
    const auto& episode = assets.episodes[s.episode - 1];
    std::string_view path = "N3DSAVE";
    if constexpr (std::is_same_v<IO, Reader>) path = io.input.path();
    for (std::size_t i = 0; i < count; ++i) {
        auto& o = level.objects[i];
        const auto at = io.position();
        U8(o.image_id); U8(o.subtype); U8(o.flags); U8(o.class_id);
        U8(o.bank); U8(o.frame_index); U32(o.deadline_ms); U8(o.enemy_index);
        U16(o.cell_x); U16(o.cell_y);
        I16(o.world_x); I16(o.world_y); I16(o.last_base_y); I16(o.lift);
        const auto& sequence = episode.images.sequences[256u + o.image_id];
        require(sequence.offset && sequence.frame_count &&
                    sequence.first_record_index < episode.images.records.size() &&
                    sequence.frame_count <= episode.images.records.size() - sequence.first_record_index,
                path, at, "object IMG descriptor has decoded frames");
        require(o.frame_index < sequence.frame_count, path, at + 5,
                "object frame indexes its current IMG descriptor");
        require(!sequence.extended_state_flag || !sequence.delay_ms, path, at + 4,
                "1010:cbd4 timed extended object random-bank branch unresolved");
        require(o.bank == 0, path, at + 4,
                "object bank is zero without a timed extended IMG descriptor");
        if (o.class_id == 0x2c || o.class_id == 0x2d)
            require((sequence.frame_count >> (o.class_id == 0x2d ? 3 : 2)) != 0,
                    path, at + 5, "1010:cb26 directional object frame-group divisor is nonzero");
    }
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(level.guard_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count == pristine->level.guard_count, io.input.path(), io.position() - 2,
                "guard count equals pristine MAP-derived count");
        level.guard_count = count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& g = level.guards[i];
        U16(g.animation_word); U32(g.seen_tick); I16(g.timer); U16(g.object_slot);
        U8(g.strategy); U8(g.state); U8(g.next_state); U8(g.hidden_object_id);
        U8(g.room_id); U8(g.orientation_aware); U8(g.hit_points); U8(g.facing);
        U8(g.resolved_octant); I8(g.step_x); I8(g.step_y); I8(g.bob_speed);
        U8(g.attack_kind); U8(g.line_of_sight); U8(g.adjacent);
    }
    if constexpr (std::is_same_v<IO, Writer>) count = static_cast<std::uint16_t>(level.vector_count);
    U16(count);
    if constexpr (std::is_same_v<IO, Reader>) {
        require(count == pristine->level.vector_count, io.input.path(), io.position() - 2,
                "vector count equals pristine MAP-derived count");
        level.vector_count = count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& v = level.vectors[i];
        U8(v.image_id); U8(v.subtype); U8(v.cached_image_id); U8(v.flags);
        U8(v.class_id); U8(v.direction); U8(v.frame_index); U32(v.deadline);
        I16(v.x0); I16(v.y0); I16(v.x1); I16(v.y1);
    }
    for (std::size_t i = 0; i < level.object_count; ++i) U16(level.object_list_x[i]);
    for (std::size_t i = 0; i < level.object_count; ++i) U16(level.object_list_y[i]);
}
#undef U8
#undef I8
#undef U16
#undef I16
#undef U32
#undef BOOL

// Validate identities against a separately reconstructed pristine level. Mutable
// geometry, flags, phase and list ordering must never be re-derived from positions.
void validate_level(GameState& s, GameState& base, const Assets& assets,
                    std::string_view path, std::size_t offset) {
    const auto& l = s.level;
    const auto& b = base.level;
    require(s.has_destructible_walls == base.has_destructible_walls, path,
            kHeaderSize + 141 + 20, "destructible-wall presence matches pristine MAP vectors");
    const auto doors_at = kHeaderSize + 79734 + 2 * s.automap_speckle_count;
    const auto panels_at = doors_at + 2 + 10 * s.door_count;
    const auto pushwalls_at = panels_at + 2 + 11 * s.panel_count;
    const auto projectiles_at = pushwalls_at + 2 + 5 * s.pushwall_count;
    const auto objects_at = projectiles_at + kProjectileCount * kProjectileRecordSize;
    const auto guards_at = objects_at + 2 + 23 * l.object_count;
    const auto vectors_at = guards_at + 2 + 25 * l.guard_count;
    const auto x_order_at = vectors_at + 2 + 19 * l.vector_count;
    const auto y_order_at = x_order_at + 2 * l.object_count;
    for (std::size_t i = 0; i < s.door_count; ++i) {
        offset = doors_at + 2 + 10 * i;
        const auto& d = s.doors[i]; const auto& original = base.doors[i];
        require(d.cell_index == original.cell_index &&
                d.vector_index == original.vector_index &&
                d.partner_vector_index == original.partner_vector_index &&
                d.cell_index < kMapCellCount && d.vector_index < l.vector_count &&
                d.partner_vector_index < l.vector_count &&
                d.vector_index != d.partner_vector_index && d.state <= 4, path, offset,
                "door cell, distinct faces and dispatchable state match pristine MAP");
    }
    for (std::size_t i = 0; i < s.panel_count; ++i) {
        offset = panels_at + 2 + 11 * i;
        const auto& p = s.panels[i]; const auto& original = base.panels[i];
        require(p.cell_index == original.cell_index && p.face_vectors == original.face_vectors &&
                p.cell_index < kMapCellCount && p.state <= 2, path, offset,
                "panel cell/faces match pristine MAP and state is dispatchable");
        for (auto face : p.face_vectors)
            require(face < l.vector_count || face == kMaxVectors, path, offset,
                    "panel face is active or source sentinel");
    }
    for (std::size_t i = 0; i < s.pushwall_count; ++i) {
        offset = pushwalls_at + 2 + 5 * i;
        const auto& w = s.pushwalls[i];
        require(w.object_slot == base.pushwalls[i].object_slot &&
                w.object_slot < l.object_count, path, offset,
                "pushwall owns its pristine active object slot");
    }
    for (std::size_t i = 0; i < l.vector_count; ++i) {
        offset = vectors_at + 2 + 19 * i;
        const auto& v = l.vectors[i]; const auto& original = b.vectors[i];
        require(v.image_id == original.image_id && v.subtype == original.subtype &&
                v.direction == original.direction && v.direction < 4, path, offset,
                "vector identity and direction match pristine MAP");
    }
    for (std::size_t i = 0; i < l.object_count; ++i) {
        offset = objects_at + 2 + 23 * i;
        const auto& o = l.objects[i]; const auto& original = b.objects[i];
        require(o.subtype == original.subtype && o.enemy_index == original.enemy_index &&
                o.cell_x < 64 && o.cell_y < 64, path, offset,
                "object identity and cell coordinates are usable");
        if (original.flags & 8u)
            require(o.enemy_index < l.guard_count && l.guards[o.enemy_index].object_slot == i,
                    path, offset, "guard and actor have reciprocal source links");
    }
    for (std::size_t i = 0; i < l.guard_count; ++i) {
        offset = guards_at + 2 + 25 * i;
        const auto& g = l.guards[i];
        require(g.object_slot == b.guards[i].object_slot && g.object_slot < l.object_count &&
                l.objects[g.object_slot].enemy_index == i, path, offset,
                "guard object slot matches pristine reciprocal link");
        require(g.state <= 21 && g.state != 17, path, offset + 11,
                "active guard state has a supported source dispatcher");
        require(g.facing < 8 && g.resolved_octant <= 8 && g.strategy <= 4 &&
                l.objects[g.object_slot].class_id >= 8 &&
                l.objects[g.object_slot].class_id <= 0x21, path, offset,
                "guard strategy, actor class and octants have source dispatchers");
        if ((g.state >= 2 && g.state <= 8) || (g.state >= 12 && g.state <= 16)) {
            const auto& ep = assets.episodes[s.episode - 1];
            const auto image = l.objects[g.object_slot].image_id;
            const auto& seq = ep.images.sequences[256u + image];
            require(seq.offset && seq.descriptor_offset <= ep.image_bytes.size() &&
                    ep.image_bytes.size() - seq.descriptor_offset >= 90, path, offset,
                    "active guard image has its extended IMG descriptor");
        }
    }
    for (std::size_t i = 0; i < s.projectiles.size(); ++i) {
        const auto& projectile = s.projectiles[i];
        require(projectile.state <= 2, path, projectiles_at + kProjectileRecordSize * i,
                "projectile state has a dispatcher");
        require(valid_projectile_lift(projectile), path,
                projectiles_at + kProjectileRecordSize * i + 21,
                "projectile lift is zero for free slots or 5..20 for active slots");
        if (projectile.state != 0) {
            const auto& seq = assets.episodes[s.episode - 1].images.sequences[
                256u + projectile.image_id];
            require(seq.offset && seq.frame_count && seq.delay_ms, path,
                    projectiles_at + kProjectileRecordSize * i + 1,
                    "active projectile image has IMG animation metadata");
        }
    }
    const auto scene_at = kHeaderSize + 69989 + 2 * s.automap_speckle_count;
    for (std::size_t dir = 0; dir < 4; ++dir)
        require(s.scene.vector_cursors[dir] <= l.vector_count_by_direction[dir],
                path, scene_at + 2 * dir,
                "retained vector cursor fits pristine direction list");
    require(s.scene.object_cursor_x <= l.object_count, path, scene_at + 8,
            "retained X object cursor fits the active list");
    require(s.scene.object_cursor_y <= l.object_count, path, scene_at + 10,
            "retained Y object cursor fits the active list");
    for (unsigned axis = 0; axis < 2; ++axis) {
        std::array<bool, kMaxObjects> seen{};
        auto& order = axis == 0 ? l.object_list_x : l.object_list_y;
        for (std::size_t rank = 0; rank < l.object_count; ++rank) {
            const auto slot = order[rank];
            require(slot < l.object_count && !seen[slot], path,
                    (axis == 0 ? x_order_at : y_order_at) + 2 * rank,
                    "object order is an exact permutation of active slots");
            seen[slot] = true;
            auto& object = s.level.objects[slot];
            if (axis == 0) object.x_order_index = static_cast<std::uint16_t>(rank);
            else object.y_order_index = static_cast<std::uint16_t>(rank);
        }
    }
    const auto& p = s.player;
    require(p.cell_x < 64 && p.cell_y < 64 && p.world_x >= 0 && p.world_y >= 0 &&
            p.world_x < 4096 && p.world_y < 4096 &&
            p.world_x / 64 == p.cell_x && p.world_y / 64 == p.cell_y &&
            p.heading < 360, path, kHeaderSize + 83,
            "player position and heading index MAP grid");
    require((p.weapon < 4 || p.weapon == 0xff) &&
            (s.draw_weapon < 4 || s.draw_weapon == 0xff) &&
            p.weapon_animation_state <= 2 &&
            (p.weapon == 0xff || p.weapon_animation_frame < 10) &&
            (p.weapon_animation_state != 2 ||
             (p.weapon_animation_frame != 0 && p.queued_weapon < 4)),
            path, kHeaderSize + 83 + 6,
            "weapon and animation indices have source dispatchers");
    detail::set_heading(base, p.heading);
    const auto& h = base.player;
    require(p.octant == h.octant && p.nearest_sector == h.nearest_sector &&
            p.octant_mask == h.octant_mask && p.sine == h.sine &&
            p.cosine == h.cosine && p.major_axis == h.major_axis &&
            p.bresenham_minor == h.bresenham_minor &&
            p.bresenham_diagonal == h.bresenham_diagonal,
            path, kHeaderSize + 83 + 8,
            "pure heading derivatives match native movement tables");
    // p.bresenham_error is deliberately NOT replaced by the reset heading error.
}

std::size_t payload_length(const GameState& s) noexcept {
    return kMinimumPayload + 2 * s.automap_speckle_count + 10 * s.door_count +
        11 * s.panel_count + 5 * s.pushwall_count + 27 * s.level.object_count +
        25 * s.level.guard_count + 19 * s.level.vector_count;
}
} // namespace

std::vector<std::byte> encode_save(const GameState& state, const Assets& assets,
                                   std::uint32_t save_anchor_ms) {
    eligible(state, assets, "N3DSAVE", 0);
    Writer io;
    io.bytes.reserve(kHeaderSize + payload_length(state));
    for (auto c : kMagic) io.u8(c);
    io.u32(kNativeSaveVersion);
    for (auto c : assets.required_content_identity) io.u8(c);
    io.u32(static_cast<std::uint32_t>(payload_length(state)));
    io.u32(0u); // u64 length high word, bounded by the frozen capacity.
    io.u32(0u); // CRC filled after the payload is complete.
    auto anchor = save_anchor_ms;
    session(io, state, anchor);
    player(io, state);
    world(io, state);
    frame_scene(io, state);
    records(io, state, assets);
    require(io.bytes.size() == kHeaderSize + payload_length(state), "N3DSAVE", io.position(),
            "all sixteen groups equal the frozen payload-length formula");
    const auto checksum = crc32(std::span<const std::byte>(io.bytes).subspan(kHeaderSize));
    for (unsigned i = 0; i < 4; ++i)
        io.bytes[52 + i] = std::byte{static_cast<std::uint8_t>(checksum >> (8 * i))};
    return io.bytes;
}

DecodedSave decode_save(std::span<const std::byte> bytes, const Assets& assets,
                        std::string_view path) {
    require(bytes.size() >= kHeaderSize, path, bytes.size(), "56-byte save header available");
    ByteReader header(path, bytes);
    for (auto expected : kMagic) {
        const auto at = header.offset();
        require(header.u8() == expected, path, at, "N3DSAVE magic matches");
    }
    require(header.u32le() == kNativeSaveVersion, path, 8,
            "native save version is 2; v1 is unsupported");
    require(assets.required_content_identity == compute_required_content_identity(),
            path, 12, "required Windows content identity is validated");
    for (auto expected : assets.required_content_identity) {
        const auto at = header.offset();
        require(header.u8() == expected, path, at,
                "save identity matches the exact required Windows content set");
    }
    const auto declared = header.u64le();
    const auto stored_crc = header.u32le();
    require(declared >= kMinimumPayload && declared <= kMaximumPayload &&
            declared == bytes.size() - kHeaderSize, path, 44,
            "payload length is exact and within frozen record capacities");
    const auto payload = bytes.subspan(kHeaderSize);
    require(crc32(payload) == stored_crc, path, 52, "IEEE CRC-32 matches payload");
    auto result = std::make_unique<GameState>();
    Reader io(path, bytes);
    std::uint32_t anchor = 0;
    session(io, *result, anchor);
    require(result->episode >= 1 && result->episode <= assets.episodes.size() &&
            result->level_index < assets.episodes[result->episode - 1].map.level_count,
            path, kHeaderSize + 1, "episode and level index reference identified MAP");
    auto pristine = std::make_unique<GameState>();
    pristine->config = result->config;
    detail::start_level(*pristine, assets, result->episode, result->level_index);
    pristine->audio.clear();
    result->level = pristine->level;
    result->doors = pristine->doors;
    result->door_count = pristine->door_count;
    result->panels = pristine->panels;
    result->panel_count = pristine->panel_count;
    result->pushwalls = pristine->pushwalls;
    result->pushwall_count = pristine->pushwall_count;
    result->ega_map = compute_ega_palette_map(assets);
    player(io, *result);
    world(io, *result);
    frame_scene(io, *result);
    records(io, *result, assets, pristine.get());
    require(io.input.empty(), path, io.position(), "payload has no trailing bytes");
    require(payload.size() == payload_length(*result), path, 44,
            "all sixteen groups match frozen payload-length formula");
    validate_level(*result, *pristine, assets, path, kHeaderSize + 141);
    result->level_loaded = true;
    result->player_life_state = 1;
    result->clocks.calibrated = true;
    result->clocks.calibration_started = false;
    result->composed_ready = true;
    result->scene.ready = false;
    result->scene.shaded_palette = &result->palette;
    result->scene.view.palette = &result->palette;
    result->audio.clear();
    eligible(*result, assets, path, kHeaderSize);
    if (result->config.music_enabled)
        result->audio.push({AudioCommandKind::music, result->current_tune, 0, 0});
    return {std::move(result), anchor};
}

} // namespace n3d
