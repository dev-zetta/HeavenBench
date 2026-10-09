/* HeavenBench — bounded GPU vertex-height sampling on the actual texture. */
#include "rhi/height_sample_batch.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>
#include <utils/compiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace heaven {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kMaximumWidth = 256;
constexpr std::uint32_t kMaximumChunk = 65'536;

struct SampleVertex {
    filament::math::float3 position;
    filament::math::float2 uv;
    filament::math::float4 custom;
};
static_assert(sizeof(SampleVertex) == 36);

template<class T> filament::backend::BufferDescriptor owned_upload(std::vector<T>&& values) {
    auto* storage = new std::vector<T>(std::move(values));
    return {storage->data(), storage->size() * sizeof(T),
        [](void*, std::size_t, void* user) { delete static_cast<std::vector<T>*>(user); }, storage};
}

struct Readback {
    explicit Readback(std::size_t floats)
        : pixels(floats, std::numeric_limits<float>::quiet_NaN()) {}
    std::vector<float> pixels;
    std::atomic_bool completed{false};
};

struct SampleFrame {
    explicit SampleFrame(filament::Renderer& value) : renderer(value) {}
    ~SampleFrame() { if (active) renderer.endFrame(); }
    void finish() { renderer.endFrame(); active = false; }
    filament::Renderer& renderer;
    bool active = true;
};

