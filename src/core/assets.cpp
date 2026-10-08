#include "n3d/assets.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace n3d {
namespace {

struct AssetFingerprint final {
    std::string_view name;
    std::size_t exact_size;
    std::string_view sha256_hex;
};

constexpr std::array<AssetFingerprint, 3> kMapFingerprints = {{
    {"MAP.1", 90626, "3EDDB0AD3A29E93D63FA16E9B8CFBB434D3699A84BD5372ED2D6F4A1E89C42FC"},
    {"MAP.2", 82434, "938B7E79C9881EAB71FDEF4B7AFA9B0482C870C808785F599A4BA52093D44756"},
    {"MAP.3", 82434, "0008775106854B3E9302F36FBCD7001607BCF2FD26CC9D8DB6BCCC732EAD4217"},
}};

constexpr std::array<AssetFingerprint, 3> kWallFingerprints = {{
    {"WALLS.1", 12296, "9FBBF889440CF7200C7C2DAD271F47C79652BAF237C27F1BBB354EE1E8B1CF84"},
    {"WALLS.2", 11918, "23F9CB3757AD92876D8F43A8AD9987CECC596E3C2D9A9EBB09B10CE0A2AF59FE"},
    {"WALLS.3", 11543, "ABF6C2BC6779E6261998BC26BD03BBBCE7E3EA608F28DDC4D78A13FD80A39CAE"},
}};

constexpr std::array<AssetFingerprint, 3> kObjectFingerprints = {{
    {"OBJECTS.1", 8725, "FCF7824A4378A22616220DF077BFB80D5C088CADC599FB02EC3DC66A1482F9B6"},
    {"OBJECTS.2", 8427, "15980CA3B47BE80809C5E2A8A8175447761BBC9B1702599A1141DAB00336BB42"},
    {"OBJECTS.3", 7228, "21D1F22F52FB7B112D84903476E9926592160DC76A9F64F9978CD723D21D7F38"},
}};

constexpr std::array<AssetFingerprint, 3> kImgFingerprints = {{
    {"IMG.1", 2250921, "0D7B5BF9A3348BE263D337201BD98E867F765B87BE3F15666164C39763DC1267"},
    {"IMG.2", 1843068, "61554B68A56EA04F0FE5B5CACAF6938AA13A6905E8702D77F659A242623D748C"},
    {"IMG.3", 1798340, "4599DAC2EE3E387E6311D6D9A422582848CE7D4D6DBBFBA74172777A50541A1C"},
}};
constexpr AssetFingerprint kUifFingerprint{"uif.dat", 527595,
                                         "D1D5A1F59283530EE025C6A95D3CC6300854929213DBD9FED881A696688A8A4D"};
constexpr AssetFingerprint kSoundFingerprint{"snd.dat", 843918,
                                           "D22F5505D2B82448B8C644540AD9E3D611855CA58EB50FEEF5B7D5547B66B749"};
constexpr std::array<AssetFingerprint, 3> kDemoFingerprints = {{
    {"DEMO.1", 1630, "50D9D333D4647831BA0786B97127A695F24CA86136C3626AD946FDF2C82AE2F7"},
    {"DEMO.2", 2174, "04E9DACD18341775C72661EE57FCE1CCA130EDB3B1616CEF87C50974997F0781"},
    {"DEMO.3", 2294, "AD6752DA2C7DA06FAB62706CF0929DD10035DD990D313321E6C8C401C5B90864"},
}};
constexpr AssetFingerprint kBsfFingerprint{"nite3d.bsf", 10370,
                                         "8C72CB380EBE2070611332C3EF8ACB3873BD198227424788B865E72407BD0B3C"};
constexpr AssetFingerprint kPaletteFingerprint{"game.pal", 5459,
                                             "B6B71824A73A74C79E0F3AE12C9A423F2AE500CA956FE156F1F0D4D2307D24FC"};
constexpr AssetFingerprint kEndingFingerprint{"ending.fli", 334855,
                                            "8CFADE6E5E6EAC08BA0E07DFD330B65AE3A82F82C756E4C72D36CA850A28542A"};

constexpr std::size_t kUifDirectoryEntryCount = 32;
constexpr std::size_t kUifActiveRecordCount = 17;
constexpr std::size_t kUifDirectorySize = 0xC0;
constexpr std::size_t kUifGlyphCount = 127;
constexpr std::size_t kUifFontCount = 3;
constexpr std::size_t kUifImageCount = 14;
constexpr std::size_t kMapHeaderSize = 514;
constexpr std::size_t kMapLevelSize = 8192;
constexpr std::size_t kImgDirectorySize = 48128;
constexpr std::size_t kImgIdCount = 512;
constexpr std::size_t kImgDescriptorOffset = 0x800;
constexpr std::size_t kImgDescriptorSize = 90;
constexpr std::size_t kPcxHeaderSize = 128;
constexpr std::size_t kPcxPaletteSize = 768;
constexpr std::size_t kMapWallClassTableOffset = 0x02;
constexpr std::size_t kMapObjectClassTableOffset = 0x102;

// maps.exe class-name table order, from maps.exe SHA-256
// 1F6A143A6E68A5F8D3ADBE32F96B3B0CBE0EF23AED4DD56FB140FCDFF3A0A7A1 at DS:00d8/016a.
// These are viewer labels only; they do not assign game-simulation meanings.
constexpr std::array<std::string_view, 73> kMapViewerWallClassNames{
    "NULL", "WALL", "REVWALL", "CONTROL", "POWER_UP", "VEND_FOOD", "VEND_BAT", "ONE_SHOT",
    "SPECIAL1", "LEVEL_UP", "LEVEL_UP2", "LEVEL_R", "LEVEL_N", "WARP_1", "WARP_2", "WARP_3",
    "WARP_4", "WARP_5", "WARP_6", "WARP_7", "WARP_8", "WARP_S1", "WARP_S2", "WARP_S3",
    "WARP_S4", "WARP_L1", "WARP_L2", "WARP_L3", "WARP_L4", "WARP_E1", "WARP_E2", "WARP_E3",
    "WARP_E4", "WARP_E5", "WARP_E6", "WARP_E7", "WARP_E8", "WARP_C1", "WARP_C2", "WARP_C3",
    "WARP_C4", "WARP_C5", "WARP_C6", "WARP_C7", "WARP_C8", "WALL_EX", "WALL_EX1", "WALL_EX2",
    "JAMB", "DOORV", "DOORH", "DOORVL", "DOORHL", "DOORVL2", "DOORHL2", "DOORVL3",
    "DOORHL3", "DOORVI", "DOORHI", "DOORVR", "DOORHR", "DOORVU", "DOORHU", "DOORVC",
    "DOORHC", "TURN", "RETREAT", "FLEE", "FLOOR", "SAFESPOT", "ACTIONSPOT", "TRIGGER1", "TRIGGER2",
};

constexpr std::array<std::string_view, 63> kMapViewerObjectClassNames{
    "NULL", "HERO", "START", "SECRET", "IMPACT", "MISSILE", "VAPID", "CAUSTIC",
    "GUARD1", "GUARD2", "GUARD3", "GUARD4", "GUARD5", "GUARD6", "GUARD7", "GUARD8",
    "GUARD9", "GUARD10", "GUARD11", "GUARD12", "GUARD13", "GUARD14", "GUARD15", "GUARD16",
    "GUARD17", "GUARD18", "GUARD19", "GUARD20", "GUARD21", "GUARD22", "GUARD23", "GUARD24",
    "GUARD25", "GUARD26", "GUARD27", "GUARD28", "GUARD29", "GUARD30", "SAFE", "TRUNK",
    "PUSH", "ACTION", "PERMEABLE", "DUMB", "DUMB4", "DUMB8", "ELEVATED", "KEY",
    "IDCARD", "GOLD", "MONEY", "FOOD", "AID", "LIFE", "WEAPON", "RADAR",
    "BATTERY", "AMMO", "CRYSTALB", "MAGICEYE", "PENTAGRAM", "SCROLL", "UIFOBJ",
};

std::array<std::uint8_t, 32> parse_hex_sha256(std::string_view hex) {
    if (hex.size() != 64) {
        throw std::invalid_argument("SHA-256 hex digest must be exactly 64 chars");
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto hi = hex[2 * index];
        const auto lo = hex[2 * index + 1];
        auto decode = [](char value) -> std::uint8_t {
            if (value >= '0' && value <= '9') {
                return static_cast<std::uint8_t>(value - '0');
            }
            if (value >= 'a' && value <= 'f') {
                return static_cast<std::uint8_t>(10 + (value - 'a'));
            }
            if (value >= 'A' && value <= 'F') {
                return static_cast<std::uint8_t>(10 + (value - 'A'));
            }
            throw std::invalid_argument("SHA-256 hex digest contains invalid hex digits");
        };
        digest[index] = static_cast<std::uint8_t>((decode(hi) << 4) | decode(lo));
    }
    return digest;
}

std::uint16_t read_u16le_at(std::string_view path, std::span<const std::byte> bytes,
                            std::size_t offset, std::string_view invariant) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        throw DataError(std::string(path), offset, invariant);
    }
    const auto first = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto second = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(first | (second << 8));
}

