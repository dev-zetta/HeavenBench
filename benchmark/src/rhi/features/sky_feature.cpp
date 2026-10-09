/*
 * HeavenBench — Procedural sky-sphere feature implementation
 *
 * Moved verbatim out of FilamentBackend::init_sky / update_sky. The
 * IBL cubemap probe stays in the backend (it is lighting infrastructure,
 * not a visible-sky concern). The backend keeps Filament's cubemap
 * Skybox attached only while this feature is disabled.
 */
#include "rhi/features/sky_feature.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "render_tuning.h"
#include "rhi/png_image.h"
#include "rhi/sky_atmosphere.h"
#include "engine/path_resolver.h"

#include <backend/PixelBufferDescriptor.h>

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <math/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <utility>

namespace heaven {
namespace {

const filament::Texture* upload_png_texture(filament::Engine& engine,
                                      const std::filesystem::path& path,
                                      filament::Texture::InternalFormat format,
                                      bool mipmaps,
                                      AssetFileSystemRef assets,
                                      IResourceProvider* resources,
                                      TextureSemantic semantic) {
    Result<PngImage> image = load_png_rgba8(path.generic_string(), assets);
    if (!image) {
        std::fprintf(stderr, "Sky: %s\n", image.error().message.c_str());
        return nullptr;
    }

    const uint32_t longest = std::max(image->width, image->height);
    const uint8_t levels = mipmaps
        ? static_cast<uint8_t>(std::floor(std::log2(std::max(longest, 1u))) + 1.0f)
        : 1;
    // Filament TextureUsage explicitly permits combinations of these bit flags.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto mipmapped_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::DEFAULT) |
        std::to_underlying(filament::Texture::Usage::GEN_MIPMAPPABLE));
    const filament::Texture::Usage usage = mipmaps
        ? mipmapped_usage : filament::Texture::Usage::DEFAULT;
    const filament::Texture* tex = filament::Texture::Builder()
        .width(image->width)
        .height(image->height)
        .levels(levels)
        .sampler(filament::Texture::Sampler::SAMPLER_2D)
        .format(format)
        .usage(usage)
        .build(engine);
    if (!tex) return nullptr;

    const size_t size = image->rgba.size();
    auto* pixels = new uint8_t[size];
    std::memcpy(pixels, image->rgba.data(), size);
    auto cleanup = [](void* p, size_t, void*) {
        delete[] static_cast<uint8_t*>(p);
    };
    tex->setImage(engine, 0,
        filament::Texture::PixelBufferDescriptor(
            pixels, size, filament::Texture::Format::RGBA,
            filament::Texture::Type::UBYTE, cleanup));
    if (mipmaps) tex->generateMipmaps(engine);
    if (resources) {
        resources->record_external_original_texture(
            image->width, image->height, semantic, mipmaps);
    }
    return tex;
}

} // namespace

void SkyFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    material_ = filament::Material::Builder()
        .package(k_heaven_sky_filamat, k_heaven_sky_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  Sky: material failed to load\n");
        return;
    }

    const filament::Texture* placeholder = ctx.resources->placeholder_texture();

    mi_ = material_->createInstance();
    if (!mi_) {
        std::printf("  Sky: material instance failed to load\n");
        return;
    }
    material_tuning::bind_sky(*mi_);

    const auto load_sky_texture = [&](SkyAsset asset,
                                      TextureSemantic semantic,
                                      filament::Texture::InternalFormat png_format,
                                      bool& owned) -> const filament::Texture* {
        if (ctx.test_scene_assets) {
            owned = false;
            return ctx.test_scene_assets->sky_texture(asset);
        }
        const auto path = ctx.path_resolver().sky_asset_path(asset);
        // Generated sky variants are DDS entries indexed by their original PNG
        // logical path. Only ask the resource cache for one when the mounted
        // manifest actually has the mapping; Original remains byte-for-byte on
        // the existing PNG decoder path.
        if (config.texture_quality != TextureQuality::Original && ctx.assets &&
            ctx.assets->get().texture_variant(path)) {
            const uint64_t handle = ctx.resources->load_texture(
                path, semantic);
            if (const filament::Texture* texture =
                    ctx.resources->get_filament_texture(handle)) {
                owned = false;
                return texture;
            }
        }
        owned = true;
        return upload_png_texture(
            *engine_, path, png_format, true, ctx.assets,
            ctx.resources, semantic);
    };

    moon_tex_ = load_sky_texture(
        SkyAsset::MoonDisk, TextureSemantic::ColorSRGB,
        filament::Texture::InternalFormat::SRGB8_A8, moon_tex_owned_);
    moon_normal_tex_ = load_sky_texture(
        SkyAsset::MoonNormal, TextureSemantic::NormalLinear,
        filament::Texture::InternalFormat::RGBA8, moon_normal_tex_owned_);
    milky_way_tex_ = load_sky_texture(
        SkyAsset::MilkyWay, TextureSemantic::ColorSRGB,
        filament::Texture::InternalFormat::SRGB8_A8, milky_way_tex_owned_);

    filament::TextureSampler clamp_sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    filament::TextureSampler repeat_sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    mi_->setParameter("moonTexture",
        moon_tex_ ? moon_tex_ : placeholder, clamp_sampler);
    mi_->setParameter("moonNormal",
        moon_normal_tex_ ? moon_normal_tex_ : placeholder, clamp_sampler);
    mi_->setParameter("milkyWayTexture",
        milky_way_tex_ ? milky_way_tex_ : placeholder, repeat_sampler);
    mi_->setParameter("time", 0.0f);

    // Build a UV sphere centred at the origin. update() re-centres it
    // on the camera each frame and scales it to the skybox radius. We
    // use a high-tessellation sphere because the viewDir interpolation
    // across triangles must be smooth — a low-poly cube produces
    // visible straight-edge silhouettes after per-pixel `normalize`.
    struct V { float x, y, z, u, v; };
    constexpr int LATS = render_tuning::sky.sphere_latitudes;
    constexpr int LONS = render_tuning::sky.sphere_longitudes;
    constexpr int VCOUNT = (LATS + 1) * (LONS + 1);
    constexpr int ICOUNT = LATS * LONS * 6;
    static V          sky_vertices[VCOUNT];
    static uint16_t   sky_indices[ICOUNT];
    {
        constexpr float PI = std::numbers::pi_v<float>;
        int v = 0;
        for (int la = 0; la <= LATS; ++la) {
            float t = static_cast<float>(la) / static_cast<float>(LATS);
            float theta = t * PI;
            float sin_t = std::sin(theta);
            float cos_t = std::cos(theta);
            for (int lo = 0; lo <= LONS; ++lo) {
                float u = static_cast<float>(lo) / static_cast<float>(LONS);
                float phi = u * 2.0f * PI;
                float sin_p = std::sin(phi);
                float cos_p = std::cos(phi);
                sky_vertices[v++] = V{ sin_t * cos_p,
                                        sin_t * sin_p,
                                        cos_t, u, t };
            }
        }
        // Inward-facing winding so the viewer (inside the sphere)
        // sees the front side. We keep `culling(false)` anyway so
        // winding is mostly cosmetic.
        int idx = 0;
        for (int la = 0; la < LATS; ++la) {
            for (int lo = 0; lo < LONS; ++lo) {
                uint16_t a = static_cast<uint16_t>( la      * (LONS + 1) + lo);
                uint16_t b = static_cast<uint16_t>( la      * (LONS + 1) + lo + 1);
                uint16_t c = static_cast<uint16_t>((la + 1) * (LONS + 1) + lo);
                uint16_t d = static_cast<uint16_t>((la + 1) * (LONS + 1) + lo + 1);
                sky_indices[idx++] = a; sky_indices[idx++] = c; sky_indices[idx++] = b;
                sky_indices[idx++] = b; sky_indices[idx++] = c; sky_indices[idx++] = d;
            }
        }
    }

    vb_ = filament::VertexBuffer::Builder()
        .vertexCount(VCOUNT)
        .bufferCount(2)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(V))
        .attribute(filament::VertexAttribute::UV0, 0,
                   filament::VertexBuffer::AttributeType::FLOAT2,
                   offsetof(V, u), sizeof(V))
        .attribute(filament::VertexAttribute::TANGENTS, 1,
                   filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
        .normalized(filament::VertexAttribute::TANGENTS)
        .build(*engine_);
    vb_->setBufferAt(*engine_, 0,
        filament::VertexBuffer::BufferDescriptor(
            sky_vertices, VCOUNT * sizeof(V)));
    {
        static filament::math::short4 t[VCOUNT];
        for (auto& tangent : t) tangent = {0, 0, 16383, 16383};
        vb_->setBufferAt(*engine_, 1,
            filament::VertexBuffer::BufferDescriptor(
                t, VCOUNT * sizeof(filament::math::short4)));
    }

    ib_ = filament::IndexBuffer::Builder()
        .indexCount(ICOUNT)
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(*engine_);
    ib_->setBuffer(*engine_,
        filament::IndexBuffer::BufferDescriptor(
            sky_indices, ICOUNT * sizeof(uint16_t)));

    entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb_, ib_)
        .material(0, mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(7)   // drawn last among opaques (highest priority value)
        .boundingBox(uncullable_box())
        .build(*engine_, entity_);

    auto& tcm = engine_->getTransformManager();
    tcm.create(entity_);

    enabled_ = config.features.sky;
    if (enabled_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    }

    std::printf("  Sky: analytic atmosphere + moon/stars/Milky Way%s\n",
                enabled_ ? "" : " (disabled)");
}

void SkyFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!entity_) return;
    if (on && !in_scene_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        ctx.scene->remove(entity_);
        in_scene_ = false;
    }
}

