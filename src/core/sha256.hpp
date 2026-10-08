#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace n3d::detail {

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::byte> bytes);

} // namespace n3d::detail