std::uint32_t read_u32le_at(std::string_view path, std::span<const std::byte> bytes,
                            std::size_t offset, std::string_view invariant) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw DataError(std::string(path), offset, invariant);
    }
    const auto first = std::to_integer<std::uint32_t>(bytes[offset]);
    const auto second = std::to_integer<std::uint32_t>(bytes[offset + 1]);
    const auto third = std::to_integer<std::uint32_t>(bytes[offset + 2]);
    const auto fourth = std::to_integer<std::uint32_t>(bytes[offset + 3]);
    return first | (second << 8) | (third << 16) | (fourth << 24);
}

void require_at(std::string_view path, std::span<const std::byte> bytes, std::size_t offset,
                std::size_t count, std::string_view invariant) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        throw DataError(std::string(path), offset, invariant);
    }
}

bool is_space(std::byte value) {
    const auto byte = std::to_integer<unsigned char>(value);
    return byte == ' ' || byte == '\t';
}

bool is_hex(std::byte value) {
    const auto byte = std::to_integer<unsigned char>(value);
    return (byte >= '0' && byte <= '9') ||
           (byte >= 'a' && byte <= 'f') ||
           (byte >= 'A' && byte <= 'F');
}

std::uint16_t parse_hex_id(std::string_view path, std::span<const std::byte> bytes,
                           std::size_t offset) {
    require_at(path, bytes, offset, 4, "definition identifier available");
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        if (!is_hex(bytes[offset + index])) {
            throw DataError(std::string(path), offset + index,
                            "definition identifier contains four hexadecimal digits");
        }
        const auto byte = std::to_integer<unsigned char>(bytes[offset + index]);
        const auto digit = byte <= '9' ? byte - '0' :
                           byte <= 'F' ? byte - 'A' + 10 : byte - 'a' + 10;
        value = static_cast<std::uint16_t>((value << 4) | digit);
    }
    return value;
}

std::string copy_ascii(std::string_view path, std::span<const std::byte> bytes,
                       std::size_t begin, std::size_t end, std::string_view invariant) {
    if (begin > end || end > bytes.size()) {
        throw DataError(std::string(path), begin, invariant);
    }
    std::string result;
    result.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        const auto byte = std::to_integer<unsigned char>(bytes[index]);
        if (byte < 0x20 || byte > 0x7e) {
            throw DataError(std::string(path), index, invariant);
        }
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

std::pair<std::size_t, std::size_t> next_field(std::string_view path,
                                                std::span<const std::byte> bytes,
                                                std::size_t begin, std::size_t end,
                                                std::string_view invariant) {
    while (begin < end && is_space(bytes[begin])) {
        ++begin;
    }
    const auto field_begin = begin;
    while (begin < end && !is_space(bytes[begin])) {
        ++begin;
    }
    if (field_begin == begin) {
        throw DataError(std::string(path), field_begin, invariant);
    }
    return {field_begin, begin};
}
bool equal_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto left_byte = static_cast<unsigned char>(left[index]);
        auto right_byte = static_cast<unsigned char>(right[index]);
        if (left_byte >= 'a' && left_byte <= 'z') {
            left_byte = static_cast<unsigned char>(left_byte - 'a' + 'A');
        }
        if (right_byte >= 'a' && right_byte <= 'z') {
            right_byte = static_cast<unsigned char>(right_byte - 'a' + 'A');
        }
        if (left_byte != right_byte) {
            return false;
        }
    }
    return true;
}

template <std::size_t ClassCount>
void validate_viewer_class_table(
    std::string_view map_path, std::size_t table_offset,
    const std::array<std::uint8_t, 256>& class_by_id,
    std::string_view catalog_path, std::span<const CatalogRecord> catalog,
    const std::array<std::string_view, ClassCount>& class_names) {
    for (std::size_t id = 0; id < class_by_id.size(); ++id) {
        if (class_by_id[id] >= class_names.size()) {
            throw DataError(std::string(map_path), table_offset + id,
                            "MAP class index is within the maps.exe viewer name table");
        }
    }

    std::array<bool, 256> defined{};
    for (const auto& record : catalog) {
        if (record.id >= class_by_id.size()) {
            throw DataError(std::string(catalog_path), 0,
                            "catalog identifier fits the byte-indexed MAP class table");
        }
        const auto id = static_cast<std::size_t>(record.id);
        if (defined[id]) {
            throw DataError(std::string(map_path), table_offset + id,
                            "catalog identifiers are unique in the MAP class table");
        }
        defined[id] = true;

        const auto name = std::find_if(
            class_names.begin(), class_names.end(), [&](std::string_view candidate) {
                return equal_ascii_case_insensitive(candidate, record.type);
            });
        if (name == class_names.end()) {
            throw DataError(std::string(map_path), table_offset + id,
                            "catalog class name exists in the maps.exe viewer name table");
        }
        const auto expected_class = static_cast<std::size_t>(name - class_names.begin());
        if (class_by_id[id] != expected_class) {
            throw DataError(std::string(map_path), table_offset + id,
                            "MAP class byte matches the same-episode viewer catalog");
        }
    }

    for (std::size_t id = 0; id < class_by_id.size(); ++id) {
        if (!defined[id] && class_by_id[id] != 0) {
            throw DataError(std::string(map_path), table_offset + id,
                            "undefined catalog ID has zero MAP class byte");
        }
    }
}

