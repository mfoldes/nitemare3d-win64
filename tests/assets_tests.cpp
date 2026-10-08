#include "n3d/assets.hpp"
#include "../src/core/sha256.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_data_error(Function&& function, std::string_view path, std::size_t offset) {
    try {
        function();
        check(false, "expected DataError");
    } catch (const n3d::DataError& error) {
        check(error.path() == path, "asset error path");
        check(error.offset() == offset, "asset error offset");
    } catch (...) {
        check(false, "wrong asset exception type");
    }
}

void sha256_known_vectors() {
    constexpr std::array<std::uint8_t, 32> empty_expected{
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    };
    const std::vector<std::byte> empty;
    check(n3d::detail::sha256(empty) == empty_expected, "SHA-256 empty input vector");

    constexpr std::array<std::uint8_t, 32> abc_expected{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    const std::vector<std::byte> abc{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
    };
    check(n3d::detail::sha256(abc) == abc_expected, "SHA-256 abc input vector");

    constexpr std::string_view two_block_text =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    constexpr std::array<std::uint8_t, 32> two_block_expected{
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
    };
    std::vector<std::byte> two_block;
    two_block.reserve(two_block_text.size());
    for (const auto character : two_block_text) {
        two_block.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    check(n3d::detail::sha256(two_block) == two_block_expected,
          "SHA-256 two-block padding vector");
}

void map_reader_checks_exact_blocks() {
    constexpr std::size_t header_size = 514;
    constexpr std::size_t level_size = 8192;
    std::vector<std::byte> bytes(header_size + level_size, std::byte{0});
    bytes[0] = std::byte{1};
    bytes[2] = std::byte{0x47};
    bytes[3] = std::byte{0x01};
    bytes[0x102] = std::byte{0x3e};
    bytes[0x201] = std::byte{0x02};

    const auto map = n3d::parse_map_store("MAP.test", bytes);
    check(map.level_count == 1 && map.header_offset == 2 && map.header_size == 512,
          "MAP header metadata");
    check(map.levels_offset == header_size && map.level_size == level_size,
          "MAP level block metadata");
    check(map.wall_class_by_id[0] == 0x47 && map.wall_class_by_id[1] == 0x01,
          "MAP wall class table extraction");
    check(map.object_class_by_id[0] == 0x3e && map.object_class_by_id[255] == 0x02,
          "MAP object class table extraction");

    bytes.pop_back();
    expect_data_error([&] { static_cast<void>(n3d::parse_map_store("MAP.test", bytes)); },
                      "MAP.test", bytes.size());
}

void map_viewer_catalogs_validate_header_references() {
    const std::array<n3d::CatalogRecord, 1> walls{{
        {1, "1010", "SYNTH_WALL.DAT", "wall", "synthetic wall"},
    }};
    const std::array<n3d::CatalogRecord, 1> objects{{
        {1, "0001", "SYNTH_OBJECT.DAT", "hero", "synthetic object"},
    }};
    n3d::MapStoreMetadata episode_one{};
    episode_one.wall_class_by_id[1] = 1;
    episode_one.object_class_by_id[1] = 1;
    n3d::validate_map_viewer_catalogs("MAP.1", episode_one, "WALLS.1", walls,
                                      "OBJECTS.1", objects);

    n3d::MapStoreMetadata episode_two{};
    episode_two.wall_class_by_id[1] = 2;
    episode_two.object_class_by_id[1] = 1;
    const std::array<n3d::CatalogRecord, 1> episode_two_walls{{
        {1, "1010", "SYNTH_WALL2.DAT", "REVWALL", "synthetic episode two wall"},
    }};
    n3d::validate_map_viewer_catalogs("MAP.2", episode_two, "WALLS.2", episode_two_walls,
                                      "OBJECTS.2", objects);

    episode_one.wall_class_by_id[1] = 2;
    expect_data_error(
        [&] { n3d::validate_map_viewer_catalogs("MAP.1", episode_one, "WALLS.1", walls,
                                                "OBJECTS.1", objects); },
        "MAP.1", 3);
    episode_one.wall_class_by_id[1] = 1;

    auto unknown_walls = walls;
    unknown_walls[0].type = "UNKNOWN";
    expect_data_error(
        [&] { n3d::validate_map_viewer_catalogs("MAP.1", episode_one, "WALLS.1", unknown_walls,
                                                "OBJECTS.1", objects); },
        "MAP.1", 3);

    episode_one.wall_class_by_id[2] = 1;
    expect_data_error(
        [&] { n3d::validate_map_viewer_catalogs("MAP.1", episode_one, "WALLS.1", walls,
                                                "OBJECTS.1", objects); },
        "MAP.1", 4);
    episode_one.wall_class_by_id[2] = 0;

    episode_one.wall_class_by_id[9] = 73;
    expect_data_error(
        [&] { n3d::validate_map_viewer_catalogs("MAP.1", episode_one, "WALLS.1", walls,
                                                "OBJECTS.1", objects); },
        "MAP.1", 11);
    episode_one.wall_class_by_id[9] = 0;

    const std::array<n3d::CatalogRecord, 1> out_of_range_walls{{
        {256, "1010", "SYNTH_WALL.DAT", "WALL", "synthetic out-of-range ID"},
    }};
    expect_data_error(
        [&] { n3d::validate_map_viewer_catalogs("MAP.1", episode_one, "WALLS.1",
                                                out_of_range_walls, "OBJECTS.1", objects); },
        "WALLS.1", 0);
}

void catalog_reader_preserves_fields() {
    constexpr std::string_view text =
        "0000 A000 SYNTH0.DAT TYPE_A Placeholder record\r\n"
        "0001 B101 SYNTH1.DAT TYPE_B Synthetic catalog row\r\n";
    std::vector<std::byte> bytes;
    for (const auto character : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    const auto records = n3d::parse_definition_catalog("SYNTHETIC.CAT", bytes);
    check(records.size() == 2, "catalog record count");
    check(records[1].id == 1, "catalog identifier");
    check(records[1].code == "B101", "catalog code");
    check(records[1].resource == "SYNTH1.DAT", "catalog resource");
    check(records[1].type == "TYPE_B", "catalog type");
    check(records[1].description == "Synthetic catalog row", "catalog description");
}

void catalog_reader_accepts_tab_delimiters() {
    constexpr std::string_view text =
        "0001 C321 SYNTH.DAT\t\tTYPE_C\t Synthetic item with spaces\r\n";
    std::vector<std::byte> bytes;
    for (const auto character : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    const auto records = n3d::parse_definition_catalog("SYNTHETIC.CAT", bytes);
    check(records.size() == 1, "tab-delimited catalog record count");
    if (records.size() != 1) {
        return;
    }
    check(records[0].id == 1, "tab-delimited catalog identifier");
    check(records[0].code == "C321", "tab-delimited catalog code");
    check(records[0].resource == "SYNTH.DAT", "tab-delimited catalog resource");
    check(records[0].type == "TYPE_C", "tab-delimited catalog type");
    check(records[0].description == "Synthetic item with spaces",
          "tab-delimited catalog description");
}

void pcx_reader_decodes_rle_and_palette() {
    std::vector<std::byte> bytes(128 + 2 + 1 + 768, std::byte{0});
    bytes[0] = std::byte{0x0a};
    bytes[1] = std::byte{0x05};
    bytes[2] = std::byte{0x01};
    bytes[3] = std::byte{0x08};
    bytes[8] = std::byte{1};
    bytes[10] = std::byte{0};
    bytes[65] = std::byte{1};
    bytes[66] = std::byte{2};
    bytes[128] = std::byte{0xc2};
    bytes[129] = std::byte{0x07};
    bytes[130] = std::byte{0x0c};
    bytes[131] = std::byte{0x11};
    bytes[132] = std::byte{0x22};
    bytes[133] = std::byte{0x33};
    const auto image = n3d::parse_pcx_image("game.pal", bytes);
    check(image.width == 2 && image.height == 1, "PCX dimensions");
    check(image.pixels.size() == 2, "PCX pixel count");
    check(image.pixels[0] == std::byte{7} && image.pixels[1] == std::byte{7}, "PCX RLE pixels");
    check(image.palette[0][0] == 0x11 && image.palette[0][1] == 0x22 &&
              image.palette[0][2] == 0x33,
          "PCX palette");
}

std::vector<std::byte> make_valid_uif_archive() {
    constexpr std::size_t font_payload = 2 + 127 * 3;
    constexpr std::size_t image_payload = 128 + 320 * 200 + 1 + 768;
    constexpr std::size_t directory_size = 0xC0;
    std::vector<std::byte> bytes(directory_size + 3 * font_payload + 14 * image_payload, std::byte{0});

    auto write_entry = [&](std::size_t index, std::size_t offset, std::size_t length) {
        const auto base = index * 6;
        bytes[base] = std::byte{static_cast<unsigned char>(length & 0xffu)};
        bytes[base + 1] = std::byte{static_cast<unsigned char>((length >> 8) & 0xffu)};
        bytes[base + 2] = std::byte{static_cast<unsigned char>(offset & 0xffu)};
        bytes[base + 3] = std::byte{static_cast<unsigned char>((offset >> 8) & 0xffu)};
        bytes[base + 4] = std::byte{static_cast<unsigned char>((offset >> 16) & 0xffu)};
        bytes[base + 5] = std::byte{static_cast<unsigned char>((offset >> 24) & 0xffu)};
    };

    std::size_t cursor = directory_size;
    const std::array<std::uint8_t, 3> font_headers{0x06, 0x0b, 0x09};
    const std::array<std::uint8_t, 3> font_header_lows{0x07, 0x0b, 0x08};
    for (std::size_t font_index = 0; font_index < 3; ++font_index) {
        write_entry(font_index, cursor, font_payload);
        bytes[cursor] = std::byte{font_headers[font_index]};
        bytes[cursor + 1] = std::byte{font_header_lows[font_index]};
        std::size_t glyph_offset = 2;
        for (std::size_t glyph_index = 0; glyph_index < 127; ++glyph_index) {
            bytes[cursor + glyph_offset] = std::byte{1};
            bytes[cursor + glyph_offset + 1] = std::byte{1};
            bytes[cursor + glyph_offset + 2] = std::byte{0x80};
            glyph_offset += 3;
        }
        cursor += font_payload;
    }

    for (std::size_t image_index = 0; image_index < 14; ++image_index) {
        write_entry(3 + image_index, cursor, image_payload);
        const auto base = cursor;
        bytes[base] = std::byte{0x0a};
        bytes[base + 1] = std::byte{0x05};
        bytes[base + 2] = std::byte{0x01};
        bytes[base + 3] = std::byte{0x08};
        bytes[base + 8] = std::byte{0x3f};
        bytes[base + 9] = std::byte{0x01};
        bytes[base + 10] = std::byte{0xc7};
        bytes[base + 11] = std::byte{0};
        bytes[base + 65] = std::byte{1};
        bytes[base + 66] = std::byte{0x40};
        bytes[base + 67] = std::byte{0x01};
        const auto marker = base + image_payload - 768 - 1;
        bytes[marker] = std::byte{0x0c};
        std::size_t offset = base + 128;
        while (offset < marker) {
            bytes[offset] = std::byte{0x80};
            offset += 2;
        }
        for (std::size_t palette_index = 0; palette_index < 256; ++palette_index) {
            bytes[marker + 1 + palette_index * 3] =
                std::byte{static_cast<unsigned char>(palette_index)};
        }
        cursor += image_payload;
    }
    return bytes;
}

void uif_reader_validates_directory_and_fonts() {
    auto bytes = make_valid_uif_archive();
    const auto archive = n3d::parse_uif_archive("uif.dat", bytes);
    check(archive.directory.size() == 32, "UIF directory entry count");
    check(archive.fonts.size() == 3 && archive.images.size() == 14,
          "UIF font and image counts");
    check(archive.fonts[0].header_bytes[0] == 0x06 && archive.fonts[0].header_bytes[1] == 0x07,
          "UIF font header values");
    check(archive.images[0].image.width == 320 && archive.images[0].image.height == 200,
          "UIF image dimensions");

    check(archive.fonts[0].glyphs[0].bitmap_offset == 0xC0 + 4 &&
          archive.fonts[0].glyphs[0].bitmap_size == 1,
          "UIF glyph points to its packed bitmap within the owned archive");

    auto invalid = bytes;
    invalid[8] = std::byte{0x40}; // Entry 1 starts one byte after font 0 ends.
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", invalid)); },
                      "uif.dat", 8);

    auto invalid_empty = bytes;
    invalid_empty[17 * 6 + 2] = std::byte{1};
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", invalid_empty)); },
                      "uif.dat", 17 * 6 + 2);

    auto truncated_font = bytes;
    truncated_font[0xC0 + 2] = std::byte{0xff};
    truncated_font[0xC0 + 3] = std::byte{0xff};
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", truncated_font)); },
                      "uif.dat", 0xC0 + 4);

    auto invalid_pcx = bytes;
    constexpr std::size_t first_image = 0xC0 + 3 * (2 + 127 * 3);
    invalid_pcx[first_image] = std::byte{0};
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", invalid_pcx)); },
                      "uif.dat", first_image);
}

