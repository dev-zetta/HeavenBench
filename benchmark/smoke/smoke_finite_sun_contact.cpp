// Real material/GPU regression for finite Sun contact visibility. This fixture
// deliberately separates a smooth lighting normal from an exact planar receiver.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/ColorSpace.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_finite_sun_contact]";
constexpr double kRadiusDegrees = 3.0;
constexpr double kLift = 0.12;
constexpr double kSideAngleDegrees = 10.0;
constexpr double kBlockerHalfWidth = 0.06;
constexpr double kBlockerHalfHeight = 0.03;
constexpr int kWarmupFrames = 120;
constexpr int kMeasuredFrames = 32;
// Linear readback checks both the mean and every measured receiver pixel.
constexpr double kHorizonTolerance = 0.025;
using Pixels = std::vector<float>;

double radians(double degrees) {
    return degrees * std::numbers::pi / 180.0;
}

template<typename Function>
double simpson(Function&& f, int subdivisions = 1024) {
    const double h = 1.0 / subdivisions;
    double total = f(0.0) + f(1.0);
    for (int i = 1; i < subdivisions; ++i) {
        total += (i % 2 == 0 ? 2.0 : 4.0) * f(i * h);
    }
    return total * h / 3.0;
}

// Independent oracle: integrate visible azimuth around spherical rings with
// sin(theta) solid-angle weight. This does not copy the shader tangent-disk CDF,
// conditional sampler, inverse CDF or angular quadrature points.
double horizon_visibility(double radius_degrees, double elevation_degrees) {
    const double alpha = radians(radius_degrees);
    const double beta = radians(elevation_degrees);
    if (beta >= alpha) return 1.0;
    if (beta <= -alpha) return 0.0;
    if (beta == 0.0) return 0.5;
    const double start = std::abs(beta);
    const double mu = std::sin(beta);
    const double q = std::cos(beta);
    const double denominator = 1.0 - std::cos(alpha);
    const double inner = beta > 0.0 ? (1.0 - std::cos(start)) / denominator : 0.0;
    return inner + simpson([&](double t) {
        if (t == 0.0) return 0.0;
        // This substitution smooths the partial annulus's square-root endpoint.
        const double theta = start + (alpha - start) * t * t;
        const double threshold = -mu * std::cos(theta) / (q * std::sin(theta));
        const double azimuth = std::acos(std::clamp(threshold, -1.0, 1.0)) /
                               std::numbers::pi;
        return azimuth * std::sin(theta) * 2.0 * (alpha - start) * t / denominator;
    });
}

// For C=+Z, a ray hits the lifted blocker plane at x=X+lift*tan(theta)*cos(phi).
// Integrating the azimuth covered by x>=0 supplies a separate geometric oracle.
double blocker_visibility(double receiver_x, double radius_degrees) {
    const double alpha = radians(radius_degrees);
    const double reach = kLift * std::tan(alpha);
    if (receiver_x <= -reach) return 1.0;
    if (receiver_x >= reach) return 0.0;
    if (receiver_x == 0.0) return 0.5;
    const double start = std::atan(std::abs(receiver_x) / kLift);
    const double denominator = 1.0 - std::cos(alpha);
    const double inner = receiver_x > 0.0
        ? (1.0 - std::cos(start)) / denominator : 0.0;
    const double blocked = inner + simpson([&](double t) {
        if (t == 0.0) return 0.0;
        const double theta = start + (alpha - start) * t * t;
        const double threshold = -receiver_x / (kLift * std::tan(theta));
        const double azimuth = std::acos(std::clamp(threshold, -1.0, 1.0)) /
                               std::numbers::pi;
        return azimuth * std::sin(theta) * 2.0 * (alpha - start) * t / denominator;
    });
    return 1.0 - blocked;
}

struct FrameCounts {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
};

enum class FrameResult { Accepted, Rejected, Failed };

struct HelperLaneOptions {
    bool dense = false;
    bool masked = false;
    std::uint8_t samples = 1;
};

constexpr std::uint8_t helper_alpha_texel(std::uint32_t x, std::uint32_t y) {
    const auto stripe = (x + y / 8) % 16;
    return stripe < 4 ? 0 : stripe < 8
        ? static_cast<std::uint8_t>((stripe - 3) * 51) : 255;
}