std::vector<std::byte> read_file_impl(const std::filesystem::path& path,
                                      std::optional<std::size_t> expected_size) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw DataError(path.string(), 0, "file can be opened");
    }
    const auto end = stream.tellg();
    if (end < 0) {
        throw DataError(path.string(), 0, "file size can be read");
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (expected_size && size != static_cast<std::uintmax_t>(*expected_size)) {
        const auto offset = size < static_cast<std::uintmax_t>(*expected_size)
                                ? static_cast<std::size_t>(size)
                                : *expected_size;
        throw DataError(path.string(), offset,
                        "asset size matches the committed Windows content fingerprint");
    }
    if (size > std::vector<std::byte>().max_size()) {
        throw DataError(path.string(), 0, "file size fits the native buffer");
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw DataError(path.string(), 0, "file size fits the stream read size");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size()))) {
        throw DataError(path.string(), 0, "file contents can be read");
    }
    return bytes;
}

std::vector<std::byte> read_verified_file(const std::filesystem::path& path,
                                          std::size_t expected_size,
                                          std::string_view expected_hex) {
    auto bytes = read_file_impl(path, expected_size);
    const auto actual_digest = detail::sha256(bytes);
    const auto expected_digest = parse_hex_sha256(expected_hex);
    if (actual_digest != expected_digest) {
        throw DataError(path.string(), 0,
                        "asset SHA-256 matches the committed Windows content fingerprint");
    }
    return bytes;
}


std::size_t validate_help_image_references(
    std::span<const std::byte> text, std::size_t file_offset,
    const std::array<EpisodeAssets, 3>& episodes) {
    std::size_t count = 0;
    for (std::size_t pos = 0; pos + 1 < text.size();) {
        if (text[pos] != std::byte{'%'} || text[pos + 1] != std::byte{'o'}) {
            ++pos;
            continue;
        }
        auto cursor = pos + 2;
        std::size_t id = 0;
        const auto first_digit = cursor;
        while (cursor < text.size()) {
            const auto character = std::to_integer<unsigned char>(text[cursor]);
            if (character < '0' || character > '9') {
                break;
            }
            const auto digit = static_cast<std::size_t>(character - '0');
            if (id > (255 - digit) / 10) {
                throw DataError("nite3d.bsf", file_offset + pos,
                                "help object image ID fits one byte");
            }
            id = id * 10 + digit;
            ++cursor;
        }
        if (cursor == first_digit || cursor == text.size() || text[cursor] != std::byte{'%'}) {
            throw DataError("nite3d.bsf", file_offset + pos,
                            "help object reference has decimal digits and closing percent");
        }
        for (const auto& episode : episodes) {
            if (episode.images.sequences[256 + id].offset == 0) {
                throw DataError("nite3d.bsf", file_offset + pos,
                                "help object image resolves in every episode");
            }
        }
        ++count;
        pos = cursor + 1;
    }
    return count;
}

} // namespace

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    return read_file_impl(path, std::nullopt);
}

MapStoreMetadata parse_map_store(std::string_view path, std::span<const std::byte> bytes) {
    const auto level_count = read_u16le_at(path, bytes, 0, "map level count available");
    if (level_count == 0) {
        throw DataError(std::string(path), 0, "map contains at least one level");
    }
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    if (level_count > (max_size - kMapHeaderSize) / kMapLevelSize) {
        throw DataError(std::string(path), 0, "map level count does not overflow file size");
    }
    const auto expected_size = kMapHeaderSize +
                               static_cast<std::size_t>(level_count) * kMapLevelSize;
    if (bytes.size() != expected_size) {
        throw DataError(std::string(path), bytes.size(),
                        "map size is header plus 8192-byte level blocks");
    }
    MapStoreMetadata map{level_count, kMapWallClassTableOffset,
                         kMapHeaderSize - kMapWallClassTableOffset,
                         kMapHeaderSize, kMapLevelSize};
    for (std::size_t id = 0; id < map.wall_class_by_id.size(); ++id) {
        map.wall_class_by_id[id] = std::to_integer<std::uint8_t>(
            bytes[kMapWallClassTableOffset + id]);
        map.object_class_by_id[id] = std::to_integer<std::uint8_t>(
            bytes[kMapObjectClassTableOffset + id]);
    }
    return map;
}

ImgArchiveMetadata parse_img_archive(std::string_view path, std::span<const std::byte> bytes) {
    if (bytes.size() < kImgDirectorySize) {
        throw DataError(std::string(path), 0, "IMG directory area is available");
    }
    ImgArchiveMetadata archive{};
    archive.directory_size = kImgDirectorySize;
    std::array<std::pair<std::size_t, std::size_t>, kImgIdCount> active{};
    for (std::size_t id = 0; id < kImgIdCount; ++id) {
        const auto directory_offset = id * sizeof(std::uint32_t);
        const auto offset = static_cast<std::size_t>(read_u32le_at(
            path, bytes, directory_offset, "IMG image ID offset is available"));
        const auto descriptor_offset = kImgDescriptorOffset + id * kImgDescriptorSize;
        const auto delay = read_u16le_at(path, bytes, descriptor_offset,
                                         "IMG descriptor delay is available");
        const auto frames = std::to_integer<std::uint8_t>(bytes[descriptor_offset + 2]);
        const auto extended = std::to_integer<std::uint8_t>(bytes[descriptor_offset + 3]);
        archive.sequences[id] = ImgSequenceMetadata{offset, descriptor_offset, 0,
                                                    delay, frames, extended};
        if (offset == 0) {
            continue; // Unused IDs can still retain an authored descriptor.
        }
        if (offset < kImgDirectorySize || offset >= bytes.size()) {
            throw DataError(std::string(path), directory_offset,
                            "IMG image ID points into the record area");
        }
        if (frames == 0 || extended > 1) {
            throw DataError(std::string(path), descriptor_offset + (frames == 0 ? 2 : 3),
                            "IMG active descriptor has frames and a supported state flag");
        }
        active[archive.active_id_count++] = {offset, id};
    }

    std::size_t offset = kImgDirectorySize;
    while (offset < bytes.size()) {
        require_at(path, bytes, offset, 10, "IMG record header is available");
        const auto width = std::to_integer<std::uint8_t>(bytes[offset]);
        const auto height = std::to_integer<std::uint8_t>(bytes[offset + 1]);
        if (width == 0 || height == 0) {
            throw DataError(std::string(path), offset, "IMG record dimensions are nonzero");
        }
        if (bytes[offset + 6] != std::byte{0} || bytes[offset + 7] != std::byte{0} ||
            bytes[offset + 8] != std::byte{0} || bytes[offset + 9] != std::byte{0}) {
            throw DataError(std::string(path), offset + 6,
                            "IMG record reserved header bytes are zero");
        }
        const auto pixel_size = static_cast<std::size_t>(width) * height;
        require_at(path, bytes, offset + 10, pixel_size, "IMG pixel payload is available");
        archive.records.push_back(ImgRecordMetadata{
            offset, width, height,
            {std::to_integer<std::uint8_t>(bytes[offset + 2]),
             std::to_integer<std::uint8_t>(bytes[offset + 3]),
             std::to_integer<std::uint8_t>(bytes[offset + 4]),
             std::to_integer<std::uint8_t>(bytes[offset + 5])},
            offset + 10, pixel_size});
        offset += 10 + pixel_size;
    }
    if (archive.records.empty() || archive.active_id_count == 0) {
        throw DataError(std::string(path), kImgDirectorySize,
                        "IMG contains indexed image records");
    }

    std::sort(active.begin(), active.begin() + static_cast<std::ptrdiff_t>(archive.active_id_count));
    std::size_t record_index = 0;
    for (std::size_t group = 0; group < archive.active_id_count;) {
        const auto group_offset = active[group].first;
        const auto first_id = active[group].second;
        if (record_index == archive.records.size() ||
            archive.records[record_index].offset != group_offset) {
            throw DataError(std::string(path), first_id * sizeof(std::uint32_t),
                            "IMG image ID points to the first record of a frame group");
        }
        const auto frames = archive.sequences[first_id].frame_count;
        const auto next_record = record_index + frames;
        if (next_record > archive.records.size()) {
            throw DataError(std::string(path), archive.sequences[first_id].descriptor_offset + 2,
                            "IMG frame count fits within the record store");
        }
        do {
            const auto id = active[group].second;
            if (archive.sequences[id].frame_count != frames) {
                throw DataError(std::string(path), archive.sequences[id].descriptor_offset + 2,
                                "IMG aliased IDs have the same frame span");
            }
            archive.sequences[id].first_record_index = record_index;
            ++group;
        } while (group < archive.active_id_count && active[group].first == group_offset);
        const auto next_offset = group < archive.active_id_count ? active[group].first : bytes.size();
        const auto frame_end = next_record < archive.records.size() ?
            archive.records[next_record].offset : bytes.size();
        if (frame_end != next_offset) {
            throw DataError(std::string(path), archive.sequences[first_id].descriptor_offset + 2,
                            "IMG frame count reaches the next indexed group");
        }
        record_index = next_record;
        ++archive.distinct_sequence_count;
    }
    if (record_index != archive.records.size()) {
        throw DataError(std::string(path), archive.records[record_index].offset,
                        "IMG directory covers every record");
    }
    return archive;
}

