// Native production distortion/output oracle, independent of either FG SDK.
#include "rhi/frame_generation/frame_generation_output.h"
#include "rhi/materials_data.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/half.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace heaven {
namespace {
constexpr const char* kTag = "[distortion_field_test]";
constexpr uint32_t kGrid = 8;
constexpr uint32_t kPackedExtent = 64;
constexpr double kAmplitude = 1.0 / 64.0;
constexpr double kFieldBound = .1 / kGrid;
constexpr double kContractionBound = .4;
constexpr double kPreStorageResidual = 2e-6;
constexpr double kInverseFp32Allowance = 2e-7;
constexpr double kFloatEpsilon = std::numeric_limits<float>::epsilon();
constexpr double kAffineLipschitz = kAmplitude * (.25 + .125);
// Conservative FP32 operation budget for viewport conversion and the explicit
// four-node interpolation; it is independent of GPU observations and HALF ULPs.
constexpr double kForwardFp32Allowance =
    64 * kFloatEpsilon / (1 - 64 * kFloatEpsilon) * kFieldBound +
    4 * kFloatEpsilon * kAffineLipschitz;
constexpr float kDepth = .25f;
constexpr std::array<float, 2> kMotion{-.015625f, .0078125f};
constexpr std::array<double, 4> kUi{.9, .15, .7, 1.0};
using Vec2 = std::array<double, 2>;
using Rgba8 = std::array<uint8_t, 4>;
using Half4 = std::array<uint16_t, 4>;
using Half2 = std::array<uint16_t, 2>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double half_spacing(double value) {
    const double magnitude = std::abs(value);
    require(std::isfinite(magnitude) && magnitude <= 65504, "HALF oracle input is not finite binary16 range");
    return magnitude < std::ldexp(1.0, -14) ? std::ldexp(1.0, -24) :
        std::ldexp(1.0, std::ilogb(magnitude) - 10);
}

std::array<double, 2> half_neighbors(double value) {
    const double spacing = half_spacing(value);
    // Scaling by a binary power is exact. This also handles negative values,
    // subnormals and the asymmetric spacing adjoining an exponent boundary.
    return {std::floor(value / spacing) * spacing, std::ceil(value / spacing) * spacing};
}

struct HalfStorageBound {
    double lower, upper, quantum;
    bool accepts(double actual) const { return actual >= lower && actual <= upper; }
    double error_from(double expected) const {
        return std::max(std::abs(lower - expected), std::abs(upper - expected));
    }
};

HalfStorageBound half_storage_bound(double expected, double fp32_allowance) {
    require(std::isfinite(fp32_allowance) && fp32_allowance >= 0, "Invalid FP32 oracle reserve");
    const double minimum = expected - fp32_allowance, maximum = expected + fp32_allowance;
    // Vulkan Floating-Point Format Conversions permits either adjacent finite
    // representation, not necessarily round-to-nearest. Therefore storage may
    // contribute one HALF ULP, rather than half an ULP. Derive both neighbors
    // from independent expected values; never derive a tolerance from readback.
    // https://docs.vulkan.org/spec/latest/chapters/fundamentals.html#fundamentals-fp-conversion
    return {half_neighbors(minimum)[0], half_neighbors(maximum)[1],
        std::max(half_spacing(minimum), half_spacing(maximum))};
}

// Dyadic affine nodes are exactly representable in the uploaded RGBA16F.
// Both components are signed and asymmetric. Their maximum is below .1/8.
Vec2 affine(Vec2 uv) {
    return {kAmplitude * (.5 + .25 * uv[0] - .125 * uv[1]),
            kAmplitude * (-.5 + .125 * uv[0] + .125 * uv[1])};
}

Vec2 inverse_affine(Vec2 target) {
    // Solve (I + A) p + b = target analytically, without the shader's
    // fixed-point iteration or the stored inverse channels as input.
    const double a = 1.0 + kAmplitude * .25, b = -kAmplitude * .125;
    const double c = kAmplitude * .125, d = 1.0 + kAmplitude * .125;
    const double x = target[0] - kAmplitude * .5;
    const double y = target[1] + kAmplitude * .5;
    const double determinant = a * d - b * c;
    return {(d * x - b * y) / determinant, (-c * x + a * y) / determinant};
}

std::array<float, 4> packed_node(uint32_t x, uint32_t y) {
    const float u = (float(x) + .5f) / float(kPackedExtent);
    const float v = (float(y) + .5f) / float(kPackedExtent);
    return {kMotion[0] + .015625f * u, kMotion[1] - .015625f * v,
        kDepth + .125f * u + .0625f * v, 1.0f};
}

Vec2 clamped_field(Vec2 uv) {
    // Independent bilinear interpolation of the known source nodes. Outside
    // the outer texel centers, CLAMP_TO_EDGE extends the endpoint value.
    const double x = std::clamp(uv[0] * kGrid - .5, 0.0, double(kGrid - 1));
    const double y = std::clamp(uv[1] * kGrid - .5, 0.0, double(kGrid - 1));
    const auto x0 = uint32_t(std::floor(x)), y0 = uint32_t(std::floor(y));
    const auto x1 = std::min(x0 + 1, kGrid - 1), y1 = std::min(y0 + 1, kGrid - 1);
    auto node = [](uint32_t column, uint32_t row) {
        return affine({(double(column) + .5) / kGrid, (double(row) + .5) / kGrid});
    };
    const auto v00 = node(x0, y0), v10 = node(x1, y0);
    const auto v01 = node(x0, y1), v11 = node(x1, y1);
    Vec2 result{};
    for (size_t channel = 0; channel < result.size(); ++channel) {
        const double top = std::lerp(v00[channel], v10[channel], x - x0);
        const double bottom = std::lerp(v01[channel], v11[channel], x - x0);
        result[channel] = std::lerp(top, bottom, y - y0);
    }
    return result;
}

Vec2 inverse_clamped_field(Vec2 target) {
    // The independently authored field is affine in each of the nine clamped
    // regions. Solve each 2x2 system directly instead of copying GPU iteration.
    constexpr double low = .5 / kGrid, high = 1 - low;
    for (int row = -1; row <= 1; ++row) for (int column = -1; column <= 1; ++column) {
        const double sx = column == 0 ? 1 : 0, sy = row == 0 ? 1 : 0;
        const double bx = column < 0 ? low : column > 0 ? high : 0;
        const double by = row < 0 ? low : row > 0 ? high : 0;
        const double a = 1 + kAmplitude * .25 * sx, b = -kAmplitude * .125 * sy;
        const double c = kAmplitude * .125 * sx, d = 1 + kAmplitude * .125 * sy;
        const double x = target[0] - kAmplitude * (.5 + .25 * bx - .125 * by);
        const double y = target[1] - kAmplitude * (-.5 + .125 * bx + .125 * by);
        const double determinant = a * d - b * c;
        const Vec2 candidate{(d * x - b * y) / determinant, (a * y - c * x) / determinant};
        const auto belongs = [](double coordinate, int region) {
            constexpr double reserve = 8 * std::numeric_limits<double>::epsilon();
            return region < 0 ? coordinate <= low + reserve : region > 0 ? coordinate >= high - reserve :
                coordinate >= low - reserve && coordinate <= high + reserve;
        };
        if (belongs(candidate[0], column) && belongs(candidate[1], row)) return candidate;
    }
    throw std::runtime_error("Independent clamped-field inverse has no matching region");
}

void check_cpu_oracles() {
    const double boundary = std::ldexp(1.0, -7), below_ulp = std::ldexp(1.0, -18);
    const double above_ulp = std::ldexp(1.0, -17), subnormal_ulp = std::ldexp(1.0, -24);
    const double normal_min = std::ldexp(1.0, -14);
    const std::array<std::array<double, 5>, 5> cases{{
        {boundary - below_ulp / 4, boundary - below_ulp, boundary, boundary - 2 * below_ulp, boundary + above_ulp},
        {boundary + above_ulp / 4, boundary, boundary + above_ulp, boundary - below_ulp, boundary + 2 * above_ulp},
        {normal_min - subnormal_ulp / 4, normal_min - subnormal_ulp, normal_min,
            normal_min - 2 * subnormal_ulp, normal_min + subnormal_ulp},
        {normal_min + subnormal_ulp / 4, normal_min, normal_min + subnormal_ulp,
            normal_min - subnormal_ulp, normal_min + 2 * subnormal_ulp},
        {subnormal_ulp / 4, 0, subnormal_ulp, -subnormal_ulp, 2 * subnormal_ulp}
    }};
    for (double sign : {-1.0, 1.0}) for (const auto& item : cases) {
        const auto bound = half_storage_bound(sign * item[0], 0);
        require(bound.accepts(sign * item[1]) && bound.accepts(sign * item[2]),
            "HALF boundary oracle rejected an adjoining representation");
        require(!bound.accepts(sign * item[3]) && !bound.accepts(sign * item[4]),
            "HALF boundary oracle accepted a non-adjoining/two-ULP representation");
    }
    for (double sign : {-1.0, 1.0}) {
        const double expected = sign * (boundary + kInverseFp32Allowance / 2);
        const auto bound = half_storage_bound(expected, kInverseFp32Allowance);
        require(bound.accepts(sign * (boundary - below_ulp)) && bound.accepts(sign * (boundary + above_ulp)),
            "FP32 reserve crossing a HALF exponent boundary lost an allowed neighbor");
        require(!bound.accepts(sign * (boundary - 2 * below_ulp)) && !bound.accepts(sign * (boundary + 2 * above_ulp)),
            "FP32 reserve crossing a HALF exponent boundary accepted two-ULP corruption");
    }
    for (double y : {-.02, .25, 1.02}) for (double x : {-.02, .25, 1.02}) {
        const Vec2 source{x, y};
        const auto offset = clamped_field(source);
        const auto recovered = inverse_clamped_field({x + offset[0], y + offset[1]});
        require(std::abs(recovered[0] - x) < 1e-14 && std::abs(recovered[1] - y) < 1e-14,
            "Independent nine-region analytic inverse failed its CPU control");
    }
    std::puts("[distortion_field_test] CPU HALF neighbors=20 two-ULP rejections=24 exponent-reserve=4 inverse-regions=9 PASS");
}

enum class Fault { None, ForwardSign, SwappedChannels };

class Fixture {
public:
    explicit Fixture(filament::Engine& engine) : engine_(engine) {}
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        engine_.flushAndWait();
        clear_read_targets();
        if (scene_view_) scene_view_->setRenderTarget(nullptr);
        output_.reset();
        if (ui_view_) ui_view_->setRenderTarget(nullptr);
        for (size_t index = 0; index < entities_.size(); ++index) {
            if (entities_[index]) {
                if (index == 0 && scene_) scene_->remove(entities_[index]);
                if (index == 1 && ui_scene_) ui_scene_->remove(entities_[index]);
                engine_.destroy(entities_[index]);
                utils::EntityManager::get().destroy(entities_[index]);
            }
        }
        for (auto* view : {scene_view_, ui_view_}) if (view) {
            view->setScene(nullptr); view->setCamera(nullptr); engine_.destroy(view);
        }
        for (auto* scene : {scene_, ui_scene_}) if (scene) engine_.destroy(scene);
        for (auto* instance : instances_) if (instance) engine_.destroy(instance);
        for (auto* material : materials_) if (material) engine_.destroy(material);
        if (vertices_) engine_.destroy(vertices_);
        if (indices_) engine_.destroy(indices_);
        for (auto* texture : {source_, packed_, coarse_}) if (texture) engine_.destroy(texture);
        if (camera_entity_) {
            engine_.destroyCameraComponent(camera_entity_);
            utils::EntityManager::get().destroy(camera_entity_);
        }
        if (renderer_) engine_.destroy(renderer_);
        if (swapchain_) engine_.destroy(swapchain_);
        engine_.flushAndWait();
    }