class FiniteSunFixture {
public:
    FiniteSunFixture(filament::Engine& engine, bool blocker, bool sun = true,
                     const HelperLaneOptions* helper = nullptr)
        : engine_(engine), textures_(engine), materials_(engine, textures_),
          meshes_(engine), blocker_(blocker), sun_(sun), helper_lanes_(helper != nullptr),
          helper_(helper ? *helper : HelperLaneOptions{}),
          width_(helper ? 256u : blocker ? 512u : 128u),
          height_(helper ? 128u : blocker ? 256u : 128u) {
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) {
            throw std::runtime_error("Finite Sun fixture material load failed");
        }
        materials_.set_colored_penumbra(false);
        if (helper_lanes_) {
            materials_.set_specular_aa(true, QualityKnobs{});
            create_helper_textures();
        }
        renderer_ = engine_.createRenderer();
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        if (helper_lanes_) {
            // Alpha-discarded samples must see a known background every frame.
            // Without a color clear, a reused single-sample target may retain
            // another fixture's pixels in holes; that is not coverage evidence.
            renderer_->setClearOptions({.clearColor = {0, 0, 0, 0},
                                        .clear = true, .discard = true});
        }
        swapchain_ = engine_.createSwapChain(width_, height_);
        camera_entity_ = utils::EntityManager::get().create();
        auto* camera = engine_.createCamera(camera_entity_);
        if (blocker_) {
            camera->setProjection(filament::Camera::Projection::ORTHO,
                                  -kBlockerHalfWidth, kBlockerHalfWidth,
                                  -kBlockerHalfHeight, kBlockerHalfHeight, 0.1, 50.0);
            const double angle = radians(kSideAngleDegrees);
            camera->lookAt({-2.0 * std::sin(angle), 0.0, -6.0 + 2.0 * std::cos(angle)},
                           {0, 0, -6}, {0, 1, 0});
        } else {
            camera->setProjection(40.0, 1.0, 0.1, 50.0);
            camera->lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
        }
        camera->setExposure(16.0f, 1.0f / 125.0f, 100.0f);
        scene_ = engine_.createScene();
        // Filament's documented usage flags are composable bits.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(width_).height(height_).levels(1)
            .format(filament::Texture::InternalFormat::RGBA16F).usage(usage).build(engine_);
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_).build(engine_);
        view_ = engine_.createView();
        view_->setScene(scene_);
        view_->setCamera(camera);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, width_, height_});
        view_->setShadowingEnabled(true);
        view_->setPostProcessingEnabled(true);
        if (helper_lanes_) view_->setBlendMode(filament::View::BlendMode::OPAQUE);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        if (helper_lanes_) {
            view_->setMultiSampleAntiAliasingOptions({.enabled = helper_.samples > 1,
                                                     .sampleCount = helper_.samples});
        }
        filament::AmbientOcclusionOptions ao;
        ao.enabled = false;
        ao.resolution = 1.0f;
        view_->setAmbientOcclusionOptions(ao);
        filament::TemporalAntiAliasingOptions taa;
        taa.enabled = !helper_lanes_;
        view_->setTemporalAntiAliasingOptions(taa);
        view_->setDithering(filament::Dithering::NONE);
        filament::LinearToneMapper linear_mapper;
        const auto linear_space = filament::color::Rec709 - filament::color::Linear -
                                  filament::color::D65;
        grading_ = filament::ColorGrading::Builder().toneMapper(&linear_mapper)
            .outputColorSpace(linear_space).build(engine_);
        view_->setColorGrading(grading_);

        const auto smooth = blocker_ ? glm::vec3(0, 0, 1)
                                    : glm::normalize(glm::vec3(1, 0, 1));
        if (helper_lanes_) {
            // Binary fractions keep both grids' positions and UVs exact. The
            // dense cells project to roughly 0.51x0.52 pixels, with triangle
            // area below 0.14 pixel: every measured pixel borders microtriangles.
            add_quad("receiver", {-0.0625f, -0.03125f, -6}, {0.0625f, 0.03125f, -6},
                     smooth, false, true, helper_.masked);
        } else {
            add_quad("receiver", {-4, -4, -6}, {4, 4, -6}, smooth, false, true, false);
        }
        if (blocker_) {
            add_quad("blocker", {0, -0.10f, -5.88f}, {0.10f, 0.10f, -5.88f},
                     {0, 0, 1}, false, false, false);
            // This corner caster enables the ordinary shadow-map path without
            // intersecting any measurement ray. Its Y gap exceeds Sun ray reach.
            add_quad("caster", {-0.002f, 0.025f, -5.99f}, {0.002f, 0.028f, -5.99f},
                     {0, 0, 1}, true, false, true);
        } else {
            add_quad("caster", {-0.07f, 1.73f, -5.8f}, {0.07f, 1.87f, -5.8f},
                     {0, 0, 1}, true, false, true);
        }
        light_ = utils::EntityManager::get().create();
        filament::LightManager::Builder(sun_ ? filament::LightManager::Type::SUN
                                             : filament::LightManager::Type::DIRECTIONAL)
            .color({1, 1, 1}).intensity(50000.0f)
            .sunAngularRadius(static_cast<float>(kRadiusDegrees))
            .direction(blocker_ ? filament::math::float3{0, 0, -1}
                               : filament::math::float3{-1, 0, 0})
            .castShadows(true).build(engine_, light_);
        scene_->addEntity(light_);
        set_contact(true);
    }

    ~FiniteSunFixture() {
        if (helper_receiver_) {
            const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
                filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
            helper_receiver_->setParameter("baseColorMap", textures_.placeholder(), sampler);
            helper_receiver_->setParameter("normalMap", textures_.placeholder_normal(), sampler);
            engine_.flushAndWait();
        }
        view_->setRenderTarget(nullptr);
        engine_.destroy(view_);
        for (auto entity : entities_) {
            scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        scene_->remove(light_);
        engine_.destroy(light_);
        utils::EntityManager::get().destroy(light_);
        engine_.destroy(scene_);
        if (helper_color_) engine_.destroy(helper_color_);
        if (helper_normal_) engine_.destroy(helper_normal_);
        engine_.destroy(grading_);
        engine_.destroy(target_);
        engine_.destroy(color_);
        engine_.destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }
    FiniteSunFixture(const FiniteSunFixture&) = delete;
    FiniteSunFixture& operator=(const FiniteSunFixture&) = delete;

    void set_elevation(double degrees) {
        auto& lights = engine_.getLightManager();
        const auto instance = lights.getInstance(light_);
        const double angle = radians(degrees);
        lights.setDirection(instance, {static_cast<float>(-std::cos(angle)), 0,
                                       static_cast<float>(-std::sin(angle))});
    }

    void set_contact(bool enabled) {
        auto& lights = engine_.getLightManager();
        const auto instance = lights.getInstance(light_);
        auto options = lights.getShadowOptions(instance);
        options.screenSpaceContactShadows = enabled;
        options.stepCount = 32;
        options.maxShadowDistance = 0.3f;
        lights.setShadowOptions(instance, options);
        const auto live = lights.getShadowOptions(instance);
        const auto expected_type = sun_ ? filament::LightManager::Type::SUN
                                       : filament::LightManager::Type::DIRECTIONAL;
        if (!lights.isShadowCaster(instance) || lights.getType(instance) != expected_type ||
            (sun_ && std::abs(lights.getSunAngularRadius(instance) - float(kRadiusDegrees)) > 1e-4f) ||
            live.screenSpaceContactShadows != enabled || live.stepCount != 32 ||
            std::abs(live.maxShadowDistance - 0.3f) > 1e-6f) {
            throw std::runtime_error("Finite Sun shadow options did not preserve the control");
        }
    }

    FrameResult frame(Pixels* pixels = nullptr) {
        if (!renderer_->beginFrame(swapchain_)) {
            ++counts_.rejected;
            return FrameResult::Rejected;
        }
        ++counts_.accepted;
        renderer_->render(view_);
        std::atomic_bool complete{false};
        if (pixels) {
            pixels->assign(static_cast<std::size_t>(width_) * height_ * 4,
                           std::numeric_limits<float>::quiet_NaN());
            renderer_->readPixels(target_, 0, 0, width_, height_,
                filament::backend::PixelBufferDescriptor(pixels->data(),
                    pixels->size() * sizeof(float), filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::FLOAT,
                    [](void*, std::size_t, void* user) {
                        static_cast<std::atomic_bool*>(user)->store(true);
                    }, &complete));
        }
        renderer_->endFrame();
        engine_.flushAndWait();
        if (pixels && !complete.load()) {
            std::printf("%s FLOAT readback callback did not complete\n", kTag);
            return FrameResult::Failed;
        }
        return FrameResult::Accepted;
    }

    bool measurement_pixel(std::uint32_t x, std::uint32_t y) const {
        if (!blocker_) {
            return x >= width_ / 4 && x < 3 * width_ / 4 &&
                   y >= height_ / 4 && y < 3 * height_ / 4;
        }
        return std::abs(receiver_x(x)) <= 0.01 &&
               y >= height_ / 2 - 12 && y < height_ / 2 + 12;
    }

    double receiver_x(std::uint32_t x) const {
        const double projected = ((double(x) + 0.5) / width_ * 2.0 - 1.0) *
                                 kBlockerHalfWidth;
        return projected / std::cos(radians(kSideAngleDegrees));
    }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    FrameCounts counts() const { return counts_; }

private:
    void create_helper_textures() {
        constexpr std::uint32_t size = 64;
        std::vector<std::uint8_t> color(static_cast<std::size_t>(size) * size * 4);
        std::vector<std::uint8_t> normal(color.size());
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const auto index = (static_cast<std::size_t>(y) * size + x) * 4;
                color[index] = color[index + 1] = color[index + 2] = 180;
                color[index + 3] = helper_.masked ? helper_alpha_texel(x, y) : 255;
                // A real sampled normal field exercises normal derivatives and
                // geometric specular AA on both sides of microtriangle edges.
                normal[index] = ((x / 4 + y / 4) % 2) == 0 ? 104 : 184;
                normal[index + 1] = (y / 4) % 2 == 0 ? 112 : 144;
                normal[index + 2] = 250;
                normal[index + 3] = 255;
            }
        }
        const auto upload = [&](const std::vector<std::uint8_t>& bytes) {
            auto* texture = filament::Texture::Builder().width(size).height(size).levels(1)
                .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
            if (!texture) throw std::runtime_error("Helper-lane texture creation failed");
            texture->setImage(engine_, 0, filament::backend::PixelBufferDescriptor(
                bytes.data(), bytes.size(), filament::backend::PixelDataFormat::RGBA,
                filament::backend::PixelDataType::UBYTE));
            return texture;
        };
        helper_color_ = upload(color);
        helper_normal_ = upload(normal);
        // Both local upload buffers stay alive until the driver consumes them.
        engine_.flushAndWait();
    }

    void add_quad(const char* role, glm::vec3 lower, glm::vec3 upper, glm::vec3 normal,
                  bool cast_shadows, bool receive_contact, bool double_sided) {
        Mesh mesh;
        Surface surface;
        const std::array positions{glm::vec3(lower.x, lower.y, lower.z),
                                  glm::vec3(upper.x, lower.y, lower.z),
                                  glm::vec3(upper.x, upper.y, lower.z),
                                  glm::vec3(lower.x, upper.y, lower.z)};
        for (std::size_t i = 0; i < positions.size(); ++i) {
            Vertex vertex{};
            vertex.px = positions[i].x; vertex.py = positions[i].y; vertex.pz = positions[i].z;
            vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
            vertex.u = i == 1 || i == 2 ? 1.0f : 0.0f;
            vertex.v = i >= 2 ? 1.0f : 0.0f;
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2, 0, 2, 3};
        if (helper_lanes_ && helper_.dense && receive_contact) {
            constexpr std::uint32_t columns = 512;
            constexpr std::uint32_t rows = 256;
            surface.vertices.clear();
            surface.indices.clear();
            surface.vertices.reserve(static_cast<std::size_t>(columns + 1) * (rows + 1));
            surface.indices.reserve(static_cast<std::size_t>(columns) * rows * 6);
            for (std::uint32_t y = 0; y <= rows; ++y) {
                for (std::uint32_t x = 0; x <= columns; ++x) {
                    Vertex vertex{};
                    vertex.u = static_cast<float>(x) / static_cast<float>(columns);
                    vertex.v = static_cast<float>(y) / static_cast<float>(rows);
                    vertex.px = lower.x + (upper.x - lower.x) * vertex.u;
                    vertex.py = lower.y + (upper.y - lower.y) * vertex.v;
                    vertex.pz = lower.z;
                    vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
                    surface.vertices.push_back(vertex);
                    if (x < columns && y < rows) {
                        const auto a = y * (columns + 1) + x;
                        const auto b = a + 1;
                        const auto d = a + columns + 1;
                        const auto c = d + 1;
                        surface.indices.insert(surface.indices.end(), {a, b, c, a, c, d});
                    }
                }
            }
        }
        mesh.surfaces.push_back(std::move(surface));
        const auto gpu = meshes_.upload(mesh);
        if (gpu.surfaces.empty()) throw std::runtime_error("Finite Sun mesh upload failed");
        const auto buffers = meshes_.find(gpu.surfaces.front().handle);
        GpuMaterial definition{};
        definition.two_sided = double_sided;
        if (helper_lanes_ && receive_contact && helper_.masked) definition.alpha_ref = 0.5f;
        auto* material = materials_.get_or_create("finite-sun-fixture", role, definition);
        if (!material) throw std::runtime_error("Finite Sun material instance failed");
        const auto* receiver_material = helper_lanes_ && helper_.masked
            ? "heaven_lit_twosided" : "heaven_lit";
        if (receive_contact && std::strcmp(material->getMaterial()->getName(), receiver_material) != 0) {
            throw std::runtime_error("Finite Sun receiver did not route to heaven_lit");
        }
        if (helper_lanes_ && receive_contact) {
            helper_receiver_ = material;
            const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
                filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
            material->setParameter("baseColorMap", helper_color_, sampler);
            material->setParameter("hasBaseColor", true);
            material->setParameter("normalMap", helper_normal_, sampler);
            material->setParameter("hasNormal", true);
            material->setParameter("normalStrength", 1.0f);
            if (!(material->getSpecularAntiAliasingVariance() > 0.0f &&
                  material->getSpecularAntiAliasingThreshold() > 0.0f)) {
                throw std::runtime_error("Helper-lane geometric specular AA is inactive");
            }
        }
        const auto entity = utils::EntityManager::get().create();
        const auto center = 0.5f * (lower + upper);
        const auto half = 0.5f * (upper - lower);
        const auto result = filament::RenderableManager::Builder(1)
            .boundingBox({{center.x, center.y, center.z}, {half.x, half.y, 0.001f}})
            .culling(false)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      buffers.vb, buffers.ib)
            .material(0, material).castShadows(cast_shadows)
            .receiveShadows(receive_contact).screenSpaceContactShadows(receive_contact)
            .build(engine_, entity);
        if (result != filament::RenderableManager::Builder::Success) {
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
            throw std::runtime_error("Finite Sun renderable build failed");
        }
        entities_.push_back(entity);
        scene_->addEntity(entity);
    }

    filament::Engine& engine_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    bool blocker_;
    bool sun_;
    bool helper_lanes_;
    HelperLaneOptions helper_;
    std::uint32_t width_, height_;
    FrameCounts counts_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* helper_color_ = nullptr;
    filament::Texture* helper_normal_ = nullptr;
    filament::MaterialInstance* helper_receiver_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::ColorGrading* grading_ = nullptr;
    utils::Entity camera_entity_, light_;
    std::vector<utils::Entity> entities_;
};