std::vector<CatalogRecord> parse_definition_catalog(std::string_view path,
                                                     std::span<const std::byte> bytes) {
    std::vector<CatalogRecord> records;
    std::size_t line_begin = 0;
    std::uint16_t previous_id = 0;
    bool first = true;
    while (line_begin < bytes.size()) {
        const auto cr = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(line_begin),
                                  bytes.end(), std::byte{'\r'});
        if (cr == bytes.end()) {
            throw DataError(std::string(path), line_begin, "definition file ends with CRLF");
        }
        const auto cr_offset = static_cast<std::size_t>(std::distance(bytes.begin(), cr));
        if (cr_offset + 1 >= bytes.size() || bytes[cr_offset + 1] != std::byte{'\n'}) {
            throw DataError(std::string(path), cr_offset, "definition record uses CRLF");
        }
        if (cr_offset == line_begin) {
            throw DataError(std::string(path), line_begin, "definition record is not empty");
        }

        const auto id = parse_hex_id(path, bytes, line_begin);
        if (cr_offset <= line_begin + 4 || bytes[line_begin + 4] != std::byte{' '}) {
            throw DataError(std::string(path), line_begin + 4,
                            "definition identifier is followed by a space");
        }
        auto cursor = line_begin + 5;
        const auto code_range = next_field(path, bytes, cursor, cr_offset,
                                           "definition code is present");
        if (code_range.second - code_range.first != 4) {
            throw DataError(std::string(path), code_range.first,
                            "definition code contains exactly four characters");
        }
        cursor = code_range.second;
        const auto resource_range = next_field(path, bytes, cursor, cr_offset,
                                               "definition resource is present");
        cursor = resource_range.second;
        const auto type_range = next_field(path, bytes, cursor, cr_offset,
                                           "definition type is present");
        cursor = type_range.second;
        while (cursor < cr_offset && is_space(bytes[cursor])) {
            ++cursor;
        }
        if (cursor == cr_offset) {
            throw DataError(std::string(path), cursor, "definition description is present");
        }

        if (!first && id <= previous_id) {
            throw DataError(std::string(path), line_begin,
                            "definition identifiers are strictly increasing");
        }
        first = false;
        previous_id = id;
        records.push_back(CatalogRecord{
            id,
            copy_ascii(path, bytes, code_range.first, code_range.second,
                       "definition code is printable ASCII"),
            copy_ascii(path, bytes, resource_range.first, resource_range.second,
                       "definition resource is printable ASCII"),
            copy_ascii(path, bytes, type_range.first, type_range.second,
                       "definition type is printable ASCII"),
            copy_ascii(path, bytes, cursor, cr_offset,
                       "definition description is printable ASCII"),
        });
        line_begin = cr_offset + 2;
    }
    if (records.empty()) {
        throw DataError(std::string(path), 0, "definition file is not empty");
    }
    return records;
}
void validate_map_viewer_catalogs(std::string_view map_path,
                                  const MapStoreMetadata& map,
                                  std::string_view wall_path,
                                  std::span<const CatalogRecord> walls,
                                  std::string_view object_path,
                                  std::span<const CatalogRecord> objects) {
    validate_viewer_class_table(map_path, kMapWallClassTableOffset,
                                map.wall_class_by_id, wall_path, walls,
                                kMapViewerWallClassNames);
    validate_viewer_class_table(map_path, kMapObjectClassTableOffset,
                                map.object_class_by_id, object_path, objects,
                                kMapViewerObjectClassNames);
}

std::size_t checked_add(std::string_view path, std::size_t left, std::size_t right,
                        std::size_t offset, std::string_view invariant) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw DataError(std::string(path), offset, invariant);
    }
    return left + right;
}

