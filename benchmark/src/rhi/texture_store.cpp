/*
 * HeavenBench — Texture store
 *
 * Extracted verbatim out of FilamentBackend::load_texture /
 * get_filament_texture and the placeholder-creation block in
 * init_pipelines().
 */
#include "rhi/texture_store.h"
#include "rhi/bloom_dirt.h"
#include "dds_loader_cpu.h"
#include "engine/dds_block_codec.h"

#include <filament/Engine.h>
#include <filament/Texture.h>

#include <backend/DriverEnums.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/half.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>

namespace heaven {
namespace {

filament::Texture::PixelBufferDescriptor make_compressed_upload(
        std::span<const std::uint8_t> bytes,
        SharedAssetBuffer owner,
        filament::backend::CompressedPixelDataType format,
        std::uint32_t image_size) {
    return filament::Texture::PixelBufferDescriptor::make(
        bytes.data(), bytes.size(), format, image_size,
        [owner = std::move(owner)](const void*, std::size_t) mutable {
            owner.reset();
        });
}

filament::Texture::PixelBufferDescriptor make_rgba_upload(
        std::span<const std::uint8_t> bytes,
        SharedAssetBuffer owner) {
    return filament::Texture::PixelBufferDescriptor::make(
        bytes.data(), bytes.size(), filament::Texture::Format::RGBA,
        filament::Texture::Type::UBYTE,
        [owner = std::move(owner)](const void*, std::size_t) mutable {
            owner.reset();
        });
}

} // namespace

TextureStore::~TextureStore() {
    clear_loaded_textures();
    if (placeholder_tex_) engine_.destroy(placeholder_tex_);
    if (placeholder_normal_tex_) engine_.destroy(placeholder_normal_tex_);
    if (placeholder_depth_tex_) engine_.destroy(placeholder_depth_tex_);
}

void TextureStore::clear_loaded_textures() {
    for (auto& [id, tex] : texture_map_) {
        engine_.destroy(tex);
    }
    texture_map_.clear();
    texture_cache_.clear();
    const TextureQuality requested = stats_.requested_quality;
    const TextureQuality active = stats_.active_quality;
    stats_ = {};
    stats_.requested_quality = requested;
    stats_.active_quality = active;
    warned_hires_fallback_ = false;
    warned_cpu_decode_ = false;
}

void TextureStore::refresh_active_quality() noexcept {
    stats_.active_quality = stats_.requested_quality;
    if (stats_.requested_quality != TextureQuality::Original &&
        (!assets_ || !assets_->get().texture_set_summary().available)) {
        stats_.active_quality = TextureQuality::Original;
    }
}

void TextureStore::set_texture_quality(TextureQuality quality) noexcept {
    stats_.requested_quality = quality;
    refresh_active_quality();
}

void TextureStore::record_external_original_texture(
        uint32_t width, uint32_t height, TextureSemantic semantic,
        bool complete_mip_chain) noexcept {
    switch (semantic) {
        case TextureSemantic::ColorSRGB: ++stats_.srgb; break;
        case TextureSemantic::ColorLegacyGamma2: ++stats_.legacy_color; break;
        case TextureSemantic::ScalarLinear: ++stats_.scalar; break;
        case TextureSemantic::NormalLinear: ++stats_.normal; break;
        case TextureSemantic::HDRLinear: ++stats_.hdr; break;
    }
    ++stats_.originals;
    uint32_t w = width;
    uint32_t h = height;
    for (;;) {
        stats_.selected_mip_bytes += uint64_t(w) * h * 4u;
        if (!complete_mip_chain || (w == 1u && h == 1u)) break;
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);
    }
    stats_.effective_max_dimension = std::max(
        {stats_.effective_max_dimension, width, height});
}