// Construct empty, then initialize through a unique_ptr. Even an exception or
// early failed build releases every resource acquired before the failure.
struct SampleJob {
    explicit SampleJob(filament::Engine& value) : engine(value) {}
    ~SampleJob() {
        if (view) { view->setRenderTarget(nullptr); view->setScene(nullptr); view->setCamera(nullptr); }
        if (entity) {
            if (scene) scene->remove(entity);
            engine.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        if (view) engine.destroy(view);
        if (scene) engine.destroy(scene);
        if (target) engine.destroy(target);
        if (output) engine.destroy(output);
        if (camera_entity) {
            engine.destroyCameraComponent(camera_entity);
            utils::EntityManager::get().destroy(camera_entity);
        }
        if (renderer) engine.destroy(renderer);
        if (swapchain) engine.destroy(swapchain);
        if (vertices) engine.destroy(vertices);
        if (indices) engine.destroy(indices);
        if (instance) engine.destroy(instance);
        // Queued destruction follows the submitted readback/draw commands.
        // A pending callback owns its CPU pixels separately, never this job.
    }

    Result<void> initialize(filament::Material& material, filament::Texture& height,
                           std::array<float, 4> transform, std::uint32_t capacity) {
        width = std::min(kMaximumWidth, capacity);
        rows = (capacity + width - 1) / width;
        instance = material.createInstance();
        if (!instance) return std::unexpected(Error{"Cannot create height sampling instance"});
        filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
        sampler.setAnisotropy(1);
        instance->setParameter("geometryHeightMap", &height, sampler);
        instance->setParameter("baseTransform", filament::math::float4{
            transform[0], transform[1], transform[2], transform[3]});
        renderer = engine.createRenderer();
        scene = engine.createScene();
        view = engine.createView();
        swapchain = engine.createSwapChain(width, rows);
        if (!renderer || !scene || !view || !swapchain)
            return std::unexpected(Error{"Cannot create height sampling view resources"});
        renderer->setDisplayInfo({.refreshRate = 0});
        renderer->setFrameRateOptions({.interval = 0});
        renderer->setClearOptions({.clearColor = {0, 0, 0, 0}, .clear = true, .discard = true});
        camera_entity = utils::EntityManager::get().create();
        auto* camera = engine.createCamera(camera_entity);
        if (!camera) return std::unexpected(Error{"Cannot create height sampling camera"});
        camera->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1, .1, 10);
        camera->lookAt({0, 0, 1}, {0, 0, 0}, {0, 1, 0});
        camera->setExposure(1);
        const auto usage = filament::Texture::Usage::COLOR_ATTACHMENT | filament::Texture::Usage::BLIT_SRC;
        output = filament::Texture::Builder().width(width).height(rows).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).usage(usage).build(engine);
        if (!output) return std::unexpected(Error{"Cannot allocate height sampling output"});
        target = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, output).build(engine);
        if (!target) return std::unexpected(Error{"Cannot create height sampling target"});
        view->setScene(scene);
        view->setCamera(camera);
        view->setRenderTarget(target);
        view->setViewport({0, 0, width, rows});
        view->setPostProcessingEnabled(false);
        view->setShadowingEnabled(false);
        view->setAntiAliasing(filament::AntiAliasing::NONE);
        view->setMultiSampleAntiAliasingOptions({.enabled = false});
        vertices = filament::VertexBuffer::Builder().vertexCount(capacity * 4).bufferCount(1)
            .attribute(filament::VertexAttribute::POSITION, 0,
                filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(SampleVertex))
            .attribute(filament::VertexAttribute::UV0, 0,
                filament::VertexBuffer::AttributeType::FLOAT2, offsetof(SampleVertex, uv), sizeof(SampleVertex))
            .attribute(filament::VertexAttribute::CUSTOM0, 0,
                filament::VertexBuffer::AttributeType::FLOAT4, offsetof(SampleVertex, custom), sizeof(SampleVertex)).build(engine);
        indices = filament::IndexBuffer::Builder().indexCount(capacity * 6)
            .bufferType(filament::IndexBuffer::IndexType::UINT).build(engine);
        if (!vertices || !indices) return std::unexpected(Error{"Cannot create height sampling buffers"});
        std::vector<std::uint32_t> data;
        data.reserve(static_cast<std::size_t>(capacity) * 6);
        for (std::uint32_t slot = 0; slot < capacity; ++slot) {
            const auto i = slot * 4;
            data.insert(data.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
        }
        indices->setBuffer(engine, owned_upload(std::move(data)));
        entity = utils::EntityManager::get().create();
        if (filament::RenderableManager::Builder(1).boundingBox({{0, 0, 0}, {1, 1, .1f}})
            .culling(false).geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vertices, indices)
            .material(0, instance).castShadows(false).receiveShadows(false).build(engine, entity)
            != filament::RenderableManager::Builder::Success)
            return std::unexpected(Error{"Cannot create height sampling renderable"});
        scene->addEntity(entity);
        return {};
    }

    filament::Engine& engine;
    filament::MaterialInstance* instance = nullptr;
    filament::Renderer* renderer = nullptr;
    filament::SwapChain* swapchain = nullptr;
    filament::Scene* scene = nullptr;
    filament::View* view = nullptr;
    filament::RenderTarget* target = nullptr;
    filament::Texture* output = nullptr;
    filament::VertexBuffer* vertices = nullptr;
    filament::IndexBuffer* indices = nullptr;
    utils::Entity camera_entity, entity;
    std::uint32_t width = 0, rows = 0;
};

float query_lod(const HeightSampleQuery& q, const std::array<float, 4>& transform,
                filament::Texture& texture) {
    // Keep the shader's multiplication order and float precision. This is a
    // eligibility/returned-LOD check, not a CPU height decoder.
    const float x = q.footprint[0] * std::abs(transform[0]) * float(texture.getWidth());
    const float y = q.footprint[1] * std::abs(transform[1]) * float(texture.getHeight());
    return std::log2(std::max({x, y, 1.0f}));
}
} // namespace

struct HeightSampleBatch::Impl {
    Impl(filament::Engine& value, HeightSampleBatchOptions options_value)
        : engine(value), options(options_value) {}
    ~Impl() { if (material) engine.destroy(material); }
    filament::Engine& engine;
    HeightSampleBatchOptions options;
    filament::Material* material = nullptr;
};

