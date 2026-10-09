// GPU regression for geometry sampling of height maps. Fine texture detail
// must not alias into coarse mesh positions, while resolvable relief survives.
// The fixture uses the shipping MeshStore and embedded lit material, with no
// original assets, reference captures, shadow bias or copied LOD calculation.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/depth_prepass.h"
#include "rhi/depth_proxy_builder.h"
#include "render_tuning.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr std::uint32_t kExtent = 256;
constexpr std::uint32_t kSegments = 15;
constexpr float kNeutralHeight = 127.0f / 255.0f;
using Pixels = std::vector<std::uint8_t>;

AssetFileSystem fixture_asset_mount() {
    // Memory assets use the ordinary logical filesystem and TextureStore loader.
    // Running from the repository root needs no original Heaven asset files.
    auto opened = AssetFileSystem::open(std::filesystem::current_path().string());
    if (!opened) throw std::runtime_error("Displacement fixture asset mount failed");
    return std::move(*opened);
}

struct StoredHeight {
    std::uint64_t handle;
    filament::Texture* texture;
};

class DisplacementFixture {
public:
    explicit DisplacementFixture(filament::Engine& engine, bool two_dimensional = false,
                                 bool window_material = false)
        : engine_(engine), window_material_(window_material), assets_(fixture_asset_mount()),
          textures_(engine, std::cref(assets_)), materials_(engine, textures_), meshes_(engine) {
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) {
            throw std::runtime_error("Displacement fixture materials failed");
        }
        renderer_ = engine_.createRenderer();
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        swapchain_ = engine_.createSwapChain(kExtent, kExtent);
        camera_entity_ = utils::EntityManager::get().create();
        auto* camera = engine_.createCamera(camera_entity_);
        camera->setProjection(filament::Camera::Projection::ORTHO,
                              -1.35, 1.35, -1.35, 1.35, 0.1, 20.0);
        camera->lookAt({0, 3, 5}, {0, -0.35, 0}, {0, 0, 1});
        camera->setExposure(1.0f);
        scene_ = engine_.createScene();
        sky_ = filament::Skybox::Builder().color({0, 0, 0, 1}).build(engine_);
        scene_->setSkybox(sky_);
        // Texture usage is a documented bitmask.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kExtent).height(kExtent).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        // With post-processing off Filament can render directly into this
        // target. It must own depth so the skybox cannot overwrite the strip.
        depth_ = filament::Texture::Builder().width(kExtent).height(kExtent).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        view_ = engine_.createView();
        view_->setScene(scene_);
        view_->setCamera(camera);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kExtent, kExtent});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setAntiAliasing(filament::AntiAliasing::NONE);

        Mesh mesh;
        mesh.path = "displacement-fixture";
        Surface surface;
        surface.name = "strip";
        const std::uint32_t rows = two_dimensional ? kSegments : 1;
        for (std::uint32_t row = 0; row <= rows; ++row) {
            for (std::uint32_t column = 0; column <= kSegments; ++column) {
                Vertex vertex{};
                vertex.px = -1.0f + 2.0f * static_cast<float>(column) / kSegments;
                vertex.py = static_cast<float>(row) / static_cast<float>(rows) - 0.9f;
                vertex.nz = 1.0f;
                // Avoid a special alignment where every vertex lands exactly
                // between the alternating texels and accidentally cancels them.
                vertex.u = (static_cast<float>(column) + 0.21f) / kSegments;
                vertex.v = two_dimensional
                    ? (static_cast<float>(row) + 0.37f) / kSegments
                    : static_cast<float>(row);
                surface.vertices.push_back(vertex);
            }
        }
        for (std::uint32_t row = 0; row < rows; ++row) {
            for (std::uint32_t column = 0; column < kSegments; ++column) {
                const auto a = row * (kSegments + 1) + column;
                const auto b = a + 1;
                const auto d = a + kSegments + 1;
                const auto c = d + 1;
                surface.indices.insert(surface.indices.end(), {a, b, c, a, c, d});
            }
        }
        mesh.surfaces.push_back(std::move(surface));
        gpu_mesh_ = meshes_.upload(mesh);
        const auto buffers = meshes_.find(gpu_mesh_.surfaces.front().handle);
        GpuMaterial initial_material;
        if (window_material_) {
            initial_material.material_name = "building_tessellation_windows";
            initial_material.alpha_ref = 0.5f;
            initial_material.two_sided = true;
        }
        material_ = materials_.get_or_create("displacement-fixture", "strip", initial_material);
        if (!material_) throw std::runtime_error("Displacement fixture instance failed");
        material_->setParameter("geometryDisplacement", true);
        material_->setParameter("hasHeight", two_dimensional);
        // Put the complete fixture inside the full-displacement distance band.
        material_->setParameter("displacementParams", filament::math::float4{1, 1, 1000, 0.001f});
        // Constant emission isolates the silhouette from shading, so a normal
        // map, tone mapper or direct light cannot imitate a geometry fix.
        material_->setParameter("hasEmissive", true);
        material_->setParameter("emissiveMap", textures_.placeholder(),
            filament::TextureSampler(filament::TextureSampler::MinFilter::NEAREST,
                filament::TextureSampler::MagFilter::NEAREST));
        material_->setParameter("emissionTint", filament::math::float3{1, 1, 1});
        material_->setParameter("emissionScale", 100.0f);
        material_->setParameter("surfaceEmissionTuning", filament::math::float4{0, 1, 0, 0.5f});
        // Exercise the standard directional-light variant also used by the
        // scene. The constant vertex normals and saturated white emission keep
        // this light from changing the binary silhouette measurement.
        light_ = utils::EntityManager::get().create();
        filament::LightManager::Builder(filament::LightManager::Type::SUN)
            .color({1, 1, 1}).intensity(100000.0f).direction({0, 0, -1})
            .castShadows(false).build(engine_, light_);
        scene_->addEntity(light_);
        entity_ = utils::EntityManager::get().create();
        filament::RenderableManager::Builder(1)
            .boundingBox({{0, -0.4f, 0}, {2, 2, 1}}).culling(false)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      buffers.vb, buffers.ib)
            .material(0, material_).castShadows(false).receiveShadows(false)
            .build(engine_, entity_);
        scene_->addEntity(entity_);
        core_ = {&engine_, scene_, view_, renderer_, camera};
    }

    ~DisplacementFixture() {
        if (shared_depth_) {
            depth_proxies_.clear(core_, shared_depth_->proxy_scene());
            shared_depth_->proxy_scene()->setSkybox(nullptr);
            shared_depth_.reset();
        }
        view_->setRenderTarget(nullptr);
        scene_->remove(entity_);
        engine_.destroy(entity_);
        utils::EntityManager::get().destroy(entity_);
        scene_->remove(light_);
        engine_.destroy(light_);
        utils::EntityManager::get().destroy(light_);
        engine_.destroy(view_);
        scene_->setSkybox(nullptr);
        engine_.destroy(scene_);
        engine_.destroy(sky_);
        materials_.clear_instances();
        for (auto* height : heights_) engine_.destroy(height);
        engine_.destroy(target_);
        engine_.destroy(color_);
        engine_.destroy(depth_);
        engine_.destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }
    DisplacementFixture(const DisplacementFixture&) = delete;
    DisplacementFixture& operator=(const DisplacementFixture&) = delete;

    filament::Texture* height_texture(std::uint32_t width, bool relief, bool carrier) {
        std::uint8_t levels = 1;
        for (auto size = width; size > 1; size /= 2) ++levels;
        auto* texture = filament::Texture::Builder().width(width).height(1).levels(levels)
            .format(filament::Texture::InternalFormat::R32F).build(engine_);
        heights_.push_back(texture);
        std::vector<float> samples(width);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            samples[x] = kNeutralHeight;
            if (relief) samples[x] += 0.18f * std::sin(2.0f * std::numbers::pi_v<float> * u);
            if (carrier) samples[x] += x % 2 == 0 ? 0.22f : -0.22f;
        }
        // An ordinary box-filtered mip chain, independent of the production
        // mesh-footprint/LOD calculation. Every paired carrier texel averages
        // to zero; the one-cycle relief remains visible at mesh resolution.
        for (std::uint8_t level = 0; level < levels; ++level) {
            auto* upload = new float[samples.size()];
            std::copy(samples.begin(), samples.end(), upload);
            texture->setImage(engine_, level, filament::backend::PixelBufferDescriptor(
                upload, samples.size() * sizeof(float), filament::backend::PixelDataFormat::R,
                filament::backend::PixelDataType::FLOAT,
                [](void* data, std::size_t, void*) { delete[] static_cast<float*>(data); }));
            if (samples.size() > 1) {
                std::vector<float> next(samples.size() / 2);
                for (std::size_t x = 0; x < next.size(); ++x) {
                    next[x] = (samples[2 * x] + samples[2 * x + 1]) * 0.5f;
                }
                samples = std::move(next);
            }
        }
        return texture;
    }

    // A 15 by 15 grid against 256 texels has a noninteger mesh footprint LOD.
    // Both texture axes vary, and the phase offsets avoid accidental cancellation.
    StoredHeight height_texture_2d(bool relief, std::uint32_t levels = 9) {
        constexpr std::uint32_t size = 256;
        if (levels < 1 || levels > 9) {
            throw std::runtime_error("Displacement fixture invalid mip count");
        }
        std::vector<std::uint8_t> bytes;
        auto put_u32 = [&](std::uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
            }
        };
        std::array<std::uint32_t, 32> header{};
        header[0] = 0x20534444u;
        header[1] = 124;
        header[2] = 0x2100fu;
        header[3] = size;
        header[4] = size;
        header[5] = size * 4;
        header[7] = levels;
        header[19] = 32;
        header[20] = 0x41;
        header[22] = 32;
        header[23] = 0x000000ffu;
        header[24] = 0x0000ff00u;
        header[25] = 0x00ff0000u;
        header[26] = 0xff000000u;
        header[27] = 0x401008u;
        for (auto value : header) put_u32(value);

        std::vector<std::uint8_t> samples(static_cast<std::size_t>(size) * size);
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / size;
                const float v = (static_cast<float>(y) + 0.5f) / size;
                float height = kNeutralHeight;
                if (relief) {
                    const float tau = 2.0f * std::numbers::pi_v<float>;
                    height += 0.15f * std::sin(tau * (3.0f * u + 0.2f * v))
                            + 0.11f * std::cos(tau * (2.0f * v - 0.15f * u))
                            + 0.08f * std::sin(tau * (7.0f * u + 5.0f * v))
                            + ((x + y) % 2 == 0 ? 0.09f : -0.09f);
                }
                samples[y * size + x] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(height, 0.0f, 1.0f) * 255.0f));
            }
        }
        for (std::uint32_t level = 0, mip_size = size; level < levels;
             ++level, mip_size /= 2) {
            for (auto height : samples) {
                bytes.insert(bytes.end(), {height, height, height, 255});
            }
            if (level + 1 == levels) break;
            const auto next_size = mip_size / 2;
            std::vector<std::uint8_t> next(static_cast<std::size_t>(next_size) * next_size);
            for (std::uint32_t y = 0; y < next_size; ++y) {
                for (std::uint32_t x = 0; x < next_size; ++x) {
                    const auto a = (2 * y) * mip_size + 2 * x;
                    const unsigned sum = samples[a] + samples[a + 1]
                                       + samples[a + mip_size] + samples[a + mip_size + 1];
                    next[y * next_size + x] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
            samples = std::move(next);
        }
        const std::string path = std::string(relief ? "smoke-displacement/relief"
                                                  : "smoke-displacement/neutral")
                + (levels == 9 ? ".dds" : "-mips-" + std::to_string(levels) + ".dds");
        auto mounted = assets_.mount_memory(path,
            std::make_shared<const AssetBuffer>(std::move(bytes)));
        if (!mounted) throw std::runtime_error("Displacement fixture memory DDS failed");
        const auto handle = textures_.load(path, TextureSemantic::ScalarLinear);
        auto* texture = textures_.get(handle);
        if (!texture || texture->getWidth() != size || texture->getHeight() != size ||
            texture->getLevels() != levels) {
            throw std::runtime_error("Displacement fixture 2D DDS upload failed");
        }
        return {handle, texture};
    }

    bool capture(filament::Texture* height, Pixels& pixels,
                 float fragment_anisotropy = 1.0f, float geometry_anisotropy = 1.0f) {
        if (window_material_) return false;
        filament::TextureSampler fragment_sampler(
            filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::REPEAT);
        fragment_sampler.setAnisotropy(fragment_anisotropy);
        filament::TextureSampler geometry_sampler(
            filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::REPEAT);
        geometry_sampler.setAnisotropy(geometry_anisotropy);
        material_->setParameter("heightMap", height, fragment_sampler);
        material_->setParameter("geometryHeightMap", height, geometry_sampler);
        return capture_frame(pixels);
    }

    bool capture_routed(StoredHeight height, Pixels& pixels,
                        float fragment_anisotropy = 8.0f) {
        // Keep texture binding and the window maximum mip in the production
        // cache. Distinct keys retain one instance for each bound texture.
        if (textures_.get(height.handle) != height.texture) return false;
        materials_.set_tessellation_config(1.0f, 1.0f);
        GpuMaterial gpu_material;
        gpu_material.height = height.handle;
        gpu_material.geometry_displacement = true;
        gpu_material.tess_scale = 1.0f;
        gpu_material.tess_power = 1.0f;
        gpu_material.tess_threshold = 1000.0f;
        gpu_material.tess_distance = 1000.0f;
        if (window_material_) {
            gpu_material.material_name = "building_tessellation_windows";
            gpu_material.alpha_ref = 0.5f;
            gpu_material.two_sided = true;
        }
        material_ = materials_.get_or_create(
            "displacement-fixture-routed-" + std::to_string(height.handle),
            "strip", gpu_material);
        if (!material_) return false;
        if (window_material_) {
            const auto* base = material_->getMaterial();
            if (std::string_view(base->getName()) != "heaven_window" ||
                !base->hasParameter("geometryHeightMaxLod")) return false;
            const float actual_last_mip =
                    material_->getParameter<float>("geometryHeightMaxLod");
            const float expected_last_mip = static_cast<float>(height.texture->getLevels() - 1);
            std::printf("[smoke_displacement_filter] routed %s, texture levels %zu, "
                        "bound maximum mip %.0f\n", base->getName(),
                        height.texture->getLevels(), actual_last_mip);
            if (actual_last_mip != expected_last_mip) return false;
            // Saturate the existing window emission controls so coverage is
            // independent of glass shading and fragment height anisotropy.
            material_->setParameter("windowEmissionTuning",
                filament::math::float4{0, 0.5f, 0, 100});
            material_->setParameter("windowEmissionColorTuning",
                filament::math::float4{0, 1, 1, 1});
        }
        material_->setParameter("hasEmissive", true);
        material_->setParameter("emissiveMap", textures_.placeholder(),
            filament::TextureSampler(filament::TextureSampler::MinFilter::NEAREST,
                filament::TextureSampler::MagFilter::NEAREST));
        material_->setParameter("emissionTint", filament::math::float3{1, 1, 1});
        material_->setParameter("emissionScale", 100.0f);
        material_->setParameter("surfaceEmissionTuning", filament::math::float4{0, 1, 0, 0.5f});
        filament::TextureSampler fragment_sampler(
            filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::REPEAT);
        fragment_sampler.setAnisotropy(fragment_anisotropy);
        material_->setParameter("heightMap", height.texture, fragment_sampler);
        auto& renderables = engine_.getRenderableManager();
        renderables.setMaterialInstanceAt(renderables.getInstance(entity_), 0, material_);
        return capture_frame(pixels);
    }

    bool capture_shared_depth(StoredHeight height, Pixels& pixels) {
        if (!shared_depth_) {
            shared_depth_ = std::make_unique<DepthPrePass>(core_);
            // Match the pixel grids for a strict coverage comparison, independent
            // of the production depth downsample. Use the real depth resources.
            shared_depth_->ensure(static_cast<int>(kExtent) * render_tuning::depth.downsample,
                                  static_cast<int>(kExtent) * render_tuning::depth.downsample);
        }
        if (!shared_depth_->material() || !shared_depth_->proxy_scene() ||
            !shared_depth_->depth_texture() || !shared_depth_->view() ||
            shared_depth_->width() != static_cast<int>(kExtent) ||
            shared_depth_->height() != static_cast<int>(kExtent)) return false;
        depth_proxies_.clear(core_, shared_depth_->proxy_scene());
        GpuMaterial gpu_material;
        gpu_material.height = height.handle;
        gpu_material.geometry_displacement = true;
        gpu_material.tess_scale = 1.0f;
        gpu_material.tess_power = 1.0f;
        gpu_material.tess_threshold = 1000.0f;
        gpu_material.tess_distance = 1000.0f;
        // Window diffuse alpha is uniformly opaque in this fixture, but
        // still exercise the actual masked proxy material and binding.
        gpu_material.alpha_ref = window_material_ ? 0.5f : 0.0f;
        const std::array instances{Instance{0, glm::mat4(1.0f)}};
        const std::array gpu_meshes{gpu_mesh_};
        const MaterialResolver resolve = [&](const std::string&, const std::string&) {
            return gpu_material;
        };
        depth_proxies_.build(core_, meshes_, shared_depth_->material(),
            shared_depth_->masked_material(), textures_, shared_depth_->proxy_scene(),
            instances, gpu_meshes, resolve, 1.0f, 1.0f);
        if (!depth_proxies_.built()) return false;
        // Linear eye depth is greater than one metre throughout this fixture.
        // In the RGBA8 capture it becomes white; a black background yields the
        // same binary coverage mask as the saturated visible emission. This
        // tests the actual proxy binding without copying its sampling policy.
        shared_depth_->proxy_scene()->setSkybox(sky_);
        view_->setScene(shared_depth_->proxy_scene());
        const bool complete = capture_frame(pixels);
        view_->setScene(scene_);
        return complete;
    }

private:
    bool capture_frame(Pixels& pixels) {
        if (!renderer_->beginFrame(swapchain_)) return false;
        renderer_->render(view_);
        pixels.resize(static_cast<std::size_t>(kExtent) * kExtent * 4);
        std::atomic_bool complete{false};
        renderer_->readPixels(target_, 0, 0, kExtent, kExtent,
            filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        renderer_->endFrame();
        engine_.flushAndWait();
        return complete.load();
    }

    filament::Engine& engine_;
    bool window_material_ = false;
    AssetFileSystem assets_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::Skybox* sky_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::MaterialInstance* material_ = nullptr;
    std::vector<filament::Texture*> heights_;
    utils::Entity camera_entity_, entity_, light_;
    GpuMesh gpu_mesh_;
    BackendCore core_;
    DepthProxyBuilder depth_proxies_;
    std::unique_ptr<DepthPrePass> shared_depth_;
};

std::size_t mask_difference(const Pixels& a, const Pixels& b) {
    std::size_t different = 0;
    for (std::size_t pixel = 0; pixel < a.size() / 4; ++pixel) {
        if ((a[pixel * 4] > 127) != (b[pixel * 4] > 127)) ++different;
    }
    return different;
}

std::size_t mask_area(const Pixels& pixels) {
    std::size_t area = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        if (pixels[pixel * 4] > 127) ++area;
    }
    return area;
}
bool differences_are_at_boundary(const Pixels& a, const Pixels& b) {
    // Manual highp and hardware bilinear weights can round differently. Every
    // admitted mismatch must be within one pixel of the other coverage mask.
    for (int y = 0; std::cmp_less(y, kExtent); ++y) {
        for (int x = 0; std::cmp_less(x, kExtent); ++x) {
            const auto pixel = static_cast<std::size_t>(y) * kExtent + x;
            const bool covered_a = a[pixel * 4] > 127;
            const bool covered_b = b[pixel * 4] > 127;
            if (covered_a == covered_b) continue;
            const auto& missing = covered_a ? b : a;
            bool adjacent_coverage = false;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || std::cmp_greater_equal(nx, kExtent) ||
                        std::cmp_greater_equal(ny, kExtent)) continue;
                    const auto neighbor = static_cast<std::size_t>(ny) * kExtent + nx;
                    adjacent_coverage |= missing[neighbor * 4] > 127;
                }
            }
            if (!adjacent_coverage) return false;
        }
    }
    return true;
}
} // namespace