struct Capture {
    Pixels average;
    double worst_mean_frame_delta = 0.0;
};

bool warm_up(FiniteSunFixture& fixture, int frames = kWarmupFrames) {
    int accepted = 0;
    for (int attempts = 0; accepted < frames && attempts < 4 * frames; ++attempts) {
        const auto result = fixture.frame();
        if (result == FrameResult::Failed) return false;
        if (result == FrameResult::Accepted) ++accepted;
    }
    if (accepted != frames) {
        std::printf("%s warmup exhausted retries after %d accepted frames\n", kTag, accepted);
    }
    return accepted == frames;
}

bool capture_average(FiniteSunFixture& fixture, Capture& capture, int frames = kMeasuredFrames) {
    Pixels previous, current;
    std::vector<double> sum;
    int accepted = 0;
    for (int attempts = 0; accepted < frames && attempts < 4 * frames; ++attempts) {
        const auto result = fixture.frame(&current);
        if (result == FrameResult::Failed) return false;
        if (result == FrameResult::Rejected) continue;
        if (sum.empty()) sum.resize(current.size(), 0.0);
        double delta = 0.0;
        std::size_t samples = 0;
        for (std::uint32_t y = 0; y < fixture.height(); ++y) {
            for (std::uint32_t x = 0; x < fixture.width(); ++x) {
                const auto index = (static_cast<std::size_t>(y) * fixture.width() + x) * 4;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const auto value = current[index + channel];
                    if (!std::isfinite(value)) {
                        std::printf("%s nonfinite FLOAT pixel at %u,%u channel %zu\n",
                                    kTag, x, y, channel);
                        return false;
                    }
                    sum[index + channel] += value;
                    if (!previous.empty() && channel < 3 && fixture.measurement_pixel(x, y)) {
                        delta += std::abs(value - previous[index + channel]);
                        ++samples;
                    }
                }
            }
        }
        if (samples > 0) {
            capture.worst_mean_frame_delta = std::max(capture.worst_mean_frame_delta,
                                                      delta / double(samples));
        }
        previous.swap(current);
        ++accepted;
    }
    if (accepted != frames) {
        std::printf("%s measurement exhausted retries after %d accepted frames\n", kTag, accepted);
        return false;
    }
    capture.average.resize(sum.size());
    std::transform(sum.begin(), sum.end(), capture.average.begin(),
                   [frames](double value) { return static_cast<float>(value / frames); });
    return true;
}