UifArchiveMetadata parse_uif_archive(std::string_view path,
                                     std::span<const std::byte> bytes) {
    if (bytes.size() < kUifDirectorySize) {
        throw DataError(std::string(path), 0, "uif directory is available");
    }

    UifArchiveMetadata archive{};
    archive.directory_size = kUifDirectorySize;

    std::size_t expected_cursor = kUifDirectorySize;
    std::size_t active_count = 0;
    bool saw_zero_entry = false;
    std::size_t first_nonzero_entry = std::numeric_limits<std::size_t>::max();

    for (std::size_t index = 0; index < kUifDirectoryEntryCount; ++index) {
        const auto entry_offset = index * 6;
        if (entry_offset + 6 > bytes.size()) {
            throw DataError(std::string(path), entry_offset,
                            "uif directory record is available");
        }
        const auto length = read_u16le_at(path, bytes, entry_offset,
                                          "uif directory entry length is available");
        const auto absolute_offset = read_u32le_at(path, bytes, entry_offset + 2,
                                                   "uif directory entry offset is available");
        const auto entry_length = static_cast<std::size_t>(length);
        const auto entry_start = static_cast<std::size_t>(absolute_offset);
        archive.directory[index] = UifDirectoryEntry{entry_start, entry_length};

        if (length == 0) {
            if (entry_start != 0) {
                throw DataError(std::string(path), entry_offset + 2,
                                "uif unused directory entries have zero offset");
            }
            saw_zero_entry = true;
            continue;
        }
        if (saw_zero_entry) {
            throw DataError(std::string(path), entry_offset,
                            "uif active entries end before the zero trailer");
        }
        if (entry_start < kUifDirectorySize) {
            throw DataError(std::string(path), entry_offset + 2,
                            "uif payload offset begins after the directory");
        }
        if (entry_start > bytes.size()) {
            throw DataError(std::string(path), entry_offset + 2,
                            "uif payload offset fits within the archive");
        }
        if (entry_length > bytes.size() - entry_start) {
            throw DataError(std::string(path), entry_offset,
                            "uif payload stays within the archive bounds");
        }
        if (entry_start != expected_cursor) {
            throw DataError(std::string(path), entry_offset + 2,
                            "uif payloads are contiguous from the directory end");
        }
        if (first_nonzero_entry == std::numeric_limits<std::size_t>::max()) {
            first_nonzero_entry = entry_offset;
        }
        expected_cursor = checked_add(path, entry_start, entry_length, entry_offset,
                                     "uif payloads do not overflow the archive");
        ++active_count;
    }

    if (first_nonzero_entry == std::numeric_limits<std::size_t>::max()) {
        throw DataError(std::string(path), 0, "uif archive contains active records");
    }
    if (active_count != kUifActiveRecordCount) {
        throw DataError(std::string(path), first_nonzero_entry,
                        "uif archive contains 17 active payloads");
    }
    if (expected_cursor != bytes.size()) {
        throw DataError(std::string(path), expected_cursor,
                        "uif active payloads consume the archive to EOF");
    }

    for (std::size_t record_index = 0; record_index < kUifFontCount; ++record_index) {
        const auto& entry = archive.directory[record_index];
        if (entry.length < 2) {
            throw DataError(std::string(path), entry.offset,
                            "uif font header bytes are available");
        }
        auto span = bytes.subspan(entry.offset, entry.length);
        std::array<std::uint8_t, 2> header{};
        header[0] = std::to_integer<std::uint8_t>(span[0]);
        header[1] = std::to_integer<std::uint8_t>(span[1]);
        if (record_index == 0 && (header[0] != 0x06 || header[1] != 0x07)) {
            throw DataError(std::string(path), entry.offset,
                            "uif font 0 header is 06 07");
        }
        if (record_index == 1 && (header[0] != 0x0b || header[1] != 0x0b)) {
            throw DataError(std::string(path), entry.offset,
                            "uif font 1 header is 0B 0B");
        }
        if (record_index == 2 && (header[0] != 0x09 || header[1] != 0x08)) {
            throw DataError(std::string(path), entry.offset,
                            "uif font 2 header is 09 08");
        }

        auto& font = archive.fonts[record_index];
        font.header_bytes = header;
        std::size_t cursor = 2;
        for (std::size_t glyph_index = 0; glyph_index < kUifGlyphCount; ++glyph_index) {
            if (cursor + 2 > span.size()) {
                throw DataError(std::string(path), entry.offset + cursor,
                                "uif glyph header is available");
            }
            const auto rows = std::to_integer<std::uint8_t>(span[cursor]);
            const auto width = std::to_integer<std::uint8_t>(span[cursor + 1]);
            if (rows == 0 || width == 0) {
                throw DataError(std::string(path), entry.offset + cursor,
                                "uif glyph rows and width are nonzero");
            }
            const auto packed_width = (static_cast<std::size_t>(width) + 7u) / 8u;
            const auto glyph_size = static_cast<std::size_t>(rows) * packed_width;
            if (glyph_size > span.size() - cursor - 2) {
                throw DataError(std::string(path), entry.offset + cursor + 2,
                                "uif glyph bitmap fits within the font record");
            }
            font.glyphs[glyph_index] = UifGlyph{rows, width,
                                                entry.offset + cursor + 2, glyph_size};
            cursor += 2 + glyph_size;
        }
        if (cursor != span.size()) {
            throw DataError(std::string(path), entry.offset + cursor,
                            "uif font record consumes its payload exactly");
        }
    }

    for (std::size_t record_index = kUifFontCount;
         record_index < kUifFontCount + kUifImageCount; ++record_index) {
        const auto& entry = archive.directory[record_index];
        if (entry.length == 0) {
            throw DataError(std::string(path), entry.offset,
                            "uif image record length is nonzero");
        }
        const auto span = bytes.subspan(entry.offset, entry.length);
        PcxImage decoded{};
        try {
            decoded = parse_pcx_image(path, span);
        } catch (const DataError& error) {
            throw DataError(std::string(path), entry.offset + error.offset(),
                            error.invariant());
        }
        if (decoded.width != 320 || decoded.height != 200 || decoded.bytes_per_line != 320) {
            throw DataError(std::string(path), entry.offset,
                            "uif image dimensions are 320x200 with 320-byte scanlines");
        }
        archive.images[record_index - kUifFontCount] = UifImage{
            record_index, entry.offset, entry.length, std::move(decoded)};
    }

    return archive;
}

PcxImage parse_pcx_image(std::string_view path, std::span<const std::byte> bytes) {
    require_at(path, bytes, 0, kPcxHeaderSize + 1 + kPcxPaletteSize,
               "PCX header, image data, and palette are available");
    if (std::to_integer<std::uint8_t>(bytes[0]) != 0x0a ||
        std::to_integer<std::uint8_t>(bytes[1]) != 0x05 ||
        std::to_integer<std::uint8_t>(bytes[2]) != 0x01 ||
        std::to_integer<std::uint8_t>(bytes[3]) != 0x08) {
        throw DataError(std::string(path), 0, "PCX uses manufacturer 0A, version 05, RLE, 8 bits");
    }
    const auto width_min = read_u16le_at(path, bytes, 4, "PCX bounds available");
    const auto height_min = read_u16le_at(path, bytes, 6, "PCX bounds available");
    const auto width_max = read_u16le_at(path, bytes, 8, "PCX bounds available");
    const auto height_max = read_u16le_at(path, bytes, 10, "PCX bounds available");
    if (width_min != 0 || height_min != 0 || width_max < width_min || height_max < height_min) {
        throw DataError(std::string(path), 4, "PCX bounds are ordered and origin at zero");
    }
    const auto width = static_cast<std::uint32_t>(width_max) + 1;
    const auto height = static_cast<std::uint32_t>(height_max) + 1;
    if (width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max()) {
        throw DataError(std::string(path), 4, "PCX dimensions fit the image metadata");
    }
    const auto planes = std::to_integer<std::uint8_t>(bytes[65]);
    const auto bytes_per_line = read_u16le_at(path, bytes, 66, "PCX scanline width available");
    if (planes != 1 || bytes_per_line < width) {
        throw DataError(std::string(path), 65, "PCX has one plane and padded scanlines");
    }
    const auto palette_marker_offset = bytes.size() - kPcxPaletteSize - 1;
    if (bytes[palette_marker_offset] != std::byte{0x0c}) {
        throw DataError(std::string(path), palette_marker_offset, "PCX palette marker is 0C");
    }
    const auto data_begin = kPcxHeaderSize;
    const auto data_end = palette_marker_offset;
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        static_cast<std::size_t>(width) * height > std::vector<std::byte>().max_size()) {
        throw DataError(std::string(path), 0, "PCX dimensions fit the native pixel buffer");
    }

    PcxImage image{
        static_cast<std::uint16_t>(width),
        static_cast<std::uint16_t>(height),
        bytes_per_line,
        std::vector<std::byte>(static_cast<std::size_t>(width) * height),
        {},
    };
    std::size_t cursor = data_begin;
    for (std::size_t y = 0; y < height; ++y) {
        std::size_t x = 0;
        while (x < bytes_per_line) {
            if (cursor >= data_end) {
                throw DataError(std::string(path), cursor, "PCX scanline data is complete");
            }
            const auto encoded = std::to_integer<std::uint8_t>(bytes[cursor++]);
            std::uint8_t value = encoded;
            std::size_t count = 1;
            if ((encoded & 0xc0) == 0xc0) {
                count = encoded & 0x3f;
                if (count == 0 || cursor >= data_end) {
                    throw DataError(std::string(path), cursor - 1,
                                    "PCX run has a nonzero count and value");
                }
                value = std::to_integer<std::uint8_t>(bytes[cursor++]);
            }
            if (count > bytes_per_line - x) {
                throw DataError(std::string(path), cursor - 1, "PCX run stays within its scanline");
            }
            if (x < width) {
                const auto visible = std::min(count, static_cast<std::size_t>(width) - x);
                std::fill(image.pixels.begin() + static_cast<std::ptrdiff_t>(y * width + x),
                          image.pixels.begin() + static_cast<std::ptrdiff_t>(y * width + x + visible),
                          std::byte{value});
            }
            x += count;
        }
    }
    if (cursor != data_end) {
        throw DataError(std::string(path), cursor, "PCX has no unconsumed image bytes");
    }
    for (std::size_t index = 0; index < image.palette.size(); ++index) {
        const auto palette_offset = palette_marker_offset + 1 + index * 3;
        image.palette[index] = {
            std::to_integer<std::uint8_t>(bytes[palette_offset]),
            std::to_integer<std::uint8_t>(bytes[palette_offset + 1]),
            std::to_integer<std::uint8_t>(bytes[palette_offset + 2]),
        };
    }
    return image;
}



