#include "n3d/winhelp.hpp"

#include "n3d/binary_reader.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace n3d::winhelp {
namespace {

constexpr std::size_t kFileSize = 12812;
constexpr std::string_view kDigest = "ADB27E79EE15BB7F653A17B93B4F8FC12A97D602D5529EA2166DED3FA49E9534";
constexpr std::array<std::string_view, 10> kFileNames{
    "|CONTEXT", "|CTXOMAP", "|FONT", "|KWBTREE", "|KWDATA", "|KWMAP",
    "|Phrases", "|SYSTEM", "|TOPIC", "|TTLBTREE"};

[[noreturn]] void fail(std::string_view path, std::size_t offset, std::string_view reason) {
    throw DataError(std::string(path), offset, reason);
}

std::uint8_t byte(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

struct Cursor final {
    std::string_view path;
    std::span<const std::byte> data;
    std::size_t base = 0;
    std::size_t pos = 0;
    const std::vector<std::size_t>* origins = nullptr;

    std::size_t offset() const { return origins && pos < origins->size() ? (*origins)[pos] : base + pos; }
    void need(std::size_t size, std::string_view reason) const {
        if (pos > data.size() || size > data.size() - pos) fail(path, offset(), reason);
    }
    std::uint8_t u8(std::string_view reason) { need(1, reason); return byte(data, pos++); }
    std::uint16_t u16(std::string_view reason) {
        need(2, reason);
        auto v = static_cast<std::uint16_t>(byte(data, pos) | (byte(data, pos + 1) << 8));
        pos += 2;
        return v;
    }
    std::uint32_t u32(std::string_view reason) {
        need(4, reason);
        std::uint32_t v = byte(data, pos) | (std::uint32_t(byte(data, pos + 1)) << 8) |
                          (std::uint32_t(byte(data, pos + 2)) << 16) |
                          (std::uint32_t(byte(data, pos + 3)) << 24);
        pos += 4;
        return v;
    }
    std::int32_t i32(std::string_view reason) { return static_cast<std::int32_t>(u32(reason)); }
    void skip(std::size_t n, std::string_view reason) { need(n, reason); pos += n; }
    std::span<const std::byte> take(std::size_t n, std::string_view reason) {
        need(n, reason);
        auto result = data.subspan(pos, n);
        pos += n;
        return result;
    }
    std::string zstr(std::string_view reason) {
        const auto begin = pos;
        while (pos < data.size() && byte(data, pos) != 0) ++pos;
        need(1, reason);
        std::string text;
        text.reserve(pos - begin);
        for (auto i = begin; i < pos; ++i) {
            if (byte(data, i) > 0x7f) fail(path, base + i, "unsupported non-ASCII WinHelp text");
            text.push_back(static_cast<char>(byte(data, i)));
        }
        ++pos;
        return text;
    }
};

Cursor slice(std::string_view path, std::span<const std::byte> bytes, std::size_t begin,
             std::size_t length, std::string_view reason) {
    if (begin > bytes.size() || length > bytes.size() - begin) fail(path, begin, reason);
    return {path, bytes.subspan(begin, length), begin};
}

struct Decoded final {
    std::vector<std::byte> data;
    std::vector<std::size_t> origins; // compressed-token file offset for every decoded byte
};

Decoded decompress(std::string_view path, std::span<const std::byte> data,
                   std::size_t base, std::size_t cap) {
    Decoded result;
    Cursor in{path, data, base};
    while (in.pos < data.size()) {
        auto flags = in.u8("LZ77 flags available");
        for (int bit = 0; bit < 8 && in.pos < data.size(); ++bit) {
            const auto token = in.offset();
            std::size_t len = 1;
            std::size_t distance = 0;
            if (flags & (1u << bit)) {
                const auto code = in.u16("LZ77 match pair complete");
                len = (code >> 12) + 3u;
                distance = (code & 0xfffu) + 1u;
                if (distance > result.data.size()) fail(path, token, "LZ77 distance exceeds decoded prefix");
            } else {
                in.need(1, "LZ77 literal available");
            }
            if (len > cap || result.data.size() > cap - len) fail(path, token, "LZ77 output exceeds bound");
            for (std::size_t i = 0; i < len; ++i) {
                result.data.push_back(distance ? result.data[result.data.size() - distance] :
                                      std::byte{byte(data, in.pos)});
                result.origins.push_back(token);
            }
            if (!distance) ++in.pos;
        }
    }
    return result;
}

struct File final { std::size_t header, payload, used; };
using Files = std::map<std::string, File, std::less<>>;

struct Leaf final {
    Cursor entries;
    std::uint16_t count;
    std::size_t consumed_end;
};

Leaf leaf(std::string_view path, std::span<const std::byte> file, File f,
          std::string_view structure, std::uint16_t page_size, std::uint16_t count) {
    auto in = slice(path, file, f.payload, f.used, "WinHelp B-tree file available");
    if (in.u16("B-tree signature") != 0x293b) fail(path, f.payload, "unknown B-tree signature");
    const auto flags = in.u16("B-tree flags");
    if (flags != (structure == "z4" ? 0x0402u : 0x0002u)) fail(path, f.payload + 2, "unsupported B-tree flags");
    if (in.u16("B-tree page size") != page_size) fail(path, f.payload + 4, "unsupported B-tree page size");
    const auto layout = in.take(16, "B-tree structure field");
    if (structure.size() >= layout.size()) fail(path, f.payload + 6, "B-tree structure name too long");
    for (std::size_t i = 0; i < structure.size(); ++i)
        if (byte(layout, i) != static_cast<std::uint8_t>(structure[i]))
            fail(path, f.payload + 6 + i, "unexpected B-tree structure");
    if (byte(layout, structure.size()) != 0) fail(path, f.payload + 6 + structure.size(), "B-tree structure not terminated");
    if (in.u16("B-tree reserved") != 0 || in.u16("B-tree split count") != 0 ||
        in.u16("B-tree root") != 0 || in.u16("B-tree predecessor") != 0xffff ||
        in.u16("B-tree pages") != 1 || in.u16("B-tree levels") != 1 ||
        in.u32("B-tree entry count") != count)
        fail(path, f.payload + 22, "unsupported B-tree layout");
    if (f.used != 38u + page_size) fail(path, f.payload, "B-tree payload/page length mismatch");
    const auto page = in.pos;
    const auto free = in.u16("B-tree leaf free space");
    if (in.u16("B-tree leaf count") != count || in.u16("B-tree previous page") != 0xffff ||
        in.u16("B-tree next page") != 0xffff || free > page_size - 8)
        fail(path, f.payload + page, "invalid B-tree leaf metadata");
    return {in, count, page + page_size - free};
}

void leaf_end(Leaf& page) {
    if (page.entries.pos != page.consumed_end)
        fail(page.entries.path, page.entries.offset(), "B-tree occupied bytes differ from entry sizes");
}

Files container(std::string_view path, std::span<const std::byte> bytes) {
    auto h = slice(path, bytes, 0, 16, "WinHelp file header available");
    if (h.u32("WinHelp magic") != 0x00035f3fu) fail(path, 0, "unknown WinHelp magic");
    const auto directory = h.u32("WinHelp directory offset");
    if (h.i32("WinHelp free-list head") != -1) fail(path, 8, "WinHelp free-list not empty");
    if (h.u32("WinHelp file length") != bytes.size()) fail(path, 12, "WinHelp file size mismatch");
    std::map<std::size_t, File> blocks;
    for (std::size_t at = 16, limit = 0; at < bytes.size(); ++limit) {
        if (limit >= 16) fail(path, at, "too many WinHelp internal blocks");
        auto b = slice(path, bytes, at, 9, "WinHelp internal file header available");
        const auto reserved = b.i32("internal file reserved length");
        const auto used = b.i32("internal file used length");
        if (reserved < 9 || used < 0 || used > reserved - 9 || b.u8("internal file flags") != 0 ||
            static_cast<std::size_t>(reserved) > bytes.size() - at)
            fail(path, at, "invalid WinHelp internal file extent");
        blocks.emplace(at, File{at, at + 9, static_cast<std::size_t>(used)});
        at += static_cast<std::size_t>(reserved);
    }
    const auto dir = blocks.find(directory);
    if (dir == blocks.end()) fail(path, 4, "directory offset does not point to an internal file");
    auto entries = leaf(path, bytes, dir->second, "z4", 1024, 10);
    Files files;
    for (int i = 0; i < 10; ++i) {
        auto name = entries.entries.zstr("directory filename terminated");
        const auto at = entries.entries.offset();
        const auto offset = entries.entries.u32("directory file offset");
        const auto found = blocks.find(offset);
        if (found == blocks.end()) fail(path, at, "directory points outside internal files");
        if (!files.emplace(name, found->second).second) fail(path, at, "duplicate internal filename");
    }
    leaf_end(entries);
    if (blocks.size() != 11 || files.size() != 10) fail(path, directory, "unexpected WinHelp internal-file count");
    std::set<std::size_t> referenced;
    for (const auto name : kFileNames) {
        const auto found = files.find(name);
        if (found == files.end()) fail(path, directory, "required WinHelp internal file missing");
        if (!referenced.emplace(found->second.header).second || found->second.header == directory)
            fail(path, directory, "internal file alias or directory alias");
    }
    return files;
}

std::vector<std::string> phrases(std::string_view path, std::span<const std::byte> bytes, File f) {
    auto in = slice(path, bytes, f.payload, f.used, "phrase table available");
    const auto count = in.u16("phrase count");
    if (count != 89 || in.u16("phrase table version") != 0x0100) fail(path, f.payload, "unsupported phrase table");
    const auto expanded = in.u32("phrase expanded size");
    if (expanded != 542) fail(path, f.payload + 4, "unexpected phrase decoded size");
    std::vector<std::uint16_t> offsets;
    offsets.reserve(count + 1);
    for (std::size_t i = 0; i <= count; ++i) offsets.push_back(in.u16("phrase offset table complete"));
    const std::size_t header_size = 2u * (count + 1u);
    if (offsets[0] != header_size || offsets.back() != header_size + expanded)
        fail(path, f.payload + 8, "phrase boundary offsets inconsistent");
    auto decoded = decompress(path, in.data.subspan(in.pos), in.offset(), 65536);
    if (decoded.data.size() != expanded) fail(path, f.payload + 4, "phrase decompressed length mismatch");
    std::vector<std::string> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (offsets[i] > offsets[i + 1] || offsets[i + 1] > header_size + expanded)
            fail(path, f.payload + 8 + i * 2, "phrase offsets not monotonic/in range");
        std::string phrase;
        for (auto j = offsets[i] - header_size; j < offsets[i + 1] - header_size; ++j) {
            if (byte(decoded.data, j) > 0x7f) fail(path, decoded.origins[j], "unsupported non-ASCII phrase");
            phrase.push_back(static_cast<char>(byte(decoded.data, j)));
        }
        result.push_back(std::move(phrase));
    }
    return result;
}

struct System final { std::string title; std::uint32_t contents = 0; };
System system(std::string_view path, std::span<const std::byte> bytes, File f) {
    auto in = slice(path, bytes, f.payload, f.used, "WinHelp SYSTEM available");
    if (in.u16("SYSTEM signature") != 0x036c || in.u16("SYSTEM minor version") != 21 ||
        in.u16("SYSTEM major version") != 1)
        fail(path, f.payload, "unsupported WinHelp SYSTEM version");
    in.skip(4, "SYSTEM generation time");
    if (in.u16("SYSTEM compression flags") != 4) fail(path, f.payload + 10, "unsupported SYSTEM flags");
    System result;
    std::set<std::uint16_t> seen;
    while (in.pos < in.data.size()) {
        auto field_at = in.offset();
        const auto type = in.u16("SYSTEM record type");
        const auto size = in.u16("SYSTEM record size");
        if (!seen.insert(type).second) fail(path, field_at, "duplicate SYSTEM record");
        const auto data = in.take(size, "SYSTEM record data");
        if (type == 1 || type == 2 || type == 4) {
            auto text = Cursor{path, data, field_at + 4}.zstr("SYSTEM string terminated");
            if (data.empty() || byte(data, data.size() - 1) != 0 || text.size() + 1 != data.size())
                fail(path, field_at + 4, "SYSTEM string not exactly terminated");
            if (type == 1) result.title = std::move(text);
            if (type == 4 && text != "BROWSEBUTTONS()") fail(path, field_at + 4, "unknown SYSTEM configuration command");
        } else if (type == 3) {
            if (size != 4) fail(path, field_at + 2, "CONTENTS size must be four bytes");
            result.contents = Cursor{path, data, field_at + 4}.u32("contents offset");
        } else fail(path, field_at, "unknown SYSTEM record type");
    }
    if (seen != std::set<std::uint16_t>{1, 2, 3, 4} || result.title.empty())
        fail(path, f.payload, "incomplete SYSTEM record set");
    return result;
}

std::vector<Font> fonts(std::string_view path, std::span<const std::byte> bytes, File f) {
    auto in = slice(path, bytes, f.payload, f.used, "FONT file available");
    const auto faces = in.u16("font face count"), descriptors = in.u16("font descriptor count");
    const auto face_offset = in.u16("font face offset"), desc_offset = in.u16("font descriptor offset");
    if (faces != 11 || descriptors != 8 || face_offset != 8 || desc_offset != face_offset + faces * 20u ||
        f.used != desc_offset + descriptors * 11u)
        fail(path, f.payload, "unsupported FONT layout");
    std::vector<std::string> names;
    for (int i = 0; i < faces; ++i) {
        auto slot = in.take(20, "FONT face slot");
        auto name = Cursor{path, slot, f.payload + face_offset + i * 20u}.zstr("FONT face terminated");
        if (name.empty()) fail(path, f.payload + face_offset + i * 20u, "empty FONT face");
        names.push_back(std::move(name));
    }
    std::vector<Font> result;
    for (int i = 0; i < descriptors; ++i) {
        const auto at = in.offset();
        Font font;
        font.attributes = in.u8("FONT attributes");
        font.half_points = in.u8("FONT half-point size");
        font.family = in.u8("FONT family");
        const auto face = in.u16("FONT face index");
        if (face >= names.size() || (font.attributes & ~7u)) fail(path, at, "invalid FONT descriptor");
        font.face = names[face];
        for (auto& channel : font.foreground) channel = in.u8("FONT foreground");
        for (auto& channel : font.background) channel = in.u8("FONT background");
        result.push_back(std::move(font));
    }
    return result;
}

struct Tables final {
    std::vector<std::pair<std::uint32_t, std::string>> titles;
    std::map<std::uint32_t, std::uint32_t> contexts;
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> keywords;
    std::vector<std::uint32_t> mapped;
};

Tables tables(std::string_view path, std::span<const std::byte> bytes, const Files& fs) {
    Tables result;
    auto ttl = leaf(path, bytes, fs.at("|TTLBTREE"), "Lz", 2048, 17);
    for (int i = 0; i < 17; ++i) {
        auto offset = ttl.entries.u32("title virtual offset");
        auto name = ttl.entries.zstr("topic title terminated");
        if (!result.titles.empty() && offset <= result.titles.back().first)
            fail(path, ttl.entries.offset(), "title offsets not strictly increasing");
        result.titles.emplace_back(offset, std::move(name));
    }
    leaf_end(ttl);
    auto ctx = leaf(path, bytes, fs.at("|CONTEXT"), "L4", 2048, 16);
    for (int i = 0; i < 16; ++i) {
        const auto at = ctx.entries.offset();
        const auto hash = ctx.entries.u32("context hash");
        const auto offset = ctx.entries.u32("context target offset");
        if (!result.contexts.emplace(hash, offset).second) fail(path, at, "duplicate context hash");
    }
    leaf_end(ctx);
    auto map = slice(path, bytes, fs.at("|CTXOMAP").payload, fs.at("|CTXOMAP").used, "CTXOMAP available");
    if (map.u16("context ID count") != 8 || map.data.size() != 66)
        fail(path, map.base, "unsupported context ID map");
    for (std::uint32_t id = 1; id <= 8; ++id) {
        if (map.u32("context ID") != id) fail(path, map.offset() - 4, "context IDs not sequential");
        result.mapped.push_back(map.u32("context ID target"));
    }
    auto kwdata = slice(path, bytes, fs.at("|KWDATA").payload, fs.at("|KWDATA").used, "KWDATA available");
    if (kwdata.data.size() != 21 * 4u) fail(path, kwdata.base, "unsupported keyword target count");
    auto kwmap = slice(path, bytes, fs.at("|KWMAP").payload, fs.at("|KWMAP").used, "KWMAP available");
    if (kwmap.u16("KWMAP page count") != 1 || kwmap.u32("KWMAP root") != 0 ||
        kwmap.u16("KWMAP reserved") != 0 || kwmap.data.size() != 8)
        fail(path, kwmap.base, "unsupported KWMAP");
    auto kwtree = leaf(path, bytes, fs.at("|KWBTREE"), "i24", 2048, 19);
    std::vector<bool> seen(21, false);
    for (int i = 0; i < 19; ++i) {
        auto name = kwtree.entries.zstr("keyword terminated");
        const auto count = kwtree.entries.u16("keyword target count");
        const auto at = kwtree.entries.offset();
        const auto from = kwtree.entries.u32("keyword KWDATA offset");
        if (count == 0 || count > 21 || from % 4 || from > kwdata.data.size() ||
            count * 4u > kwdata.data.size() - from)
            fail(path, at, "keyword KWDATA span invalid");
        auto targets = slice(path, kwdata.data, from, count * 4u, "keyword target list");
        targets.base += kwdata.base;
        std::vector<std::uint32_t> values;
        for (int j = 0; j < count; ++j) {
            if (seen[from / 4 + j]) fail(path, at, "overlapping keyword target spans");
            seen[from / 4 + j] = true;
            values.push_back(targets.u32("keyword target virtual offset"));
        }
        result.keywords.emplace_back(std::move(name), std::move(values));
    }
    leaf_end(kwtree);
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
        fail(path, kwdata.base, "unreferenced KWDATA target");
    return result;
}

std::string expand(std::string_view path, std::span<const std::byte> coded, std::size_t origin,
                   const std::vector<std::string>& dictionary, std::size_t cap) {
    std::string text;
    for (std::size_t i = 0; i < coded.size(); ++i) {
        const auto b = byte(coded, i);
        if (b > 0 && b <= 0x0f) {
            if (i + 1 >= coded.size()) fail(path, origin, "truncated phrase token");
            const std::size_t token = (std::size_t(b - 1) << 8) | byte(coded, ++i);
            const auto index = token >> 1;
            if (index >= dictionary.size()) fail(path, origin, "phrase index outside table");
            if (dictionary[index].size() + (token & 1) > cap - text.size())
                fail(path, origin, "expanded phrase text exceeds bound");
            text += dictionary[index];
            if (token & 1) text += ' ';
        } else {
            if (b > 0x7f) fail(path, origin, "unsupported non-ASCII topic character");
            if (text.size() == cap) fail(path, origin, "expanded topic text exceeds bound");
            text.push_back(static_cast<char>(b));
        }
    }
    return text;
}

struct Record final {
    std::uint32_t pos;
    std::int32_t previous, next;
    std::uint8_t type;
    std::uint32_t data_len2;
    std::size_t source;
    std::span<const std::byte> one, two;
    std::vector<std::size_t> one_origin, two_origin;
};

struct TopicBlock final { Decoded decoded; std::size_t header, first, last, last_header; };

std::vector<TopicBlock> blocks(std::string_view path, std::span<const std::byte> bytes, File file) {
    std::vector<TopicBlock> result;
    for (std::size_t start = 0; start < file.used; start += 4096) {
        const auto size = std::min<std::size_t>(4096, file.used - start);
        auto in = slice(path, bytes, file.payload + start, size, "TOPIC block available");
        if (size < 12) fail(path, in.base, "TOPIC block header truncated");
        auto previous = in.i32("TOPIC previous-link block pointer");
        auto first = in.i32("TOPIC first-link block pointer");
        auto last = in.i32("TOPIC last-header block pointer");
        if (first < 0 || last < 0 || (previous < -1)) fail(path, in.base, "invalid TOPIC block header");
        auto decoded = decompress(path, in.data.subspan(12), in.base + 12, 0x4000 - 12);
        result.push_back({std::move(decoded), in.base, static_cast<std::size_t>(first),
                          static_cast<std::size_t>(previous), static_cast<std::size_t>(last)});
    }
    if (result.size() != 2 || result[0].first != 0x0c || result[0].last != static_cast<std::size_t>(-1) ||
        result[1].first != 0x400c || result[1].last != 0x13b2 ||
        result[0].last_header != 0 || result[1].last_header != 0x1372)
        fail(path, file.payload, "unsupported TOPIC block links");
    return result;
}

std::vector<Record> records(std::string_view path, const std::vector<TopicBlock>& all) {
    std::vector<Record> result;
    std::int32_t current = 0x0c;
    std::int32_t prior = -1;
    std::array<std::size_t, 2> occupied{};
    while (current != -1) {
        if (result.size() >= 35) fail(path, all[0].header, "TOPIC chain exceeds record bound");
        const auto block = static_cast<std::uint32_t>(current) >> 14;
        const auto within = static_cast<std::uint32_t>(current) & 0x3fff;
        if (block >= all.size() || within < 12) fail(path, all[0].header, "TOPIC record position outside blocks");
        const auto& decoded = all[block].decoded;
        const auto off = static_cast<std::size_t>(within - 12);
        if (off >= decoded.data.size()) fail(path, all[block].header, "TOPIC record offset outside decoded block");
        Cursor in{path, decoded.data, all[block].header + 12, off, &decoded.origins};
        const auto source = in.offset();
        const auto size = in.i32("TOPIC record length");
        const auto text_size = in.i32("TOPIC expanded data length");
        const auto back = in.i32("TOPIC previous record");
        const auto next = in.i32("TOPIC next record");
        const auto data1 = in.i32("TOPIC first data length");
        const auto type = in.u8("TOPIC record type");
        if (size < 21 || text_size < 0 || text_size > 16384 || data1 < 21 || data1 > size ||
            static_cast<std::size_t>(size) > decoded.data.size() - off)
            fail(path, source, "invalid TOPIC record extents");
        if (type != 0x02 && type != 0x20)
            fail(path, decoded.origins[in.pos - 1], "unknown TOPIC record type");
        if (back != prior || (next != -1 && next <= current)) fail(path, source, "TOPIC record chain is not forward/reciprocal");
        const auto one_at = in.pos;
        auto one = in.take(data1 - 21, "TOPIC LinkData1");
        const auto two_at = in.pos;
        auto two = in.take(size - data1, "TOPIC LinkData2");
        result.push_back({static_cast<std::uint32_t>(current), back, next, type,
                          static_cast<std::uint32_t>(text_size), source, one, two,
                          {decoded.origins.begin() + one_at, decoded.origins.begin() + two_at},
                          {decoded.origins.begin() + two_at, decoded.origins.begin() + in.pos}});
        if (off != occupied[block]) fail(path, source, "gap or overlap in TOPIC record chain");
        occupied[block] = off + static_cast<std::size_t>(size);
        prior = current;
        current = next;
    }
    if (result.size() != 35 || result.back().pos != 0x400c)
        fail(path, all[0].header, "unsupported TOPIC record count/terminator");
    if (occupied[1] != all[1].decoded.data.size() ||
        all[0].decoded.data.size() - occupied[0] != 22)
        fail(path, all[0].header, "unaccounted TOPIC block output");
    for (auto i = occupied[0]; i < all[0].decoded.data.size(); ++i)
        if (byte(all[0].decoded.data, i) != 0)
            fail(path, all[0].decoded.origins[i], "nonzero TOPIC block padding");
    return result;
}

struct TopicMeta final {
    std::uint32_t start, size;
    std::int32_t back, forward;
    std::uint32_t scroll, next;
    std::size_t source;
};

std::size_t target_topic(std::string_view path, std::size_t at, std::uint32_t virtual_offset,
                         const std::vector<TopicMeta>& meta) {
    for (std::size_t i = meta.size(); i-- > 0;)
        if (virtual_offset >= meta[i].start && virtual_offset < meta[i].start + meta[i].size) return i;
    fail(path, at, "virtual topic target outside real topics");
}

std::size_t hash_topic(std::string_view path, std::size_t at, std::uint32_t hash,
                       const Tables& tables, const std::vector<TopicMeta>& meta) {
    const auto found = tables.contexts.find(hash);
    if (found == tables.contexts.end()) fail(path, at, "hotspot has dangling context hash");
    return target_topic(path, at, found->second, meta);
}

void append_text(Paragraph& paragraph, std::uint16_t font, HotspotKind kind,
                 std::optional<std::size_t> target, std::string text, bool tab = false) {
    if (text.empty() && !tab) return;
    if (!paragraph.runs.empty()) {
        auto& last = paragraph.runs.back();
        if (!last.tab && !tab && last.font_index == font && last.hotspot == kind &&
            last.target_topic == target) { last.text += text; return; }
    }
    paragraph.runs.push_back({font, std::move(text), kind, target, tab});
}

void paragraph(std::string_view path, const Record& record, Topic& topic,
               const std::vector<std::string>& dictionary, const std::vector<Font>& font_table,
               const Tables& tables, const std::vector<TopicMeta>& meta,
               std::array<std::size_t, 2>* hotspot_counts = nullptr) {
    Cursor cmd{path, record.one, record.source, 0, &record.one_origin};
    const auto size_at = cmd.offset();
    const auto initial = cmd.u16("paragraph compressed size");
    if ((initial & 1u) != 0 || initial < 0x8000)
        fail(path, size_at, "unsupported long paragraph-size encoding");
    const auto remaining = (initial >> 1) - 0x4000;
    const auto length_at = cmd.offset();
    const auto b = cmd.u8("paragraph compressed text length");
    const std::uint32_t text_length = (b & 1u)
        ? (std::uint32_t(b | (std::uint32_t(cmd.u8("paragraph text length continuation")) << 8)) >> 1)
        : b >> 1;
    if (remaining != record.one.size() - cmd.pos)
        fail(path, size_at, "paragraph command length mismatch");
    if (text_length != record.data_len2) fail(path, length_at, "paragraph expanded text length mismatch");
    constexpr std::array<std::uint8_t, 6> kKnownParagraphHeader{0, 0x80, 0, 0, 0, 0};
    for (auto expected : kKnownParagraphHeader) {
        const auto at = cmd.offset();
        if (cmd.u8("paragraph header flags") != expected) fail(path, at, "unsupported paragraph attributes");
    }
    std::string text = expand(path, record.two, record.two_origin.empty() ? record.source : record.two_origin[0],
                              dictionary, record.data_len2);
    if (text.size() != record.data_len2) fail(path, record.source, "paragraph expanded byte count mismatch");
    if (topic.paragraphs.empty()) topic.paragraphs.emplace_back();
    else if (!topic.paragraphs.back().runs.empty()) topic.paragraphs.emplace_back();
    std::size_t read = 0;
    std::uint16_t font = 0;
    HotspotKind kind = HotspotKind::None;
    std::optional<std::size_t> target;
    bool ended = false;
    while (cmd.pos < cmd.data.size()) {
        const auto op_at = cmd.offset();
        const auto op = cmd.u8("paragraph command");
        if (ended) fail(path, op_at, "command after paragraph terminator");
        const auto start = read;
        while (read < text.size() && text[read] != '\0') ++read;
        if (read == text.size()) fail(path, op_at, "missing text string before paragraph command");
        append_text(topic.paragraphs.back(), font, kind, target, text.substr(start, read - start));
        ++read;
        switch (op) {
        case 0x80: {
            const auto at = cmd.offset();
            font = cmd.u16("font-change index");
            if (font >= font_table.size()) fail(path, at, "font-change index outside descriptors");
            break;
        }
        case 0x82:
            if (kind != HotspotKind::None) fail(path, op_at, "paragraph break inside hotspot");
            topic.paragraphs.emplace_back();
            break;
        case 0x83:
            append_text(topic.paragraphs.back(), font, kind, target, {}, true);
            break;
        case 0xe2:
        case 0xe3: {
            if (kind != HotspotKind::None) fail(path, op_at, "nested hotspot");
            const auto at = cmd.offset();
            const auto hash = cmd.u32("hotspot context hash");
            target = hash_topic(path, at, hash, tables, meta);
            kind = op == 0xe2 ? HotspotKind::Popup : HotspotKind::Jump;
            if (hotspot_counts) ++(*hotspot_counts)[op == 0xe2 ? 0 : 1];
            break;
        }
        case 0x89:
            if (kind == HotspotKind::None) fail(path, op_at, "unmatched hotspot end");
            kind = HotspotKind::None;
            target.reset();
            break;
        case 0xff:
            if (kind != HotspotKind::None) fail(path, op_at, "unclosed hotspot");
            ended = true;
            break;
        default: fail(path, op_at, "unknown WinHelp paragraph command");
        }
    }
    if (!ended || read != text.size()) fail(path, record.source, "paragraph command/string count mismatch");
}

} // namespace