    void init(Fault fault) {
        renderer_ = engine_.createRenderer();
        scene_ = engine_.createScene(); ui_scene_ = engine_.createScene();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        require(renderer_ && scene_ && ui_scene_ && camera_, "Native distortion core unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        camera_->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1, .1, 10);
        camera_->lookAt({0, 0, 1}, {0, 0, 0}, {0, 1, 0});
        camera_->setExposure(1);
        scene_view_ = make_view(scene_); ui_view_ = make_view(ui_scene_);
        packed_ = filament::Texture::Builder().width(kPackedExtent).height(kPackedExtent).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).build(engine_);
        using Usage = filament::Texture::Usage;
        using UsageBits = std::underlying_type_t<Usage>;
        constexpr auto coarse_usage = std::bit_cast<Usage>(static_cast<UsageBits>(
            std::to_underlying(Usage::DEFAULT) | std::to_underlying(Usage::COLOR_ATTACHMENT) |
            std::to_underlying(Usage::BLIT_SRC)));
        coarse_ = filament::Texture::Builder().width(kGrid).height(kGrid).levels(1)
            .format(filament::Texture::InternalFormat::RGBA16F).usage(coarse_usage).build(engine_);
        require(packed_ && coarse_, "Synthetic temporal/coarse textures unavailable");
        for (uint32_t row = 0; row < kPackedExtent; ++row) for (uint32_t column = 0; column < kPackedExtent; ++column)
            packed_nodes_[size_t(row) * kPackedExtent + column] = packed_node(column, row);
        packed_->setImage(engine_, 0, {packed_nodes_.data(), sizeof(packed_nodes_),
            filament::Texture::Format::RGBA, filament::Texture::Type::FLOAT});
        for (uint32_t row = 0; row < kGrid; ++row) for (uint32_t column = 0; column < kGrid; ++column) {
            auto value = affine({(double(column) + .5) / kGrid, (double(row) + .5) / kGrid});
            if (fault == Fault::ForwardSign) { value[0] = -value[0]; value[1] = -value[1]; }
            if (fault == Fault::SwappedChannels) std::swap(value[0], value[1]);
            coarse_nodes_[size_t(row) * kGrid + column] = {float(value[0]), float(value[1]), 0.0f, 1.0f};
        }
        coarse_->setImage(engine_, 0, {coarse_nodes_.data(), sizeof(coarse_nodes_),
            filament::Texture::Format::RGBA, filament::Texture::Type::FLOAT});
        make_geometry();
        output_ = std::make_unique<FrameGenerationOutput>(BackendCore{
            .engine = &engine_, .scene = scene_, .view = scene_view_, .renderer = renderer_, .camera = camera_});
        resize(320, 256);
    }

