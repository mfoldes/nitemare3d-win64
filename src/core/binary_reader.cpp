#include "n3d/binary_reader.hpp"

#include <string>
#include <utility>

namespace n3d {
namespace {

std::string make_error_message(std::string_view path, std::size_t offset, std::string_view invariant) {
    std::string message;
    message.reserve(path.size() + invariant.size() + 48);
    message.append("data error in ");
    message.append(path);
    message.append(" at byte ");
    message.append(std::to_string(offset));
    message.append(": ");
    message.append(invariant);
    return message;
}

} // namespace

DataError::DataError(std::string path, std::size_t offset, std::string_view invariant)
    : std::runtime_error(make_error_message(path, offset, invariant)),
      path_(std::move(path)),
      offset_(offset),
      invariant_(invariant) {}

ByteReader::ByteReader(std::string_view path, std::span<const std::byte> bytes) noexcept
    : path_(path), bytes_(bytes) {}

void ByteReader::require_available(std::size_t count, std::string_view invariant) const {
    if (count > bytes_.size() - offset_) {
        throw DataError(std::string(path_), offset_, invariant);
    }
}

void ByteReader::require(std::size_t count, std::string_view invariant) {
    require_available(count, invariant);
    offset_ += count;
}

std::uint8_t ByteReader::u8(std::string_view invariant) {
    require_available(1, invariant);
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
}

std::uint16_t ByteReader::u16le(std::string_view invariant) {
    require_available(2, invariant);
    const auto first = std::to_integer<std::uint16_t>(bytes_[offset_]);
    const auto second = std::to_integer<std::uint16_t>(bytes_[offset_ + 1]);
    offset_ += 2;
    return static_cast<std::uint16_t>(first | (second << 8));
}

std::uint32_t ByteReader::u32le(std::string_view invariant) {
    require_available(4, invariant);
    const auto first = std::to_integer<std::uint32_t>(bytes_[offset_]);
    const auto second = std::to_integer<std::uint32_t>(bytes_[offset_ + 1]);
    const auto third = std::to_integer<std::uint32_t>(bytes_[offset_ + 2]);
    const auto fourth = std::to_integer<std::uint32_t>(bytes_[offset_ + 3]);
    offset_ += 4;
    return first | (second << 8) | (third << 16) | (fourth << 24);
}

std::uint64_t ByteReader::u64le(std::string_view invariant) {
    require_available(8, invariant);
    const auto first = std::to_integer<std::uint64_t>(bytes_[offset_]);
    const auto second = std::to_integer<std::uint64_t>(bytes_[offset_ + 1]);
    const auto third = std::to_integer<std::uint64_t>(bytes_[offset_ + 2]);
    const auto fourth = std::to_integer<std::uint64_t>(bytes_[offset_ + 3]);
    const auto fifth = std::to_integer<std::uint64_t>(bytes_[offset_ + 4]);
    const auto sixth = std::to_integer<std::uint64_t>(bytes_[offset_ + 5]);
    const auto seventh = std::to_integer<std::uint64_t>(bytes_[offset_ + 6]);
    const auto eighth = std::to_integer<std::uint64_t>(bytes_[offset_ + 7]);
    offset_ += 8;
    return first | (second << 8) | (third << 16) | (fourth << 24) |
           (fifth << 32) | (sixth << 40) | (seventh << 48) | (eighth << 56);
}

std::span<const std::byte> ByteReader::bytes(std::size_t count, std::string_view invariant) {
    require_available(count, invariant);
    const auto result = bytes_.subspan(offset_, count);
    offset_ += count;
    return result;
}

} // namespace n3d
