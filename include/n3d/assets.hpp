#pragma once

#include "n3d/binary_reader.hpp"
#include "n3d/frame.hpp"
#include "n3d/sound.hpp"
#include "n3d/recordings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace n3d {

struct MapStoreMetadata final {
    std::uint16_t level_count;
    std::size_t header_offset;
    std::size_t header_size;
    std::size_t levels_offset;
    std::size_t level_size;
    std::array<std::uint8_t, 256> wall_class_by_id{};
    std::array<std::uint8_t, 256> object_class_by_id{};
};

MapStoreMetadata parse_map_store(std::string_view path, std::span<const std::byte> bytes);
struct ImgRecordMetadata final {
    std::size_t offset;
    std::uint8_t width;
    std::uint8_t height;
    std::array<std::uint8_t, 4> stamp_bytes;
    std::size_t pixels_offset;
    std::size_t pixels_size;
};

struct ImgSequenceMetadata final {
    std::size_t offset = 0; // Zero means no record for this image ID.
    std::size_t descriptor_offset = 0;
    std::size_t first_record_index = 0; // Valid only when offset is nonzero.
    std::uint16_t delay_ms = 0;
    std::uint8_t frame_count = 0;
    std::uint8_t extended_state_flag = 0;
};

struct ImgArchiveMetadata final {
    std::size_t directory_size = 0;
    std::array<ImgSequenceMetadata, 512> sequences{};
    std::size_t active_id_count = 0;
    std::size_t distinct_sequence_count = 0;
    std::vector<ImgRecordMetadata> records;
};

ImgArchiveMetadata parse_img_archive(std::string_view path, std::span<const std::byte> bytes);


struct CatalogRecord final {
    std::uint16_t id;
    std::string code;
    std::string resource;
    std::string type;
    std::string description;
};
void validate_map_viewer_catalogs(std::string_view map_path,
                                  const MapStoreMetadata& map,
                                  std::string_view wall_path,
                                  std::span<const CatalogRecord> walls,
                                  std::string_view object_path,
                                  std::span<const CatalogRecord> objects);

std::vector<CatalogRecord> parse_definition_catalog(
    std::string_view path, std::span<const std::byte> bytes);

struct PcxImage final {
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t bytes_per_line;
    std::vector<std::byte> pixels;
    std::array<std::array<std::uint8_t, 3>, 256> palette;
};

PcxImage parse_pcx_image(std::string_view path, std::span<const std::byte> bytes);

struct UifGlyph final {
    std::uint8_t rows = 0;
    std::uint8_t width = 0;
    std::size_t bitmap_offset = 0; // Into the owning Assets::uif_bytes.
    std::size_t bitmap_size = 0;
};

struct UifFont final {
    std::array<std::uint8_t, 2> header_bytes{};
    std::array<UifGlyph, 127> glyphs{};
};

struct UifDirectoryEntry final {
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct UifImage final {
    std::size_t directory_index = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
    PcxImage image;
};

struct UifArchiveMetadata final {
    std::size_t directory_size = 0;
    std::array<UifDirectoryEntry, 32> directory{};
    std::array<UifFont, 3> fonts{};
    std::array<UifImage, 14> images{};
};

UifArchiveMetadata parse_uif_archive(std::string_view path,
                                     std::span<const std::byte> bytes);


struct FliHeader final {
    std::uint32_t file_size;
    std::uint16_t frames;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t depth;
    std::uint16_t flags;
    std::uint16_t speed;
    std::uint32_t frame1_offset;
    std::uint32_t frame2_offset;
};

FliHeader parse_fli_header(std::string_view path, std::span<const std::byte> bytes);
struct FliStreamMetadata final {
    FliHeader header;
    std::size_t frame_records;
    std::size_t palette_chunks;
    std::size_t lc_chunks;
    std::size_t brun_chunks;
    std::size_t one_byte_frame_end_adjustments;
    std::size_t one_byte_chunk_padding;
};

// The view and palette are valid only during the callback; palette channels retain
// their FLI COLOR_64 six-bit values until presentation scales them.
using FliFrameSink = void (*)(void* user, std::size_t frame_index,
                              std::span<const std::byte> pixels,
                              const std::array<std::array<std::uint8_t, 3>, 256>& palette);
FliStreamMetadata validate_fli_stream(std::string_view path,
                                      std::span<const std::byte> bytes,
                                      FliFrameSink on_frame = nullptr, void* user = nullptr);


std::vector<std::byte> read_file(const std::filesystem::path& path);

struct EpisodeAssets final {
    std::vector<std::byte> map_bytes;
    std::vector<std::byte> wall_bytes;
    std::vector<std::byte> object_bytes;
    std::vector<std::byte> image_bytes;
    MapStoreMetadata map;
    std::vector<CatalogRecord> walls;
    std::vector<CatalogRecord> objects;
    ImgArchiveMetadata images;
};

struct Assets final {
    std::filesystem::path directory;
    std::array<EpisodeAssets, 3> episodes;
    std::vector<std::byte> uif_bytes;
    UifArchiveMetadata uif;
    std::vector<std::byte> palette_bytes;
    PcxImage palette;
    std::vector<std::byte> sound_bytes;
    SoundArchiveMetadata sound;
    std::array<std::vector<std::byte>, 3> demo_bytes;
    std::array<DemoRecording, 3> demos;
    std::vector<std::byte> bsf_bytes;
    BsfArchive bsf;
    std::size_t help_image_reference_count = 0;
    std::vector<std::byte> ending_bytes;
    FliStreamMetadata ending;
    // Zero until a complete, fingerprint-validated load succeeds.
    std::array<std::uint8_t, 32> required_content_identity{};
};

// Holds one mutable decoded frame. Assets (including ending_bytes) must outlive
// the player and remain unchanged during playback. The FLI's COLOR_64 palette
// is scaled from six-bit channels to RGB8 here, unlike the inspector callback.
class FliPlayer final {
public:
    explicit FliPlayer(const Assets& assets);
    FliPlayer(const FliPlayer&) = delete;
    FliPlayer& operator=(const FliPlayer&) = delete;

    // Decodes exactly one declared frame. False means the declared frames are
    // exhausted; an extra terminal file record is not a presentation frame.
    [[nodiscard]] bool next_frame();
    void reset() noexcept;

    // Throws before a successful next_frame(), including after reset(). The
    // returned reference is overwritten by the next decode or reset.
    [[nodiscard]] const IndexedFrame& frame() const;
    [[nodiscard]] std::size_t decoded_frames() const noexcept { return decoded_frames_; }
    [[nodiscard]] std::size_t frame_count() const noexcept { return header_.frames; }
    // FLI speed units are 15 ms, rather than an FPS denominator.
    [[nodiscard]] std::uint32_t frame_interval_ms() const noexcept {
        return static_cast<std::uint32_t>(header_.speed) * 15u;
    }
    [[nodiscard]] bool finished() const noexcept { return decoded_frames_ == header_.frames; }

private:
    const Assets& assets_;
    FliHeader header_;
    IndexedFrame frame_{};
    std::size_t next_offset_ = 128;
    std::size_t decoded_frames_ = 0;
};

// Native save identity of the exact required Windows file fingerprints (not paths).
[[nodiscard]] std::array<std::uint8_t, 32> compute_required_content_identity();

Assets load_assets(const std::filesystem::path& directory);

} // namespace n3d