void uif_reader_rejects_provenance_and_bounds_violations() {
    auto bytes = make_valid_uif_archive();

    std::vector<std::byte> truncated_directory(0xC0 - 1, std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", truncated_directory)); },
                      "uif.dat", 0);

    auto overlong_offset = bytes;
    overlong_offset[2] = std::byte{0xff};
    overlong_offset[3] = std::byte{0xff};
    overlong_offset[4] = std::byte{0xff};
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", overlong_offset)); },
                      "uif.dat", 2);

    auto bad_font_header = bytes;
    bad_font_header[0xC0] = std::byte{0x07};
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", bad_font_header)); },
                      "uif.dat", 0xC0);

    auto trailing_byte = bytes;
    trailing_byte.push_back(std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_uif_archive("uif.dat", trailing_byte)); },
                      "uif.dat", bytes.size());
}

void img_reader_checks_indexed_frame_groups() {
    constexpr std::size_t directory_size = 48128;
    std::vector<std::byte> bytes(directory_size + 12 + 12 + 11, std::byte{0});
    auto set_offset = [](std::vector<std::byte>& target, std::size_t id, std::size_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            target[id * 4 + byte] =
                std::byte{static_cast<unsigned char>(value >> (byte * 8))};
        }
    };
    const auto first = directory_size;
    const auto second = first + 12;
    const auto third = second + 12;
    set_offset(bytes, 1, first);
    set_offset(bytes, 2, first); // Two IDs alias the same two-frame sequence.
    set_offset(bytes, 3, third);
    bytes[0x800 + 1 * 90 + 2] = std::byte{2};
    bytes[0x800 + 2 * 90 + 2] = std::byte{2};
    bytes[0x800 + 3 * 90 + 2] = std::byte{1};
    bytes[first] = std::byte{2}; bytes[first + 1] = std::byte{1};
    bytes[second] = std::byte{1}; bytes[second + 1] = std::byte{2};
    bytes[third] = std::byte{1}; bytes[third + 1] = std::byte{1};

    const auto image = n3d::parse_img_archive("IMG.test", bytes);
    check(image.records.size() == 3 && image.active_id_count == 3 &&
          image.distinct_sequence_count == 2, "IMG indexed records and aliases");
    check(image.sequences[1].first_record_index == 0 &&
          image.sequences[2].first_record_index == 0 &&
          image.sequences[3].first_record_index == 2,
          "IMG IDs resolve to correct frame groups");

    auto bad_alias = bytes;
    bad_alias[0x800 + 2 * 90 + 2] = std::byte{1};
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", bad_alias)); },
                      "IMG.test", 0x800 + 2 * 90 + 2);

    auto bad_boundary = bytes;
    set_offset(bad_boundary, 3, third + 1);
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", bad_boundary)); },
                      "IMG.test", 0x800 + 1 * 90 + 2);
}

