// GPU readback of the production material, not a shader-equation mirror. A
// world-space plate and a separate hemisphere ray integral classify the light
// arriving in each reflected lobe. All geometry is retained in AO ON/OFF pairs.
#include "../smoke/smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/ColorSpace.h>
#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/ToneMapper.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_specular_occlusion]";
constexpr std::uint32_t kSize = 320;
constexpr int kWarmup = 6;
constexpr int kMeasurements = 4;
constexpr double kFov = 40.0;
using Pixels = std::vector<float>;

glm::dvec3 reflection_direction(double polar, double azimuth) {
    const double theta = polar * std::numbers::pi / 180.0;
    const double phi = azimuth * std::numbers::pi / 180.0;
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi),
            std::cos(theta)};
}

// Independent ray/triangle-plane geometry. No SSAO depth, angular horizons,
// blocked moment, fitted cap or shader interpolation enters this oracle.
bool hits_plate(glm::dvec3 origin, glm::dvec3 direction) {
    if (direction.x <= 0.0) return false;
    const double t = (0.30 - origin.x) / direction.x;
    const glm::dvec3 point = origin + t * direction;
    return t > 0.0 && std::abs(point.y) <= 0.35 &&
            point.z >= 0.0 && point.z <= 0.40;
}

// Equal-solid-angle hemisphere quadrature of the GGX reflection BRDF. This
// deliberately has a different domain and sampling policy from the producer.
double reference_visibility(glm::dvec3 view, glm::dvec3 normal, double alpha) {
    constexpr int kRings = 192;
    constexpr int kAzimuths = 384;
    const double nv = glm::dot(normal, view);
    const double a2 = alpha * alpha;
    const auto masking = [a2](double cosine) {
        return 2.0 * cosine /
                (cosine + std::sqrt(a2 + (1.0 - a2) * cosine * cosine));
    };
    double total = 0.0;
    double clear = 0.0;
    for (int ring = 0; ring < kRings; ++ring) {
        const double z = (static_cast<double>(ring) + 0.5) / kRings;
        const double r = std::sqrt(1.0 - z * z);
        for (int azimuth = 0; azimuth < kAzimuths; ++azimuth) {
            const double phi = 2.0 * std::numbers::pi *
                    (static_cast<double>(azimuth) + 0.5) / kAzimuths;
            const glm::dvec3 ray{r * std::cos(phi), r * std::sin(phi), z};
            const double nl = glm::dot(normal, ray);
            if (nl <= 0.0) continue;
            const glm::dvec3 half = glm::normalize(view + ray);
            const double nh = std::max(0.0, glm::dot(normal, half));
            const double denominator = nh * nh * (a2 - 1.0) + 1.0;
            const double distribution = a2 /
                    (std::numbers::pi * denominator * denominator);
            const double weight = distribution * masking(nv) * masking(nl) / (4.0 * nv);
            total += weight;
            if (!hits_plate({0.0, 0.0, 0.0}, ray)) clear += weight;
        }
    }
    if (!(total > 0.0 && std::isfinite(total))) {
        throw std::runtime_error("Specular oracle has no valid reflected lobe");
    }
    return clear / total;
}

struct Measurement {
    double mean = 0.0;
    double temporal_delta = 0.0;
    std::size_t samples = 0;
    std::vector<double> pixels;
};

