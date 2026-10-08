#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace n3d {

struct Rgb8 final {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct IndexedFrame final {
    static constexpr std::size_t kWidth = 320;
    static constexpr std::size_t kHeight = 200;
    static constexpr std::size_t kPixelCount = kWidth * kHeight;
    static constexpr std::size_t kPaletteCount = 256;

    std::array<std::uint8_t, kPixelCount> pixels{};
    std::array<Rgb8, kPaletteCount> palette{};

    void clear(std::uint8_t fill_index = 0) {
        pixels.fill(fill_index);
    }
};

} // namespace n3d
