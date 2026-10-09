/*
 * HeavenBench — deterministic BC5 normal-to-BC4 ground-height generation
 */
#include "dds_loader_cpu.h"
#include "engine/dds_block_codec.h"
#include "engine/ground_relief_generator.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <span>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

template <typename T>
void append_object(std::vector<uint8_t>& output, const T& object) {
    for (std::byte byte : std::as_bytes(std::span(&object, 1))) {
        output.push_back(std::to_integer<uint8_t>(byte));
    }
}

std::vector<uint8_t> constant_bc5_dds(uint8_t x, uint8_t y) {
    heaven::dds_detail::Header header{};
    header.size = sizeof(header);
    header.flags = 0x00081007u;
    header.height = 4;
    header.width = 4;
    header.pitchOrLinearSize = 16;
    header.mipMapCount = 1;
    header.pixelFormat.size = sizeof(heaven::dds_detail::PixelFormat);
    header.pixelFormat.flags = heaven::dds_detail::DDPF_FOURCC;
    header.pixelFormat.fourCC =
        heaven::dds_detail::make_fourcc('A', 'T', 'I', '2');
    header.caps = 0x1000u;
    std::vector<uint8_t> result;
    append_object(result, heaven::dds_detail::DDS_MAGIC);
    append_object(result, header);
    result.insert(result.end(), {x, x, 0, 0, 0, 0, 0, 0,
                                 y, y, 0, 0, 0, 0, 0, 0});
    return result;
}

std::array<uint8_t, 8> bc4_block(
        uint8_t endpoint0, uint8_t endpoint1,
        const std::array<uint8_t, 16>& indices) {
    std::array<uint8_t, 8> result{endpoint0, endpoint1};
    uint64_t packed = 0;
    for (std::size_t index = 0; index < indices.size(); ++index) {
        packed |= static_cast<uint64_t>(indices[index] & 7u) << (index * 3u);
    }
    for (std::size_t byte = 0; byte < 6; ++byte) {
        result[2 + byte] = static_cast<uint8_t>(packed >> (byte * 8u));
    }
    return result;
}

std::vector<uint8_t> patterned_bc5_dds() {
    auto result = constant_bc5_dds(128, 128);
    result.resize(sizeof(uint32_t) + sizeof(heaven::dds_detail::Header));
    const std::array<uint8_t, 16> x_indices{
        4, 5, 5, 4, 4, 5, 5, 4,
        4, 5, 5, 4, 4, 5, 5, 4};
    const std::array<uint8_t, 16> y_indices{
        4, 4, 4, 4, 5, 5, 5, 5,
        5, 5, 5, 5, 4, 4, 4, 4};
    const auto x_block = bc4_block(255, 0, x_indices);
    const auto y_block = bc4_block(255, 0, y_indices);
    result.insert(result.end(), x_block.begin(), x_block.end());
    result.insert(result.end(), y_block.begin(), y_block.end());
    return result;
}

std::array<uint8_t, 16> deepbump_reference_height() {
    // Independent O(N^4) DFT transcription of DeepBump's
    // normals-to-height Frankot-Chellappa stage. Keeping this tiny golden out
    // of pocketfft makes it sensitive to FFT layout, frequency, and sign bugs.
    constexpr std::array<uint8_t, 8> palette{
        255, 0, 218, 182, 145, 109, 72, 36};
    constexpr std::array<uint8_t, 16> x_indices{
        4, 5, 5, 4, 4, 5, 5, 4,
        4, 5, 5, 4, 4, 5, 5, 4};
    constexpr std::array<uint8_t, 16> y_indices{
        4, 4, 4, 4, 5, 5, 5, 5,
        5, 5, 5, 5, 4, 4, 4, 4};
    using Complex = std::complex<double>;
    std::array<double, 16> du{};
    std::array<double, 16> dv{};
    for (std::size_t index = 0; index < du.size(); ++index) {
        const double nx = static_cast<double>(palette[x_indices[index]]) /
                              127.5 - 1.0;
        const double ny = static_cast<double>(palette[y_indices[index]]) /
                              127.5 - 1.0;
        const double nz = std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny));
        du[index] = -nx / std::max(nz, 0.20);
        dv[index] = -ny / std::max(nz, 0.20);
    }

    std::array<Complex, 16> spectrum{};
    for (int ky = 0; ky < 4; ++ky) {
        for (int kx = 0; kx < 4; ++kx) {
            Complex u{};
            Complex v{};
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const double angle = -2.0 * std::numbers::pi *
                        (static_cast<double>(kx * x + ky * y) / 4.0);
                    const Complex phase = std::polar(1.0, angle);
                    const auto index = static_cast<std::size_t>(y) * 4u + static_cast<std::size_t>(x);
                    u += du[index] * phase;
                    v += dv[index] * phase;
                }
            }
            const int signed_x = kx <= 2 ? kx : kx - 4;
            const int signed_y = ky <= 2 ? ky : ky - 4;
            const double frequency_x = 2.0 * std::numbers::pi * signed_x / 4.0;
            const double frequency_y = 2.0 * std::numbers::pi * signed_y / 4.0;
            const double denominator = frequency_x * frequency_x +
                                       frequency_y * frequency_y;
            spectrum[static_cast<std::size_t>(ky) * 4u + static_cast<std::size_t>(kx)] =
                denominator == 0.0 ? Complex{} :
                    Complex{0.0, -1.0} *
                    (frequency_x * u + frequency_y * v) / denominator;
        }
    }

    std::array<double, 16> height{};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            Complex value{};
            for (int ky = 0; ky < 4; ++ky) {
                for (int kx = 0; kx < 4; ++kx) {
                    const double angle = 2.0 * std::numbers::pi *
                        (static_cast<double>(kx * x + ky * y) / 4.0);
                    value += spectrum[static_cast<std::size_t>(ky) * 4u + static_cast<std::size_t>(kx)] *
                             std::polar(1.0, angle);
                }
            }
            height[static_cast<std::size_t>(y) * 4u + static_cast<std::size_t>(x)] = value.real() / 16.0;
        }
    }

    auto sorted = height;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double quantile) {
        const double position = quantile * (sorted.size() - 1);
        const auto low = static_cast<std::size_t>(std::floor(position));
        const auto high = static_cast<std::size_t>(std::ceil(position));
        const double fraction = position - static_cast<double>(low);
        return sorted[low] * (1.0 - fraction) + sorted[high] * fraction;
    };
    const double scale = std::max(std::abs(percentile(0.02)),
                                  std::abs(percentile(0.98)));
    std::array<uint8_t, 16> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double normalized = std::clamp(
            0.5 + height[index] / (2.0 * scale), 0.0, 1.0);
        result[index] = static_cast<uint8_t>(
            std::floor(normalized * 255.0 + 0.5));
    }
    return result;
}