HeightSampleBatch::HeightSampleBatch(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HeightSampleBatch::~HeightSampleBatch() = default;

Result<std::unique_ptr<HeightSampleBatch>> HeightSampleBatch::create(
    filament::Engine& engine, std::span<const std::uint8_t> package, HeightSampleBatchOptions options) {
#if !UTILS_HAS_THREADING
    return std::unexpected(Error{"Bounded height readback requires the threaded Filament backend"});
#else
    if (package.empty() || options.max_queries_per_chunk == 0 ||
        options.max_queries_per_chunk > kMaximumChunk || options.max_total_queries == 0 ||
        options.completion_timeout.count() <= 0 || options.completion_timeout > std::chrono::seconds(60))
        return std::unexpected(Error{"Invalid height sampler package or bounded options"});
    if (!filament::Texture::isTextureFormatSupported(engine, filament::Texture::InternalFormat::RGBA32F))
        return std::unexpected(Error{"RGBA32F height readback is unsupported"});
    try {
        auto impl = std::make_unique<Impl>(engine, options);
        impl->material = filament::Material::Builder().package(package.data(), package.size()).build(engine);
        if (!impl->material || std::string_view(impl->material->getName()) != "heaven_height_sample")
            return std::unexpected(Error{"Missing heaven_height_sample material package"});
        return std::unique_ptr<HeightSampleBatch>(new HeightSampleBatch(std::move(impl)));
    } catch (const std::exception& e) {
        return std::unexpected(Error{std::string("Height sampler construction: ") + e.what()});
    }
#endif
}

Result<std::vector<HeightSampleValue>> HeightSampleBatch::sample(
    filament::Texture* height, std::span<const HeightSampleQuery> queries,
    std::array<float, 4> transform) {
#if !UTILS_HAS_THREADING
    return std::unexpected(Error{"Bounded height readback requires the threaded Filament backend"});
#else
    if (queries.empty()) return std::vector<HeightSampleValue>{};
    if (!height || height->getTarget() != filament::Texture::Sampler::SAMPLER_2D ||
        height->getWidth() == 0 || height->getHeight() == 0 || height->getLevels() == 0 ||
        queries.size() > impl_->options.max_total_queries || impl_->engine.isPaused() ||
        !std::ranges::all_of(transform, [](float x) { return std::isfinite(x); }))
        return std::unexpected(Error{"Invalid selected height texture, transform, request count or engine state"});
    try {
        std::unordered_set<std::uint64_t> unique_ids;
        unique_ids.reserve(queries.size());
        std::vector<float> expected_lod;
        expected_lod.reserve(queries.size());
        for (const auto& q : queries) {
            if (!unique_ids.insert(q.id).second ||
                !std::ranges::all_of(q.uv, [](float x) { return std::isfinite(x); }) ||
                !std::ranges::all_of(q.footprint, [](float x) { return std::isfinite(x) && x >= 0; }))
                return std::unexpected(Error{"Height requests require unique IDs, finite UVs and nonnegative footprints"});
            const float lod = query_lod(q, transform, *height);
            if (!std::isfinite(lod) || lod < 1.0f ||
                !std::isfinite(q.uv[0] * transform[0] + transform[2]) ||
                !std::isfinite(q.uv[1] * transform[1] + transform[3]))
                return std::unexpected(Error{"Height request is not an eligible static mesh-LOD>=1 vertex sample"});
            expected_lod.push_back(lod);
        }
        const auto deadline = Clock::now() + impl_->options.completion_timeout;
        auto job = std::make_unique<SampleJob>(impl_->engine);
        const auto capacity = static_cast<std::uint32_t>(std::min<std::size_t>(
            queries.size(), impl_->options.max_queries_per_chunk));
        if (auto init = job->initialize(*impl_->material, *height, transform, capacity); !init)
            return std::unexpected(init.error());
        std::vector<HeightSampleValue> output;
        output.reserve(queries.size());
        for (std::size_t first = 0; first < queries.size(); first += capacity) {
            if (Clock::now() >= deadline) return std::unexpected(Error{"Height sampling deadline expired"});
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(capacity, queries.size() - first));
            std::vector<SampleVertex> data;
            data.reserve(static_cast<std::size_t>(count) * 4);
            for (std::uint32_t slot = 0; slot < count; ++slot) {
                const auto& q = queries[first + slot];
                const auto x = slot % job->width, y = slot / job->width;
                const float left = -1 + 2 * float(x) / float(job->width), right = -1 + 2 * float(x + 1) / float(job->width);
                const float bottom = -1 + 2 * float(y) / float(job->rows), top = -1 + 2 * float(y + 1) / float(job->rows);
                for (auto pos : std::array{filament::math::float3{left, bottom, 0}, filament::math::float3{right, bottom, 0},
                                          filament::math::float3{right, top, 0}, filament::math::float3{left, top, 0}})
                    data.push_back({pos, {q.uv[0], q.uv[1]}, {0, q.footprint[0], q.footprint[1], float(slot + 1)}});
            }
            job->vertices->setBufferAt(impl_->engine, 0, owned_upload(std::move(data)));
            auto& manager = impl_->engine.getRenderableManager();
            const auto renderable = manager.getInstance(job->entity);
            manager.setGeometryAt(renderable, 0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                job->vertices, job->indices, 0, static_cast<std::size_t>(count) * 6);
            if (!job->renderer->beginFrame(job->swapchain))
                return std::unexpected(Error{"Height sampling frame was rejected"});
            SampleFrame frame(*job->renderer);
            job->renderer->render(job->view);
            auto readback = std::make_shared<Readback>(std::size_t(job->width) * job->rows * 4);
            auto* callback_owner = new std::shared_ptr<Readback>(readback);
            job->renderer->readPixels(job->target, 0, 0, job->width, job->rows,
                filament::backend::PixelBufferDescriptor(readback->pixels.data(), readback->pixels.size() * sizeof(float),
                    filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::FLOAT,
                    [](void*, std::size_t, void* user) {
                        auto* owner = static_cast<std::shared_ptr<Readback>*>(user);
                        (*owner)->completed.store(true, std::memory_order_release);
                        delete owner;
                    }, callback_owner));
            frame.finish();
            while (!readback->completed.load(std::memory_order_acquire)) {
                const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now());
                if (remaining.count() <= 0 || !impl_->engine.flushAndWait(static_cast<std::uint64_t>(remaining.count())))
                    return std::unexpected(Error{"Height readback did not complete before the deadline"});
                impl_->engine.pumpMessageQueues();
            }
            // IDs identify slots independent of Vulkan/OpenGL target orientation.
            std::vector<HeightSampleValue> chunk(count);
            std::vector<bool> seen(count, false);
            std::size_t padding = 0;
            for (std::size_t pixel = 0; pixel < readback->pixels.size(); pixel += 4) {
                const float h = readback->pixels[pixel], id = readback->pixels[pixel + 1];
                const float mesh = readback->pixels[pixel + 2], selected = readback->pixels[pixel + 3];
                if (!std::isfinite(h) || !std::isfinite(id) || !std::isfinite(mesh) || !std::isfinite(selected))
                    return std::unexpected(Error{"Nonfinite height readback"});
                if (id == 0) {
                    if (h != 0 || mesh != 0 || selected != 0)
                        return std::unexpected(Error{"Height sampling clear/padding was contaminated"});
                    ++padding;
                    continue;
                }
                if (id < 1 || id > float(count) || id != std::round(id))
                    return std::unexpected(Error{"Invalid height readback slot"});
                const auto slot = static_cast<std::uint32_t>(id) - 1;
                const float expected = expected_lod[first + slot];
                if (seen[slot] || h < 0 || h > 1 || mesh < 1 || selected != mesh ||
                    std::abs(mesh - expected) > 2e-5f * std::max(1.0f, std::abs(expected)))
                    return std::unexpected(Error{"Height readback coverage, scalar range or shader LOD disagrees"});
                seen[slot] = true;
                chunk[slot] = {queries[first + slot].id, h, mesh, selected};
            }
            if (std::ranges::count(seen, true) != count || padding != std::size_t(job->width) * job->rows - count)
                return std::unexpected(Error{"Incomplete height sampling coverage"});
            output.insert(output.end(), chunk.begin(), chunk.end());
        }
        return output;
    } catch (const std::exception& e) {
        return std::unexpected(Error{std::string("Height sampling failed: ") + e.what()});
    }
#endif
}
} // namespace heaven
