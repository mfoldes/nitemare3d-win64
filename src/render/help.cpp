#include "n3d/help.hpp"

#include "n3d/ui.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace n3d {
namespace {

constexpr std::string_view kBsfPath = "nite3d.bsf";
constexpr std::size_t kHelpBlock = 1;
constexpr std::uint8_t kFont = 1;
constexpr std::size_t kWidth = IndexedFrame::kWidth;
constexpr std::size_t kHeight = IndexedFrame::kHeight;
constexpr std::uint8_t kTransparentImageIndex = 0x29;

std::uint8_t byte_at(const std::vector<std::byte>& text, std::size_t offset) {
    return std::to_integer<std::uint8_t>(text[offset]);
}

struct Command final {
    std::uint8_t type;
    std::uint16_t value;
    std::size_t end;
};

Command parse_command(const std::vector<std::byte>& text, std::size_t pos,
                      std::size_t limit, std::size_t file_offset) {
    if (pos + 2 >= limit) {
        throw DataError(std::string(kBsfPath), file_offset + pos,
                        "help command has a type and closing percent within its page");
    }
    const auto type = byte_at(text, pos + 1);
    if (type == 'd') {
        if (byte_at(text, pos + 2) != '%') {
            throw DataError(std::string(kBsfPath), file_offset + pos,
                            "help default-colour command is %d%");
        }
        return {type, 0, pos + 3};
    }
    if (type != 'C' && type != 'o') {
        throw DataError(std::string(kBsfPath), file_offset + pos,
                        "help command is a known colour or image token");
    }
    std::size_t cursor = pos + 2;
    if (cursor >= limit || byte_at(text, cursor) < '0' || byte_at(text, cursor) > '9') {
        throw DataError(std::string(kBsfPath), file_offset + pos,
                        "help command has decimal digits");
    }
    std::uint32_t value = 0;
    for (; cursor < limit; ++cursor) {
        const auto ch = byte_at(text, cursor);
        if (ch < '0' || ch > '9') break;
        const auto digit = static_cast<std::uint32_t>(ch - '0');
        if (value > (static_cast<std::uint32_t>(type == 'o' ? 255 : 65535) - digit) / 10) {
            throw DataError(std::string(kBsfPath), file_offset + pos,
                            "help command value fits the Windows token operand");
        }
        value = value * 10 + digit;
    }
    if (cursor >= limit || byte_at(text, cursor) != '%') {
        throw DataError(std::string(kBsfPath), file_offset + pos,
                        "help command ends with a percent on this page");
    }
    return {type, static_cast<std::uint16_t>(value), cursor + 1};
}

void fill_box(IndexedFrame& frame, int x, int y, int width, int height,
              std::uint8_t colour) {
    for (int row = y; row < y + height; ++row) {
        std::fill_n(frame.pixels.begin() + static_cast<std::size_t>(row) * kWidth + x,
                    width, colour);
    }
}

void blit_help_image(const EpisodeAssets& episode, std::uint16_t id,
                     int x, int y, IndexedFrame& frame) {
    const auto& sequence = episode.images.sequences[256u + id];
    if (sequence.offset == 0 || sequence.frame_count == 0 ||
        sequence.first_record_index >= episode.images.records.size()) {
        throw DataError("IMG", sequence.descriptor_offset,
                        "help object image has a first frame in the active episode");
    }
    const auto& image = episode.images.records[sequence.first_record_index];
    if (image.width == 0 || image.height == 0 ||
        image.pixels_size != static_cast<std::size_t>(image.width) * image.height ||
        image.pixels_offset > episode.image_bytes.size() ||
        image.pixels_size > episode.image_bytes.size() - image.pixels_offset) {
        throw DataError("IMG", image.offset, "help object image has bounded column-major pixels");
    }
    const auto& pixels = episode.image_bytes;
    // IMG records are column-major; 0x29 is a transparent source index, not
    // a palette remapping. The original pager does not advance its text cursor.
    for (int column = 0; column < image.width; ++column) {
        const int px = x + column;
        if (px < 0 || px >= static_cast<int>(kWidth)) continue;
        const auto source = image.pixels_offset + static_cast<std::size_t>(column) * image.height;
        for (int row = 0; row < image.height; ++row) {
            const int py = y + row;
            if (py < 0 || py >= static_cast<int>(kHeight)) continue;
            const auto colour = std::to_integer<std::uint8_t>(pixels[source + row]);
            if (colour != kTransparentImageIndex) frame.pixels[static_cast<std::size_t>(py) * kWidth + px] = colour;
        }
    }
}

void draw_page_counter(const HelpPager& pager, const Assets& assets,
                       IndexedFrame& frame, std::uint8_t colour) {
    char label[24]{};
    auto* const end = label + sizeof(label);
    auto current = std::to_chars(label, end, pager.page_index() + 1);
    *current.ptr++ = ' ';
    *current.ptr++ = 'o';
    *current.ptr++ = 'f';
    *current.ptr++ = ' ';
    const auto total = std::to_chars(current.ptr, end, HelpPager::kPageCount);
    // 1018:17d8 clears a 55x10 box at (255,188), then prints "%d of %d".
    fill_box(frame, 255, 188, 55, 10, 0xBA);
    draw_indexed_text(assets, frame, kFont,
                      {label, static_cast<std::size_t>(total.ptr - label)},
                      257, 190, colour);
}

} // namespace

