#pragma once

#include "n3d/assets.hpp"
#include "n3d/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace n3d {

// Page offsets refer to Assets::bsf.blocks[1]; the decrypted text remains owned
// by Assets. Construct once per asset set and retain this object across repaints.
class HelpPager final {
public:
    static constexpr std::size_t kPageCount = 22;

    explicit HelpPager(const Assets& assets);

    [[nodiscard]] std::size_t page_index() const noexcept { return page_index_; }
    void reset() noexcept { page_index_ = 0; }
    [[nodiscard]] bool next() noexcept;
    [[nodiscard]] bool previous() noexcept;

private:
    friend void render_help(const HelpPager&, const Assets&, std::uint8_t, IndexedFrame&);

    std::array<std::size_t, kPageCount> page_starts_{};
    std::array<std::size_t, kPageCount> page_ends_{};
    std::array<std::uint8_t, 16> ega_map_{};
    std::size_t page_index_ = 0;
};

// Episode is numbered 1..3. The page artwork, fonts, IMG pixels and palette
// are read from the supplied Assets; this function performs no I/O.
void render_help(const HelpPager& pager, const Assets& assets,
                 std::uint8_t episode, IndexedFrame& frame);

// 1018:1cae→054c draws this transient centred banner before order.txt's
// modal Warning; caller repaints the same Help page after dismissal.
void render_help_order_banner(const Assets& assets, IndexedFrame& frame);

} // namespace n3d
