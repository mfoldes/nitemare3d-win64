#include "n3d/config.hpp"

#include "n3d/binary_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace n3d {
namespace {

constexpr std::size_t kViewportWidthOffset = 0u;
constexpr std::size_t kOutputWidthOffset = 4u;
constexpr std::size_t kMouseSensitivityOffset = 8u;
constexpr std::size_t kJoystickDivisorOffset = 9u;
constexpr std::size_t kMusicVolumeOffset = 10u;
constexpr std::size_t kEffectsVolumeOffset = 11u;

void require_slider_range(std::string_view path, std::size_t offset,
                          std::string_view field_name, std::uint8_t value) {
    if (value > 100u) {
        throw DataError(std::string(path), offset,
                        std::string(field_name) + " is within the original 0..100 slider range");
    }
}

} // namespace

Config Config::decode(std::span<const std::byte> bytes, std::string_view path) {
    if (bytes.size() != kSerializedSize) {
        throw DataError(std::string(path), bytes.size(),
                        "config.sav payload is exactly 20 bytes");
    }

    ByteReader reader(path, bytes);
    Config config{};
    config.viewport_width = reader.u16le("viewport width available");
    config.unknown_bytes[0] = reader.u8("opaque +2 byte preserved");
    config.unknown_bytes[1] = reader.u8("opaque +3 byte preserved");
    config.output_width = reader.u16le("output width available");
    config.output_height = reader.u16le("output height available");
    config.mouse_sensitivity = reader.u8("mouse sensitivity available");
    config.joystick_divisor = reader.u8("joystick divisor available");
    config.music_volume = reader.u8("music volume available");
    config.effects_volume = reader.u8("effects volume available");
    config.mouse_enabled = reader.u8("mouse enabled flag available");
    config.music_enabled = reader.u8("music enabled flag available");
    config.effects_enabled = reader.u8("effects enabled flag available");
    config.joystick_enabled = reader.u8("joystick enabled flag available");
    config.cheat_omniscient = reader.u8("cheat Omniscient value available");
    config.cheat_omnipotent = reader.u8("cheat Omnipotent value available");
    config.cheat_omnifarious = reader.u8("cheat Omnifarious value available");
    config.cheat_omnificent = reader.u8("cheat Omnificent value available");

    return config;
}

void Config::validate(std::string_view path, std::size_t offset) const {
    const auto base = offset;

    if (viewport_width == 0u || viewport_width > 320u) {
        throw DataError(std::string(path), base + kViewportWidthOffset,
                        "viewport width is nonzero and within the original 320-column frame");
    }
    if ((viewport_width & 7u) != 0u) {
        throw DataError(std::string(path), base + kViewportWidthOffset,
                        "viewport width is aligned to the original eight-pixel renderer grid");
    }
    if (output_width == 0u || output_height == 0u) {
        throw DataError(std::string(path), base + kOutputWidthOffset,
                        "output dimensions are nonzero");
    }

    // 1018:2e12 clamps every hardware slider to 0..100; arbitrary enable
    // flag bytes still retain their original raw values on decode/encode.
    if (joystick_enabled != 0u && joystick_divisor == 0u) {
        throw DataError(std::string(path), base + kJoystickDivisorOffset,
                        "joystick divisor is nonzero when the joystick is enabled");
    }

    require_slider_range(path, base + kMouseSensitivityOffset,
                         "mouse sensitivity", mouse_sensitivity);
    require_slider_range(path, base + kJoystickDivisorOffset,
                         "joystick divisor", joystick_divisor);
    require_slider_range(path, base + kMusicVolumeOffset,
                         "music volume", music_volume);
    require_slider_range(path, base + kEffectsVolumeOffset,
                         "effects volume", effects_volume);
}

std::array<std::byte, Config::kSerializedSize> Config::encode() const {
    std::array<std::byte, kSerializedSize> bytes{};
    std::size_t index = 0;

    const auto append_u16 = [&bytes, &index](std::uint16_t value) {
        bytes[index++] = std::byte{static_cast<std::uint8_t>(value & 0xffu)};
        bytes[index++] = std::byte{static_cast<std::uint8_t>((value >> 8u) & 0xffu)};
    };
    const auto append_u8 = [&bytes, &index](std::uint8_t value) {
        bytes[index++] = std::byte{static_cast<std::uint8_t>(value)};
    };

    append_u16(viewport_width);
    append_u8(unknown_bytes[0]);
    append_u8(unknown_bytes[1]);
    append_u16(output_width);
    append_u16(output_height);
    append_u8(mouse_sensitivity);
    append_u8(joystick_divisor);
    append_u8(music_volume);
    append_u8(effects_volume);
    append_u8(mouse_enabled);
    append_u8(music_enabled);
    append_u8(effects_enabled);
    append_u8(joystick_enabled);
    append_u8(cheat_omniscient);
    append_u8(cheat_omnipotent);
    append_u8(cheat_omnifarious);
    append_u8(cheat_omnificent);

    return bytes;
}

} // namespace n3d
