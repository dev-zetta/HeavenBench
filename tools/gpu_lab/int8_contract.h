#ifndef HEAVEN_GPU_LAB_INT8_CONTRACT_H_
#define HEAVEN_GPU_LAB_INT8_CONTRACT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace heaven::gpu_lab {

// The GPU reference, packed dot and cooperative implementations consume the
// same bytes: A is row-major and B is column-major, with four signed bytes per
// little-endian uint32. Only the cooperative shader needs 8-bit storage/types.
inline std::int32_t int8_input(std::uint32_t index) {
    constexpr std::array<std::int32_t, 8> edges{-128, 127, -127, 126, -1, 0, 1, 64};
    if ((index & 63u) < edges.size()) return edges[index & 7u];
    auto value = index + 0x12345u;
    value = (value ^ (value >> 16)) * 0x7feb352du;
    value = (value ^ (value >> 15)) * 0x846ca68bu;
    return static_cast<std::int32_t>((value ^ (value >> 16)) & 255u) - 128;
}

inline std::uint32_t pack_int8(std::span<const std::int32_t, 4> values) {
    std::uint32_t result = 0;
    for (std::size_t lane = 0; lane < values.size(); ++lane) {
        if (values[lane] < -128 || values[lane] > 127) throw std::runtime_error("INT8 packing range exceeded");
        result |= (static_cast<std::uint32_t>(values[lane]) & 255u) << (lane * 8);
    }
    return result;
}

inline std::int32_t unpack_int8(std::uint32_t packed, std::uint32_t lane) {
    const auto value = (packed >> (lane * 8)) & 255u;
    return value < 128 ? static_cast<std::int32_t>(value) : static_cast<std::int32_t>(value) - 256;
}

inline std::vector<std::int32_t> int8_oracle(std::span<const std::int32_t> inputs,
                                           std::uint32_t size, std::uint32_t batches) {
    const auto count = std::size_t{size} * size * batches;
    if (!size || size > 4096 || !batches || batches > 256 || inputs.size() != count * 2)
        throw std::runtime_error("Invalid INT8 oracle workload");
    for (const auto value : inputs) if (value < -128 || value > 127)
        throw std::runtime_error("INT8 oracle input range exceeded");
    std::vector<std::int32_t> expected(count);
    for (std::uint32_t batch = 0; batch < batches; ++batch) {
        const auto base = std::size_t{batch} * size * size;
        for (std::size_t row = 0; row < size; ++row) for (std::size_t column = 0; column < size; ++column) {
            std::int64_t sum = 0;
            for (std::size_t k = 0; k < size; ++k)
                sum += std::int64_t{inputs[base + row * size + k]} * inputs[count + base + column * size + k];
            if (sum < std::numeric_limits<std::int32_t>::min() || sum > std::numeric_limits<std::int32_t>::max())
                throw std::runtime_error("INT32 accumulator range exceeded");
            expected[base + row * size + column] = static_cast<std::int32_t>(sum);
        }
    }
    return expected;
}

inline void verify_int8(std::span<const std::int32_t> actual, std::span<const std::int32_t> expected) {
    if (actual.size() != expected.size()) throw std::runtime_error("INT32 output coverage differs");
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (actual[i] != expected[i]) throw std::runtime_error("GPU INT32 output disagrees with independent INT64 CPU oracle at element " + std::to_string(i));
}

} // namespace heaven::gpu_lab
#endif