void fli_reader_exposes_only_declared_frames() {
    std::vector<std::byte> bytes(128 + 38 + 16, std::byte{0});
    auto put16 = [&](std::size_t offset, std::uint16_t value) {
        bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
        bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
    };
    auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes[offset + shift / 8] = std::byte{static_cast<std::uint8_t>(value >> shift)};
        }
    };
    put32(0, static_cast<std::uint32_t>(bytes.size()));
    put16(4, 0xaf11);
    put16(6, 1); // One declared frame and an extra empty file record.
    put16(8, 2);
    put16(10, 1);
    put16(12, 8);
    put16(16, 9);
    put32(128, 38);
    put16(132, 0xf1fa);
    put16(134, 2);
    put32(144, 13); // COLOR_64: one packet sets palette index 0.
    put16(148, 0x000b);
    put16(150, 1);
    bytes[153] = std::byte{1};
    bytes[154] = std::byte{10};
    bytes[155] = std::byte{20};
    bytes[156] = std::byte{30};
    put32(157, 9); // BRUN fills the two pixels with index 5.
    put16(161, 0x000f);
    bytes[163] = std::byte{1};
    bytes[164] = std::byte{2};
    bytes[165] = std::byte{5};
    put32(166, 16);
    put16(170, 0xf1fa);

    struct Probe final {
        std::size_t calls = 0;
        std::size_t frame_index = 0;
        std::array<std::byte, 2> pixels{};
        std::array<std::uint8_t, 3> first_colour{};
    } probe;
    const auto sink = +[](void* context, std::size_t index,
                          std::span<const std::byte> pixels,
                          const std::array<std::array<std::uint8_t, 3>, 256>& palette) {
        auto& output = *static_cast<Probe*>(context);
        ++output.calls;
        output.frame_index = index;
        output.pixels = {pixels[0], pixels[1]};
        output.first_colour = palette[0];
    };
    const auto stream = n3d::validate_fli_stream("ending.test", bytes, sink, &probe);
    check(stream.header.frames == 1 && stream.frame_records == 2 && probe.calls == 1 &&
          probe.frame_index == 0, "FLI ignores the extra empty record for playback");
    check(probe.pixels == std::array<std::byte, 2>{std::byte{5}, std::byte{5}} &&
          probe.first_colour == std::array<std::uint8_t, 3>{10, 20, 30},
          "FLI BRUN pixels and raw six-bit COLOR_64 values reach the frame consumer");

    auto bad_palette = bytes;
    bad_palette[154] = std::byte{64};
    expect_data_error([&] { static_cast<void>(n3d::validate_fli_stream("ending.test", bad_palette)); },
                      "ending.test", 154);
    auto missing_frame = bytes;
    missing_frame[6] = std::byte{3};
    expect_data_error([&] { static_cast<void>(n3d::validate_fli_stream("ending.test", missing_frame)); },
                      "ending.test", bytes.size());
}