bool unit_ground_relief_generator() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven-ground-generator-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path data = root / "Data_Root";
    fs::create_directories(data / "heaven/textures/ruins");
    const fs::path normal = data / "heaven/textures/ruins/ruins_ground_n.dds";
    const auto source = constant_bc5_dds(128, 128);
    std::ofstream output(normal, std::ios::binary);
    output.write(reinterpret_cast<const char*>(source.data()),
                 static_cast<std::streamsize>(source.size()));
    output.close();

    auto assets = heaven::AssetFileSystem::open(data.string());
    auto paths = heaven::PathResolver::create(data.string());
    if (!assets || !paths) return false;
    auto first = heaven::ensure_ground_height(*paths, *assets, true);
    if (!first || !first->has_value()) return false;
    const auto generated_path = paths->relative_path(
        heaven::kGroundHeightRelativePath);
    const auto dds = heaven::load_dds_cpu(generated_path, std::cref(*assets));
    bool ok = dds && dds.format == heaven::DDSFormatCPU::BC4 &&
              dds.width == 4 && dds.height == 4 && dds.mip_count == 3 &&
              first->value().metadata.deepbump_revision ==
                  heaven::kDeepBumpRevision &&
              first->value().metadata.pocketfft_revision ==
                  heaven::kPocketFftRevision &&
              first->value().metadata.bcdec_revision ==
                  heaven::kBcdecRevision;
    auto mounted = assets->read_all(generated_path);
    ok = ok && mounted && *mounted == *first->value().bytes;

    const std::array<uint8_t, 8> truncated{};
    ok = ok && !heaven::decode_bc4_channel(truncated, 8, 8);

    const std::array<uint8_t, 16> all_indices{
        0, 1, 2, 3, 4, 5, 6, 7,
        7, 6, 5, 4, 3, 2, 1, 0};
    const auto known_block = bc4_block(255, 0, all_indices);
    const auto known_decoded = heaven::decode_bc4_channel(known_block, 4, 4);
    constexpr std::array<uint8_t, 8> expected_palette{
        255, 0, 218, 182, 145, 109, 72, 36};
    if (!known_decoded) return false;
    for (std::size_t index = 0; index < all_indices.size(); ++index) {
        ok = ok && known_decoded->pixels[index] ==
            expected_palette[all_indices[index]];
    }

    auto second_assets = heaven::AssetFileSystem::open(data.string());
    auto second = heaven::ensure_ground_height(*paths, *second_assets, true);
    ok = ok && second && second->has_value() &&
         *second->value().bytes == *first->value().bytes &&
         second->value().metadata.output_fingerprint ==
             first->value().metadata.output_fingerprint;

    const auto patterned_source = patterned_bc5_dds();
    std::ofstream patterned_output(normal, std::ios::binary | std::ios::trunc);
    patterned_output.write(
        reinterpret_cast<const char*>(patterned_source.data()),
        static_cast<std::streamsize>(patterned_source.size()));
    patterned_output.close();
    auto patterned_assets = heaven::AssetFileSystem::open(data.string());
    auto patterned = heaven::ensure_ground_height(*paths, *patterned_assets, true);
    if (!patterned || !patterned->has_value()) return false;
    const auto patterned_dds = heaven::load_dds_cpu(
        generated_path, std::cref(*patterned_assets));
    if (!patterned_dds || patterned_dds.mip_data.empty()) return false;
    const auto decoded_height = heaven::decode_bc4_channel(
        patterned_dds.mip_data.front().bytes(), 4, 4);
    if (!decoded_height) return false;
    const auto reference = deepbump_reference_height();
    for (std::size_t index = 0; index < reference.size(); ++index) {
        // BC4 is deliberately lossy; the direct floating-point reference must
        // nevertheless agree to within one eighth of the 8-bit range.
        ok = ok && std::abs(static_cast<int>(decoded_height->pixels[index]) -
                            static_cast<int>(reference[index])) <= 32;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::fprintf(stderr, "[unit_ground_relief_generator] %s\n",
                 ok ? "PASSED" : "FAILED");
    return ok;
}

const heaven::testing::Register registered(unit_ground_relief_generator);

} // namespace