    void resize(uint32_t width, uint32_t height) {
        // Each packed texel spans an integer number of output pixels. Pixel
        // centers therefore never land on a NEAREST boundary: (i+.5)/N
        // cannot be an integer for integer N. Odd extents would still put
        // their center pixel at UV=.5, a boundary of the even-sized input.
        require(width >= kPackedExtent && height >= kPackedExtent &&
            width % kPackedExtent == 0 && height % kPackedExtent == 0,
            "Distortion fixture extent creates ambiguous nearest samples");
        engine_.flushAndWait(); clear_read_targets();
        scene_view_->setRenderTarget(nullptr);
        if (output_->valid()) output_->prepare_distortion(nullptr);
        instances_[0]->setParameter("sceneTexture", packed_, nearest());
        if (source_) engine_.destroy(source_);
        source_ = nullptr;
        require(output_->ensure(int(width), int(height)), "Production distortion output resize failed");
        require(output_->distortion_texture() == nullptr, "Resize retained an active borrowed distortion source");
        if (swapchain_) engine_.destroy(swapchain_);
        swapchain_ = engine_.createSwapChain(width, height, filament::SwapChain::CONFIG_READABLE);
        require(swapchain_, "Readable native distortion swapchain unavailable");
        width_ = width; height_ = height;
        source_ = filament::Texture::Builder().width(width).height(height).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
        require(source_, "Synthetic scene texture unavailable");
        source_pixels_.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            const bool marker = x > width / 2 && x < width * 3 / 4 && y > height / 3 && y < height * 2 / 3;
            source_pixels_[size_t(y) * width + x] = {uint8_t(x * 255 / (width - 1)),
                uint8_t(y * 255 / (height - 1)), uint8_t(marker ? 235 : 30), 255};
        }
        source_->setImage(engine_, 0, {source_pixels_.data(), source_pixels_.size() * sizeof(Rgba8),
            filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE});
        instances_[0]->setParameter("sceneTexture", source_, nearest());
        scene_view_->setViewport({0, 0, width, height}); ui_view_->setViewport({0, 0, width, height});
        engine_.flushAndWait();
    }

    void capture(const char* label, bool distorted, bool has_ui) {
        engine_.flushAndWait(); clear_read_targets();
        output_->prepare(packed_, int(width_), int(height_), has_ui);
        require(output_->prepare_distortion(distorted ? coarse_ : nullptr), "Production distortion preparation failed");
        require((output_->distortion_texture() != nullptr) == distorted, "Distortion availability does not match explicit opt-in");
        scene_view_->setRenderTarget(output_->scene_rt());
        read_targets_[0] = target(output_->color_texture());
        read_targets_[1] = target(output_->depth_texture());
        read_targets_[2] = target(output_->motion_texture());
        if (distorted) read_targets_[3] = target(output_->distortion_texture());
        read_targets_[4] = target(coarse_);
        const size_t count = size_t(width_) * height_;
        std::vector<Rgba8> color(count), real(count), raw(count);
        std::vector<float> depth(count);
        std::vector<Half2> motion(count);
        std::vector<Half4> field(count);
        std::array<Half4, static_cast<size_t>(kGrid) * kGrid> coarse_read{};
        std::array<std::atomic_bool, 7> done{};
        bool accepted = false;
        for (int retry = 0; retry < 8 && !accepted; ++retry) {
            accepted = renderer_->beginFrame(swapchain_);
            if (!accepted) engine_.flushAndWait();
        }
        require(accepted, "Distortion frame rejected all bounded attempts");
        renderer_->render(scene_view_);
        output_->render_extract();
        if (has_ui) { output_->bind_ui(*ui_view_); renderer_->render(ui_view_); output_->unbind_ui(); }
        output_->render_composite();
        read(read_targets_[0], color.data(), count * sizeof(Rgba8), filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE, done[0]);
        read(read_targets_[1], depth.data(), count * sizeof(float), filament::Texture::Format::R, filament::Texture::Type::FLOAT, done[1]);
        read(read_targets_[2], motion.data(), count * sizeof(Half2), filament::Texture::Format::RG, filament::Texture::Type::HALF, done[2]);
        if (distorted) read(read_targets_[3], field.data(), count * sizeof(Half4), filament::Texture::Format::RGBA, filament::Texture::Type::HALF, done[3]);
        else done[3] = true;
        read(output_->scene_rt(), raw.data(), count * sizeof(Rgba8), filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE, done[4]);
        renderer_->readPixels(0, 0, width_, height_, {real.data(), count * sizeof(Rgba8),
            filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE, complete, &done[5]});
        renderer_->readPixels(read_targets_[4], 0, 0, kGrid, kGrid, {coarse_read.data(), sizeof(coarse_read),
            filament::Texture::Format::RGBA, filament::Texture::Type::HALF, complete, &done[6]});
        renderer_->endFrame(); engine_.flushAndWait();
        require(std::ranges::all_of(done, [](const auto& value) { return value.load(); }), "Distortion readback incomplete after drain");
        check_coarse_upload(label, coarse_read);
        if (distorted) report_field_diagnostics(label, field);
        check(label, distorted, has_ui, color, real, raw, depth, motion, field);
    }