void map_reader_rejects_zero_level_count() {
    constexpr std::size_t header_size = 514;
    std::vector<std::byte> zero_levels(header_size + 8192, std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_map_store("MAP.test", zero_levels)); },
                      "MAP.test", 0);
}

void img_reader_rejects_truncation_and_corrupt_records() {
    constexpr std::size_t directory_size = 48128;
    std::vector<std::byte> truncated_dir(directory_size - 1, std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", truncated_dir)); },
                      "IMG.test", 0);

    std::vector<std::byte> bytes(directory_size + 10 + 4, std::byte{0});
    const auto rec_offset = directory_size;
    bytes[0] = std::byte{static_cast<unsigned char>(rec_offset & 0xff)};
    bytes[1] = std::byte{static_cast<unsigned char>((rec_offset >> 8) & 0xff)};
    bytes[2] = std::byte{static_cast<unsigned char>((rec_offset >> 16) & 0xff)};
    bytes[3] = std::byte{static_cast<unsigned char>((rec_offset >> 24) & 0xff)};
    bytes[0x800 + 0] = std::byte{10};
    bytes[0x800 + 1] = std::byte{0};
    bytes[0x800 + 2] = std::byte{1};
    bytes[0x800 + 3] = std::byte{0};
    bytes[rec_offset + 0] = std::byte{2};
    bytes[rec_offset + 1] = std::byte{2};

    const auto valid_img = n3d::parse_img_archive("IMG.test", bytes);
    check(valid_img.active_id_count == 1 && valid_img.records.size() == 1,
          "IMG minimal single-record archive parses correctly");

    auto out_of_bounds_offset = bytes;
    out_of_bounds_offset[0] = std::byte{0xff};
    out_of_bounds_offset[1] = std::byte{0xff};
    out_of_bounds_offset[2] = std::byte{0x00};
    out_of_bounds_offset[3] = std::byte{0x00};
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", out_of_bounds_offset)); },
                      "IMG.test", 0);

    auto dirty_reserved = bytes;
    dirty_reserved[rec_offset + 6] = std::byte{1};
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", dirty_reserved)); },
                      "IMG.test", rec_offset + 6);

    auto zero_dim = bytes;
    zero_dim[rec_offset + 0] = std::byte{0};
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", zero_dim)); },
                      "IMG.test", rec_offset);

    auto zero_frames_descriptor = bytes;
    zero_frames_descriptor[0x800 + 2] = std::byte{0};
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", zero_frames_descriptor)); },
                      "IMG.test", 0x800 + 2);

    auto bad_extended = bytes;
    bad_extended[0x800 + 3] = std::byte{2};
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", bad_extended)); },
                      "IMG.test", 0x800 + 3);

    auto truncated_payload = bytes;
    truncated_payload.pop_back();
    expect_data_error([&] { static_cast<void>(n3d::parse_img_archive("IMG.test", truncated_payload)); },
                      "IMG.test", rec_offset + 10);
}