HelpPager::HelpPager(const Assets& assets)
    : ega_map_(compute_ega_palette_map(assets)) {
    const auto& text = assets.bsf.blocks[kHelpBlock];
    const auto origin = static_cast<std::size_t>(assets.bsf.header.block_offsets[kHelpBlock]);
    if (text.empty()) {
        throw DataError(std::string(kBsfPath), origin, "help block contains its first page");
    }
    page_starts_[0] = 0;
    std::size_t page = 0;
    bool line_start = true;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto ch = byte_at(text, pos);
        if (ch == '#' && line_start) {
            if (page + 1 >= kPageCount || pos == page_starts_[page]) {
                throw DataError(std::string(kBsfPath), origin + pos,
                                "help has exactly 22 nonempty marker-delimited pages");
            }
            page_ends_[page++] = pos;
            page_starts_[page] = ++pos;
            line_start = false;
            continue;
        }
        if (ch == '%') {
            pos = parse_command(text, pos, text.size(), origin).end;
            line_start = false;
            continue;
        }
        if (ch == '\r' && (pos + 1 >= text.size() || byte_at(text, pos + 1) != '\n')) {
            throw DataError(std::string(kBsfPath), origin + pos,
                            "help carriage return belongs to a CRLF line ending");
        }
        if (ch == '\n' && (pos == 0 || byte_at(text, pos - 1) != '\r')) {
            throw DataError(std::string(kBsfPath), origin + pos,
                            "help line ending is CRLF");
        }
        if (ch == 0 || ch > 127) {
            throw DataError(std::string(kBsfPath), origin + pos,
                            "help text is nonzero seven-bit ASCII");
        }
        line_start = ch == '\n';
        ++pos;
    }
    if (page + 1 != kPageCount || page_starts_[page] == text.size()) {
        throw DataError(std::string(kBsfPath), origin + text.size(),
                        "help has 21 delimiters and a nonempty final page");
    }
    page_ends_[page] = text.size();
}

bool HelpPager::next() noexcept {
    if (page_index_ + 1 >= kPageCount) return false;
    ++page_index_;
    return true;
}

bool HelpPager::previous() noexcept {
    if (page_index_ == 0) return false;
    --page_index_;
    return true;
}

void render_help(const HelpPager& pager, const Assets& assets,
                 std::uint8_t episode, IndexedFrame& frame) {
    if (episode == 0 || episode > assets.episodes.size()) {
        throw DataError("IMG", 0, "help episode is numbered 1 through 3");
    }
    const auto& art = assets.uif.images[16 - 3];
    if (art.directory_index != 16 || art.image.width != kWidth ||
        art.image.height != kHeight || art.image.pixels.size() != IndexedFrame::kPixelCount) {
        throw DataError("uif.dat", art.offset, "UIF16 help artwork fills 320x200");
    }
    for (std::size_t i = 0; i < frame.palette.size(); ++i) {
        const auto& rgb = assets.palette.palette[i];
        frame.palette[i] = Rgb8{rgb[0], rgb[1], rgb[2]};
    }
    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
        frame.pixels[i] = std::to_integer<std::uint8_t>(art.image.pixels[i]);
    }
    fill_box(frame, 10, 5, 300, 180, 0xBA);
    draw_page_counter(pager, assets, frame, pager.ega_map_[8]);

    const auto& text = assets.bsf.blocks[kHelpBlock];
    const auto origin = static_cast<std::size_t>(assets.bsf.header.block_offsets[kHelpBlock]);
    const auto start = pager.page_starts_[pager.page_index_];
    const auto end = pager.page_ends_[pager.page_index_];
    if (start > end || end > text.size()) {
        throw DataError(std::string(kBsfPath), origin + start,
                        "help page offsets stay inside the decrypted block");
    }
    const auto& images = assets.episodes[episode - 1];
    std::uint8_t colour = pager.ega_map_[8];
    int x = 12;
    int y = 7;
    unsigned column = 0;
    for (std::size_t pos = start; pos < end;) {
        const auto ch = byte_at(text, pos);
        if (ch == '%') {
            const auto command = parse_command(text, pos, end, origin);
            if (command.type == 'C') colour = static_cast<std::uint8_t>(command.value);
            else if (command.type == 'd') colour = pager.ega_map_[8];
            else blit_help_image(images, command.value, x, y, frame);
            column += static_cast<unsigned>(command.end - pos);
            pos = command.end;
        } else if (ch == '\r') {
            ++pos; // CRLF is one logical line ending; LF advances 11 pixels.
        } else if (ch == '\n') {
            x = 12;
            y += 11;
            column = 0;
            ++pos;
        } else if (ch == '\t') {
            column += 4u - (column % 4u);
            x = static_cast<int>((column + 2u) * 6u);
            ++pos;
        } else if (ch == ' ') {
            x += 6;
            ++column;
            ++pos;
        } else {
            const char character = static_cast<char>(ch);
            const std::string_view glyph(&character, 1);
            draw_indexed_text(assets, frame, kFont, glyph, x, y, colour);
            x += static_cast<int>(indexed_text_width(assets, kFont, glyph));
            ++column;
            ++pos;
        }
    }
}

void render_help_order_banner(const Assets& assets, IndexedFrame& frame) {
    // 1018:054c/032a: offset shadow first, then main EGA8 box. 2aac maps
    // EGA0/8/14 to source palette indices 10/20/170 for the supported set.
    fill_box(frame, 73, 71, 183, 27, 10);
    fill_box(frame, 69, 67, 183, 27, 20);
    constexpr std::string_view text = "Thanks!  Printing order.txt...";
    draw_indexed_text(assets, frame, kFont, text, 78, 76, 10);
    draw_indexed_text(assets, frame, kFont, text, 77, 75, 170);
}

} // namespace n3d