namespace detail {
std::vector<std::byte> decode_lz77(std::string_view path, std::span<const std::byte> compressed,
                                    std::size_t absolute_start, std::size_t max_output) {
    return decompress(path, compressed, absolute_start, max_output).data;
}

std::vector<Paragraph> parse_paragraph_fixture(std::string_view path,
    std::span<const std::byte> link_data1, std::span<const std::byte> link_data2,
    std::size_t absolute_start, std::uint32_t known_hash) {
    std::vector<std::size_t> one_sources, two_sources;
    for (std::size_t i = 0; i < link_data1.size(); ++i) one_sources.push_back(absolute_start + i);
    for (std::size_t i = 0; i < link_data2.size(); ++i) two_sources.push_back(absolute_start + link_data1.size() + i);
    const Record record{0, -1, -1, 0x20, static_cast<std::uint32_t>(link_data2.size()),
                        absolute_start, link_data1, link_data2, std::move(one_sources), std::move(two_sources)};
    Tables known;
    known.contexts.emplace(known_hash, 0);
    const std::vector<TopicMeta> topics{{0, 1, -1, -1, 0, 0, absolute_start}};
    Topic topic;
    paragraph(path, record, topic, {}, std::vector<Font>(8), known, topics);
    return topic.paragraphs;
}

Document parse_maps_help_structure(std::string_view path, std::span<const std::byte> bytes) {
    const auto files = container(path, bytes);
    const auto dictionary = phrases(path, bytes, files.at("|Phrases"));
    const auto sys = system(path, bytes, files.at("|SYSTEM"));
    Document doc;
    doc.title = sys.title;
    doc.fonts = fonts(path, bytes, files.at("|FONT"));
    const auto index = tables(path, bytes, files);
    const auto topic_blocks = blocks(path, bytes, files.at("|TOPIC"));
    const auto links = records(path, topic_blocks);
    std::vector<TopicMeta> meta;
    std::uint32_t virtual_pos = 0;
    std::size_t first_record = 0;
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto& link = links[i];
        if (link.type != 0x02) continue;
        if (link.one.size() != 28) fail(path, link.source, "unsupported TOPICHEADER data length");
        Cursor in{path, link.one, link.source, 0, &link.one_origin};
        const auto extent = in.i32("topic record span");
        const auto back = in.i32("topic browse backward");
        const auto forward = in.i32("topic browse forward");
        const auto number = in.i32("topic ordinal");
        const auto non_scroll = in.i32("topic non-scroll position");
        const auto scroll = in.i32("topic scroll position");
        const auto next = in.i32("next TOPICHEADER position");
        if (extent < 0 || number != static_cast<std::int32_t>(meta.size()) || non_scroll != -1 ||
            back < -1 || forward < -1 || next < -1 || scroll < -1)
            fail(path, link.source, "invalid TOPICHEADER fields");
        std::uint32_t size = 0;
        std::size_t paragraph_count = 0;
        for (auto j = i + 1; j < links.size() && links[j].type != 0x02; ++j) {
            size += links[j].data_len2;
            ++paragraph_count;
        }
        std::uint32_t record_total = 0;
        for (auto j = i; j < links.size() && (j == i || links[j].type != 0x02); ++j)
            record_total += static_cast<std::uint32_t>(links[j].one.size() + links[j].two.size() + 21);
        auto decoded_title = expand(path, link.two, link.two_origin.empty() ? link.source : link.two_origin[0],
                                    dictionary, 1024);
        if (decoded_title.size() != link.data_len2 || (paragraph_count && decoded_title.empty()) ||
            (meta.size() < 16 && extent != static_cast<std::int32_t>(record_total)))
            fail(path, link.source, "TOPICHEADER title/record span mismatch");
        if (meta.size() < 16) {
            if (paragraph_count == 0 || i + paragraph_count + 1 >= links.size() ||
                scroll != static_cast<std::int32_t>(links[i + 1].pos) ||
                next != static_cast<std::int32_t>(links[i + paragraph_count + 1].pos))
                fail(path, link.source, "TOPICHEADER paragraph/next pointers inconsistent");
            if (index.titles[meta.size()].first != virtual_pos || index.titles[meta.size()].second != decoded_title)
                fail(path, link.source, "TTLBTREE does not match TOPICHEADER");
            doc.topics.push_back({std::move(decoded_title), virtual_pos});
        } else {
            if (meta.size() != 16 || i != links.size() - 1 || paragraph_count || extent != 24 ||
                next != -1 || scroll != -1 || back != -1 || forward != -1 ||
                !decoded_title.empty() || index.titles[16].first != 0x8000 ||
                !index.titles[16].second.empty())
                fail(path, link.source, "unexpected terminal TOPIC sentinel");
        }
        meta.push_back({virtual_pos, size, back, forward, static_cast<std::uint32_t>(scroll),
                        static_cast<std::uint32_t>(next), link.source});
        if (meta.size() <= 16) virtual_pos += size;
        first_record = i;
    }
    if (meta.size() != 17 || doc.topics.size() != 16 || first_record != links.size() - 1 ||
        virtual_pos != 5555)
        fail(path, files.at("|TOPIC").payload, "unsupported real-topic/sentinel counts");
    auto resolved = [&](std::uint32_t offset, std::size_t at) { return target_topic(path, at, offset, meta); };
    doc.contents_topic = resolved(sys.contents, files.at("|SYSTEM").payload);
    for (std::size_t i = 0; i < doc.topics.size(); ++i) {
        const auto& m = meta[i];
        auto& topic = doc.topics[i];
        if (m.back != -1) topic.browse_previous = resolved(static_cast<std::uint32_t>(m.back), m.source);
        if (m.forward != -1) topic.browse_next = resolved(static_cast<std::uint32_t>(m.forward), m.source);
        if (topic.browse_previous || topic.browse_next) doc.browse_chain.push_back(i);
        if (index.mapped.size() > i && resolved(index.mapped[i], files.at("|CTXOMAP").payload) != i)
            fail(path, files.at("|CTXOMAP").payload, "context ID points at wrong topic");
    }
    if (doc.browse_chain.size() != 8)
        fail(path, files.at("|TOPIC").payload, "unsupported browse chain length");
    for (std::size_t i = 0; i < doc.browse_chain.size(); ++i) {
        const auto topic = doc.browse_chain[i];
        if ((i ? std::optional<std::size_t>{doc.browse_chain[i - 1]} : std::optional<std::size_t>{}) !=
                doc.topics[topic].browse_previous ||
            (i + 1 < doc.browse_chain.size() ? std::optional<std::size_t>{doc.browse_chain[i + 1]} :
                                                  std::optional<std::size_t>{}) != doc.topics[topic].browse_next)
            fail(path, meta[topic].source, "browse chain not reciprocal and linear");
    }
    for (const auto& [hash, offset] : index.contexts)
        (void)resolved(offset, files.at("|CONTEXT").payload);
    for (const auto& [name, offsets] : index.keywords) {
        Keyword keyword{name};
        for (const auto offset : offsets) keyword.topics.push_back(resolved(offset, files.at("|KWDATA").payload));
        doc.keywords.push_back(std::move(keyword));
    }
    std::size_t topic_index = 0;
    std::size_t paragraph_count = 0;
    std::array<std::size_t, 2> hotspot_counts{};
    for (const auto& link : links) {
        if (link.type == 0x02) {
            if (link.pos != 0x400c && paragraph_count && topic_index < doc.topics.size()) ++topic_index;
            continue;
        }
        if (topic_index >= doc.topics.size()) fail(path, link.source, "paragraph after terminal sentinel");
        paragraph(path, link, doc.topics[topic_index], dictionary, doc.fonts, index, meta, &hotspot_counts);
        ++paragraph_count;
    }
    if (paragraph_count != 18 || hotspot_counts != std::array<std::size_t, 2>{12, 15})
        fail(path, files.at("|TOPIC").payload, "unexpected paragraph/hotspot record count");
    return doc;
}
} // namespace detail

Document parse_maps_help(std::string_view path, std::span<const std::byte> bytes) {
    if (bytes.size() != kFileSize) fail(path, bytes.size(), "unsupported maps.hlp file size");
    const auto hash = n3d::detail::sha256(bytes);
    constexpr auto hex = "0123456789ABCDEF";
    for (std::size_t i = 0; i < hash.size(); ++i)
        if (kDigest[2 * i] != hex[hash[i] >> 4] || kDigest[2 * i + 1] != hex[hash[i] & 0xf])
            fail(path, 0, "unsupported maps.hlp SHA-256 identity");
    return detail::parse_maps_help_structure(path, bytes);
}

Document load_maps_help(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) fail(path.string(), 0, "cannot open maps.hlp");
    const auto length = file.tellg();
    if (length != static_cast<std::streamoff>(kFileSize))
        fail(path.string(), length < 0 ? 0 : static_cast<std::size_t>(length), "unsupported maps.hlp file size");
    file.seekg(0);
    std::vector<std::byte> bytes(kFileSize);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size()))
        fail(path.string(), static_cast<std::size_t>(file.gcount()), "maps.hlp truncated while reading");
    return parse_maps_help(path.string(), bytes);
}

} // namespace n3d::winhelp