void fli_reader_rejects_header_and_chunk_boundary_violations() {
    std::vector<std::byte> header_too_small(127, std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_fli_header("ending.test", header_too_small)); },
                      "ending.test", 0);

    std::vector<std::byte> bad_magic(128, std::byte{0});
    bad_magic[4] = std::byte{0x12};
    bad_magic[5] = std::byte{0x34};
    expect_data_error([&] { static_cast<void>(n3d::parse_fli_header("ending.test", bad_magic)); },
                      "ending.test", 4);

    std::vector<std::byte> size_mismatch(128, std::byte{0});
    size_mismatch[0] = std::byte{200}; // Declares 200 bytes but only 128 present
    size_mismatch[4] = std::byte{0x11};
    size_mismatch[5] = std::byte{0xaf};
    expect_data_error([&] { static_cast<void>(n3d::parse_fli_header("ending.test", size_mismatch)); },
                      "ending.test", 0);

    std::vector<std::byte> zero_frames(128, std::byte{0});
    zero_frames[0] = std::byte{128};
    zero_frames[4] = std::byte{0x11};
    zero_frames[5] = std::byte{0xaf};
    zero_frames[6] = std::byte{0}; // 0 frames
    zero_frames[8] = std::byte{2}; // width
    zero_frames[10] = std::byte{1}; // height
    zero_frames[12] = std::byte{8}; // depth
    expect_data_error([&] { static_cast<void>(n3d::parse_fli_header("ending.test", zero_frames)); },
                      "ending.test", 6);

    auto put16 = [](std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
        bytes[offset] = std::byte{static_cast<std::uint8_t>(value)};
        bytes[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 8)};
    };
    auto put32 = [](std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes[offset + shift / 8] = std::byte{static_cast<std::uint8_t>(value >> shift)};
        }
    };
    auto make_stream = [&] {
        std::vector<std::byte> stream(128 + 38, std::byte{0});
        put32(stream, 0, static_cast<std::uint32_t>(stream.size()));
        put16(stream, 4, 0xaf11);
        put16(stream, 6, 1);
        put16(stream, 8, 2);
        put16(stream, 10, 1);
        put16(stream, 12, 8);
        put32(stream, 128, 38);
        put16(stream, 132, 0xf1fa);
        put16(stream, 134, 1); // One chunk per frame.
        put32(stream, 144, 22); // Chunk ends exactly at the frame and file end.
        put16(stream, 148, 0x000b);
        return stream;
    };

    auto overlong_chunk = make_stream();
    put32(overlong_chunk, 144, 30);
    expect_data_error([&] { static_cast<void>(n3d::validate_fli_stream("ending.test", overlong_chunk)); },
                      "ending.test", 144);

    auto chunk_past_frame = make_stream();
    put32(chunk_past_frame, 128, 24);
    put32(chunk_past_frame, 144, 10);
    expect_data_error([&] { static_cast<void>(n3d::validate_fli_stream("ending.test", chunk_past_frame)); },
                      "ending.test", 144);

    auto overlong_frame = make_stream();
    put32(overlong_frame, 128, 39);
    expect_data_error([&] { static_cast<void>(n3d::validate_fli_stream("ending.test", overlong_frame)); },
                      "ending.test", 128);
}

