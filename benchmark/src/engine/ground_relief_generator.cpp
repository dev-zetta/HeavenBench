/*
 * HeavenBench — deterministic DeepBump-inspired ground-height generation
 *
 * The Frankot-Chellappa projection is adapted from DeepBump's GPL-3.0
 * module_normals_to_height.py at kDeepBumpRevision. This implementation uses
 * the renderer's tangent-normal and UV conventions, bounded DDS decoding,
 * double-precision pocketfft, robust normalization, and BC4 DDS output.
 */
#include "engine/ground_relief_generator.h"

#include "dds_loader_cpu.h"
#include "engine/dds_block_codec.h"

#include <pocketfft_hdronly.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace heaven {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t hash_bytes(uint64_t hash, std::span<const std::byte> bytes) {
    for (std::byte byte : bytes) {
        hash ^= std::to_integer<uint8_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string hex64(uint64_t value) {
    return std::format("{:016x}", value);
}

Result<std::string>
fingerprint_asset(std::string_view path, AssetFileSystemRef assets) {
    auto opened = open_asset_stream(path, assets);
    if (!opened) return std::unexpected(opened.error());
    auto stream = std::move(*opened);
    std::array<std::byte, std::size_t{64} * 1024> buffer{};
    uint64_t hash = kFnvOffset;
    while (true) {
        auto count = stream->read(buffer);
        if (!count) return std::unexpected(count.error());
        if (*count == 0) break;
        hash = hash_bytes(hash, std::span(buffer).first(*count));
    }
    return hex64(hash);
}

Result<std::vector<double>>
integrate_normals(const DecodedChannel& x_channel,
                  const DecodedChannel& y_channel,
                  std::span<const double> normal_z) {
    const std::size_t count = x_channel.pixels.size();
    if (y_channel.pixels.size() != count || normal_z.size() != count) {
        return std::unexpected(Error{"normal channel dimensions do not match"});
    }

    using Complex = std::complex<double>;
    std::vector<Complex> gradient_u(count);
    std::vector<Complex> gradient_v(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double nx = static_cast<double>(x_channel.pixels[index]) /
                              127.5 - 1.0;
        const double ny = static_cast<double>(y_channel.pixels[index]) /
                              127.5 - 1.0;
        const double divisor = std::max(normal_z[index], 0.20);
        gradient_u[index] = Complex{-nx / divisor, 0.0};
        gradient_v[index] = Complex{-ny / divisor, 0.0};
    }

    const pocketfft::shape_t shape{x_channel.height, x_channel.width};
    const pocketfft::stride_t stride{
        static_cast<std::ptrdiff_t>(sizeof(Complex) * x_channel.width),
        static_cast<std::ptrdiff_t>(sizeof(Complex))};
    const pocketfft::shape_t axes{0, 1};
    try {
        pocketfft::c2c(shape, stride, stride, axes, true,
                       gradient_u.data(), gradient_u.data(), 1.0, 1);
        pocketfft::c2c(shape, stride, stride, axes, true,
                       gradient_v.data(), gradient_v.data(), 1.0, 1);
    } catch (const std::exception& exception) {
        return std::unexpected(
            Error{"ground-height forward FFT failed: " +
                  std::string(exception.what())});
    }

    const Complex negative_i{0.0, -1.0};
    for (uint32_t y = 0; y < x_channel.height; ++y) {
        const int64_t signed_y = y <= x_channel.height / 2
            ? static_cast<int64_t>(y)
            : static_cast<int64_t>(y) - x_channel.height;
        const double frequency_v = 2.0 * std::numbers::pi *
                                   static_cast<double>(signed_y) /
                                   x_channel.height;
        for (uint32_t x = 0; x < x_channel.width; ++x) {
            const int64_t signed_x = x <= x_channel.width / 2
                ? static_cast<int64_t>(x)
                : static_cast<int64_t>(x) - x_channel.width;
            const double frequency_u = 2.0 * std::numbers::pi *
                                       static_cast<double>(signed_x) /
                                       x_channel.width;
            const double denominator = frequency_u * frequency_u +
                                       frequency_v * frequency_v;
            const std::size_t index =
                static_cast<std::size_t>(y) * x_channel.width + x;
            gradient_u[index] = denominator == 0.0
                ? Complex{}
                : negative_i *
                      (frequency_u * gradient_u[index] +
                       frequency_v * gradient_v[index]) /
                      denominator;
        }
    }

    try {
        pocketfft::c2c(shape, stride, stride, axes, false,
                       gradient_u.data(), gradient_u.data(),
                       1.0 / static_cast<double>(count), 1);
    } catch (const std::exception& exception) {
        return std::unexpected(
            Error{"ground-height inverse FFT failed: " +
                  std::string(exception.what())});
    }

    std::vector<double> height(count);
    for (std::size_t index = 0; index < count; ++index) {
        height[index] = gradient_u[index].real();
        if (!std::isfinite(height[index])) {
            return std::unexpected(
                Error{"ground-height FFT produced a non-finite value"});
        }
    }
    return height;
}

double percentile(std::span<const double> sorted, double quantile) {
    if (sorted.empty()) return 0.0;
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const std::size_t low = static_cast<std::size_t>(std::floor(position));
    const std::size_t high = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(low);
    return sorted[low] * (1.0 - fraction) + sorted[high] * fraction;
}

Result<std::vector<uint8_t>>
normalize_height(std::span<const double> height) {
    std::vector<double> sorted(height.begin(), height.end());
    std::sort(sorted.begin(), sorted.end());
    const double low = percentile(sorted, 0.02);
    const double high = percentile(sorted, 0.98);
    const double scale = std::max(std::abs(low), std::abs(high));

    std::vector<uint8_t> result(height.size(), 128);
    if (scale <= std::numeric_limits<double>::epsilon()) return result;
    for (std::size_t index = 0; index < height.size(); ++index) {
        const double normalized = std::clamp(
            0.5 + height[index] / (2.0 * scale), 0.0, 1.0);
        result[index] = static_cast<uint8_t>(
            std::floor(normalized * 255.0 + 0.5));
    }
    return result;
}

double mean_normal_error(std::span<const double> height,
                         const DecodedChannel& x_channel,
                         const DecodedChannel& y_channel,
                         std::span<const double> normal_z,
                         bool flat,
                         double height_sign = 1.0) {
    double total = 0.0;
    for (uint32_t y = 0; y < x_channel.height; ++y) {
        const uint32_t next_y = (y + 1) % x_channel.height;
        for (uint32_t x = 0; x < x_channel.width; ++x) {
            const uint32_t next_x = (x + 1) % x_channel.width;
            const std::size_t index =
                static_cast<std::size_t>(y) * x_channel.width + x;
            const double dx = flat ? 0.0 : height_sign *
                (height[index] - height[static_cast<std::size_t>(y) *
                                        x_channel.width + next_x]);
            const double dy = flat ? 0.0 : height_sign *
                (height[index] - height[static_cast<std::size_t>(next_y) *
                                        x_channel.width + x]);
            const double reconstructed_length = std::sqrt(dx * dx + dy * dy + 1.0);
            const double nx = static_cast<double>(x_channel.pixels[index]) /
                                  127.5 - 1.0;
            const double ny = static_cast<double>(y_channel.pixels[index]) /
                                  127.5 - 1.0;
            const double source_length =
                std::sqrt(nx * nx + ny * ny + normal_z[index] * normal_z[index]);
            const double cosine = std::clamp(
                (dx * nx + dy * ny + normal_z[index]) /
                    (reconstructed_length * source_length),
                -1.0, 1.0);
            total += std::acos(cosine);
        }
    }
    return total / static_cast<double>(height.size());
}

std::vector<uint8_t> downsample(std::span<const uint8_t> source,
                                uint32_t width,
                                uint32_t height) {
    const uint32_t next_width = std::max(1u, width / 2);
    const uint32_t next_height = std::max(1u, height / 2);
    std::vector<uint8_t> result(
        static_cast<std::size_t>(next_width) * next_height);
    for (uint32_t y = 0; y < next_height; ++y) {
        for (uint32_t x = 0; x < next_width; ++x) {
            uint32_t sum = 0;
            uint32_t samples = 0;
            for (uint32_t offset_y = 0; offset_y < 2; ++offset_y) {
                for (uint32_t offset_x = 0; offset_x < 2; ++offset_x) {
                    const uint32_t source_x = std::min(width - 1, x * 2 + offset_x);
                    const uint32_t source_y = std::min(height - 1, y * 2 + offset_y);
                    sum += source[static_cast<std::size_t>(source_y) * width + source_x];
                    ++samples;
                }
            }
            result[static_cast<std::size_t>(y) * next_width + x] =
                static_cast<uint8_t>((sum + samples / 2) / samples);
        }
    }
    return result;
}

std::vector<uint8_t> encode_bc4(std::span<const uint8_t> source,
                                uint32_t width,
                                uint32_t height) {
    const uint32_t blocks_wide = (width + 3) / 4;
    const uint32_t blocks_high = (height + 3) / 4;
    std::vector<uint8_t> encoded(
        static_cast<std::size_t>(blocks_wide) * blocks_high * 8);
    for (uint32_t block_y = 0; block_y < blocks_high; ++block_y) {
        for (uint32_t block_x = 0; block_x < blocks_wide; ++block_x) {
            std::array<uint8_t, 16> pixels{};
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t image_x = std::min(width - 1, block_x * 4 + x);
                    const uint32_t image_y = std::min(height - 1, block_y * 4 + y);
                    pixels[y * 4 + x] =
                        source[static_cast<std::size_t>(image_y) * width + image_x];
                }
            }
            const std::size_t offset =
                (static_cast<std::size_t>(block_y) * blocks_wide + block_x) * 8;
            // stb_dxt is a third-party C API and is the only raw-buffer boundary
            // in the encoder.
            stb_compress_bc4_block(encoded.data() + offset, pixels.data());
        }
    }
    return encoded;
}