bool capture_pair(FiniteSunFixture& fixture, Capture& on, Capture& off) {
    fixture.set_contact(true);
    if (!warm_up(fixture) || !capture_average(fixture, on)) return false;
    fixture.set_contact(false);
    return warm_up(fixture) && capture_average(fixture, off);
}

double mean_rgb(const Pixels& pixels, std::size_t index) {
    return (double(pixels[index]) + pixels[index + 1] + pixels[index + 2]) / 3.0;
}

bool horizon_case(FiniteSunFixture& fixture, double degrees, bool sun = true) {
    fixture.set_elevation(degrees);
    const auto before = fixture.counts();
    Capture on, off;
    if (!capture_pair(fixture, on, off)) return false;
    std::array<double, 3> on_sum{}, off_sum{};
    std::size_t samples = 0;
    bool valid_reference = true;
    double worst_pixel_error = 0.0;
    const double expected = sun ? horizon_visibility(kRadiusDegrees, degrees)
                                : (degrees > 0.0 ? 1.0 : 0.0);
    for (std::uint32_t y = 0; y < fixture.height(); ++y) {
        for (std::uint32_t x = 0; x < fixture.width(); ++x) {
            if (!fixture.measurement_pixel(x, y)) continue;
            const auto index = (static_cast<std::size_t>(y) * fixture.width() + x) * 4;
            const double reference = mean_rgb(off.average, index);
            valid_reference = valid_reference && reference > 0.01 && reference < 0.9;
            if (!(reference > 0.01)) continue;
            const double ratio = mean_rgb(on.average, index) / reference;
            worst_pixel_error = std::max(worst_pixel_error, std::abs(ratio - expected));
            for (std::size_t channel = 0; channel < 3; ++channel) {
                on_sum[channel] += on.average[index + channel];
                off_sum[channel] += off.average[index + channel];
            }
            ++samples;
        }
    }
    std::array<double, 3> ratios{};
    double worst_error = 0.0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (!(off_sum[channel] > 0.0)) return false;
        ratios[channel] = on_sum[channel] / off_sum[channel];
        worst_error = std::max(worst_error, std::abs(ratios[channel] - expected));
    }
    const auto after = fixture.counts();
    std::printf("%s %s horizon beta %.1f degrees, expected %.10f, linear ratios %.6f/%.6f/%.6f, "
                "max mean error %.6f, max pixel error %.6f, reference mean %.6f, "
                "worst on/off frame delta %.6f/%.6f, accepted %llu, rejected %llu\n",
                kTag, sun ? "SUN" : "DIRECTIONAL", degrees, expected,
                ratios[0], ratios[1], ratios[2], worst_error,
                worst_pixel_error, samples > 0 ? off_sum[0] / double(samples) : 0.0,
                on.worst_mean_frame_delta, off.worst_mean_frame_delta,
                static_cast<unsigned long long>(after.accepted - before.accepted),
                static_cast<unsigned long long>(after.rejected - before.rejected));
    return valid_reference && samples > 0 && worst_error <= kHorizonTolerance &&
           worst_pixel_error <= 2.0 * kHorizonTolerance;
}