uint64_t TextureStore::load(
    const std::string& path, TextureSemantic semantic) {
    // Legacy color retains encoded values for composition and shader decoding.
    const bool srgb = semantic == TextureSemantic::ColorSRGB;
    std::string selected_path = path;
    uint32_t starting_mip = 0;
    bool selected_hires = false;
    bool requested_fallback = false;

    if (stats_.requested_quality != TextureQuality::Original) {
        std::optional<TextureVariantInfo> variant;
        if (assets_) variant = assets_->get().texture_variant(path);
        if (!variant) {
            ++stats_.missing_variants;
            requested_fallback = true;
        } else {
            const bool bc7 = variant->format == "BC7" ||
                             variant->format == "BC7_UNORM" ||
                             variant->format == "BC7_SRGB";
            const auto bc7_format = srgb
                ? filament::Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM
                : filament::Texture::InternalFormat::RGBA_BPTC_UNORM;
            if (bc7 && !filament::Texture::isTextureFormatSupported(
                           engine_, bc7_format)) {
                ++stats_.unsupported_formats;
                requested_fallback = true;
            } else {
                selected_path = variant->variant_path;
                selected_hires = selected_path != variant->original_path;
                if (stats_.requested_quality == TextureQuality::High2K) {
                    uint32_t edge = std::max(variant->width, variant->height);
                    while (edge > 2048u) {
                        ++starting_mip;
                        edge = std::max(1u, edge >> 1u);
                    }
                }
            }
        }
    }

    const std::string cache_key =
        selected_path + "#" + std::to_string(static_cast<int>(semantic)) +
        "#" + std::to_string(starting_mip) + "#" +
        std::string(texture_quality_id(stats_.requested_quality));
    // Check cache
    if (auto it = texture_cache_.find(cache_key); it != texture_cache_.end())
        return it->second;

    // Load DDS file to CPU memory
    DDSTextureCPU dds = load_dds_cpu(selected_path, assets_, starting_mip);
    if ((!dds || dds.width == 0 || dds.height == 0) && selected_hires) {
        ++stats_.invalid_variants;
        requested_fallback = true;
        selected_hires = false;
        selected_path = path;
        dds = load_dds_cpu(path, assets_);
    }
    if (requested_fallback && !warned_hires_fallback_) {
        std::fprintf(
            stderr,
            "Texture quality '%s' is not fully available; exact authored "
            "textures will be used for missing, invalid, or unsupported "
            "entries (see benchmark texture fallback counters).\n",
            std::string(texture_quality_id(stats_.requested_quality)).c_str());
        warned_hires_fallback_ = true;
    }
    if (!dds || dds.width == 0 || dds.height == 0) {
        ++stats_.fallbacks;
        return 0;
    }
    if (requested_fallback) ++stats_.fallbacks;

    // Map DDS format to Filament format
    filament::Texture::InternalFormat internal_fmt;
    filament::backend::CompressedPixelDataType compressed_type{};
    bool is_compressed = false;

    switch (dds.format) {
        case DDSFormatCPU::BC1:
            internal_fmt = srgb
                ? filament::Texture::InternalFormat::DXT1_SRGBA
                : filament::Texture::InternalFormat::DXT1_RGBA;
            compressed_type = srgb
                ? filament::backend::CompressedPixelDataType::DXT1_SRGBA
                : filament::backend::CompressedPixelDataType::DXT1_RGBA;
            is_compressed = true;
            break;
        case DDSFormatCPU::BC3:
            internal_fmt = srgb
                ? filament::Texture::InternalFormat::DXT5_SRGBA
                : filament::Texture::InternalFormat::DXT5_RGBA;
            compressed_type = srgb
                ? filament::backend::CompressedPixelDataType::DXT5_SRGBA
                : filament::backend::CompressedPixelDataType::DXT5_RGBA;
            is_compressed = true;
            break;
        case DDSFormatCPU::BC4:
            internal_fmt = filament::Texture::InternalFormat::RED_RGTC1;
            compressed_type = filament::backend::CompressedPixelDataType::RED_RGTC1;
            is_compressed = true;
            break;
        case DDSFormatCPU::BC5:
            internal_fmt = filament::Texture::InternalFormat::RED_GREEN_RGTC2;
            compressed_type = filament::backend::CompressedPixelDataType::RED_GREEN_RGTC2;
            is_compressed = true;
            break;
        case DDSFormatCPU::BC7:
            internal_fmt = srgb
                ? filament::Texture::InternalFormat::SRGB_ALPHA_BPTC_UNORM
                : filament::Texture::InternalFormat::RGBA_BPTC_UNORM;
            compressed_type = srgb
                ? filament::backend::CompressedPixelDataType::SRGB_ALPHA_BPTC_UNORM
                : filament::backend::CompressedPixelDataType::RGBA_BPTC_UNORM;
            is_compressed = true;
            break;
        case DDSFormatCPU::RGBA8:
        case DDSFormatCPU::RGB8:
            // Filament's Vulkan backend does not implement an RGB8
            // texture format directly — it always uses RGBA8 under
            // the hood. We therefore upload as RGBA8 and expand the
            // 24-bit source (normalized to RGB by dds_loader_cpu)
            // into 32-bit RGBA below in the upload loop. The
            // alpha channel is filled with 0xFF since the on-disk
            // format has no alpha.
            internal_fmt = srgb
                ? filament::Texture::InternalFormat::SRGB8_A8
                : filament::Texture::InternalFormat::RGBA8;
            is_compressed = false;
            break;
        default:
            return 0;
    }

    if (is_compressed &&
        !filament::Texture::isTextureFormatSupported(engine_, internal_fmt)) {
        ++stats_.unsupported_formats;
        if (!requested_fallback) ++stats_.fallbacks;
        // The CPU block decoder accepts one 2D image at a time. Cubemap
        // expansion needs a separate per-face conversion; reject this path
        // before creating an unsupported GPU texture.
        if (dds.is_cubemap) {
            std::fprintf(stderr,
                "Unsupported compressed DDS cubemap format: %s\n", selected_path.c_str());
            return 0;
        }
        if (dds.mip_data.size() != dds.mip_count) {
            std::fprintf(stderr, "Incomplete DDS mip chain: %s\n", selected_path.c_str());
            return 0;
        }
        const bool compressed_srgb = srgb &&
            dds.format != DDSFormatCPU::BC4 && dds.format != DDSFormatCPU::BC5;
        for (uint32_t level = 0; level < dds.mip_count; ++level) {
            const uint32_t mip_width = std::max(1u, dds.width >> level);
            const uint32_t mip_height = std::max(1u, dds.height >> level);
            auto decoded = decode_dds_blocks_rgba(
                dds.mip_data[level].bytes(), mip_width, mip_height, dds.format);
            if (!decoded) {
                std::fprintf(stderr, "Cannot decode DDS mip %u in %s: %s\n",
                    level, selected_path.c_str(), decoded.error().message.c_str());
                return 0;
            }
            auto storage = std::make_shared<AssetBuffer>(std::move(decoded->pixels));
            dds.mip_data[level] = DDSMipLevelCPU(storage, 0, storage->size());
        }
        internal_fmt = compressed_srgb
            ? filament::Texture::InternalFormat::SRGB8_A8
            : filament::Texture::InternalFormat::RGBA8;
        dds.format = DDSFormatCPU::RGBA8;
        is_compressed = false;
        if (!warned_cpu_decode_) {
            std::fprintf(stderr,
                "GPU does not support some DDS block formats; affected 2D "
                "textures are decoded to RGBA8 on the CPU with all mip levels.\n");
            warned_cpu_decode_ = true;
        }
    }

    uint8_t levels = static_cast<uint8_t>(dds.mip_count);

    // Create Filament texture
    filament::Texture* tex = nullptr;

    if (dds.is_cubemap) {
        tex = filament::Texture::Builder()
            .width(dds.width)
            .height(dds.height)
            .levels(levels)
            .sampler(filament::Texture::Sampler::SAMPLER_CUBEMAP)
            .format(internal_fmt)
            .build(engine_);
    } else {
        tex = filament::Texture::Builder()
            .width(dds.width)
            .height(dds.height)
            .levels(levels)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .format(internal_fmt)
            .build(engine_);
    }

    if (!tex) return 0;

    // Upload mip levels
    for (uint8_t level = 0; level < levels; level++) {
        if (level >= dds.mip_data.size()) break;

        const auto& mip = dds.mip_data[level];
        std::span<const std::uint8_t> upload = mip.bytes();
        SharedAssetBuffer upload_owner = mip.owner();

        // RGB8 has no native Filament/Vulkan upload format. Expand it once;
        // all other mip payloads retain the DDS loader's immutable backing.
        const bool expand_rgb_to_rgba = (dds.format == DDSFormatCPU::RGB8);
        if (expand_rgb_to_rgba) {
            const size_t pixel_count = upload.size() / 3;
            auto expanded =
                std::make_shared<AssetBuffer>(pixel_count * 4, 0xFFu);
            for (size_t i = 0; i < pixel_count; ++i) {
                (*expanded)[i * 4 + 0] = upload[i * 3 + 0];
                (*expanded)[i * 4 + 1] = upload[i * 3 + 1];
                (*expanded)[i * 4 + 2] = upload[i * 3 + 2];
            }
            upload_owner = std::move(expanded);
            upload = *upload_owner;
        }
        const size_t size = upload.size();

        if (dds.is_cubemap) {
            size_t face_size = size / 6;
            const uint32_t mip_width = std::max<uint32_t>(1, dds.width >> level);
            const uint32_t mip_height = std::max<uint32_t>(1, dds.height >> level);

            if (is_compressed) {
                tex->setImage(engine_, level, 0, 0, 0, mip_width, mip_height, 6,
                    make_compressed_upload(
                        upload, upload_owner, compressed_type,
                        static_cast<uint32_t>(face_size)));
            } else {
                tex->setImage(engine_, level, 0, 0, 0, mip_width, mip_height, 6,
                    make_rgba_upload(upload, upload_owner));
            }
        } else {
            if (is_compressed) {
                tex->setImage(engine_, level,
                    make_compressed_upload(
                        upload, upload_owner, compressed_type,
                        static_cast<uint32_t>(size)));
            } else {
                tex->setImage(engine_, level,
                    make_rgba_upload(upload, upload_owner));
            }
        }
    }

    // Cache and return
    uint64_t handle = next_tex_id_++;
    texture_map_[handle] = tex;
    texture_cache_[cache_key] = handle;
    switch (semantic) {
        case TextureSemantic::ColorSRGB: ++stats_.srgb; break;
        case TextureSemantic::ColorLegacyGamma2: ++stats_.legacy_color; break;
        case TextureSemantic::ScalarLinear: ++stats_.scalar; break;
        case TextureSemantic::NormalLinear: ++stats_.normal; break;
        case TextureSemantic::HDRLinear: ++stats_.hdr; break;
    }
    if (selected_hires) ++stats_.hires;
    else ++stats_.originals;
    for (const DDSMipLevelCPU& mip : dds.mip_data) {
        stats_.selected_mip_bytes += mip.size();
    }
    stats_.effective_max_dimension = std::max(
        {stats_.effective_max_dimension, dds.width, dds.height});
    return handle;
}