class OcclusionFixture {
public:
    explicit OcclusionFixture(filament::Engine& engine)
            : engine_(engine), textures_(engine), materials_(engine, textures_), meshes_(engine) {
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) {
            throw std::runtime_error("Specular fixture cannot load production material");
        }
        materials_.set_colored_penumbra(false);
        renderer_ = engine_.createRenderer();
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        camera_->setProjection(kFov, 1.0, 0.1, 10.0);
        camera_->setExposure(16.0f, 1.0f / 125.0f, 100.0f);
        scene_ = engine_.createScene();
        // These are Filament's documented composable texture-usage bits.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
                std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
                std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
                .format(filament::Texture::InternalFormat::RGBA16F).usage(usage).build(engine_);
        target_ = filament::RenderTarget::Builder()
                .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_).build(engine_);
        view_ = engine_.createView();
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setShadowingEnabled(false);
        view_->setPostProcessingEnabled(true);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        filament::LinearToneMapper linear;
        grading_ = filament::ColorGrading::Builder().toneMapper(&linear)
                .outputColorSpace(filament::color::Rec709 - filament::color::Linear -
                        filament::color::D65).build(engine_);
        view_->setColorGrading(grading_);

        normal_texture_ = filament::Texture::Builder().width(1).height(1).levels(1)
                .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
        base_texture_ = filament::Texture::Builder().width(1).height(1).levels(1)
                .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
        // Compatibility metals derive F0 from baseColor, not the gloss map.
        // A linear F0=.20 retains actual occlusion instead of white-metal
        // multibounce compensation concealing a positive blocker response.
        auto* base = new std::uint8_t[4]{51, 51, 51, 255};
        base_texture_->setImage(engine_, 0, filament::Texture::PixelBufferDescriptor(base, 4,
                filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE,
                [](void* data, std::size_t, void*) { delete[] static_cast<std::uint8_t*>(data); }));
        cube_ = filament::Texture::Builder().width(1).height(1).levels(1)
                .sampler(filament::Texture::Sampler::SAMPLER_CUBEMAP)
                .format(filament::Texture::InternalFormat::RGBA16F).build(engine_);
        // Exact binary16 unity avoids an unrelated FLOAT-to-HALF staging-image
        // conversion for a narrow six-face cube. No environment resampling or
        // per-face variation belongs in this constant-radiance oracle.
        auto* texels = new std::uint16_t[24];
        std::fill_n(texels, 24, std::uint16_t{0x3c00});
        cube_->setImage(engine_, 0, 0, 0, 0, 1, 1, 6,
                filament::Texture::PixelBufferDescriptor(texels, 24 * sizeof(std::uint16_t),
                        filament::Texture::Format::RGBA, filament::Texture::Type::HALF,
                        [](void* data, std::size_t, void*) { delete[] static_cast<std::uint16_t*>(data); }));
        ibl_ = filament::IndirectLight::Builder().reflections(cube_).intensity(30000.0f).build(engine_);
        scene_->setIndirectLight(ibl_);
        add_mesh("receiver", {{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}},
                {0, 0, 1}, receiver_, true);
        // Face normal -X makes the plate visible from each negative-X camera.
        add_mesh("plate", {{{0.30f, -0.35f, 0}, {0.30f, -0.35f, 0.40f},
                {0.30f, 0.35f, 0.40f}, {0.30f, 0.35f, 0}}}, {-1, 0, 0}, plate_, false);
        scene_->remove(plate_);
        set_camera({-0.7, 0.0, 2.0});
    }

    ~OcclusionFixture() {
        view_->setRenderTarget(nullptr);
        engine_.destroy(view_);
        for (const auto entity : {receiver_, plate_}) {
            scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        receiver_material_->setParameter("normalMap", textures_.placeholder_normal(),
                filament::TextureSampler{});
        receiver_material_->setParameter("baseColorMap", textures_.placeholder(),
                filament::TextureSampler{});
        scene_->setIndirectLight(nullptr);
        engine_.destroy(scene_);
        engine_.destroy(ibl_);
        engine_.destroy(cube_);
        engine_.destroy(normal_texture_);
        engine_.destroy(base_texture_);
        engine_.destroy(grading_);
        engine_.destroy(target_);
        engine_.destroy(color_);
        engine_.destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }
    OcclusionFixture(const OcclusionFixture&) = delete;
    OcclusionFixture& operator=(const OcclusionFixture&) = delete;

    void set_camera(glm::dvec3 position) {
        camera_position_ = position;
        camera_->lookAt({position.x, position.y, position.z}, {0, 0, 0}, {0, 1, 0});
        inverse_clip_ = glm::inverse(glm::perspective(glm::radians(kFov), 1.0, 0.1, 10.0) *
                glm::lookAt(position, glm::dvec3(0.0), glm::dvec3(0, 1, 0)));
    }
    void set_plate(bool enabled) {
        if (enabled) scene_->addEntity(plate_);
        else scene_->remove(plate_);
    }
    void set_ao_resolution(float resolution) { ao_resolution_ = resolution; }
    glm::dvec3 set_normal(glm::dvec3 reflection, double alpha, bool mapped) {
        receiver_material_->setParameter("hasNormal", mapped);
        const float roughness = static_cast<float>(std::sqrt(alpha));
        receiver_material_->setParameter("surfaceMetallicTuning",
                filament::math::float4{0, 0, roughness, roughness});
        if (!mapped) return {0, 0, 1};
        const auto desired = glm::normalize(glm::normalize(camera_position_) + reflection);
        const auto tangent = glm::normalize(glm::inverse(tangent_basis_) * desired);
        auto* bytes = new std::uint8_t[4];
        glm::dvec3 decoded;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto axis = static_cast<glm::length_t>(channel);
            bytes[channel] = static_cast<std::uint8_t>(std::lround(
                    std::clamp(0.5 * tangent[axis] + 0.5, 0.0, 1.0) * 255.0));
            decoded[axis] = 2.0 * bytes[channel] / 255.0 - 1.0;
        }
        bytes[3] = 255;
        normal_texture_->setImage(engine_, 0, filament::Texture::PixelBufferDescriptor(bytes, 4,
                filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE,
                [](void* data, std::size_t, void*) { delete[] static_cast<std::uint8_t*>(data); }));
        decoded.z = std::max(decoded.z, 1e-4);
        return glm::normalize(tangent_basis_ * glm::normalize(decoded));
    }
    void set_ao(bool enabled, float intensity, bool directional = true) {
        filament::AmbientOcclusionOptions ao;
        ao.enabled = enabled;
        ao.bentNormals = directional;
        ao.aoType = filament::AmbientOcclusionOptions::AmbientOcclusionType::GTAO;
        ao.radius = 0.8f;
        ao.power = 1.0f;
        ao.intensity = intensity;
        ao.resolution = ao_resolution_;
        ao.quality = filament::QualityLevel::HIGH;
        ao.lowPassFilter = filament::QualityLevel::HIGH;
        ao.upsampling = filament::QualityLevel::HIGH;
        ao.gtao.sampleSliceCount = 8;
        ao.gtao.sampleStepsPerSlice = 8;
        view_->setAmbientOcclusionOptions(ao);
        const auto actual = view_->getAmbientOcclusionOptions();
        if (actual.enabled != enabled || actual.bentNormals != directional ||
                actual.aoType != ao.aoType || actual.gtao.useVisibilityBitmasks ||
                actual.intensity != intensity || actual.resolution != ao_resolution_) {
            throw std::runtime_error("Specular fixture did not activate requested GTAO");
        }
    }
    bool measure(Measurement& result) {
        int accepted = 0;
        std::vector<double> previous;
        for (int attempt = 0; attempt < 4 * (kWarmup + kMeasurements) &&
                accepted < kWarmup + kMeasurements; ++attempt) {
            if (!renderer_->beginFrame(swapchain_)) { ++rejected_; continue; }
            ++accepted;
            renderer_->render(view_);
            std::atomic_bool complete{false};
            Pixels pixels;
            if (accepted > kWarmup) {
                pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4,
                        std::numeric_limits<float>::quiet_NaN());
                renderer_->readPixels(target_, 0, 0, kSize, kSize,
                        filament::backend::PixelBufferDescriptor(pixels.data(),
                                pixels.size() * sizeof(float), filament::backend::PixelDataFormat::RGBA,
                                filament::backend::PixelDataType::FLOAT,
                                [](void*, std::size_t, void* user) {
                                    static_cast<std::atomic_bool*>(user)->store(true);
                                }, &complete));
            }
            renderer_->endFrame();
            engine_.flushAndWait();
            if (accepted <= kWarmup) continue;
            if (!complete.load() || !std::ranges::all_of(pixels,
                    [](float value) { return std::isfinite(value); })) return false;
            double mean = 0.0;
            std::size_t samples = 0;
            std::vector<double> current;
            for (std::uint32_t y = 0; y < kSize; ++y) {
                for (std::uint32_t x = 0; x < kSize; ++x) {
                    if (!measurement_pixel(x, y)) continue;
                    const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
                    const double value = (static_cast<double>(pixels[index]) + pixels[index + 1] +
                            pixels[index + 2]) / 3.0;
                    mean += value;
                    current.push_back(value);
                    ++samples;
                }
            }
            if (samples < 32) return false;
            mean /= static_cast<double>(samples);
            if (result.pixels.empty()) result.pixels.resize(current.size(), 0.0);
            for (std::size_t i = 0; i < current.size(); ++i) {
                result.pixels[i] += current[i] / kMeasurements;
                if (!previous.empty()) {
                    result.temporal_delta = std::max(result.temporal_delta,
                            std::abs(current[i] - previous[i]));
                }
            }
            previous = std::move(current);
            result.mean += mean / kMeasurements;
            result.samples = samples;
        }
        return accepted == kWarmup + kMeasurements;
    }
    [[nodiscard]] glm::dvec3 view_direction() const { return glm::normalize(camera_position_); }
    [[nodiscard]] std::uint64_t rejected() const { return rejected_; }