bool blocker_case(filament::Engine& engine) {
    // Ownership and screen-space availability are independent geometry checks.
    const double angle = radians(kSideAngleDegrees);
    const double reach = kLift * std::tan(radians(kRadiusDegrees));
    const double maximum_measured_x = 0.01;
    const double maximum_hit_projection = (maximum_measured_x + reach) * std::cos(angle) +
                                          kLift * std::sin(angle);
    const double maximum_measurement_y = kBlockerHalfHeight * 24.0 / 256.0;
    if (!(maximum_measured_x < kLift * std::tan(angle) &&
          maximum_hit_projection < (5.0 / 6.0) * kBlockerHalfWidth &&
          maximum_measurement_y + reach < (5.0 / 6.0) * kBlockerHalfHeight &&
          kLift / std::cos(radians(kRadiusDegrees)) < 0.3)) {
        std::printf("%s blocker fixture ownership, no-fade region or distance invalid\n", kTag);
        return false;
    }
    FiniteSunFixture fixture(engine, true);
    Capture on, off;
    if (!capture_pair(fixture, on, off)) return false;
    double worst_error = 0.0, total_error = 0.0, umbra_sum = 0.0, exterior_sum = 0.0;
    std::size_t columns = 0, intermediate = 0, umbra_columns = 0, exterior_columns = 0;
    bool valid_reference = true;
    double lowest_reference = std::numeric_limits<double>::infinity();
    double highest_reference = 0.0;
    for (std::uint32_t x = 0; x < fixture.width(); ++x) {
        if (std::abs(fixture.receiver_x(x)) > maximum_measured_x) continue;
        double on_sum = 0.0, off_sum = 0.0;
        std::size_t samples = 0;
        for (std::uint32_t y = 0; y < fixture.height(); ++y) {
            if (!fixture.measurement_pixel(x, y)) continue;
            const auto index = (static_cast<std::size_t>(y) * fixture.width() + x) * 4;
            const double reference = mean_rgb(off.average, index);
            valid_reference = valid_reference && reference > 0.01 && reference < 0.9;
            lowest_reference = std::min(lowest_reference, reference);
            highest_reference = std::max(highest_reference, reference);
            on_sum += mean_rgb(on.average, index);
            off_sum += reference;
            ++samples;
        }
        if (samples == 0 || !(off_sum > 0.0)) return false;
        const double ratio = on_sum / off_sum;
        const double world_x = fixture.receiver_x(x);
        const double expected = blocker_visibility(world_x, kRadiusDegrees);
        const double error = std::abs(ratio - expected);
        worst_error = std::max(worst_error, error);
        total_error += error;
        ++columns;
        if (ratio > 0.1 && ratio < 0.9) ++intermediate;
        if (world_x >= 0.008) { umbra_sum += ratio; ++umbra_columns; }
        if (world_x <= -0.008) { exterior_sum += ratio; ++exterior_columns; }
        std::printf("%s blocker X %.7f, expected %.6f, measured %.6f\n",
                    kTag, world_x, expected, ratio);
    }
    if (columns == 0 || umbra_columns == 0 || exterior_columns == 0) return false;
    const double mean_error = total_error / double(columns);
    const double umbra = umbra_sum / double(umbra_columns);
    const double exterior = exterior_sum / double(exterior_columns);
    // With contact disabled the raised blocker casts no cascade shadow. The
    // orthographic planar receiver has constant direct shading, so a dark band
    // here would mean the proposed contact-only positive control is not isolated.
    const double reference_spread = (highest_reference - lowest_reference) /
                                   std::max(highest_reference, 1e-9);
    const auto counts = fixture.counts();
    std::printf("%s independent blocker, columns %zu, intermediate %zu, max/mean profile error "
                "%.6f/%.6f, umbra %.6f, exterior %.6f, worst on/off frame delta %.6f/%.6f, "
                "reference relative spread %.6f, accepted %llu, rejected %llu\n",
                kTag, columns, intermediate, worst_error,
                mean_error, umbra, exterior, on.worst_mean_frame_delta, off.worst_mean_frame_delta,
                reference_spread,
                static_cast<unsigned long long>(counts.accepted),
                static_cast<unsigned long long>(counts.rejected));
    // The profile oracle is continuous; angular quadrature and screen-space depth
    // have finite error. Endpoints and intermediate coverage prevent a disabled
    // contact path or a single hard ray from passing this positive control.
    return valid_reference && reference_spread <= 0.02 &&
           worst_error <= 0.15 && mean_error <= 0.07 &&
           intermediate >= 8 && umbra <= 0.05 && exterior >= 0.95;
}