uint64_t TextureStore::load_bloom_dirt_gain(const std::string& path) {
    try {
        const std::string key = path + "#bloom-dirt-linear-gain-v1-original";
        if (const auto cached = texture_cache_.find(key); cached != texture_cache_.end())
            return cached->second;
        const auto dds = load_dds_cpu(path, assets_);
        if (!dds || dds.is_cubemap || dds.mip_data.size() != dds.mip_count ||
            dds.format == DDSFormatCPU::BC4 || dds.format == DDSFormatCPU::BC5 ||
            !filament::Texture::isTextureFormatSupported(
                engine_, filament::Texture::InternalFormat::RGBA16F)) {
            ++stats_.fallbacks;
            return 0;
        }
        const auto destroy = [this](filament::Texture* texture) { engine_.destroy(texture); };
        std::unique_ptr<filament::Texture, decltype(destroy)> texture(
            filament::Texture::Builder().width(dds.width).height(dds.height)
                .levels(static_cast<uint8_t>(dds.mip_count))
                .sampler(filament::Texture::Sampler::SAMPLER_2D)
                .format(filament::Texture::InternalFormat::RGBA16F).build(engine_), destroy);
        if (!texture) {
            ++stats_.fallbacks;
            return 0;
        }
        uint64_t uploaded_bytes = 0;
        for (uint32_t level = 0; level < dds.mip_count; ++level) {
            const uint32_t width = std::max(1u, dds.width >> level);
            const uint32_t height = std::max(1u, dds.height >> level);
            const auto source = dds.mip_data[level].bytes();
            std::vector<std::uint8_t> decoded;
            unsigned channels = dds.format == DDSFormatCPU::RGB8 ? 3u : 4u;
            std::span<const std::uint8_t> rgba = source;
            if (dds.format != DDSFormatCPU::RGB8 && dds.format != DDSFormatCPU::RGBA8) {
                auto pixels = decode_dds_blocks_rgba(source, width, height, dds.format);
                if (!pixels) {
                    ++stats_.fallbacks;
                    return 0;
                }
                decoded = std::move(pixels->pixels);
                rgba = decoded;
            }
            if (rgba.size() != uint64_t{width} * height * channels) {
                ++stats_.fallbacks;
                return 0;
            }
            auto gain = make_bloom_dirt_gain_rgba(rgba, channels);
            if (!gain) {
                ++stats_.fallbacks;
                return 0;
            }
            auto upload = std::make_shared<std::vector<filament::math::half>>();
            upload->reserve(gain->size());
            for (const float value : *gain) upload->emplace_back(value);
            const auto bytes = upload->size() * sizeof(filament::math::half);
            const auto* data = upload->data();
            uploaded_bytes += bytes;
            texture->setImage(engine_, static_cast<uint8_t>(level),
                filament::Texture::PixelBufferDescriptor::make(
                    data, bytes, filament::Texture::Format::RGBA,
                    filament::Texture::Type::HALF,
                    [owner = std::move(upload)](const void*, std::size_t) mutable {
                        owner.reset();
                    }));
        }
        // Reserve both containers before publishing either cache entry; the
        // unique owner releases the GPU texture if allocation fails.
        texture_map_.reserve(texture_map_.size() + 1);
        texture_cache_.reserve(texture_cache_.size() + 1);
        const uint64_t handle = next_tex_id_;
        texture_cache_.emplace(key, handle);
        try {
            texture_map_.emplace(handle, texture.get());
        } catch (...) {
            texture_cache_.erase(key);
            throw;
        }
        texture.release();
        ++next_tex_id_;
        ++stats_.hdr;
        ++stats_.originals;
        stats_.selected_mip_bytes += uploaded_bytes;
        stats_.effective_max_dimension = std::max(
            {stats_.effective_max_dimension, dds.width, dds.height});
        return handle;
    } catch (const std::bad_alloc&) {
        ++stats_.fallbacks;
        std::fprintf(stderr, "Cannot allocate lens-dirt gain; using clean bloom: %s\n", path.c_str());
        return 0;
    }
}