void pcx_reader_rejects_boundary_violations() {
    std::vector<std::byte> short_pcx(100, std::byte{0});
    expect_data_error([&] { static_cast<void>(n3d::parse_pcx_image("game.pal", short_pcx)); },
                      "game.pal", 0);

    std::vector<std::byte> bad_marker(128 + 2 + 1 + 768, std::byte{0});
    bad_marker[0] = std::byte{0x0a};
    bad_marker[1] = std::byte{0x05};
    bad_marker[2] = std::byte{0x01};
    bad_marker[3] = std::byte{0x08};
    bad_marker[8] = std::byte{1};
    bad_marker[65] = std::byte{1};
    bad_marker[66] = std::byte{2};
    bad_marker[128] = std::byte{0xc2};
    bad_marker[129] = std::byte{0x07};
    bad_marker[130] = std::byte{0x00};
    expect_data_error([&] { static_cast<void>(n3d::parse_pcx_image("game.pal", bad_marker)); },
                      "game.pal", 130);

    auto scanline_overflow = bad_marker;
    scanline_overflow[130] = std::byte{0x0c};
    scanline_overflow[128] = std::byte{0xc5};
    expect_data_error([&] { static_cast<void>(n3d::parse_pcx_image("game.pal", scanline_overflow)); },
                      "game.pal", 129);
}

} // namespace

int run_asset_tests() {
    sha256_known_vectors();
    map_reader_checks_exact_blocks();
    map_viewer_catalogs_validate_header_references();
    catalog_reader_preserves_fields();
    catalog_reader_accepts_tab_delimiters();
    pcx_reader_decodes_rle_and_palette();
    img_reader_checks_indexed_frame_groups();
    uif_reader_validates_directory_and_fonts();
    uif_reader_rejects_provenance_and_bounds_violations();
    fli_reader_exposes_only_declared_frames();
    map_reader_rejects_zero_level_count();
    img_reader_rejects_truncation_and_corrupt_records();
    fli_reader_rejects_header_and_chunk_boundary_violations();
    pcx_reader_rejects_boundary_violations();
    if (failures != 0) {
        std::cerr << failures << " asset checks failed\n";
        return failures;
    }
    std::cout << "n3d asset checks passed\n";
    return 0;
}