private:
    struct Vertex { filament::math::float3 position; filament::math::float2 uv; filament::math::float4 color; };
    static filament::TextureSampler nearest() {
        return {filament::TextureSampler::MinFilter::NEAREST, filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};
    }
    filament::View* make_view(filament::Scene* scene) {
        auto* view = engine_.createView(); require(view, "Native distortion view unavailable");
        view->setScene(scene); view->setCamera(camera_);
        view->setPostProcessingEnabled(false); view->setShadowingEnabled(false);
        view->setAntiAliasing(filament::AntiAliasing::NONE);
        view->setMultiSampleAntiAliasingOptions({.enabled = false});
        view->setTemporalAntiAliasingOptions({.enabled = false});
        view->setDithering(filament::Dithering::NONE);
        view->setBlendMode(filament::View::BlendMode::OPAQUE);
        return view;
    }
    void make_geometry() {
        const std::array<std::array<double, 4>, 2> rectangles{{{0, 0, 1, 1}, {.12, .15, .38, .35}}};
        for (size_t item = 0; item < rectangles.size(); ++item) {
            const auto& rect = rectangles[item];
            const std::array<Vec2, 4> corners{{{rect[0], rect[1]}, {rect[0], rect[3]}, {rect[2], rect[3]}, {rect[2], rect[1]}}};
            for (size_t corner = 0; corner < corners.size(); ++corner) {
                const auto& point = corners[corner];
                vertices_cpu_[item * 4 + corner] = {{float(2 * point[0] - 1), float(1 - 2 * point[1]), 0},
                    {float(point[0]), float(point[1])}, {float(kUi[0]), float(kUi[1]), float(kUi[2]), 1}};
            }
        }
        static constexpr std::array<uint16_t, 12> triangles{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        vertices_ = filament::VertexBuffer::Builder().vertexCount(8).bufferCount(1)
            .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3, offsetof(Vertex, position), sizeof(Vertex))
            .attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex))
            .attribute(filament::VertexAttribute::COLOR, 0, filament::VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, color), sizeof(Vertex)).build(engine_);
        indices_ = filament::IndexBuffer::Builder().indexCount(uint32_t(triangles.size()))
            .bufferType(filament::IndexBuffer::IndexType::USHORT).build(engine_);
        require(vertices_ && indices_, "Synthetic scene/UI geometry unavailable");
        vertices_->setBufferAt(engine_, 0, {vertices_cpu_.data(), sizeof(vertices_cpu_)});
        indices_->setBuffer(engine_, {triangles.data(), sizeof(triangles)});
        materials_[0] = filament::Material::Builder().package(k_heaven_blit_filamat, k_heaven_blit_filamat_size).build(engine_);
        materials_[1] = filament::Material::Builder().package(k_heaven_unlit_filamat, k_heaven_unlit_filamat_size).build(engine_);
        for (size_t item = 0; item < entities_.size(); ++item) {
            require(materials_[item], "Production scene/UI material unavailable");
            instances_[item] = materials_[item]->createInstance();
            require(instances_[item], "Production scene/UI instance unavailable");
            entities_[item] = utils::EntityManager::get().create();
            require(filament::RenderableManager::Builder(1)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vertices_, indices_, item * 6, 6)
                .material(0, instances_[item]).culling(false).castShadows(false).receiveShadows(false)
                .boundingBox({{0, 0, 0}, {1, 1, .01f}}).build(engine_, entities_[item]) ==
                    filament::RenderableManager::Builder::Success, "Synthetic scene/UI renderable unavailable");
        }
        instances_[1]->setParameter("hasTexture", false);
        instances_[1]->setParameter("baseColorMap", packed_, nearest());
        scene_->addEntity(entities_[0]); ui_scene_->addEntity(entities_[1]);
    }
    filament::RenderTarget* target(filament::Texture* texture) {
        auto* result = filament::RenderTarget::Builder().texture(filament::RenderTarget::AttachmentPoint::COLOR0, texture).build(engine_);
        require(result, "Distortion readback target unavailable"); return result;
    }
    void clear_read_targets() {
        for (auto*& target : read_targets_) { if (target) engine_.destroy(target); target = nullptr; }
    }
    static void complete(void* /*buffer*/, size_t /*bytes*/, void* user) { static_cast<std::atomic_bool*>(user)->store(true); }
    void read(filament::RenderTarget* target, void* data, size_t bytes, filament::Texture::Format format,
              filament::Texture::Type type, std::atomic_bool& done) {
        renderer_->readPixels(target, 0, 0, width_, height_, {data, bytes, format, type, complete, &done});
    }
    std::array<double, 4> scene_sample(Vec2 uv) const {
        const double x = std::clamp(uv[0] * width_ - .5, 0.0, double(width_ - 1));
        const double y = std::clamp(uv[1] * height_ - .5, 0.0, double(height_ - 1));
        const auto x0 = uint32_t(std::floor(x)), y0 = uint32_t(std::floor(y));
        const auto x1 = std::min(x0 + 1, width_ - 1), y1 = std::min(y0 + 1, height_ - 1);
        std::array<double, 4> result{};
        for (size_t channel = 0; channel < result.size(); ++channel) {
            const double top = std::lerp(double(source_pixels_[size_t(y0) * width_ + x0][channel]),
                double(source_pixels_[size_t(y0) * width_ + x1][channel]), x - x0);
            const double bottom = std::lerp(double(source_pixels_[size_t(y1) * width_ + x0][channel]),
                double(source_pixels_[size_t(y1) * width_ + x1][channel]), x - x0);
            result[channel] = std::lerp(top, bottom, y - y0);
        }
        return result;
    }
    void check_coarse_upload(const char* label,
            const std::array<Half4, static_cast<size_t>(kGrid) * kGrid>& readback) const {
        double max_error = 0;
        size_t mismatches = 0, nonfinite = 0;
        for (size_t node = 0; node < readback.size(); ++node) {
            for (size_t channel = 0; channel < readback[node].size(); ++channel) {
                const double actual = float(filament::math::makeHalf(readback[node][channel]));
                const double expected = coarse_nodes_[node][channel];
                if (!std::isfinite(actual)) ++nonfinite;
                const double error = std::abs(actual - expected);
                max_error = std::max(max_error, error);
                if (actual != expected) {
                    if (mismatches == 0)
                        std::fprintf(stderr, "%s %s first coarse upload mismatch node=(%zu,%zu) channel=%zu raw_half=0x%04x actual=%.12g expected=%.12g\n",
                            kTag, label, node % kGrid, node / kGrid, channel,
                            static_cast<unsigned>(readback[node][channel]), actual, expected);
                    ++mismatches;
                }
            }
        }
        std::printf("%s %s coarse upload nodes=%zu components=%zu max_error=%.12g mismatches=%zu nonfinite=%zu\n",
            kTag, label, readback.size(), readback.size() * readback.front().size(), max_error, mismatches, nonfinite);
        require(mismatches == 0 && nonfinite == 0, "Dyadic coarse nodes changed during native RGBA16F upload/readback");
    }
    void report_field_diagnostics(const char* label, const std::vector<Half4>& fields) const {
        size_t identity_zero = 0, nonfinite_pixels = 0, nonfinite_components = 0, sign_mismatch = 0, bounds = 0;
        double max_error = -1;
        size_t worst = 0;
        std::array<double, 4> minimum, maximum;
        minimum.fill(std::numeric_limits<double>::infinity());
        maximum.fill(-std::numeric_limits<double>::infinity());
        std::array<double, 4> worst_field{};
        Vec2 worst_expected{}, worst_residual{};
        const double bound = kFieldBound;
        const double stored_bound = half_storage_bound(bound, 0).upper;
        for (size_t index = 0; index < fields.size(); ++index) {
            std::array<double, 4> field{};
            bool finite = true;
            for (size_t channel = 0; channel < field.size(); ++channel) {
                field[channel] = float(filament::math::makeHalf(fields[index][channel]));
                if (!std::isfinite(field[channel])) { finite = false; ++nonfinite_components; }
                else {
                    minimum[channel] = std::min(minimum[channel], field[channel]);
                    maximum[channel] = std::max(maximum[channel], field[channel]);
                }
            }
            if (!finite) ++nonfinite_pixels;
            if (std::ranges::all_of(field, [](double value) { return value == 0; })) ++identity_zero;
            if (!(field[0] > 0 && field[1] < 0 && field[2] < 0 && field[3] > 0)) ++sign_mismatch;
            if (std::ranges::any_of(field, [stored_bound](double value) { return std::abs(value) > stored_bound; })) ++bounds;
            const Vec2 uv{(double(index % width_) + .5) / width_, (double(index / width_) + .5) / height_};
            const auto expected = clamped_field(uv);
            const double error = finite ? std::max(std::abs(field[0] - expected[0]), std::abs(field[1] - expected[1])) :
                std::numeric_limits<double>::infinity();
            if (error > max_error) {
                worst = index; max_error = error; worst_field = field; worst_expected = expected;
                if (finite) {
                    const Vec2 inverse{uv[0] + field[2], uv[1] + field[3]};
                    const auto offset = clamped_field(inverse);
                    worst_residual = {inverse[0] + offset[0] - uv[0], inverse[1] + offset[1] - uv[1]};
                } else worst_residual.fill(std::numeric_limits<double>::quiet_NaN());
            }
        }
        std::printf("%s %s field diagnostic identity_zero=%zu/%zu nonfinite_pixels=%zu nonfinite_components=%zu sign_mismatch=%zu bounds=%zu bound=%.12g min_RGBA=(%.12g,%.12g,%.12g,%.12g) max_RGBA=(%.12g,%.12g,%.12g,%.12g)\n",
            kTag, label, identity_zero, fields.size(), nonfinite_pixels, nonfinite_components, sign_mismatch, bounds, bound,
            minimum[0], minimum[1], minimum[2], minimum[3], maximum[0], maximum[1], maximum[2], maximum[3]);
        std::printf("%s %s MAX field mismatch output=(%zu,%zu) uv=(%.12g,%.12g) error=%.12g actual RGBA=(%.12g,%.12g,%.12g,%.12g) expected RG=(%.12g,%.12g) inverse residual=(%.12g,%.12g)\n",
            kTag, label, worst % width_, worst / width_, (double(worst % width_) + .5) / width_,
            (double(worst / width_) + .5) / height_, max_error, worst_field[0], worst_field[1], worst_field[2], worst_field[3],
            worst_expected[0], worst_expected[1], worst_residual[0], worst_residual[1]);
    }
    void check(const char* label, bool distorted, bool has_ui, const std::vector<Rgba8>& color,
            const std::vector<Rgba8>& real, const std::vector<Rgba8>& raw, const std::vector<float>& depths,
            const std::vector<Half2>& motions, const std::vector<Half4>& fields) const {
        double forward_error = 0, inverse_error = 0, residual_error = 0, raw_error = 0, color_error = 0, ui_error = 0;
        double forward_allowance = 0, inverse_allowance = 0, residual_allowance = 0;
        size_t interior = 0, signed_pixels = 0, changed_pixels = 0, ui_pixels = 0;
        size_t forward_violations = 0, inverse_violations = 0, residual_violations = 0;
        bool field_mismatch_reported = false;
        const double stored_bound = half_storage_bound(kFieldBound, 0).upper;
        for (uint32_t y = 0; y < height_; ++y) for (uint32_t x = 0; x < width_; ++x) {
            const size_t index = size_t(y) * width_ + x;
            const Vec2 uv{(double(x) + .5) / width_, (double(y) + .5) / height_};
            Vec2 forward{};
            if (distorted) {
                std::array<double, 4> field{};
                for (size_t channel = 0; channel < field.size(); ++channel) {
                    field[channel] = float(filament::math::makeHalf(fields[index][channel]));
                    require(std::isfinite(field[channel]), "Nonfinite signed distortion component");
                    require(std::abs(field[channel]) <= stored_bound, "Stored signed distortion exceeds the coarse-node bound");
                }
                forward = {field[0], field[1]};
                const auto expected = clamped_field(uv);
                const auto exact_inverse = inverse_clamped_field(uv);
                const Vec2 inverse{uv[0] + field[2], uv[1] + field[3]};
                const auto inverse_forward = clamped_field(inverse);
                double inverse_quantum = 0;
                bool forward_failed = false;
                for (size_t channel = 0; channel < forward.size(); ++channel) {
                    const auto forward_bound = half_storage_bound(expected[channel], kForwardFp32Allowance);
                    const double expected_inverse = exact_inverse[channel] - uv[channel];
                    const auto inverse_bound = half_storage_bound(expected_inverse, kInverseFp32Allowance);
                    forward_allowance = std::max(forward_allowance, forward_bound.error_from(expected[channel]));
                    inverse_allowance = std::max(inverse_allowance, inverse_bound.error_from(expected_inverse));
                    inverse_quantum = std::max(inverse_quantum, inverse_bound.quantum);
                    if (!forward_bound.accepts(forward[channel])) { ++forward_violations; forward_failed = true; }
                    if (!inverse_bound.accepts(field[channel + 2])) ++inverse_violations;
                    forward_error = std::max(forward_error, std::abs(forward[channel] - expected[channel]));
                }
                // The pre-storage shader residual is <2e-6. Moving its inverse
                // by a HALF storage error q changes p+field(p) by at most
                // (1+Lip)*q in L-infinity, with the production Lip bound 0.4.
                // The independent full-frame inverse bound above also limits
                // FP32 error to 2e-7; (1+0.4)*2e-7 is covered by the 2e-6 term.
                const double residual_bound = kPreStorageResidual + (1 + kContractionBound) * inverse_quantum;
                residual_allowance = std::max(residual_allowance, residual_bound);
                for (size_t channel = 0; channel < forward.size(); ++channel) {
                    const double residual = std::abs(inverse[channel] + inverse_forward[channel] - uv[channel]);
                    residual_error = std::max(residual_error, residual);
                    if (residual > residual_bound) ++residual_violations;
                }
                if (!field_mismatch_reported && forward_failed) {
                    std::fprintf(stderr, "%s %s first field mismatch output=(%u,%u) uv=(%.12g,%.12g) actual RGBA=(%.12g,%.12g,%.12g,%.12g) expected RG=(%.12g,%.12g) inverse residual=(%.12g,%.12g)\n",
                        kTag, label, x, y, uv[0], uv[1], field[0], field[1], field[2], field[3], expected[0], expected[1],
                        inverse[0] + inverse_forward[0] - uv[0], inverse[1] + inverse_forward[1] - uv[1]);
                    field_mismatch_reported = true;
                }
                if (uv[0] > .15 && uv[0] < .85 && uv[1] > .15 && uv[1] < .85) {
                    const auto analytic = inverse_affine(uv);
                    for (size_t channel = 0; channel < forward.size(); ++channel) {
                        inverse_error = std::max(inverse_error, std::abs(inverse[channel] - analytic[channel]));
                        require(half_storage_bound(analytic[channel] - uv[channel], kInverseFp32Allowance)
                            .accepts(field[channel + 2]), "Interior analytic inverse exceeds adjoining HALF plus FP32 bound");
                    }
                    ++interior;
                }
                if (field[0] > 0 && field[1] < 0 && field[2] < 0 && field[3] > 0) ++signed_pixels;
            }
            // Spatially varying SDK inputs catch accidental warping, unlike a
            // constant depth/vector which survives any erroneous resampling.
            const auto packed_x = std::min(uint32_t(uv[0] * kPackedExtent), kPackedExtent - 1);
            const auto packed_y = std::min(uint32_t(uv[1] * kPackedExtent), kPackedExtent - 1);
            const auto packed = packed_node(packed_x, packed_y);
            if (!std::isfinite(depths[index]) || std::abs(depths[index] - packed[2]) >= 1e-6f) {
                std::fprintf(stderr, "%s %s first SDK-input mismatch output=(%u,%u) uv=(%.12g,%.12g) source=(%u,%u) sample=(%.12g,%.12g) depth actual=%.12g expected=%.12g motion actual=(%.12g,%.12g) expected=(%.12g,%.12g)\n",
                    kTag, label, x, y, uv[0], uv[1], packed_x, packed_y,
                    uv[0] * kPackedExtent, uv[1] * kPackedExtent, depths[index], packed[2],
                    float(filament::math::makeHalf(motions[index][0])), float(filament::math::makeHalf(motions[index][1])),
                    packed[0], packed[1]);
            }
            require(std::isfinite(depths[index]) && std::abs(depths[index] - packed[2]) < 1e-6f,
                "Heat distortion changed SDK depth");
            for (size_t channel = 0; channel < kMotion.size(); ++channel)
                require(std::abs(float(filament::math::makeHalf(motions[index][channel])) - packed[channel]) < 1e-6f,
                    "Heat distortion changed signed SDK motion");
            const auto expected_color = scene_sample({uv[0] + forward[0], uv[1] + forward[1]});
            const bool ui = has_ui && uv[0] > .12 && uv[0] < .38 && uv[1] > .15 && uv[1] < .35;
            const bool ui_edge = std::min({std::abs(uv[0] - .12) * width_, std::abs(uv[0] - .38) * width_,
                std::abs(uv[1] - .15) * height_, std::abs(uv[1] - .35) * height_}) < 1.5;
            bool changed = false;
            for (size_t channel = 0; channel < expected_color.size(); ++channel) {
                raw_error = std::max(raw_error, std::abs(double(raw[index][channel]) - source_pixels_[index][channel]));
                color_error = std::max(color_error, std::abs(double(color[index][channel]) - expected_color[channel]));
                if (!ui_edge) {
                    const double expected_real = ui ? kUi[channel] * 255.0 : color[index][channel];
                    ui_error = std::max(ui_error, std::abs(double(real[index][channel]) - expected_real));
                }
                changed |= color[index][channel] != raw[index][channel];
            }
            if (changed) ++changed_pixels;
            if (ui && !ui_edge) ++ui_pixels;
        }
        std::printf("%s %s %ux%u forward=%.9g inverse=%.9g residual=%.9g raw=%.4g warp=%.4g ui=%.4g signed=%zu analytic=%zu changed=%zu ui_pixels=%zu\n",
            kTag, label, width_, height_, forward_error, inverse_error, residual_error,
            raw_error, color_error, ui_error, signed_pixels, interior, changed_pixels, ui_pixels);
        std::printf("%s %s per-component HALF max_allowance RG=%.12g BA=%.12g residual=%.12g violations RG=%zu BA=%zu residual=%zu FP32_RG=%.12g FP32_BA=%.12g\n",
            kTag, label, forward_allowance, inverse_allowance, residual_allowance,
            forward_violations, inverse_violations, residual_violations, kForwardFp32Allowance, kInverseFp32Allowance);
        require(forward_violations == 0, "Forward distortion exceeds independent adjoining HALF plus FP32 bounds");
        require(inverse_violations == 0 && residual_violations == 0,
            "BA inverse exceeds independent HALF storage or contraction residual bounds");
        require(raw_error <= 1.5, "Undistorted source target differs from independent color/marker image");
        require(color_error <= 2.5, "HUDless scene color does not follow stored forward distortion");
        require(ui_error <= 2.5, "UI moved, warped or leaked into HUDless scene");
        require(!has_ui || ui_pixels > 500, "Insufficient unwarped UI oracle coverage");
        require(!distorted || (interior > 1000 && signed_pixels > color.size() * 9 / 10 && changed_pixels > 1000),
            "Distortion oracle did not exercise signed field, analytic inverse and scene motion");
        require(distorted || changed_pixels == 0, "Off/resize path retained stale distortion");
    }

    filament::Engine& engine_;
    filament::Renderer* renderer_ = nullptr;
    filament::Camera* camera_ = nullptr;
    utils::Entity camera_entity_;
    filament::Scene* scene_ = nullptr;
    filament::Scene* ui_scene_ = nullptr;
    filament::View* scene_view_ = nullptr;
    filament::View* ui_view_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::VertexBuffer* vertices_ = nullptr;
    filament::IndexBuffer* indices_ = nullptr;
    std::array<filament::Material*, 2> materials_{};
    std::array<filament::MaterialInstance*, 2> instances_{};
    std::array<utils::Entity, 2> entities_{};
    std::array<Vertex, 8> vertices_cpu_{};
    std::array<std::array<float, 4>, static_cast<size_t>(kGrid) * kGrid> coarse_nodes_{};
    std::array<std::array<float, 4>, static_cast<size_t>(kPackedExtent) * kPackedExtent> packed_nodes_{};
    std::vector<Rgba8> source_pixels_;
    filament::Texture* source_ = nullptr;
    filament::Texture* packed_ = nullptr;
    filament::Texture* coarse_ = nullptr;
    std::array<filament::RenderTarget*, 5> read_targets_{};
    std::unique_ptr<FrameGenerationOutput> output_;
    uint32_t width_ = 0, height_ = 0;
};

