#pragma once

#include "n3d/game.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace n3d {

// Capture only at a completed host dispatch after settings/audio effects have
// been consumed. The anchor is the virtual time supplied to that dispatch.
[[nodiscard]] std::vector<std::byte> encode_save(const GameState& state,
                                                 const Assets& assets,
                                                 std::uint32_t save_anchor_ms);

struct DecodedSave final {
    std::unique_ptr<GameState> state;
    std::uint32_t save_anchor_ms = 0;
};

// Caller owns atomic publication, first-frame presentation and physical-input
// release/rearm. Failed decoding never touches the caller's live GameState.
[[nodiscard]] DecodedSave decode_save(std::span<const std::byte> bytes,
                                      const Assets& assets,
                                      std::string_view path = "N3DSAVE");

} // namespace n3d