CaseResult run_displacement_filter_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto* engine = static_cast<FilamentBackend&>(ctx.backend).debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    DisplacementFixture fixture(*engine);
    std::array<Pixels, 4> images;
    const std::array textures{
        fixture.height_texture(64, true, true),
        fixture.height_texture(256, true, true),
        fixture.height_texture(64, true, false),
        fixture.height_texture(64, false, false)};
    for (std::size_t image = 0; image < images.size(); ++image) {
        if (!fixture.capture(textures[image], images[image])) return CaseResult::Fail;
        std::uint8_t maximum = 0;
        for (std::size_t pixel = 0; pixel < images[image].size() / 4; ++pixel) {
            maximum = std::max(maximum, images[image][pixel * 4]);
        }
        std::printf("[smoke_displacement_filter] image %zu, maximum red %u\n", image,
                    static_cast<unsigned>(maximum));
    }
    const auto carrier_error = mask_difference(images[0], images[2]);
    const auto resolution_error = mask_difference(images[0], images[1]);
    const auto retained_relief = mask_difference(images[2], images[3]);
    const auto area = mask_area(images[3]);
    std::printf("[smoke_displacement_filter] carrier pixels %zu, resolution pixels %zu, "
                "retained relief pixels %zu, flat area %zu\n",
                carrier_error, resolution_error, retained_relief, area);
    // Small raster-boundary differences are allowed between independently
    // sampled texture resolutions. Wholesale displacement suppression would
    // pass those comparisons but fails the explicit low-frequency control.
    if (!(carrier_error <= 8 && resolution_error <= 32 && retained_relief >= 128
            && area >= 5000 && area <= 25000)) return CaseResult::Fail;

    DisplacementFixture grid(*engine, true);
    const auto relief = grid.height_texture_2d(true);
    const auto neutral = grid.height_texture_2d(false);
    Pixels isotropic, anisotropic_fragment, neutral_pixels, shared_depth;
    Pixels anisotropic_geometry;
    if (!grid.capture(relief.texture, isotropic, 1.0f, 1.0f) ||
        !grid.capture(relief.texture, anisotropic_fragment, 8.0f, 1.0f) ||
        !grid.capture(neutral.texture, neutral_pixels, 8.0f, 1.0f) ||
        !grid.capture_shared_depth(relief, shared_depth) ||
        !grid.capture(relief.texture, anisotropic_geometry, 1.0f, 8.0f)) {
        return CaseResult::Fail;
    }
    const auto fragment_motion = mask_difference(isotropic, anisotropic_fragment);
    const auto depth_mismatch = mask_difference(isotropic, shared_depth);
    const auto relief_control = mask_difference(isotropic, neutral_pixels);
    const auto sampler_sensitivity = mask_difference(isotropic, anisotropic_geometry);
    const auto relief_area = mask_area(isotropic);
    std::printf("[smoke_displacement_filter] 2D fragment sampler motion %zu, "
                "shared depth mismatch %zu, retained relief %zu, area %zu, "
                "anisotropic geometry sensitivity %zu\n",
                fragment_motion, depth_mismatch, relief_control, relief_area,
                sampler_sensitivity);
    // Independence must be exact: every paired capture uses identical geometry
    // inputs and no temporal or antialiasing state. The relief control prevents
    // a disabled displacement path from passing. Geometry sensitivity reports
    // whether this GPU exhibits the original anisotropic explicit-LOD behavior;
    // drivers that produce identical fields still satisfy the separation rule.
    if (!(fragment_motion == 0 && depth_mismatch == 0 && relief_control >= 128
          && relief_area >= 5000 && relief_area <= 25000)) return CaseResult::Fail;

    DisplacementFixture windows(*engine, true, true);
    Pixels full_window, partial_window;
    for (const std::uint32_t levels : {9u, 3u}) {
        const auto reference_height = levels == 9 ? relief : grid.height_texture_2d(true, levels);
        const auto window_height = windows.height_texture_2d(true, levels);
        const auto window_neutral = windows.height_texture_2d(false, levels);
        Pixels reference, window_anisotropic, window_isotropic, window_depth, flat_window;
        if (!grid.capture_routed(reference_height, reference) ||
            !windows.capture_routed(window_height, window_anisotropic) ||
            !windows.capture_routed(window_height, window_isotropic, 1.0f) ||
            !windows.capture_shared_depth(window_height, window_depth) ||
            !windows.capture_routed(window_neutral, flat_window)) return CaseResult::Fail;
        const auto window_fragment_motion = mask_difference(window_anisotropic, window_isotropic);
        const auto window_lit_mismatch = mask_difference(window_anisotropic, reference);
        const auto window_depth_mismatch = mask_difference(window_anisotropic, window_depth);
        const auto window_relief = mask_difference(window_anisotropic, flat_window);
        const auto window_area = mask_area(window_anisotropic);
        std::printf("[smoke_displacement_filter] window levels %u, fragment motion %zu, "
                    "isotropic lit mismatch %zu, masked depth mismatch %zu, "
                    "retained relief %zu, area %zu\n", levels, window_fragment_motion,
                    window_lit_mismatch, window_depth_mismatch, window_relief, window_area);
        // Provisional small weight-rounding bound. GPU evidence should decide
        // whether this can be tightened to exact equality on retained targets.
        constexpr std::size_t weight_rounding_pixels = 8;
        if (!(window_fragment_motion == 0 && window_lit_mismatch <= weight_rounding_pixels &&
              window_depth_mismatch <= weight_rounding_pixels && window_relief >= 128 &&
              window_area >= 5000 && window_area <= 25000 &&
              differences_are_at_boundary(window_anisotropic, reference) &&
              differences_are_at_boundary(window_anisotropic, window_depth))) return CaseResult::Fail;
        (levels == 9 ? full_window : partial_window) = std::move(window_anisotropic);
    }
    std::printf("[smoke_displacement_filter] full versus partial window field difference %zu\n",
                mask_difference(full_window, partial_window));
    return CaseResult::Pass;
}
} // namespace heaven::smoke
