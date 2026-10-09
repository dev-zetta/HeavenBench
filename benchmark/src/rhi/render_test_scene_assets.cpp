#include "rhi/render_test_scene_assets.h"
#include "rhi/render_feature.h"
#include "rhi/bloom_dirt.h"

#include <filament/Engine.h>
#include <filament/Texture.h>
#include <math/half.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace heaven {
namespace {
constexpr std::uint32_t kTextureSize = 128;

float hash_unit(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return static_cast<float>(x & 0x00ffffffu) / 16777216.0f;
}

std::uint8_t byte(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

enum class Image { Grass, Variation, Flower, Cloud1, Cloud2, CloudMask,
                   Glow, Drop, Moon, MoonNormal, Stars, LensDirt };

filament::Texture* make_texture(filament::Engine& engine, Image image, bool srgb) {
    using Texture = filament::Texture;
    auto* texture = Texture::Builder().width(kTextureSize).height(kTextureSize)
        .levels(8).sampler(Texture::Sampler::SAMPLER_2D)
        .usage(Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE)
        .format(image == Image::LensDirt ? Texture::InternalFormat::RGBA16F
            : srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
        .build(engine);
    if (!texture) return nullptr;
    const auto cleanup = [&engine](Texture* p) { engine.destroy(p); };
    std::unique_ptr<Texture, decltype(cleanup)> owned(texture, cleanup);
    constexpr std::size_t bytes = static_cast<std::size_t>(kTextureSize) * kTextureSize * 4;
    auto pixels = std::make_unique<std::uint8_t[]>(bytes);
    for (std::uint32_t y = 0; y < kTextureSize; ++y) {
        for (std::uint32_t x = 0; x < kTextureSize; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / kTextureSize;
            const float v = (static_cast<float>(y) + 0.5f) / kTextureSize;
            const float px = 2.0f * u - 1.0f, py = 2.0f * v - 1.0f;
            const float r = std::sqrt(px * px + py * py);
            const float noise = hash_unit(x + y * kTextureSize);
            float red = 1.0f, green = 1.0f, blue = 1.0f, alpha = 1.0f;
            switch (image) {
                case Image::Grass: {
                    const float width = 0.08f + 0.32f * (1.0f - v);
                    alpha = 1.0f - smooth(width - 0.025f, width, std::abs(u - 0.5f));
                    alpha *= 1.0f - smooth(0.95f, 1.0f, v);
                    red = 0.20f + 0.22f * v; green = 0.42f + 0.28f * v;
                    blue = 0.08f + 0.11f * v;
                    break;
                }
                case Image::Variation:
                    red = 0.30f + 0.30f * noise; green = 0.38f + 0.28f * noise;
                    blue = 0.10f + 0.20f * noise; break;
                case Image::Flower: {
                    // Repeated quarter-width / half-height cards also fill the
                    // existing authored flower-atlas UV rectangle.
                    const float fu = std::fmod(u * 4.0f, 1.0f) * 2.0f - 1.0f;
                    const float fv = std::fmod(v * 2.0f, 1.0f);
                    const float head = std::sqrt(fu * fu + (fv - 0.72f) * (fv - 0.72f) * 8.0f);
                    const float petals = 1.0f - smooth(0.32f, 0.48f, head);
                    const float stem = (1.0f - smooth(0.04f, 0.075f, std::abs(fu))) *
                                       (1.0f - smooth(0.64f, 0.72f, fv));
                    alpha = std::max(petals, stem);
                    red = 0.16f + 0.79f * petals; green = 0.40f + 0.40f * petals;
                    blue = 0.10f + 0.35f * petals; break;
                }
                case Image::Cloud1:
                case Image::Cloud2:
                case Image::CloudMask: {
                    const float phase = image == Image::Cloud2 ? 1.7f : 0.0f;
                    const float broad = 0.50f + 0.17f * std::sin(u * 12.5663706f + phase) +
                        0.16f * std::cos(v * 18.8495559f - phase) +
                        0.11f * std::sin((u + v) * 31.4159265f);
                    red = green = blue = image == Image::CloudMask
                        ? 0.65f + 0.35f * smooth(0.3f, 0.7f, broad)
                        : std::clamp(broad + 0.12f, 0.0f, 1.0f);
                    break;
                }
                case Image::Glow:
                    red = green = blue = std::exp(-r * r * 5.5f) * (1.0f - smooth(0.8f, 1.0f, r));
                    alpha = red; break;
                case Image::Drop:
                    red = green = blue = std::exp(-px * px * 18.0f) *
                        (1.0f - smooth(0.65f, 1.0f, std::abs(py)));
                    alpha = red; break;
                case Image::Moon: {
                    const float disk = 1.0f - smooth(0.88f, 0.98f, r);
                    const float crater = 0.62f + 0.20f * std::sin(u * 35.0f) * std::cos(v * 29.0f);
                    red = green = blue = crater * disk; alpha = disk; break;
                }
                case Image::MoonNormal:
                    red = 0.5f + 0.035f * std::sin(u * 35.0f);
                    green = 0.5f + 0.035f * std::cos(v * 29.0f); blue = 1.0f; break;
                case Image::Stars: {
                    const float band = std::exp(-std::pow((v - 0.5f - 0.13f * std::sin(u * 6.2831853f)) * 8.0f, 2.0f));
                    const float star = noise > 0.992f ? 0.8f : 0.0f;
                    red = 0.035f * band + star; green = 0.045f * band + star;
                    blue = 0.08f * band + star; break;
                }
                case Image::LensDirt: {
                    const float spots = std::pow(std::max(0.0f,
                        std::sin(u * 37.0f) * std::cos(v * 29.0f)), 12.0f);
                    red = green = blue = 0.02f + 0.65f * spots;
                    break;
                }
            }
            const auto offset = (static_cast<std::size_t>(y) * kTextureSize + x) * 4;
            pixels[offset] = byte(red); pixels[offset + 1] = byte(green);
            pixels[offset + 2] = byte(blue); pixels[offset + 3] = byte(alpha);
        }
    }
    if (image == Image::LensDirt) {
        const auto gain = make_bloom_dirt_gain_rgba(
            std::span<const std::uint8_t>(pixels.get(), bytes), 4);
        if (!gain) return nullptr;
        auto half_pixels = std::make_unique<filament::math::half[]>(gain->size());
        for (std::size_t i = 0; i < gain->size(); ++i)
            half_pixels[i] = filament::math::half((*gain)[i]);
        texture->setImage(engine, 0, Texture::PixelBufferDescriptor(
            half_pixels.release(), gain->size() * sizeof(filament::math::half),
            Texture::Format::RGBA, Texture::Type::HALF,
            [](void* p, std::size_t, void*) { delete[] static_cast<filament::math::half*>(p); }));
    } else {
        texture->setImage(engine, 0, Texture::PixelBufferDescriptor(
            pixels.release(), bytes, Texture::Format::RGBA, Texture::Type::UBYTE,
            [](void* p, std::size_t, void*) { delete[] static_cast<std::uint8_t*>(p); }));
    }
    texture->generateMipmaps(engine);
    return owned.release();
}
} // namespace

Result<std::unique_ptr<RenderTestSceneAssets>> RenderTestSceneAssets::create(
        filament::Engine& engine) {
    auto result = std::unique_ptr<RenderTestSceneAssets>(new RenderTestSceneAssets(engine));
    auto initialized = result->init();
    if (!initialized) return std::unexpected(initialized.error());
    return result;
}

Result<void> RenderTestSceneAssets::init() {
    const auto add_feature = [&](FeatureAsset asset, Image image, bool srgb) {
        auto*& texture = feature_textures_[static_cast<std::size_t>(asset)];
        texture = make_texture(engine_, image, srgb);
        return texture != nullptr;
    };
    if (!add_feature(FeatureAsset::GrassDiffuse, Image::Grass, true) ||
        !add_feature(FeatureAsset::GrassIrregularity, Image::Variation, true) ||
        !add_feature(FeatureAsset::GrassClutter, Image::Flower, true) ||
        !add_feature(FeatureAsset::CloudLayer1, Image::Cloud1, true) ||
        !add_feature(FeatureAsset::CloudLayer2, Image::Cloud2, true) ||
        !add_feature(FeatureAsset::CloudMask, Image::CloudMask, false) ||
        !add_feature(FeatureAsset::Glow, Image::Glow, true) ||
        !add_feature(FeatureAsset::RainDrop, Image::Drop, true)) {
        return std::unexpected(Error{"procedural feature texture allocation failed"});
    }
    sky_textures_[static_cast<std::size_t>(SkyAsset::MoonDisk)] = make_texture(engine_, Image::Moon, true);
    sky_textures_[static_cast<std::size_t>(SkyAsset::MoonNormal)] = make_texture(engine_, Image::MoonNormal, false);
    sky_textures_[static_cast<std::size_t>(SkyAsset::MilkyWay)] = make_texture(engine_, Image::Stars, true);
    if (std::ranges::any_of(sky_textures_, [](const auto* p) { return p == nullptr; })) {
        return std::unexpected(Error{"procedural sky texture allocation failed"});
    }
    lens_dirt_ = make_texture(engine_, Image::LensDirt, true);
    if (!lens_dirt_) return std::unexpected(Error{"procedural lens dirt texture allocation failed"});
    std::printf("  Render test assets: 12 procedural textures, complete mip chains\n");
    return {};
}

RenderTestSceneAssets::~RenderTestSceneAssets() {
    for (auto* texture : feature_textures_) if (texture) engine_.destroy(texture);
    for (auto* texture : sky_textures_) if (texture) engine_.destroy(texture);
    if (lens_dirt_) engine_.destroy(lens_dirt_);
}

filament::Texture* RenderTestSceneAssets::feature_texture(FeatureAsset asset) const {
    const auto index = static_cast<std::size_t>(asset);
    if (index >= feature_textures_.size() || !feature_textures_[index]) {
        throw std::logic_error("render test scene lacks requested procedural feature texture");
    }
    return feature_textures_[index];
}

filament::Texture* RenderTestSceneAssets::sky_texture(SkyAsset asset) const {
    const auto index = static_cast<std::size_t>(asset);
    if (index >= sky_textures_.size() || !sky_textures_[index]) {
        throw std::logic_error("render test scene lacks requested procedural sky texture");
    }
    return sky_textures_[index];
}

Result<void> RenderTestSceneAssets::set_ground_patch(
        std::size_t instance_index, glm::vec2 local_min, glm::vec2 local_max,
        float local_z, std::size_t blades, std::size_t flowers) {
    if (!std::isfinite(local_min.x) || !std::isfinite(local_min.y) ||
        !std::isfinite(local_max.x) || !std::isfinite(local_max.y) ||
        !std::isfinite(local_z) || local_min.x >= local_max.x || local_min.y >= local_max.y ||
        blades == 0 || blades > 100000 || flowers > 10000) {
        return std::unexpected(Error{"invalid procedural grass ground or root count"});
    }
    RenderTestGrassPatch patch;
    patch.instance_index = instance_index;
    const auto roots = [&](std::size_t count, std::uint32_t seed) {
        std::vector<GrassRoot> result(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = static_cast<std::uint32_t>(i);
            const float x = hash_unit(id * 2u + seed);
            const float y = hash_unit(id * 2u + seed + 1u);
            result[i].position = {local_min.x + x * (local_max.x - local_min.x),
                                  local_min.y + y * (local_max.y - local_min.y), local_z};
        }
        return result;
    };
    patch.roots = roots(blades, 0x71a55u);
    patch.flowers = roots(flowers, 0xf10a3u);
    grass_patches_.clear();
    grass_patches_.push_back(std::move(patch));
    return {};
}

filament::Texture* resolve_feature_texture(
        RenderContext& ctx, FeatureAsset asset, TextureSemantic semantic) {
    if (ctx.test_scene_assets) return ctx.test_scene_assets->feature_texture(asset);
    return ctx.resources->get_filament_texture(ctx.resources->load_texture(
        feature_asset_path(ctx.path_resolver(), asset), semantic));
}

} // namespace heaven