template <typename T>
void append_object(AssetBuffer& output, const T& object) {
    const auto bytes = std::as_bytes(std::span(&object, 1));
    for (std::byte byte : bytes) {
        output.push_back(std::to_integer<uint8_t>(byte));
    }
}

AssetBuffer make_bc4_dds(std::span<const uint8_t> base,
                         uint32_t width,
                         uint32_t height) {
    std::vector<std::vector<uint8_t>> mips;
    mips.emplace_back(base.begin(), base.end());
    uint32_t mip_width = width;
    uint32_t mip_height = height;
    while (mip_width > 1 || mip_height > 1) {
        mips.push_back(downsample(mips.back(), mip_width, mip_height));
        mip_width = std::max(1u, mip_width / 2);
        mip_height = std::max(1u, mip_height / 2);
    }

    dds_detail::Header header{};
    header.size = sizeof(dds_detail::Header);
    header.flags = 0x000A1007u;
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize =
        ((width + 3) / 4) * ((height + 3) / 4) * 8;
    header.mipMapCount = static_cast<uint32_t>(mips.size());
    header.pixelFormat.size = sizeof(dds_detail::PixelFormat);
    header.pixelFormat.flags = dds_detail::DDPF_FOURCC;
    header.pixelFormat.fourCC = dds_detail::make_fourcc('A', 'T', 'I', '1');
    header.caps = mips.size() > 1 ? 0x00401008u : 0x00001000u;

    AssetBuffer output;
    output.reserve(4 + sizeof(header) +
                   static_cast<std::size_t>(header.pitchOrLinearSize) * 4 / 3 + 16);
    append_object(output, dds_detail::DDS_MAGIC);
    append_object(output, header);
    mip_width = width;
    mip_height = height;
    for (const auto& mip : mips) {
        auto encoded = encode_bc4(mip, mip_width, mip_height);
        output.insert(output.end(), encoded.begin(), encoded.end());
        mip_width = std::max(1u, mip_width / 2);
        mip_height = std::max(1u, mip_height / 2);
    }
    return output;
}

} // namespace