struct HelperLaneImages {
    std::array<Capture, 2> on;
    std::array<Capture, 2> off;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class AlphaNeighborhood { Transparent, Opaque, FractionalOrEdge };

AlphaNeighborhood helper_alpha_neighborhood(std::uint32_t x, std::uint32_t y,
                                             std::uint32_t width, std::uint32_t height) {
    bool all_zero = true, all_one = true;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const double world_x = (((double(x) + 0.5 + dx) / width * 2.0 - 1.0) *
                                    kBlockerHalfWidth) / std::cos(radians(kSideAngleDegrees));
            const double u = (world_x + 0.0625) / 0.125;
            for (int orientation : {-1, 1}) {
                // Raw target-row orientation differs across backends. Requiring
                // both Y orientations is conservative without guessing a flip.
                const double world_y = orientation *
                    ((double(y) + 0.5 + dy) / height * 2.0 - 1.0) * kBlockerHalfHeight;
                const double v = (world_y + 0.03125) / 0.0625;
                // The full derivative neighborhood must have constant alpha,
                // not just the covered pixel. Reserve 0.001 texel for the
                // affine interpolation's FLOAT rounding at texel boundaries.
                constexpr double texel_reserve = 0.001;
                const auto min_x = static_cast<int>(std::floor(u * 64.0 - 0.5 - texel_reserve));
                const auto max_x = static_cast<int>(std::floor(u * 64.0 - 0.5 + texel_reserve));
                const auto min_y = static_cast<int>(std::floor(v * 64.0 - 0.5 - texel_reserve));
                const auto max_y = static_cast<int>(std::floor(v * 64.0 - 0.5 + texel_reserve));
                for (int base_y = min_y; base_y <= max_y; ++base_y) {
                    for (int base_x = min_x; base_x <= max_x; ++base_x) {
                        for (int ty : {0, 1}) {
                            for (int tx : {0, 1}) {
                                const auto wrapped_x = static_cast<std::uint32_t>(((base_x + tx) % 64 + 64) % 64);
                                const auto wrapped_y = static_cast<std::uint32_t>(((base_y + ty) % 64 + 64) % 64);
                                const auto alpha = helper_alpha_texel(wrapped_x, wrapped_y);
                                all_zero = all_zero && alpha == 0;
                                all_one = all_one && alpha == 255;
                            }
                        }
                    }
                }
            }
        }
    }
    if (all_one) return AlphaNeighborhood::Opaque;
    if (all_zero) return AlphaNeighborhood::Transparent;
    return AlphaNeighborhood::FractionalOrEdge;
}

bool dump_helper_capture(const Capture& capture, std::uint32_t width, std::uint32_t height,
                         std::uint8_t samples, bool masked, std::size_t geometry, bool contact) {
    const auto* configured = std::getenv("HEAVEN_SMOKE_HELPER_DUMP_DIR");
    if (!configured || !*configured) return true;
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    const std::filesystem::path directory(configured);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::printf("%s helper dump directory failed: %s\n", kTag, error.message().c_str());
        return false;
    }
    const std::string name = std::string(masked ? "masked-" : "opaque-") +
        std::to_string(static_cast<unsigned int>(samples)) + "x-" +
        (geometry == 0 ? "coarse-" : "dense-") + (contact ? "on" : "off");
    const auto bytes = capture.average.size() * sizeof(float);
    const auto raw_path = directory / (name + ".rgba32f");
    std::ofstream raw(raw_path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!raw) {
        std::printf("%s helper FLOAT dump cannot create exclusive file %s\n", kTag,
                    raw_path.string().c_str());
        return false;
    }
    raw.write(reinterpret_cast<const char*>(capture.average.data()), static_cast<std::streamsize>(bytes));
    raw.close();
    if (!raw) return false;
    std::ofstream metadata(directory / (name + ".json"), std::ios::out | std::ios::noreplace);
    metadata << "{\n  \"schema\":1,\n  \"width\":" << width << ",\n  \"height\":" << height
        << ",\n  \"channels\":4,\n  \"component\":\"IEEE754_FLOAT32\",\n  \"byte_order\":\""
        << (std::endian::native == std::endian::little ? "little" : "big")
        << "\",\n  \"row_order\":\"native_readPixels\",\n  \"color\":\"linear_Rec709\",\n"
        << "  \"averaged_frames\":8,\n  \"clear_every_frame\":true,\n  \"clear_rgba\":[0,0,0,0],\n"
        << "  \"view_blend_mode\":\"opaque\",\n  \"bytes\":" << bytes << ",\n  \"msaa_samples\":"
        << static_cast<unsigned int>(samples) << ",\n  \"masked_a2c\":" << (masked ? "true" : "false")
        << ",\n  \"triangles\":" << (geometry == 0 ? 2 : 262144)
        << ",\n  \"contact_shadows\":" << (contact ? "true" : "false") << "\n}\n";
    metadata.close();
    if (!metadata) return false;
    std::printf("%s helper FLOAT dump %s (%zu bytes)\n", kTag, raw_path.string().c_str(), bytes);
    return true;
}