filament::Texture* TextureStore::get(uint64_t handle) const {
    if (handle == 0) return nullptr;
    auto it = texture_map_.find(handle);
    return (it != texture_map_.end()) ? it->second : nullptr;
}

void TextureStore::create_placeholders() {
    // ── Create a 1x1 white placeholder texture ──────────────────
    // Used when a material slot has no texture assigned
    placeholder_tex_ = filament::Texture::Builder()
        .width(1).height(1).levels(1)
        .format(filament::Texture::InternalFormat::RGBA8)
        .sampler(filament::Texture::Sampler::SAMPLER_2D)
        .build(engine_);
    {
        static constexpr std::array<std::uint8_t, 4> white = {
            255, 255, 255, 255};
        placeholder_tex_->setImage(engine_, 0,
            filament::Texture::PixelBufferDescriptor(
                white.data(), white.size(),
                filament::Texture::Format::RGBA,
                filament::Texture::Type::UBYTE));
    }

    // 1x1 flat normal placeholder (0.5, 0.5, 1.0 = flat normal in tangent space)
    placeholder_normal_tex_ = filament::Texture::Builder()
        .width(1).height(1).levels(1)
        .format(filament::Texture::InternalFormat::RGBA8)
        .sampler(filament::Texture::Sampler::SAMPLER_2D)
        .build(engine_);
    {
        static constexpr std::array<std::uint8_t, 4> flat_normal = {
            128, 128, 255, 255};
        placeholder_normal_tex_->setImage(engine_, 0,
            filament::Texture::PixelBufferDescriptor(
                flat_normal.data(), flat_normal.size(),
                filament::Texture::Format::RGBA,
                filament::Texture::Type::UBYTE));
    }

    // ── 1x1 zero-depth placeholder ──────────────────────────────
    // The depth-sampler fallback for scene-depth consumers. Its red channel
    // reads 0.0; every consumer's shader treats a non-positive depth as
    // "far / no geometry" (sceneDist <= 0 → 2000 m). Binding THIS (instead of
    // the white placeholder, which reads 1.0 m and would falsely occlude the
    // whole scene) makes consumers fall back to NO occlusion when the real
    // Scene_Depth_Texture is absent — before the depth pre-pass first runs or
    // when the F target / depth material failed to build.
    placeholder_depth_tex_ = filament::Texture::Builder()
        .width(1).height(1).levels(1)
        .format(filament::Texture::InternalFormat::RGBA8)
        .sampler(filament::Texture::Sampler::SAMPLER_2D)
        .build(engine_);
    {
        static constexpr std::array<std::uint8_t, 4> zero = {
            0, 0, 0, 0};
        placeholder_depth_tex_->setImage(engine_, 0,
            filament::Texture::PixelBufferDescriptor(
                zero.data(), zero.size(),
                filament::Texture::Format::RGBA,
                filament::Texture::Type::UBYTE));
    }
}

} // namespace heaven
