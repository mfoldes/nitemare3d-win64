#include "sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace n3d::detail {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u}};

std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
}

void transform(std::array<std::uint32_t, 8>& state, const std::array<std::byte, 64>& block) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4;
        schedule[index] = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[offset])) << 24) |
                          (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[offset + 1])) << 16) |
                          (static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[offset + 2])) << 8) |
                          static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[offset + 3]));
    }

    for (std::size_t index = 16; index < schedule.size(); ++index) {
        const auto s0 = rotr(schedule[index - 15], 7u) ^ rotr(schedule[index - 15], 18u) ^
                        (schedule[index - 15] >> 3u);
        const auto s1 = rotr(schedule[index - 2], 17u) ^ rotr(schedule[index - 2], 19u) ^
                        (schedule[index - 2] >> 10u);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];

    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const auto s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const auto ch = (e & f) ^ ((~e) & g);
        const auto temp1 = h + s1 + ch + kRoundConstants[index] + schedule[index];
        const auto s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

std::array<std::uint8_t, 32> sha256(std::span<const std::byte> bytes) {
    std::array<std::uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<std::byte, 64> block{};
    std::size_t block_index = 0;
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8u) {
        throw std::length_error("SHA-256 input exceeds the 64-bit bit-length limit");
    }
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8u;

    for (const auto byte : bytes) {
        block[block_index++] = byte;
        if (block_index == block.size()) {
            transform(state, block);
            block_index = 0;
        }
    }

    block[block_index++] = std::byte{0x80};
    if (block_index > 56) {
        while (block_index < block.size()) {
            block[block_index++] = std::byte{0};
        }
        transform(state, block);
        block_index = 0;
    }
    while (block_index < 56) {
        block[block_index++] = std::byte{0};
    }
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>(56u - 8u * index);
        block[56 + index] = std::byte{static_cast<unsigned char>((bit_length >> shift) & 0xffu)};
    }
    transform(state, block);

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        const auto word = state[index];
        digest[index * 4] = static_cast<std::uint8_t>((word >> 24) & 0xffu);
        digest[index * 4 + 1] = static_cast<std::uint8_t>((word >> 16) & 0xffu);
        digest[index * 4 + 2] = static_cast<std::uint8_t>((word >> 8) & 0xffu);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(word & 0xffu);
    }
    return digest;
}

} // namespace n3d::detail