bool helper_lane_case(filament::Engine& engine, std::uint8_t samples, bool masked,
                      HelperLaneImages& images, const HelperLaneImages* opaque) {
    constexpr int warmup_frames = 16;
    constexpr int measured_frames = 8;
    constexpr double maximum_pixel_delta = 0.006;
    constexpr double maximum_mean_delta = 0.0003;
    constexpr double maximum_ratio_delta = 0.025;
    for (std::size_t geometry = 0; geometry < 2; ++geometry) {
        const HelperLaneOptions options{.dense = geometry != 0, .masked = masked,
                                        .samples = samples};
        FiniteSunFixture fixture(engine, true, true, &options);
        images.width = fixture.width();
        images.height = fixture.height();
        fixture.set_contact(false);
        if (!warm_up(fixture, warmup_frames) ||
            !capture_average(fixture, images.off[geometry], measured_frames)) return false;
        if (!dump_helper_capture(images.off[geometry], images.width, images.height,
                                  samples, masked, geometry, false)) return false;
        fixture.set_contact(true);
        if (!warm_up(fixture, warmup_frames) ||
            !capture_average(fixture, images.on[geometry], measured_frames)) return false;
        if (!dump_helper_capture(images.on[geometry], images.width, images.height,
                                  samples, masked, geometry, true)) return false;
    }

    std::size_t pixels = 0, lit_pixels = 0, umbra_pixels = 0, exterior_pixels = 0;
    std::size_t derivative_pixels = 0, holes = 0, alpha_edges = 0;
    std::size_t stable_opaque_pixels = 0;
    std::size_t stable_transparent_pixels = 0, coverage_mismatches = 0, stable_coverage_mismatches = 0;
    double max_pixel_delta = 0.0, absolute_delta = 0.0, max_ratio_delta = 0.0;
    double transparent_background_max_rgb = 0.0;
    double umbra_sum = 0.0, exterior_sum = 0.0, contact_delta = 0.0;
    double derivative_max_delta = 0.0, lowest_reference = 1.0, highest_reference = 0.0;
    for (std::uint32_t y = images.height / 2 - 12; y < images.height / 2 + 12; ++y) {
        for (std::uint32_t x = 1; x + 1 < images.width; ++x) {
            const double world_x = (((double(x) + 0.5) / images.width * 2.0 - 1.0) *
                                    kBlockerHalfWidth) / std::cos(radians(kSideAngleDegrees));
            if (std::abs(world_x) > 0.01) continue;
            const auto index = (static_cast<std::size_t>(y) * images.width + x) * 4;
            const double coarse_reference = mean_rgb(images.off[0].average, index);
            const double dense_reference = mean_rgb(images.off[1].average, index);
            const auto alpha_neighborhood = helper_alpha_neighborhood(x, y, images.width, images.height);
            const bool stable_alpha = alpha_neighborhood != AlphaNeighborhood::FractionalOrEdge;
            stable_opaque_pixels += alpha_neighborhood == AlphaNeighborhood::Opaque;
            stable_transparent_pixels += alpha_neighborhood == AlphaNeighborhood::Transparent;
            if ((coarse_reference > 0.01) != (dense_reference > 0.01)) {
                ++coverage_mismatches;
                if (stable_alpha) ++stable_coverage_mismatches;
            }
            double pixel_delta = 0.0;
            for (const auto* captures : {&images.on, &images.off}) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const double delta = std::abs(double((*captures)[0].average[index + channel]) -
                                                          (*captures)[1].average[index + channel]);
                    absolute_delta += delta;
                    pixel_delta = std::max(pixel_delta, delta);
                    if (masked && alpha_neighborhood == AlphaNeighborhood::Transparent) {
                        transparent_background_max_rgb = std::max({transparent_background_max_rgb,
                            std::abs(double((*captures)[0].average[index + channel])),
                            std::abs(double((*captures)[1].average[index + channel]))});
                    }
                }
            }
            max_pixel_delta = std::max(max_pixel_delta, pixel_delta);
            ++pixels;
            if (opaque) {
                const double full_reference = mean_rgb(opaque->off[0].average, index);
                if (full_reference > 0.01) {
                    const double coverage = coarse_reference / full_reference;
                    holes += coverage < 0.02;
                    // The masked shader's authored foliage-detail multiplier
                    // lowers fully covered normal-mapped colors by up to ~12%.
                    // A <0.8 ratio instead detects genuine partial coverage.
                    alpha_edges += coverage > 0.05 && coverage < 0.8;
                }
            }
            if (coarse_reference <= 0.01 || dense_reference <= 0.01) continue;
            ++lit_pixels;
            lowest_reference = std::min(lowest_reference, coarse_reference);
            highest_reference = std::max(highest_reference, coarse_reference);
            const double coarse_ratio = mean_rgb(images.on[0].average, index) / coarse_reference;
            const double dense_ratio = mean_rgb(images.on[1].average, index) / dense_reference;
            max_ratio_delta = std::max(max_ratio_delta, std::abs(coarse_ratio - dense_ratio));
            contact_delta += coarse_reference - mean_rgb(images.on[0].average, index);
            if (world_x >= 0.008) { umbra_sum += dense_ratio; ++umbra_pixels; }
            if (world_x <= -0.008) { exterior_sum += dense_ratio; ++exterior_pixels; }
            const double gradient = std::abs(mean_rgb(images.off[0].average, index - 4) -
                                             mean_rgb(images.off[0].average, index + 4));
            if (gradient > 0.003) {
                ++derivative_pixels;
                derivative_max_delta = std::max(derivative_max_delta, pixel_delta);
            }
        }
    }
    if (pixels == 0 || lit_pixels == 0 || umbra_pixels == 0 || exterior_pixels == 0) {
        std::printf("%s helper lanes have empty measured/bright/umbra/exterior sets: "
                    "%zu/%zu/%zu/%zu\n", kTag, pixels, lit_pixels, umbra_pixels, exterior_pixels);
        return false;
    }
    const double mean_delta = absolute_delta / (double(pixels) * 6.0);
    const double umbra = umbra_sum / double(umbra_pixels);
    const double exterior = exterior_sum / double(exterior_pixels);
    const double mean_contact_delta = contact_delta / double(lit_pixels);
    const bool correct = lit_pixels >= 256 && derivative_pixels >= 16 &&
        lowest_reference > 0.01 && highest_reference < 0.95 &&
        umbra <= 0.08 && exterior >= 0.90 && mean_contact_delta > 0.005 &&
        max_pixel_delta <= maximum_pixel_delta && mean_delta <= maximum_mean_delta &&
        max_ratio_delta <= maximum_ratio_delta && derivative_max_delta <= maximum_pixel_delta &&
        (!masked || (holes >= 16 && stable_opaque_pixels >= 32 && stable_transparent_pixels >= 64 &&
                     coverage_mismatches == 0 && stable_coverage_mismatches == 0 &&
                     transparent_background_max_rgb <= 1e-6 && (samples == 1 || alpha_edges >= 8)));
    std::printf("%s helper lanes %ux MSAA %s, coarse/dense triangles 2/262144, "
                "measured/bright pixels %zu/%zu, FLOAT parity max/mean RGB delta %.8g/%.8g, "
                "max contact-ratio delta %.8g, normal-derivative pixels %zu max delta %.8g, "
                "reference range %.6f/%.6f, dense umbra/exterior %.6f/%.6f, "
                "mean contact darkening %.6f, alpha holes/edges %zu/%zu, excluded pixels 0, "
                "stable opaque/transparent %zu/%zu, coverage mismatches total/stable %zu/%zu, "
                "known-transparent background max RGB %.8g: %s\n",
                kTag, static_cast<unsigned int>(samples), masked ? "masked A2C" : "opaque",
                pixels, lit_pixels, max_pixel_delta, mean_delta, max_ratio_delta,
                derivative_pixels, derivative_max_delta, lowest_reference, highest_reference,
                umbra, exterior, mean_contact_delta, holes, alpha_edges,
                stable_opaque_pixels, stable_transparent_pixels, coverage_mismatches, stable_coverage_mismatches,
                transparent_background_max_rgb, correct ? "PASS" : "FAIL");
    return correct;
}
} // namespace