private:
    bool measurement_pixel(std::uint32_t x, std::uint32_t y) const {
        // Independent pixel-center perspective ray intersects the original
        // plane. The selected square is away from either visible silhouette.
        const glm::dvec4 clip{2.0 * (static_cast<double>(x) + 0.5) / kSize - 1.0,
                2.0 * (static_cast<double>(y) + 0.5) / kSize - 1.0, -1.0, 1.0};
        const auto point4 = inverse_clip_ * clip;
        const glm::dvec3 point = glm::dvec3(point4) / point4.w;
        const auto direction = point - camera_position_;
        const double t = -camera_position_.z / direction.z;
        const auto hit = camera_position_ + t * direction;
        return std::abs(hit.x) < 0.035 && std::abs(hit.y) < 0.035 &&
                !hits_plate(camera_position_, direction);
    }
    void add_mesh(const char* role, const std::array<glm::vec3, 4>& positions,
            glm::vec3 normal, utils::Entity& entity, bool receiver) {
        Mesh mesh;
        Surface surface;
        for (std::size_t i = 0; i < positions.size(); ++i) {
            Vertex vertex{};
            vertex.px = positions[i].x; vertex.py = positions[i].y; vertex.pz = positions[i].z;
            vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
            vertex.u = i == 1 || i == 2 ? 1.0f : 0.0f;
            vertex.v = i >= 2 ? 1.0f : 0.0f;
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2, 0, 2, 3};
        mesh.surfaces.push_back(surface);
        const auto gpu = meshes_.upload(mesh);
        if (gpu.surfaces.empty()) throw std::runtime_error("Specular fixture mesh upload failed");
        const auto handle = gpu.surfaces.front().handle;
        const auto buffers = meshes_.find(handle);
        GpuMaterial definition{};
        auto* material = materials_.get_or_create("specular-occlusion-fixture", role, definition);
        if (!material || std::strcmp(material->getMaterial()->getName(), "heaven_lit") != 0) {
            throw std::runtime_error("Specular fixture did not use production heaven_lit");
        }
        if (receiver) {
            receiver_material_ = material;
            material->setParameter("hasBaseColor", true);
            material->setParameter("baseColorMap", base_texture_, filament::TextureSampler{});
            material->setParameter("hasSpecular", true);
            material->setParameter("isMetallic", true);
            material->setParameter("specGlossWorkflow", false);
            material->setParameter("normalStrength", 1.0f);
            material->setParameter("surfaceSamplingTuning", filament::math::float4{1, 1, 0, 0.001f});
            material->setParameter("normalMap", normal_texture_, filament::TextureSampler{});
            material->setParameter("specularMap", textures_.placeholder(), filament::TextureSampler{});
            auto packing = meshes_.take_original_packing(handle, surface.vertices, surface.indices);
            if (!packing || packing->tangent_frames.empty()) {
                throw std::runtime_error("Specular fixture lacks original uploaded tangent frame");
            }
            const auto packed = packing->tangent_frames.front();
            for (const auto& other : packing->tangent_frames) {
                if (packed.x != other.x || packed.y != other.y ||
                        packed.z != other.z || packed.w != other.w) {
                    throw std::runtime_error("Specular receiver tangent frame varies across plane");
                }
            }
            const glm::dvec4 q{static_cast<double>(packed.x) / 32767.0,
                    static_cast<double>(packed.y) / 32767.0,
                    static_cast<double>(packed.z) / 32767.0,
                    static_cast<double>(packed.w) / 32767.0};
            const glm::dvec3 n = glm::dvec3(0, 0, 1) +
                    glm::dvec3(2, -2, -2) * q.x * glm::dvec3(q.z, q.w, q.x) +
                    glm::dvec3(2, 2, -2) * q.y * glm::dvec3(q.w, q.z, q.y);
            const glm::dvec3 t = glm::dvec3(1, 0, 0) +
                    glm::dvec3(-2, 2, -2) * q.y * glm::dvec3(q.y, q.x, q.w) +
                    glm::dvec3(-2, 2, 2) * q.z * glm::dvec3(q.z, q.w, q.x);
            tangent_basis_ = glm::dmat3(t, glm::cross(n, t) * (q.w < 0.0 ? -1.0 : 1.0), n);
        }
        entity = utils::EntityManager::get().create();
        const auto result = filament::RenderableManager::Builder(1)
                .boundingBox({{0, 0, 0}, {2, 2, 2}}).culling(false)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, buffers.vb, buffers.ib)
                .material(0, material).castShadows(false).receiveShadows(false)
                .screenSpaceContactShadows(false).build(engine_, entity);
        if (result != filament::RenderableManager::Builder::Success) {
            throw std::runtime_error("Specular fixture renderable build failed");
        }
        scene_->addEntity(entity);
    }

    filament::Engine& engine_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* cube_ = nullptr;
    filament::Texture* normal_texture_ = nullptr;
    filament::Texture* base_texture_ = nullptr;
    filament::IndirectLight* ibl_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::ColorGrading* grading_ = nullptr;
    filament::MaterialInstance* receiver_material_ = nullptr;
    utils::Entity camera_entity_, receiver_, plate_;
    glm::dmat3 tangent_basis_{1.0};
    glm::dmat4 inverse_clip_{1.0};
    glm::dvec3 camera_position_{0.0};
    std::uint64_t rejected_ = 0;
    float ao_resolution_ = 1.0f;
};