FliHeader parse_fli_header(std::string_view path, std::span<const std::byte> bytes) {
    require_at(path, bytes, 0, 128, "FLI header is available");
    const auto file_size = read_u32le_at(path, bytes, 0, "FLI size is available");
    const auto magic = read_u16le_at(path, bytes, 4, "FLI magic is available");
    if (magic != 0xaf11) {
        throw DataError(std::string(path), 4, "FLI magic is AF11");
    }
    if (file_size != bytes.size()) {
        throw DataError(std::string(path), 0, "FLI header size matches the file size");
    }
    const auto frames = read_u16le_at(path, bytes, 6, "FLI frame count is available");
    const auto width = read_u16le_at(path, bytes, 8, "FLI width is available");
    const auto height = read_u16le_at(path, bytes, 10, "FLI height is available");
    const auto depth = read_u16le_at(path, bytes, 12, "FLI depth is available");
    const auto flags = read_u16le_at(path, bytes, 14, "FLI flags are available");
    const auto speed = read_u16le_at(path, bytes, 16, "FLI speed is available");
    if (frames == 0 || width == 0 || height == 0 || depth != 8) {
        throw DataError(std::string(path), 6, "FLI has nonzero frames and an 8-bit image");
    }
    const auto frame1_offset = read_u32le_at(path, bytes, 0x50,
                                              "FLI frame-1 offset is available");
    const auto frame2_offset = read_u32le_at(path, bytes, 0x54,
                                              "FLI frame-2 offset is available");
    return FliHeader{file_size, frames, width, height, depth, flags, speed,
                     frame1_offset, frame2_offset};
}

namespace {

template <typename SetColour>
std::size_t decode_fli_frame(std::string_view path,
                             std::span<const std::byte> bytes,
                             const FliHeader& header, std::size_t frame_offset,
                             std::span<std::uint8_t> pixels,
                             SetColour&& set_colour, FliStreamMetadata* metadata) {

    auto read_u8 = [&](std::size_t& cursor, std::size_t end,
                       std::string_view invariant) -> std::uint8_t {
        if (cursor >= end) {
            throw DataError(std::string(path), cursor, invariant);
        }
        return std::to_integer<std::uint8_t>(bytes[cursor++]);
    };
    auto read_u16 = [&](std::size_t& cursor, std::size_t end,
                        std::string_view invariant) -> std::uint16_t {
        if (cursor > end || end - cursor < 2) {
            throw DataError(std::string(path), cursor, invariant);
        }
        const auto value = static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(bytes[cursor]) |
            (std::to_integer<std::uint16_t>(bytes[cursor + 1]) << 8));
        cursor += 2;
        return value;
    };
    auto require_chunk_bytes = [&](std::size_t cursor, std::size_t end, std::size_t count,
                                   std::string_view invariant) {
        if (cursor > end || count > end - cursor) {
            throw DataError(std::string(path), cursor, invariant);
        }
    };