CaseResult run_finite_sun_contact_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    bool passed = true;
    {
        FiniteSunFixture fixture(*engine, false);
        for (double degrees : {-4.0, -2.0, 0.0, 2.0, 4.0}) {
            // Execute every angle even after a failure so the receipt remains useful.
            const bool correct = horizon_case(fixture, degrees);
            passed = correct && passed;
        }
    }
    {
        // At these angles the projected caster/receiver bounds do not overlap,
        // so contact must survive without a visible cascade. DIRECTIONAL takes
        // the existing single-ray path and isolates uniform publication from
        // the finite-Sun integration checked above.
        FiniteSunFixture fixture(*engine, false, false);
        for (double degrees : {-2.0, 2.0}) {
            const bool correct = horizon_case(fixture, degrees, false);
            passed = correct && passed;
        }
    }
    const bool blocker_correct = blocker_case(*engine);
    return blocker_correct && passed ? CaseResult::Pass : CaseResult::Fail;
}

CaseResult run_contact_helper_lanes_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament || ctx.backend.render_api_name() != "Vulkan") {
        std::printf("%s helper lanes SKIP: this dense/coarse FLOAT oracle is Vulkan-only; "
                    "cross-tessellation MSAA centroid parity is not established on other APIs. "
                    "The ordinary finite-Sun fixture remains independent.\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    // A selected standalone case has no earlier suite case to initialize the
    // backend's published RenderContext. The fixture still owns only its own
    // scenes/views; ordinary backend initialization remains the harness path.
    if (const auto initialized = backend.init_pipelines(ctx.config); !initialized) {
        std::printf("%s helper-lane backend initialization failed: %s\n", kTag,
                    initialized.error().message.c_str());
        return CaseResult::Fail;
    }
    auto* engine = backend.debug_render_context().engine;
    if (!engine) {
        std::printf("%s helper-lane active backend has no engine\n", kTag);
        return CaseResult::Fail;
    }
    bool passed = true;
    for (const std::uint8_t samples : {std::uint8_t{1}, std::uint8_t{4}}) {
        HelperLaneImages opaque, masked;
        const bool opaque_correct = helper_lane_case(*engine, samples, false, opaque, nullptr);
        passed = opaque_correct && passed;
        // Test A2C even if opaque numerical parity fails, retaining diagnostic
        // coverage instead of hiding later modes behind an early return.
        if (!opaque.off[0].average.empty()) {
            const bool masked_correct = helper_lane_case(*engine, samples, true, masked, &opaque);
            passed = masked_correct && passed;
        } else {
            passed = false;
        }
    }
    return passed ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