enum class Expectation { Open, Blocked, Outside, Grazing };

bool check_case(OcclusionFixture& fixture, const char* label, bool plate, bool mapped,
        double polar, double azimuth, Expectation expectation, float intensity,
        double& attenuation) {
    constexpr double kAlpha = 0.15;
    fixture.set_plate(plate);
    const auto normal = fixture.set_normal(reflection_direction(polar, azimuth), kAlpha, mapped);
    const double oracle = plate ? reference_visibility(fixture.view_direction(), normal, kAlpha) : 1.0;
    fixture.set_ao(false, intensity);
    Measurement off;
    if (!fixture.measure(off)) return false;
    // SIMPLE's scalar occlusion remains intentional. The separate live scalar
    // draw prevents mistaking that nondirectional term for a blocker-cap defect.
    fixture.set_ao(true, intensity, false);
    Measurement scalar;
    if (!fixture.measure(scalar)) return false;
    fixture.set_ao(true, intensity);
    Measurement on;
    if (!fixture.measure(on)) return false;
    const double ratio = on.mean / off.mean;
    const double directional_ratio = on.mean / scalar.mean;
    attenuation = 1.0 - directional_ratio;
    double worst_pixel_delta = 0.0;
    for (std::size_t i = 0; i < off.pixels.size(); ++i) {
        if (off.pixels[i] > 0.005) {
            worst_pixel_delta = std::max(worst_pixel_delta,
                    std::abs(on.pixels[i] / off.pixels[i] - 1.0));
        }
    }
    bool correct = off.mean > 0.005 && off.mean < 0.9 && on.mean >= 0.0 &&
            scalar.mean > 0.005 && std::isfinite(ratio) && std::isfinite(directional_ratio) &&
            on.temporal_delta <= 0.02 && off.temporal_delta <= 0.02 && scalar.temporal_delta <= 0.02;
    switch (expectation) {
        case Expectation::Open:
            correct = correct && std::abs(ratio - 1.0) <= 0.03 && worst_pixel_delta <= 0.03;
            break;
        case Expectation::Blocked:
            // Intensity is opacity strength. Retain the full-strength 20%
            // gate, requiring at least that attenuation times requested dose.
            correct = correct && oracle < 0.40 && attenuation > 0.20 * intensity;
            break;
        case Expectation::Outside:
            correct = correct && oracle > 0.95 && directional_ratio >= 0.97 && directional_ratio <= 1.03;
            break;
        case Expectation::Grazing:
            // A fitted cap approximates a clipped rectangle, so this is a
            // bounded partial-response gate, not pixel agreement with GGX.
            correct = correct && oracle > 0.05 && oracle < 0.95 &&
                    directional_ratio > 0.02 && directional_ratio < 0.98;
            break;
    }
    std::printf("%s %s plate=%d mapped=%d strength=%.2f samples=%zu "
            "off=%.7f scalar=%.7f directional=%.7f on/off=%.6f direction/scalar=%.6f independentGGX=%.6f "
            "temporalPixel=%.7f/%.7f/%.7f worstPixelOnOff=%.6f %s\n", kTag, label, static_cast<int>(plate),
            static_cast<int>(mapped), static_cast<double>(intensity), off.samples,
            off.mean, scalar.mean, on.mean, ratio, directional_ratio, oracle,
            off.temporal_delta, scalar.temporal_delta, on.temporal_delta, worst_pixel_delta,
            correct ? "PASS" : "FAIL");
    return correct;
}
} // namespace

