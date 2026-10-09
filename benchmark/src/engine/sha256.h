/*
 * HeavenBench — small GPU-free SHA-256 helper for release asset validation.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_SHA256_H_
#define HEAVENBENCH_SRC_ENGINE_SHA256_H_

#include "engine/types.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace heaven {

class Sha256 {
public:
    void update(std::span<const std::byte> input) noexcept {
        total_bytes_ += input.size();
        while (!input.empty()) {
            const std::size_t amount =
                std::min(input.size(), block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, input.data(), amount);
            block_size_ += amount;
            input = input.subspan(amount);
            if (block_size_ == block_.size()) {
                transform(block_);
                block_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const uint64_t bit_count = total_bytes_ * 8u;
        block_[block_size_++] = std::byte{0x80};
        if (block_size_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                      block_.end(), std::byte{0});
            transform(block_);
            block_size_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                  block_.begin() + 56, std::byte{0});
        for (int i = 0; i < 8; ++i) {
            block_[63 - i] = std::byte((bit_count >> (i * 8)) & 0xffu);
        }
        transform(block_);

        static constexpr char hex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (std::size_t i = 0; i < state_.size(); ++i) {
            for (int byte = 0; byte < 4; ++byte) {
                const uint8_t value = static_cast<uint8_t>(
                    state_[i] >> (24 - byte * 8));
                out[(i * 4 + byte) * 2] = hex[value >> 4];
                out[(i * 4 + byte) * 2 + 1] = hex[value & 0x0f];
            }
        }
        return out;
    }

private:
    static constexpr std::array<uint32_t, 64> k_ = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };

    static constexpr uint32_t choose(uint32_t x, uint32_t y, uint32_t z) noexcept {
        return (x & y) ^ (~x & z);
    }
    static constexpr uint32_t majority(uint32_t x, uint32_t y, uint32_t z) noexcept {
        return (x & y) ^ (x & z) ^ (y & z);
    }
    static constexpr uint32_t sigma0(uint32_t x) noexcept {
        return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
    }
    static constexpr uint32_t sigma1(uint32_t x) noexcept {
        return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
    }
    static constexpr uint32_t gamma0(uint32_t x) noexcept {
        return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
    }
    static constexpr uint32_t gamma1(uint32_t x) noexcept {
        return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
    }

    void transform(const std::array<std::byte, 64>& block) noexcept {
        std::array<uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(block.data()) + i * 4;
            words[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            words[i] = gamma1(words[i - 2]) + words[i - 7] +
                       gamma0(words[i - 15]) + words[i - 16];
        }

        uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const uint32_t t1 = h + sigma1(e) + choose(e,f,g) + k_[i] + words[i];
            const uint32_t t2 = sigma0(a) + majority(a,b,c);
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }

    std::array<uint32_t, 8> state_ = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };
    std::array<std::byte, 64> block_{};
    std::size_t block_size_ = 0;
    uint64_t total_bytes_ = 0;
};

[[nodiscard]] inline Result<std::string>
sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"failed to open for SHA-256: " + path.string()});
    }
    Sha256 digest;
    std::array<std::byte, static_cast<size_t>(64 * 1024)> bytes{};
    while (input) {
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const std::streamsize count = input.gcount();
        if (count > 0) digest.update(std::span(bytes).first(
            static_cast<std::size_t>(count)));
    }
    if (!input.eof()) {
        return std::unexpected(Error{"failed while hashing: " + path.string()});
    }
    return digest.finish();
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_SHA256_H_
