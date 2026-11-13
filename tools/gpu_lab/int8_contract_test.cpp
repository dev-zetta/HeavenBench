#include "int8_contract.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("INT8 contract test failed");
}
template<typename Function>
void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected);
}
}

int main() try {
    using heaven::gpu_lab::pack_int8;
    using heaven::gpu_lab::unpack_int8;
    using heaven::gpu_lab::int8_oracle;
    using heaven::gpu_lab::verify_int8;
    const std::array<std::int32_t, 4> extremes{-128, 127, -1, 0};
    const auto packed = pack_int8(extremes);
    require(packed == 0x00ff7f80u);
    for (std::uint32_t lane = 0; lane < 4; ++lane) require(unpack_int8(packed, lane) == extremes[lane]);
    for (std::int32_t value = -128; value <= 127; ++value) {
        const std::array<std::int32_t, 4> lanes{value, value, value, value};
        for (std::uint32_t lane = 0; lane < 4; ++lane) require(unpack_int8(pack_int8(lanes), lane) == value);
    }
    const std::array<std::int32_t, 4> invalid{0, 0, 128, 0};
    rejects([&] { static_cast<void>(pack_int8(invalid)); });
    // Independently written 2x2 A rows and B columns, including sign and limits.
    const std::array<std::int32_t, 8> input{-128, 127, -1, 0, -128, -1, 127, 0};
    const std::array<std::int32_t, 4> exact{16257, -16256, 128, -127};
    verify_int8(int8_oracle(input, 2, 1), exact);
    const std::array<std::int32_t, 4> zero{};
    rejects([&] { verify_int8(zero, exact); });
    auto poison = exact;
    poison.back() = std::numeric_limits<std::int32_t>::min();
    rejects([&] { verify_int8(poison, exact); });
    auto unsigned_result = exact;
    unsigned_result.front() = 128 * 128 + 127 * 255;
    rejects([&] { verify_int8(unsigned_result, exact); });
    rejects([&] { verify_int8(std::span(exact).first(3), exact); });
    rejects([&] { static_cast<void>(int8_oracle(input, 0, 1)); });
    // Independent logical matrix multiplication checks every production output;
    // it does not reuse the packed decode or production oracle's B addressing.
    constexpr std::uint32_t size = 256, batches = 4;
    constexpr std::size_t count = std::size_t{size} * size * batches;
    std::vector<std::int32_t> inputs(count * 2), reference(count);
    for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i] = heaven::gpu_lab::int8_input(static_cast<std::uint32_t>(i));
    std::vector<std::int32_t> b_rows(count);
    for (std::size_t batch = 0; batch < batches; ++batch) for (std::size_t r = 0; r < size; ++r)
        for (std::size_t c = 0; c < size; ++c) b_rows[batch*size*size+r*size+c] = inputs[count+batch*size*size+c*size+r];
    for (std::size_t batch = 0; batch < batches; ++batch) {
        const auto base = batch * size * size;
        for (std::size_t r = 0; r < size; ++r) for (std::size_t k = 0; k < size; ++k)
            for (std::size_t c = 0; c < size; ++c)
                reference[base+r*size+c] += inputs[base+r*size+k] * b_rows[base+k*size+c];
    }
    const auto output = int8_oracle(inputs, size, batches);
    verify_int8(output, reference);
    require(std::ranges::any_of(output, [](auto value) { return value < 0; }));
    require(std::ranges::any_of(output, [](auto value) { return value > 0; }));
    std::vector<std::int32_t> wrong(count);
    rejects([&] { verify_int8(wrong, output); });
    std::printf("INT8 contract: all %zu production outputs, signed extremes, poison/zero/unsigned/coverage negatives passed\n", count);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