CaseResult run_specular_occlusion_case(const CaseContext& ctx) try {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    auto* engine = backend.debug_render_context().engine;
    if (!engine) {
        // A selected standalone smoke case has not run the earlier pipeline
        // cases that populate this borrowed context in the full registry.
        const auto initialized = backend.init_pipelines(ctx.config);
        if (!initialized) {
            std::printf("%s pipeline initialization failed: %s\n", kTag,
                    initialized.error().message.c_str());
            return CaseResult::Fail;
        }
        engine = backend.debug_render_context().engine;
    }
    if (!engine) {
        std::printf("%s initialized Engine is missing\n", kTag);
        return CaseResult::Fail;
    }
    OcclusionFixture fixture(*engine);
    bool passed = true;
    const auto run = [&](const char* label, bool plate, bool mapped, double polar,
            double azimuth, Expectation expectation, float intensity = 1.0f) {
        double attenuation = std::numeric_limits<double>::quiet_NaN();
        passed = check_case(fixture, label, plate, mapped, polar, azimuth,
                expectation, intensity, attenuation) && passed;
        return attenuation;
    };
    run("open-flat", false, false, 0, 0, Expectation::Open);
    run("open-mapped", false, true, 65, 0, Expectation::Open);
    const double full_attenuation = run("blocked-lobe", true, true, 65, 0, Expectation::Blocked);
    run("opposite-lobe", true, true, 65, 180, Expectation::Outside);
    run("grazing-lower-edge", true, true, 36.87, 0, Expectation::Grazing);
    run("grazing-side-edge", true, true, 65, 49.40, Expectation::Grazing);
    const double reduced_attenuation = run("strength-0.8", true, true, 65, 0, Expectation::Blocked, 0.8f);
    const double dose_ratio = reduced_attenuation / full_attenuation;
    // This is a final-renderer response, including quantization and retained
    // nonlinear multibounce. Exact linear opacity is separately host-tested.
    const bool dose_correct = std::isfinite(dose_ratio) && reduced_attenuation < full_attenuation &&
            std::abs(dose_ratio - 0.8) <= 0.10;
    std::printf("%s strength-contract attenuation=%.6f/%.6f ratio=%.6f %s\n",
            kTag, reduced_attenuation, full_attenuation, dose_ratio, dose_correct ? "PASS" : "FAIL");
    passed = dose_correct && passed;
    fixture.set_ao_resolution(0.5f);
    run("half-resolution-blocked", true, true, 65, 0, Expectation::Blocked);
    run("half-resolution-opposite", true, true, 65, 180, Expectation::Outside);
    fixture.set_ao_resolution(1.0f);
    fixture.set_camera({-1.4, 0.25, 2.0});
    run("oblique-blocked", true, true, 65, 0, Expectation::Blocked);
    run("oblique-opposite", true, true, 65, 180, Expectation::Outside);
    std::printf("%s %d states, %d measured frames/state, rejected=%llu; "
            "production material and final FLOAT readback; no exact-model claim\n",
            kTag, 33, kMeasurements, static_cast<unsigned long long>(fixture.rejected()));
    return passed ? CaseResult::Pass : CaseResult::Fail;
} catch (const std::exception& error) {
    std::printf("%s failure: %s\n", kTag, error.what());
    return CaseResult::Fail;
}
} // namespace heaven::smoke