void SkyFeature::update(RenderContext& ctx, const FrameContext& frame) {
    if (!mi_ || !entity_) return;
    const Camera& camera = *frame.camera;
    const LightingParams& lighting = *frame.lighting;

    // Centre the sphere on the camera with a large radius. The
    // radius needs to be far enough that the camera never punches
    // through it (the camera moves up to ~150 m), but well inside
    // the camera's far-plane (2000 m).
    glm::mat4 m(1.0f);
    constexpr float r = render_tuning::sky.sphere_radius;
    m[0][0] = r; m[1][1] = r; m[2][2] = r;
    m[3] = glm::vec4(camera.position.x, camera.position.y, camera.position.z, 1.0f);
    auto& tcm = ctx.engine->getTransformManager();
    auto ti = tcm.getInstance(entity_);
    if (ti) tcm.setTransform(ti, glm_to_filament(m));

    // Per-frame parameters: analytic Z-up atmosphere, sun disk, moon,
    // Milky Way and screen-adaptive stars.
    mi_->setParameter("sunDir",
        filament::math::float3{lighting.sun_dir.x,
                                lighting.sun_dir.y,
                                lighting.sun_dir.z});
    mi_->setParameter("sunColor",
        filament::math::float3{lighting.sun_color.r,
                                lighting.sun_color.g,
                                lighting.sun_color.b});

    const SkyAtmosphereParams sky = make_sky_atmosphere_params(lighting);
    mi_->setParameter("depthR",
        filament::math::float3{sky.depth_r.x, sky.depth_r.y, sky.depth_r.z});
    mi_->setParameter("depthM",
        filament::math::float3{sky.depth_m.x, sky.depth_m.y, sky.depth_m.z});
    mi_->setParameter("ozone",
        filament::math::float3{sky.ozone.x, sky.ozone.y, sky.ozone.z});
    mi_->setParameter("multiScattering",
        filament::math::float3{sky.multi_scattering.x, sky.multi_scattering.y, sky.multi_scattering.z});
    mi_->setParameter("nightColor",
        filament::math::float3{sky.night_color.x, sky.night_color.y, sky.night_color.z});
    mi_->setParameter("miePhaseParams",
        filament::math::float2{sky.mie_phase_params.x, sky.mie_phase_params.y});
    mi_->setParameter("horizonGlow", sky.horizon_glow);
    mi_->setParameter("sunRadiance", sky.sun_radiance);
    mi_->setParameter("sunDiskRadiance", sky.sun_disk_radiance);
    mi_->setParameter("sunCosRadius", sky.sun_cos_radius);
    mi_->setParameter("sunLimbDarkening", sky.sun_limb_darkening);
    mi_->setParameter("contrast", sky.contrast);
    mi_->setParameter("moonDir",
        filament::math::float3{sky.moon_dir.x, sky.moon_dir.y, sky.moon_dir.z});
    mi_->setParameter("moonIntensity", sky.moon_intensity);
    mi_->setParameter("moonCosRadius", sky.moon_cos_radius);
    mi_->setParameter("moonSinRadius", sky.moon_sin_radius);
    mi_->setParameter("milkyWayRow0",
        filament::math::float3{sky.milky_way_row0.x, sky.milky_way_row0.y, sky.milky_way_row0.z});
    mi_->setParameter("milkyWayRow1",
        filament::math::float3{sky.milky_way_row1.x, sky.milky_way_row1.y, sky.milky_way_row1.z});
    mi_->setParameter("milkyWayRow2",
        filament::math::float3{sky.milky_way_row2.x, sky.milky_way_row2.y, sky.milky_way_row2.z});
    mi_->setParameter("milkyWayControl",
        filament::math::float3{sky.milky_way_control.x, sky.milky_way_control.y, sky.milky_way_control.z});
    const float pixel_scale = std::tan(glm::radians(camera.fov) * 0.5f)
        / static_cast<float>(std::max(frame.viewport_h, 1));
    mi_->setParameter("starControl",
        filament::math::float4{0.0015f, 1.0f, 360.0f, pixel_scale * 1.3f});
    mi_->setParameter("time", frame.time);
}

void SkyFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    if (entity_) {
        if (in_scene_) ctx.scene->remove(entity_);
        engine_->destroy(entity_);
        utils::EntityManager::get().destroy(entity_);
        entity_ = {};
    }
    if (mi_) { engine_->destroy(mi_); mi_ = nullptr; }
    if (material_) { engine_->destroy(material_); material_ = nullptr; }
    if (vb_) { engine_->destroy(vb_); vb_ = nullptr; }
    if (ib_) { engine_->destroy(ib_); ib_ = nullptr; }
    if (moon_tex_ && moon_tex_owned_) engine_->destroy(moon_tex_);
    moon_tex_ = nullptr;
    moon_tex_owned_ = false;
    if (moon_normal_tex_ && moon_normal_tex_owned_) {
        engine_->destroy(moon_normal_tex_);
    }
    moon_normal_tex_ = nullptr;
    moon_normal_tex_owned_ = false;
    if (milky_way_tex_ && milky_way_tex_owned_) engine_->destroy(milky_way_tex_);
    milky_way_tex_ = nullptr;
    milky_way_tex_owned_ = false;
    // cloud_tex_/cloud_mask_ are owned by the resource cache.
    cloud_tex_ = cloud_mask_ = nullptr;
}

} // namespace heaven