    if (frame_offset < bytes.size()) {
        require_at(path, bytes, frame_offset, 16, "FLI frame header is available");
        const auto frame_size = read_u32le_at(path, bytes, frame_offset,
                                              "FLI frame size is available");
        const auto frame_type = read_u16le_at(path, bytes, frame_offset + 4,
                                              "FLI frame type is available");
        if (frame_size < 16 || frame_size > bytes.size() - frame_offset) {
            throw DataError(std::string(path), frame_offset,
                            "FLI frame stays within the file and has a header");
        }
        if (frame_type != 0xf1fa) {
            throw DataError(std::string(path), frame_offset + 4, "FLI frame type is F1FA");
        }
        const auto frame_end = frame_offset + frame_size;
        const auto chunk_count = read_u16le_at(path, bytes, frame_offset + 6,
                                               "FLI chunk count is available");
        std::size_t cursor = frame_offset + 16;
        for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            require_at(path, bytes, cursor, 6, "FLI chunk header is available");
            const auto chunk_offset = cursor;
            const auto chunk_size = read_u32le_at(path, bytes, cursor,
                                                  "FLI chunk size is available");
            const auto chunk_type = read_u16le_at(path, bytes, cursor + 4,
                                                  "FLI chunk type is available");
            if (chunk_size < 6 || chunk_size > bytes.size() - chunk_offset) {
                throw DataError(std::string(path), chunk_offset,
                                "FLI chunk stays within the file and has a header");
            }
            auto chunk_end = chunk_offset + chunk_size;
            if (chunk_end > frame_end) {
                if (chunk_index + 1 == chunk_count && chunk_end == frame_end + 1) {
                    chunk_end = frame_end;
                    if (metadata) ++metadata->one_byte_frame_end_adjustments;
                } else {
                    throw DataError(std::string(path), chunk_offset,
                                    "FLI chunk stays within its frame");
                }
            }
            cursor = chunk_offset + 6;
            if (chunk_type == 0x000b) {
                if (metadata) ++metadata->palette_chunks;
                auto packet_count = read_u16(cursor, chunk_end, "FLI palette packet count is available");
                std::size_t palette_index = 0;
                for (std::size_t packet = 0; packet < packet_count; ++packet) {
                    palette_index += read_u8(cursor, chunk_end, "FLI palette skip is available");
                    const auto count_byte = read_u8(cursor, chunk_end,
                                                    "FLI palette count is available");
                    const auto color_count = count_byte == 0 ? std::size_t{256} : count_byte;
                    if (palette_index > 256 || color_count > 256 - palette_index) {
                        throw DataError(std::string(path), cursor - 1,
                                        "FLI palette packet stays within 256 colors");
                    }
                    require_chunk_bytes(cursor, chunk_end, color_count * 3,
                                        "FLI palette colors are available");
                    for (std::size_t color = 0; color < color_count; ++color) {
                        for (std::size_t component = 0; component < 3; ++component) {
                            if (std::to_integer<std::uint8_t>(bytes[cursor + component]) > 63) {
                                throw DataError(std::string(path), cursor + component,
                                                "FLI COLOR_64 palette channels are six-bit");
                            }
                        }
                        set_colour(palette_index++,
                                   std::to_integer<std::uint8_t>(bytes[cursor]),
                                   std::to_integer<std::uint8_t>(bytes[cursor + 1]),
                                   std::to_integer<std::uint8_t>(bytes[cursor + 2]));
                        cursor += 3;
                    }
                }
            } else if (chunk_type == 0x000c) {
                if (metadata) ++metadata->lc_chunks;
                const auto start_line = read_u16(cursor, chunk_end,
                                                 "FLI LC start line is available");
                const auto line_count = read_u16(cursor, chunk_end,
                                                 "FLI LC line count is available");
                if (start_line > header.height || line_count > header.height - start_line) {
                    throw DataError(std::string(path), cursor - 2,
                                    "FLI LC lines stay within the frame");
                }
                for (std::size_t line = 0; line < line_count; ++line) {
                    const auto packet_count = read_u8(cursor, chunk_end,
                                                      "FLI LC packet count is available");
                    std::size_t x = 0;
                    for (std::size_t packet = 0; packet < packet_count; ++packet) {
                        x += read_u8(cursor, chunk_end, "FLI LC skip is available");
                        if (x > header.width) {
                            throw DataError(std::string(path), cursor - 1,
                                            "FLI LC skip stays within the scanline");
                        }
                        const auto count_byte = read_u8(cursor, chunk_end,
                                                        "FLI LC count is available");
                        const auto signed_count = count_byte < 128 ?
                            static_cast<int>(count_byte) : static_cast<int>(count_byte) - 256;
                        if (signed_count >= 0) {
                            const auto count = static_cast<std::size_t>(signed_count);
                            if (count > header.width - x) {
                                throw DataError(std::string(path), cursor - 1,
                                                "FLI LC literal packet stays within the scanline");
                            }
                            require_chunk_bytes(cursor, chunk_end, count,
                                                "FLI LC literal pixels are available");
                            for (std::size_t pixel = 0; pixel < count; ++pixel) {
                                pixels[(static_cast<std::size_t>(start_line) + line) * header.width +
                                       x++] = std::to_integer<std::uint8_t>(bytes[cursor++]);
                            }
                        } else {
                            const auto count = static_cast<std::size_t>(-signed_count);
                            if (count == 0 || count > header.width - x) {
                                throw DataError(std::string(path), cursor - 1,
                                                "FLI LC repeat packet stays within the scanline");
                            }
                            const auto value = read_u8(cursor, chunk_end,
                                                       "FLI LC repeat pixel is available");
                            std::fill_n(pixels.data() +
                                            (static_cast<std::size_t>(start_line) + line) * header.width + x,
                                        count, value);
                            x += count;
                        }
                    }
                }
            } else if (chunk_type == 0x000f) {
                if (metadata) ++metadata->brun_chunks;
                for (std::size_t line = 0; line < header.height; ++line) {
                    const auto packet_count = read_u8(cursor, chunk_end,
                                                      "FLI BRUN packet count is available");
                    std::size_t x = 0;
                    for (std::size_t packet = 0; packet < packet_count; ++packet) {
                        const auto count_byte = read_u8(cursor, chunk_end,
                                                        "FLI BRUN count is available");
                        const auto signed_count = count_byte < 128 ?
                            static_cast<int>(count_byte) : static_cast<int>(count_byte) - 256;
                        if (signed_count >= 0) {
                            const auto count = static_cast<std::size_t>(signed_count);
                            if (count > header.width - x) {
                                throw DataError(std::string(path), cursor - 1,
                                                "FLI BRUN repeat packet stays within the scanline");
                            }
                            const auto value = read_u8(cursor, chunk_end,
                                                       "FLI BRUN repeat pixel is available");
                            std::fill_n(pixels.data() + line * header.width + x, count, value);
                            x += count;
                        } else {
                            const auto count = static_cast<std::size_t>(-signed_count);
                            if (count == 0 || count > header.width - x) {
                                throw DataError(std::string(path), cursor - 1,
                                                "FLI BRUN literal packet stays within the scanline");
                            }
                            require_chunk_bytes(cursor, chunk_end, count,
                                                "FLI BRUN literal pixels are available");
                            for (std::size_t pixel = 0; pixel < count; ++pixel) {
                                pixels[line * header.width + x++] =
                                    std::to_integer<std::uint8_t>(bytes[cursor++]);
                            }
                        }
                    }
                    if (x != header.width) {
                        throw DataError(std::string(path), cursor,
                                        "FLI BRUN packets fill each scanline");
                    }
                }
            } else {
                throw DataError(std::string(path), chunk_offset + 4,
                                "FLI stream uses a supported palette, LC, or BRUN chunk");
            }
            if (cursor != chunk_end && chunk_type == 0x000c && cursor + 1 == chunk_end &&
                bytes[cursor] == std::byte{0}) {
                if (metadata) ++metadata->one_byte_chunk_padding;
                cursor = chunk_end;
            }
            if (cursor != chunk_end) {
                throw DataError(std::string(path), cursor,
                                "FLI chunk decoder consumes its bounded payload");
            }
            cursor = chunk_end;
        }
        if (cursor != frame_end) {
            throw DataError(std::string(path), cursor,
                            "FLI frame decoder consumes its bounded chunks");
        }
        return frame_end;
    }
    throw DataError(std::string(path), frame_offset,
                    "FLI contains every header-declared frame");
}

} // namespace

FliStreamMetadata validate_fli_stream(std::string_view path,
                                      std::span<const std::byte> bytes,
                                      FliFrameSink on_frame, void* user) {
    const auto header = parse_fli_header(path, bytes);
    const auto pixel_count = static_cast<std::size_t>(header.width) * header.height;
    if (pixel_count > std::vector<std::uint8_t>().max_size()) {
        throw DataError(std::string(path), 8, "FLI dimensions fit the native frame buffer");
    }
    std::vector<std::uint8_t> pixels(pixel_count);
    std::array<std::array<std::uint8_t, 3>, 256> palette{};
    FliStreamMetadata result{header, 0, 0, 0, 0, 0, 0};
    auto set_colour = [&](std::size_t index, std::uint8_t r,
                          std::uint8_t g, std::uint8_t b) {
        palette[index] = {r, g, b};
    };
    std::size_t frame_offset = 128;
    while (frame_offset < bytes.size()) {
        frame_offset = decode_fli_frame(path, bytes, header, frame_offset, pixels,
                                        set_colour, &result);
        if (on_frame != nullptr && result.frame_records < header.frames) {
            on_frame(user, result.frame_records, std::as_bytes(std::span(pixels)), palette);
        }
        ++result.frame_records;
    }
    if (result.frame_records < header.frames) {
        throw DataError(std::string(path), frame_offset,
                        "FLI contains every header-declared frame");
    }
    return result;
}

FliPlayer::FliPlayer(const Assets& assets)
    : assets_(assets), header_(parse_fli_header("ending.fli", assets.ending_bytes)) {
    if (header_.width != IndexedFrame::kWidth) {
        throw DataError("ending.fli", 8, "FLI playback width is 320");
    }
    if (header_.height != IndexedFrame::kHeight) {
        throw DataError("ending.fli", 10, "FLI playback height is 200");
    }
}

bool FliPlayer::next_frame() {
    if (finished()) return false;
    auto set_colour = [this](std::size_t index, std::uint8_t r,
                             std::uint8_t g, std::uint8_t b) {
        frame_.palette[index] = Rgb8{static_cast<std::uint8_t>(r << 2u),
                                     static_cast<std::uint8_t>(g << 2u),
                                     static_cast<std::uint8_t>(b << 2u)};
    };
    next_offset_ = decode_fli_frame("ending.fli", assets_.ending_bytes, header_, next_offset_,
                                    frame_.pixels, set_colour, nullptr);
    ++decoded_frames_;
    return true;
}