Result<GeneratedAsset>
generate_ground_height(std::string_view normal_path,
                       std::string_view output_path,
                       AssetFileSystemRef assets) {
    const DDSTextureCPU normal = load_dds_cpu(std::string(normal_path), assets);
    if (!normal || normal.is_cubemap || normal.format != DDSFormatCPU::BC5 ||
        normal.mip_data.empty()) {
        return std::unexpected(
            Error{"ground normal must be a valid 2D BC5 DDS: " +
                  std::string(normal_path)});
    }
    auto normal_x = decode_bc4_channel(
        normal.mip_data.front().bytes(), normal.width, normal.height, 16, 0);
    if (!normal_x) return std::unexpected(normal_x.error());
    auto normal_y = decode_bc4_channel(
        normal.mip_data.front().bytes(), normal.width, normal.height, 16, 8);
    if (!normal_y) return std::unexpected(normal_y.error());

    std::vector<double> normal_z(normal_x->pixels.size());
    for (std::size_t index = 0; index < normal_z.size(); ++index) {
        const double nx = static_cast<double>(normal_x->pixels[index]) /
                              127.5 - 1.0;
        const double ny = static_cast<double>(normal_y->pixels[index]) /
                              127.5 - 1.0;
        normal_z[index] = std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny));
    }

    auto height = integrate_normals(*normal_x, *normal_y, normal_z);
    if (!height) return std::unexpected(height.error());
    const double integrated_error =
        mean_normal_error(*height, *normal_x, *normal_y, normal_z, false);
    const double flat_error =
        mean_normal_error(*height, *normal_x, *normal_y, normal_z, true);
    const double inverted_error =
        mean_normal_error(*height, *normal_x, *normal_y, normal_z, false, -1.0);
    const bool spatially_varying =
        !std::all_of(normal_x->pixels.begin(), normal_x->pixels.end(),
                     [&](uint8_t value) {
                         return value == normal_x->pixels.front();
                     }) ||
        !std::all_of(normal_y->pixels.begin(), normal_y->pixels.end(),
                     [&](uint8_t value) {
                         return value == normal_y->pixels.front();
                     });
    if (spatially_varying && !(integrated_error < flat_error)) {
        return std::unexpected(Error{std::format(
            "ground-height UV orientation validation did not improve normals "
            "({:.4f} rad integrated, {:.4f} inverted, {:.4f} flat)",
            integrated_error, inverted_error, flat_error)});
    }

    auto normalized = normalize_height(*height);
    if (!normalized) return std::unexpected(normalized.error());
    auto bytes = std::make_shared<const AssetBuffer>(
        make_bc4_dds(*normalized, normal.width, normal.height));
    auto source_fingerprint = fingerprint_asset(normal_path, assets);
    if (!source_fingerprint) {
        return std::unexpected(source_fingerprint.error());
    }
    const std::string output_fingerprint = hex64(hash_bytes(
        kFnvOffset, std::as_bytes(std::span(*bytes))));

    GeneratedAsset result;
    result.logical_path = std::string(output_path);
    result.bytes = std::move(bytes);
    result.metadata = GeneratedAssetMetadata{
        .generator_id = std::string(kGroundHeightGeneratorId),
        .generator_version = kGroundHeightGeneratorVersion,
        .deepbump_revision = std::string(kDeepBumpRevision),
        .pocketfft_revision = std::string(kPocketFftRevision),
        .bcdec_revision = std::string(kBcdecRevision),
        .source_path = std::string(normal_path),
        .output_path = std::string(output_path),
        .source_fingerprint = std::move(*source_fingerprint),
        .output_fingerprint = output_fingerprint,
    };
    return result;
}

Result<std::optional<GeneratedAsset>>
ensure_ground_height(const PathResolver& paths, AssetFileSystem& assets,
                     bool regenerate) {
    const std::string output = paths.relative_path(kGroundHeightRelativePath);
    if (!regenerate && assets.exists(output)) {
        return std::optional<GeneratedAsset>{};
    }

    const std::string source = paths.relative_path(kGroundNormalRelativePath);
    auto generated = generate_ground_height(
        source, output, std::cref(assets));
    if (!generated) return std::unexpected(generated.error());
    generated->metadata.source_path = std::string(kGroundNormalRelativePath);
    generated->metadata.output_path = std::string(kGroundHeightRelativePath);
    auto logical = assets.logical_path(output);
    if (!logical) return std::unexpected(logical.error());
    generated->logical_path = std::move(*logical);
    auto mounted = assets.mount_memory(output, generated->bytes);
    if (!mounted) return std::unexpected(mounted.error());
    return std::optional<GeneratedAsset>(std::move(*generated));
}

} // namespace heaven
