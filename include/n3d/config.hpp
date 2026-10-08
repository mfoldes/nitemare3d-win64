#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace n3d {

// Windows config.sav format:
// - exact 20-byte little-endian payload with no header/checksum
// - bytes +2..+3 are preserved as opaque unknown state rather than normalized
// - the raw 20-byte layout is round-tripped exactly; validation is separate and
//   intentionally narrow for runtime safety.
struct Config {
    static constexpr std::size_t kSerializedSize = 20;

    static constexpr std::uint16_t kDefaultViewportWidth = 304u;
    static constexpr std::uint16_t kDefaultOutputWidth = 320u;
    static constexpr std::uint16_t kDefaultOutputHeight = 200u;
    static constexpr std::uint8_t kDefaultMouseSensitivity = 50u;
    static constexpr std::uint8_t kDefaultJoystickDivisor = 50u;
    static constexpr std::uint8_t kDefaultMusicVolume = 60u;
    static constexpr std::uint8_t kDefaultEffectsVolume = 70u;
    static constexpr std::uint8_t kDefaultMouseEnabled = 0u;
    static constexpr std::uint8_t kDefaultMusicEnabled = 1u;
    static constexpr std::uint8_t kDefaultEffectsEnabled = 1u;
    static constexpr std::uint8_t kDefaultJoystickEnabled = 0u;

    std::uint16_t viewport_width = kDefaultViewportWidth;
    std::array<std::uint8_t, 2> unknown_bytes{0u, 0u};
    std::uint16_t output_width = kDefaultOutputWidth;
    std::uint16_t output_height = kDefaultOutputHeight;
    std::uint8_t mouse_sensitivity = kDefaultMouseSensitivity;
    std::uint8_t joystick_divisor = kDefaultJoystickDivisor;
    std::uint8_t music_volume = kDefaultMusicVolume;
    std::uint8_t effects_volume = kDefaultEffectsVolume;
    std::uint8_t mouse_enabled = kDefaultMouseEnabled;
    std::uint8_t music_enabled = kDefaultMusicEnabled;
    std::uint8_t effects_enabled = kDefaultEffectsEnabled;
    std::uint8_t joystick_enabled = kDefaultJoystickEnabled;
    std::uint8_t cheat_omniscient = 0u;
    std::uint8_t cheat_omnipotent = 0u;
    std::uint8_t cheat_omnifarious = 0u;
    std::uint8_t cheat_omnificent = 0u;

    static Config decode(std::span<const std::byte> bytes,
                         std::string_view path = "config.sav");

    void validate(std::string_view path = "config.sav",
                  std::size_t offset = 0) const;

    [[nodiscard]] std::array<std::byte, kSerializedSize> encode() const;
};

} // namespace n3d