void FliPlayer::reset() noexcept {
    frame_.clear();
    frame_.palette.fill(Rgb8{});
    next_offset_ = 128;
    decoded_frames_ = 0;
}

const IndexedFrame& FliPlayer::frame() const {
    if (decoded_frames_ == 0) {
        throw std::logic_error("FLI frame unavailable before the first decoded frame");
    }
    return frame_;
}
std::array<std::uint8_t, 32> compute_required_content_identity() {
    // References, not a second set of fingerprints: the same entries gate load_assets.
    constexpr std::array<const AssetFingerprint*, 20> fingerprints{{
        &kMapFingerprints[0], &kMapFingerprints[1], &kMapFingerprints[2],
        &kWallFingerprints[0], &kWallFingerprints[1], &kWallFingerprints[2],
        &kObjectFingerprints[0], &kObjectFingerprints[1], &kObjectFingerprints[2],
        &kImgFingerprints[0], &kImgFingerprints[1], &kImgFingerprints[2],
        &kUifFingerprint, &kSoundFingerprint,
        &kDemoFingerprints[0], &kDemoFingerprints[1], &kDemoFingerprints[2],
        &kBsfFingerprint, &kPaletteFingerprint, &kEndingFingerprint,
    }};
    constexpr std::size_t capacity = [&] {
        std::size_t size = 8;
        for (const auto* fingerprint : fingerprints) {
            size += 1 + fingerprint->name.size() + 32;
        }
        return size;
    }();
    static_assert(capacity <= std::numeric_limits<std::uint64_t>::max() / 8);
    auto sorted = fingerprints;
    std::sort(sorted.begin(), sorted.end(), [](const auto* lhs, const auto* rhs) {
        return std::lexicographical_compare(
            lhs->name.begin(), lhs->name.end(), rhs->name.begin(), rhs->name.end(),
            [](char a, char b) {
                return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
            });
    });

    std::array<std::byte, capacity> input{};
    constexpr std::array<std::byte, 8> domain{
        std::byte{'N'}, std::byte{'3'}, std::byte{'D'}, std::byte{'W'},
        std::byte{'I'}, std::byte{'N'}, std::byte{'1'}, std::byte{0}};
    std::copy(domain.begin(), domain.end(), input.begin());
    std::size_t offset = domain.size();
    for (const auto* fingerprint : sorted) {
        const auto name = fingerprint->name;
        if (name.empty() || name.size() > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("required asset filename length exceeds one byte");
        }
        input[offset++] = static_cast<std::byte>(name.size());
        for (const unsigned char character : name) {
            if (character > 0x7f) {
                throw std::invalid_argument("required asset filename must be ASCII");
            }
            input[offset++] = static_cast<std::byte>(character);
        }
        const auto digest = parse_hex_sha256(fingerprint->sha256_hex);
        for (const auto byte : digest) {
            input[offset++] = static_cast<std::byte>(byte);
        }
    }
    return detail::sha256(input);
}

Assets load_assets(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw DataError(directory.string(), 0, "asset directory exists");
    }

    Assets assets;
    assets.directory = directory;
    for (std::size_t index = 0; index < assets.episodes.size(); ++index) {
        const auto suffix = std::to_string(index + 1);
        auto& episode = assets.episodes[index];

        const auto map_fingerprint = kMapFingerprints[index];
        const auto wall_fingerprint = kWallFingerprints[index];
        const auto object_fingerprint = kObjectFingerprints[index];
        const auto img_fingerprint = kImgFingerprints[index];

        episode.map_bytes = read_verified_file(directory / std::filesystem::path{map_fingerprint.name},
                                               map_fingerprint.exact_size,
                                               map_fingerprint.sha256_hex);
        episode.wall_bytes = read_verified_file(directory / std::filesystem::path{wall_fingerprint.name},
                                               wall_fingerprint.exact_size,
                                               wall_fingerprint.sha256_hex);
        episode.object_bytes = read_verified_file(directory / std::filesystem::path{object_fingerprint.name},
                                                 object_fingerprint.exact_size,
                                                 object_fingerprint.sha256_hex);
        episode.image_bytes = read_verified_file(directory / std::filesystem::path{img_fingerprint.name},
                                                img_fingerprint.exact_size,
                                                img_fingerprint.sha256_hex);
        episode.map = parse_map_store("MAP." + suffix, episode.map_bytes);
        episode.walls = parse_definition_catalog("WALLS." + suffix, episode.wall_bytes);
        episode.objects = parse_definition_catalog("OBJECTS." + suffix, episode.object_bytes);
        validate_map_viewer_catalogs("MAP." + suffix, episode.map,
                                     "WALLS." + suffix, episode.walls,
                                     "OBJECTS." + suffix, episode.objects);
        episode.images = parse_img_archive("IMG." + suffix, episode.image_bytes);
    }

    assets.uif_bytes = read_verified_file(directory / std::filesystem::path{kUifFingerprint.name},
                                         kUifFingerprint.exact_size,
                                         kUifFingerprint.sha256_hex);
    assets.uif = parse_uif_archive("uif.dat", assets.uif_bytes);

    assets.palette_bytes = read_verified_file(directory / std::filesystem::path{kPaletteFingerprint.name},
                                             kPaletteFingerprint.exact_size,
                                             kPaletteFingerprint.sha256_hex);
    assets.palette = parse_pcx_image("game.pal", assets.palette_bytes);
    for (const auto& image : assets.uif.images) {
        if (image.image.palette != assets.palette.palette) {
            throw DataError("uif.dat", image.offset + image.length - kPcxPaletteSize,
                            "uif image palette matches game.pal");
        }
    }
    assets.sound_bytes = read_verified_file(directory / std::filesystem::path{kSoundFingerprint.name},
                                           kSoundFingerprint.exact_size,
                                           kSoundFingerprint.sha256_hex);
    assets.sound = parse_sound_archive("snd.dat", assets.sound_bytes);
    for (std::size_t index = 0; index < assets.demos.size(); ++index) {
        const auto& fingerprint = kDemoFingerprints[index];
        assets.demo_bytes[index] = read_verified_file(
            directory / std::filesystem::path{fingerprint.name},
            fingerprint.exact_size, fingerprint.sha256_hex);
        assets.demos[index] = parse_demo_file(fingerprint.name, assets.demo_bytes[index]);
    }
    assets.bsf_bytes = read_verified_file(directory / std::filesystem::path{kBsfFingerprint.name},
                                          kBsfFingerprint.exact_size,
                                          kBsfFingerprint.sha256_hex);
    assets.bsf = parse_bsf("nite3d.bsf", assets.bsf_bytes);
    assets.help_image_reference_count = validate_help_image_references(
        assets.bsf.blocks[1], assets.bsf.header.block_offsets[1], assets.episodes);

    assets.ending_bytes = read_verified_file(directory / std::filesystem::path{kEndingFingerprint.name},
                                            kEndingFingerprint.exact_size,
                                            kEndingFingerprint.sha256_hex);
    assets.ending = validate_fli_stream("ending.fli", assets.ending_bytes);
    assets.required_content_identity = compute_required_content_identity();
    return assets;
}

} // namespace n3d