void run(filament::Engine& engine, Fault fault) {
    Fixture fixture(engine);
    fixture.init(fault);
    fixture.capture("initial-off", false, true);
    fixture.capture("signed-affine-and-ui", true, true);
    fixture.capture("ui-cleared", true, false);
    fixture.capture("disabled-cleared", false, false);
    fixture.resize(192, 128);
    fixture.capture("resized-off", false, true);
    fixture.capture("resized-distortion", true, true);
    fixture.capture("resized-disabled", false, true);
    fixture.resize(320, 256);
    fixture.capture("restored-size-off", false, false);
}
} // namespace
} // namespace heaven

int main(int argc, char** argv) {
    heaven::Fault fault = heaven::Fault::None;
    bool cpu_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--negative-forward-sign") fault = heaven::Fault::ForwardSign;
        else if (argument == "--negative-swapped-channels") fault = heaven::Fault::SwappedChannels;
        else if (argument == "--cpu-only") cpu_only = true;
        else { std::fprintf(stderr, "Usage: %s [--cpu-only] [--negative-forward-sign|--negative-swapped-channels]\n", argv[0]); return 2; }
    }
    filament::Engine* engine = nullptr;
    int result = 0;
    try {
        heaven::check_cpu_oracles();
        if (cpu_only) return 0;
        engine = filament::Engine::create(filament::Engine::Backend::VULKAN);
        heaven::require(engine, "Vulkan Engine creation failed");
        heaven::run(*engine, fault);
        std::puts(fault == heaven::Fault::None ? "[distortion_field_test] PASS" :
            "[distortion_field_test] NEGATIVE CONTROL NOT DETECTED");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[distortion_field_test] FAIL: %s\n", error.what()); result = 1;
    }
    if (engine) filament::Engine::destroy(&engine);
    return result;
}
