#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace n3d {

class DataError final : public std::runtime_error {
public:
    DataError(std::string path, std::size_t offset, std::string_view invariant);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] const std::string& invariant() const noexcept { return invariant_; }

private:
    std::string path_;
    std::size_t offset_;
    std::string invariant_;
};

class ByteReader final {
public:
    ByteReader(std::string_view path, std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }

    std::uint8_t u8(std::string_view invariant = "uint8 available");
    std::uint16_t u16le(std::string_view invariant = "uint16 little-endian value available");
    std::uint32_t u32le(std::string_view invariant = "uint32 little-endian value available");
    std::uint64_t u64le(std::string_view invariant = "uint64 little-endian value available");
    std::span<const std::byte> bytes(std::size_t count, std::string_view invariant = "byte span available");
    void require(std::size_t count, std::string_view invariant);

private:
    void require_available(std::size_t count, std::string_view invariant) const;

    std::string_view path_;
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

} // namespace n3d
