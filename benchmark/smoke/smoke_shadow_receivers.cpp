// Diagnose the distinction between public caster flags and actual moment-map
// submissions. Receiver promotion is reported as the current baseline policy;
// the independent off-camera blocker must still darken receiving geometry.
#include "smoke_cases.h"
#include "benchmark/microprofiler.h"
#include "engine/grass_cpu.h"
#include "rhi/filament_backend.h"
#include "rhi/grass_tangent_upload.h"
#include "rhi/material_instance_cache.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "rhi/mesh_store.h"
#include "rhi/profile_bridge.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Frustum.h>
#include <filament/IndexBuffer.h>
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
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_shadow_receivers]";
constexpr std::uint32_t kSize = 192;
using Pixels = std::vector<std::uint8_t>;
using Float3 = filament::math::float3;
constexpr Float3 kBlockerPosition{4, 0, 4};
constexpr Float3 kLightDirection{-1, 0, -1};

struct Geometry {
    filament::VertexBuffer* vertices = nullptr;
    filament::IndexBuffer* indices = nullptr;
    std::uint32_t index_count = 0;
    filament::Box bounds{};
};

struct PassWork {
    std::uint64_t passes = 0, commands = 0, primitives = 0, instances = 0;
};

struct Capture {
    Pixels pixels;
    PassWork shadow, color;
    std::int32_t camera_visible = -1;
};

struct CpuGeometry {
    std::vector<GrassVertex> vertices;
    std::vector<std::uint32_t> indices;
};

void append_quad(CpuGeometry& mesh, const std::array<Float3, 4>& positions, Float3 normal) {
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto p = positions[i];
        mesh.vertices.push_back({p.x, p.y, p.z, normal.x, normal.y, normal.z,
            uv[i][0], uv[i][1], 1, 1, 1, 1});
    }
    for (const auto local : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(first + local);
}

CpuGeometry receiving_geometry(bool overlapping) {
    CpuGeometry mesh;
    append_quad(mesh, {{{-1.5f, -1.5f, 0}, {1.5f, -1.5f, 0},
        {1.5f, 1.5f, 0}, {-1.5f, 1.5f, 0}}}, {0, 0, 1});
    if (overlapping) {
        // The camera sees this sloped card at x >= 0.2, while its shadow on
        // the lower card is at x = [-0.3, 0.1]. Those regions cannot be confused
        // with a color silhouette. Both cards belong to the same renderable.
        append_quad(mesh, {{{0.2f, -0.7f, 0.5f}, {1, -0.7f, 0.9f},
            {1, 0.7f, 0.9f}, {0.2f, 0.7f, 0.5f}}}, {-0.4472136f, 0, 0.8944272f});
    }
    return mesh;
}

CpuGeometry blocking_geometry() {
    CpuGeometry mesh;
    struct Face { Float3 normal, tangent, bitangent; };
    constexpr std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
        {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}
    }};
    for (const auto& face : faces) {
        append_quad(mesh, {{0.5f * (face.normal - face.tangent - face.bitangent),
            0.5f * (face.normal + face.tangent - face.bitangent),
            0.5f * (face.normal + face.tangent + face.bitangent),
            0.5f * (face.normal - face.tangent + face.bitangent)}}, face.normal);
    }
    return mesh;
}

class ReceiverFixture {
public:
    explicit ReceiverFixture(filament::Engine& engine)
        : engine_(engine), textures_(engine), materials_(engine, textures_),
          meshes_(engine),
          recorder_({.memory_bytes = std::size_t{8} * 1024 * 1024, .max_threads = 64}), bridge_(recorder_) {}
    ReceiverFixture(const ReceiverFixture&) = delete;
    ReceiverFixture& operator=(const ReceiverFixture&) = delete;
    ~ReceiverFixture() {
        // ProfileBridge borrows this recorder. Drain its driver callbacks while
        // both are alive, then destroy all renderable borrowers before owners.
        engine_.flushAndWait();
        recorder_.set_active(false);
        bridge_.update({});
        for (auto entity : {receiver_, blocker_}) {
            if (!entity) continue;
            if (scene_) scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        if (view_) {
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
            view_->setRenderTarget(nullptr);
        }
        if (light_) {
            if (scene_) scene_->remove(light_);
            engine_.destroy(light_);
            utils::EntityManager::get().destroy(light_);
        }
        engine_.destroy(view_);
        engine_.destroy(scene_);
        engine_.destroy(target_);
        engine_.destroy(depth_);
        engine_.destroy(color_);
        for (auto& geometry : grass_geometry_) {
            engine_.destroy(geometry.vertices);
            engine_.destroy(geometry.indices);
        }
        engine_.destroy(grass_instance_);
        engine_.destroy(grass_material_);
        engine_.destroy(grass_diffuse_);
        engine_.destroy(grass_variation_);
        if (camera_entity_) {
            engine_.destroyCameraComponent(camera_entity_);
            utils::EntityManager::get().destroy(camera_entity_);
        }
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
        engine_.flushAndWait();
    }

    void init() {
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) throw std::runtime_error("lit material load failed");
        materials_.set_colored_penumbra(false);
        lit_instance_ = materials_.get_or_create("shadow-receiver-fixture", "lit", GpuMaterial{});
        grass_material_ = filament::Material::Builder()
            .package(k_heaven_grass_filamat, k_heaven_grass_filamat_size).build(engine_);
        if (!lit_instance_ || !grass_material_) throw std::runtime_error("fixture materials unavailable");
        grass_instance_ = grass_material_->createInstance();
        if (!grass_instance_) throw std::runtime_error("grass material instance unavailable");
        // Opaque generated textures make every submitted masked triangle have
        // known coverage; no external asset or placeholder fallback is sampled.
        grass_diffuse_ = solid_texture({128, 180, 110, 255});
        grass_variation_ = solid_texture({128, 128, 128, 255});
        const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST);
        grass_instance_->setParameter("grassDiffuse", grass_diffuse_, sampler);
        grass_instance_->setParameter("grassVariation", grass_variation_, sampler);
        material_tuning::bind_grass_visible(*grass_instance_);
        grass_instance_->setParameter("variationScale", 0.05f);
        grass_instance_->setParameter("variationStrength", 0.0f);
        grass_instance_->setParameter("grassDiffuseUv", filament::math::float4{1, 1, 0, 0});
        grass_instance_->setParameter("time", 0.0f);
        // Freeze only this synthetic fixture's animation so shadow controls
        // compare identical card positions, while using the production shader.
        grass_instance_->setParameter("grassWindTuning", filament::math::float4{1, 1, 0, 0});
        grass_geometry_[0] = upload(receiving_geometry(false));
        grass_geometry_[1] = upload(receiving_geometry(true));
        geometry_[0] = upload_lit(receiving_geometry(false), "receiver-flat");
        geometry_[1] = upload_lit(receiving_geometry(true), "receiver-overlap");
        geometry_[2] = upload_lit(blocking_geometry(), "blocker");
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        if (!renderer_ || !swapchain_ || !scene_ || !view_ || !camera_) {
            throw std::runtime_error("fixture view creation failed");
        }
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setProjection(filament::Camera::Projection::ORTHO, -2, 2, -2, 2, 0.1, 100);
        camera_->lookAt({0, 0, 10}, {0, 0, 0}, {0, 1, 0});
        camera_->setExposure(16, 1.0f / 125, 100);
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        if (!color_ || !depth_) throw std::runtime_error("fixture capture texture creation failed");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        if (!target_) throw std::runtime_error("fixture capture target creation failed");
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        view_->setShadowingEnabled(true);
        light_ = utils::EntityManager::get().create();
        filament::LightManager::ShadowOptions shadow;
        shadow.mapSize = 1024;
        shadow.shadowCascades = 1;
        shadow.screenSpaceContactShadows = false;
        shadow.stable = true;
        shadow.lispsm = false;
        shadow.normalBias = 0;
        shadow.constantBias = 0;
        // A DIRECTIONAL light's default PCSS bulb radius is 1 radian. At the
        // blocker distance that erases this small cube's umbra. Use an explicit
        // finite angular source, approximately a 0.57-degree Sun, preserving a
        // soft edge and an independently predicted interior shadow region.
        shadow.shadowBulbRadius = 0.01f;
        if (filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
                .color({1, 1, 1}).intensity(50000).direction(kLightDirection)
                .castShadows(true).shadowOptions(shadow).build(engine_, light_) !=
                filament::LightManager::Builder::Success) {
            throw std::runtime_error("fixture light creation failed");
        }
        scene_->addEntity(light_);
        receiver_ = utils::EntityManager::get().create();
        blocker_ = utils::EntityManager::get().create();
        build_renderable(receiver_, geometry_[0], lit_instance_, false, true);
        auto blocker_geometry = geometry_[2];
        // Upstream far_uses_shadowcasters clips the fitting volume to caster
        // depth before intersecting the camera. A tight, entirely off-camera
        // cube can therefore omit the whole PCF map even when its shadow hits
        // the visible receiver. Keep this policy fixture independent of that
        // fitting limitation: extend only its conservative box to a support
        // point at the receiver origin's light depth. Fix x/y so the complete
        // expanded box remains outside the camera; actual vertices stay put.
        // Tight-box per-cascade culling has its own separate fixture.
        constexpr Float3 receiver_reference{0, 0, 0};
        Float3 support_world = kBlockerPosition;
        support_world.z += dot(kLightDirection, receiver_reference - support_world) / kLightDirection.z;
        const Float3 support_local = support_world - kBlockerPosition;
        const Float3 blocker_min = min(blocker_geometry.bounds.center - blocker_geometry.bounds.halfExtent,
            support_local);
        const Float3 blocker_max = max(blocker_geometry.bounds.center + blocker_geometry.bounds.halfExtent,
            support_local);
        blocker_geometry.bounds = {(blocker_min + blocker_max) * 0.5f, (blocker_max - blocker_min) * 0.5f};
        build_renderable(blocker_, blocker_geometry, lit_instance_, true, false);
        auto& transforms = engine_.getTransformManager();
        transforms.create(receiver_);
        transforms.create(blocker_);
        transforms.setTransform(transforms.getInstance(blocker_),
            filament::math::mat4f::translation(kBlockerPosition));
        scene_->addEntity(receiver_);
        if (camera_->getFrustum().intersects(filament::Box{
                blocker_geometry.bounds.center + kBlockerPosition, blocker_geometry.bounds.halfExtent})) {
            throw std::runtime_error("complete conservative blocker box intersects camera frustum");
        }
    }

    void configure(filament::ShadowType type, bool overlapping, bool grass) {
        view_->setShadowType(type);
        auto& renderables = engine_.getRenderableManager();
        const auto receiver = renderables.getInstance(receiver_);
        const auto index = overlapping ? 1u : 0u;
        const auto& geometry = grass ? grass_geometry_[index] : geometry_[index];
        renderables.setGeometryAt(receiver, 0, filament::RenderableManager::PrimitiveType::TRIANGLES,
            geometry.vertices, geometry.indices, 0, geometry.index_count);
        renderables.setAxisAlignedBoundingBox(receiver, geometry.bounds);
        renderables.setMaterialInstanceAt(receiver, 0, grass ? grass_instance_ : lit_instance_);
    }

    Capture capture(bool cast, bool receive, bool blocker_present) {
        auto& renderables = engine_.getRenderableManager();
        const auto receiver = renderables.getInstance(receiver_);
        renderables.setCastShadows(receiver, cast);
        renderables.setReceiveShadows(receiver, receive);
        if (renderables.isShadowCaster(receiver) != cast || renderables.isShadowReceiver(receiver) != receive) {
            throw std::runtime_error("public caster/receiver flag readback mismatch");
        }
        if (blocker_present) scene_->addEntity(blocker_);
        else scene_->remove(blocker_);
        // Capture exactly one accepted native frame after shader/map warmup.
        // No timing estimate or whole-frame debug counter substitutes for the
        // actual named shadow geometry pass's completed workload.
        recorder_.set_active(false);
        bridge_.update({});
        for (int warmup = 0; warmup < 3; ++warmup) draw();
        recorder_.begin_capture();
        const profile::Context context{++attempt_, attempt_, 0, 0, 0, recorder_.session_id()};
        bridge_.update(context);
        renderer_->setProfilerContext(utils::profiling::getContext());
        draw();
        const auto snapshot = recorder_.snapshot();
        recorder_.set_active(false);
        bridge_.update({});
        Capture result;
        bool valid_samples = snapshot.dropped_events == 0 && snapshot.overwritten_events == 0;
        for (const auto& sample : snapshot.gpu_samples) {
            if (sample.name != "Shadow Pass" && sample.name != "Color Pass") continue;
            valid_samples &= sample.context.capture_id == recorder_.session_id() &&
                sample.context.attempt_id == attempt_ && sample.context.submission_id == attempt_ &&
                sample.context.renderer_id != 0 && sample.context.filament_frame_id != 0 &&
                sample.context.view_id == reinterpret_cast<std::uintptr_t>(view_) && sample.pass_id != 0 &&
                sample.status != profile::SampleStatus::Dropped && sample.status != profile::SampleStatus::Error;
            auto& work = sample.name == "Shadow Pass" ? result.shadow : result.color;
            ++work.passes;
            work.commands += sample.command_count;
            work.primitives += sample.primitive_count;
            work.instances += sample.draw_instance_invocations;
        }
        if (!valid_samples || result.color.passes != 1 || result.shadow.passes > 1) {
            throw std::runtime_error("one-frame named workload observations missing or ambiguous");
        }
        result.camera_visible = view_->getVisibleRenderableCount();
        result.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
        std::atomic_bool complete{false};
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(result.pixels.data(), result.pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        engine_.flushAndWait();
        if (!complete.load()) throw std::runtime_error("fixture pixel readback incomplete");
        return result;
    }

private:
    Geometry upload_lit(const CpuGeometry& input, const char* name) {
        Mesh mesh;
        mesh.name = mesh.path = name;
        Surface surface;
        surface.name = name;
        surface.indices = input.indices;
        for (const auto& source : input.vertices) {
            Vertex vertex{};
            vertex.px = source.px;
            vertex.py = source.py;
            vertex.pz = source.pz;
            vertex.nx = source.nx;
            vertex.ny = source.ny;
            vertex.nz = source.nz;
            vertex.u = vertex.u1 = source.u;
            vertex.v = vertex.v1 = source.v;
            surface.vertices.push_back(vertex);
        }
        mesh.surfaces.push_back(std::move(surface));
        // MeshStore supplies the UV1 and all displacement carrier attributes
        // required by the actual heaven_lit package, even with displacement off.
        auto uploaded = meshes_.upload(mesh);
        if (uploaded.surfaces.size() != 1) throw std::runtime_error("lit fixture mesh upload failed");
        const auto& gpu_surface = uploaded.surfaces.front();
        const auto buffers = meshes_.find(gpu_surface.handle);
        if (!buffers.vb || !buffers.ib) throw std::runtime_error("lit fixture buffer lookup failed");
        const auto lo = gpu_surface.bbox_min;
        const auto hi = gpu_surface.bbox_max;
        return {buffers.vb, buffers.ib, gpu_surface.index_count,
            {{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f},
             {(hi.x - lo.x) * 0.5f + 0.001f, (hi.y - lo.y) * 0.5f + 0.001f,
              (hi.z - lo.z) * 0.5f + 0.001f}}};
    }

    filament::Texture* solid_texture(std::array<std::uint8_t, 4> rgba) {
        auto* texture = filament::Texture::Builder().width(1).height(1).levels(1)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
        if (!texture) throw std::runtime_error("generated texture creation failed");
        auto pixels = std::make_unique<std::uint8_t[]>(rgba.size());
        std::ranges::copy(rgba, pixels.get());
        texture->setImage(engine_, 0, filament::backend::PixelBufferDescriptor(
            pixels.release(), rgba.size(), filament::backend::PixelDataFormat::RGBA,
            filament::backend::PixelDataType::UBYTE,
            [](void* buffer, std::size_t, void*) { delete[] static_cast<std::uint8_t*>(buffer); }));
        return texture;
    }

    Geometry upload(const CpuGeometry& mesh) {
        Geometry result;
        result.vertices = filament::VertexBuffer::Builder()
            .vertexCount(static_cast<std::uint32_t>(mesh.vertices.size())).bufferCount(2)
            .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3,
                offsetof(GrassVertex, px), sizeof(GrassVertex))
            .attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2,
                offsetof(GrassVertex, u), sizeof(GrassVertex))
            .attribute(filament::VertexAttribute::COLOR, 0, filament::VertexBuffer::AttributeType::FLOAT4,
                offsetof(GrassVertex, r), sizeof(GrassVertex))
            .attribute(filament::VertexAttribute::TANGENTS, 1, filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
            .normalized(filament::VertexAttribute::TANGENTS).build(engine_);
        result.indices = filament::IndexBuffer::Builder()
            .indexCount(static_cast<std::uint32_t>(mesh.indices.size()))
            .bufferType(filament::IndexBuffer::IndexType::UINT).build(engine_);
        if (!result.vertices || !result.indices) {
            engine_.destroy(result.vertices);
            engine_.destroy(result.indices);
            throw std::runtime_error("fixture geometry upload failed");
        }
        auto vertices = std::make_unique<GrassVertex[]>(mesh.vertices.size());
        std::ranges::copy(mesh.vertices, vertices.get());
        result.vertices->setBufferAt(engine_, 0, filament::VertexBuffer::BufferDescriptor(
            vertices.release(), mesh.vertices.size() * sizeof(GrassVertex),
            [](void* buffer, std::size_t, void*) { delete[] static_cast<GrassVertex*>(buffer); }));
        upload_grass_tangent_frames(engine_, *result.vertices, mesh.vertices);
        auto indices = std::make_unique<std::uint32_t[]>(mesh.indices.size());
        std::ranges::copy(mesh.indices, indices.get());
        result.indices->setBuffer(engine_, filament::IndexBuffer::BufferDescriptor(
            indices.release(), mesh.indices.size() * sizeof(std::uint32_t),
            [](void* buffer, std::size_t, void*) { delete[] static_cast<std::uint32_t*>(buffer); }));
        Float3 minimum{mesh.vertices.front().px, mesh.vertices.front().py, mesh.vertices.front().pz};
        Float3 maximum = minimum;
        for (const auto& vertex : mesh.vertices) {
            minimum = min(minimum, Float3{vertex.px, vertex.py, vertex.pz});
            maximum = max(maximum, Float3{vertex.px, vertex.py, vertex.pz});
        }
        // A zero-thickness receiver volume makes directional scene intersection
        // empty before any raster pass. Enclose the unchanged plane by 1 mm.
        result.bounds = {(minimum + maximum) * 0.5f,
            (maximum - minimum) * 0.5f + Float3{0.001f}};
        result.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        return result;
    }

    void build_renderable(utils::Entity entity, const Geometry& geometry,
            filament::MaterialInstance* material, bool cast, bool receive) {
        if (filament::RenderableManager::Builder(1)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                    geometry.vertices, geometry.indices, 0, geometry.index_count)
                .material(0, material).boundingBox(geometry.bounds).culling(true)
                .castShadows(cast).receiveShadows(receive).build(engine_, entity) !=
                filament::RenderableManager::Builder::Success) {
            throw std::runtime_error("fixture renderable creation failed");
        }
    }

    void draw() {
        for (int retry = 0; retry < 12; ++retry) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_);
            renderer_->endFrame();
            engine_.flushAndWait();
            return;
        }
        throw std::runtime_error("fixture native frame repeatedly rejected");
    }

    filament::Engine& engine_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    profile::Recorder recorder_;
    ProfileBridge bridge_;
    std::array<Geometry, 3> geometry_{};
    std::array<Geometry, 2> grass_geometry_{};
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::Texture *color_ = nullptr, *depth_ = nullptr;
    filament::Material* grass_material_ = nullptr;
    filament::MaterialInstance *grass_instance_ = nullptr, *lit_instance_ = nullptr;
    filament::Texture *grass_diffuse_ = nullptr, *grass_variation_ = nullptr;
    utils::Entity camera_entity_, light_, receiver_, blocker_;
    std::uint64_t attempt_ = 0;
};

std::size_t darker_pixels(const Pixels& unshadowed, const Pixels& shadowed,
        float left, float right, float bottom, float top) {
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        const float world_y = (static_cast<float>(y) + 0.5f) * 4 / static_cast<float>(kSize) - 2;
        if (world_y < bottom || world_y > top) continue;
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float world_x = (static_cast<float>(x) + 0.5f) * 4 / static_cast<float>(kSize) - 2;
            if (world_x < left || world_x > right) continue;
            const auto i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            if (static_cast<int>(unshadowed[i]) > static_cast<int>(shadowed[i]) + 8) ++count;
        }
    }
    return count;
}

bool save_capture(const std::string& name, const Pixels& pixels) {
    const char* output = std::getenv("HEAVEN_SMOKE_SHADOW_RECEIVERS_OUTPUT_DIR");
    if (!output || !*output) return true;
    const auto path = std::filesystem::path(output) / (name + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    return write_screenshot_ppm(path.string(), kSize, kSize, pixels).has_value();
}

bool check_receiver(ReceiverFixture& fixture, filament::ShadowType type,
        const char* shadow_name, bool overlapping, bool grass) {
    fixture.configure(type, overlapping, grass);
    const auto implicit_self = fixture.capture(false, true, false);
    const auto explicit_self = fixture.capture(true, true, false);
    const auto external = fixture.capture(false, true, true);
    const auto nonreceiving = fixture.capture(false, false, true);
    const auto explicit_external = fixture.capture(true, true, true);
    const std::uint64_t receiver_triangles = overlapping ? 4 : 2;
    bool ok = true;
    for (const auto* capture : {&implicit_self, &explicit_self, &external, &nonreceiving, &explicit_external}) {
        ok &= capture->camera_visible == 1 && capture->color.commands > 0 &&
            capture->color.primitives == receiver_triangles && capture->color.instances == 1;
    }
    // With reception disabled on the only camera-visible object, Filament may
    // omit the entire map. If it retains one, only the 12-triangle blocker can
    // contribute. The explicit controls isolate both known geometry counts.
    ok &= ((nonreceiving.shadow.primitives == 0 && nonreceiving.shadow.instances == 0) ||
        (nonreceiving.shadow.primitives == 12 && nonreceiving.shadow.instances == 1)) &&
        explicit_self.shadow.primitives == receiver_triangles && explicit_self.shadow.instances == 1 &&
        explicit_external.shadow.primitives == receiver_triangles + 12 && explicit_external.shadow.instances == 2;
    const auto external_delta = darker_pixels(implicit_self.pixels, external.pixels, -0.8f, -0.4f, -0.25f, 0.25f);
    const auto received = darker_pixels(nonreceiving.pixels, external.pixels, -0.8f, -0.4f, -0.25f, 0.25f);
    const auto implicit_dark = darker_pixels(nonreceiving.pixels, implicit_self.pixels, -0.27f, 0.07f, -0.5f, 0.5f);
    const auto explicit_dark = darker_pixels(nonreceiving.pixels, explicit_self.pixels, -0.27f, 0.07f, -0.5f, 0.5f);
    ok &= external_delta > 32 && received > 32;
    // PCF honors receiver-only flags; unlike moment-map modes, it needs no
    // implicit self coverage. VSM/PCSS promotion and self-darkening are recorded
    // diagnostically rather than mistaken for a feature-exclusion regression.
    if (type == filament::ShadowType::PCF) {
        ok &= implicit_self.shadow.primitives == 0 && external.shadow.primitives == 12 && implicit_dark == 0;
    }
    if (overlapping) ok &= explicit_dark > 16;
    const std::string name = std::string(shadow_name) + (overlapping ? "-overlap" : "-flat") +
        (grass ? "-grass" : "-lit");
    std::printf("%s %s shadow primitives implicit/self/external/nonreceiving/explicit=%llu/%llu/%llu/%llu/%llu; "
        "commands=%llu/%llu/%llu/%llu/%llu; instances=%llu/%llu/%llu/%llu/%llu; "
        "external-new/received pixels=%zu/%zu; implicit/explicit self pixels=%zu/%zu: %s\n", kTag, name.c_str(),
        static_cast<unsigned long long>(implicit_self.shadow.primitives), static_cast<unsigned long long>(explicit_self.shadow.primitives),
        static_cast<unsigned long long>(external.shadow.primitives), static_cast<unsigned long long>(nonreceiving.shadow.primitives),
        static_cast<unsigned long long>(explicit_external.shadow.primitives),
        static_cast<unsigned long long>(implicit_self.shadow.commands), static_cast<unsigned long long>(explicit_self.shadow.commands),
        static_cast<unsigned long long>(external.shadow.commands), static_cast<unsigned long long>(nonreceiving.shadow.commands),
        static_cast<unsigned long long>(explicit_external.shadow.commands),
        static_cast<unsigned long long>(implicit_self.shadow.instances), static_cast<unsigned long long>(explicit_self.shadow.instances),
        static_cast<unsigned long long>(external.shadow.instances), static_cast<unsigned long long>(nonreceiving.shadow.instances),
        static_cast<unsigned long long>(explicit_external.shadow.instances), external_delta, received, implicit_dark, explicit_dark,
        ok ? "PASS" : "FAIL");
    ok &= save_capture(name + "-implicit", implicit_self.pixels) && save_capture(name + "-self", explicit_self.pixels) &&
        save_capture(name + "-external", external.pixels) && save_capture(name + "-nonreceiving", nonreceiving.pixels) &&
        save_capture(name + "-explicit", explicit_external.pixels);
    return ok;
}
} // namespace

CaseResult run_shadow_receivers_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    const auto counts = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
            engine->getViewCount(), engine->getTextureCount(), engine->getMaterialCount(),
            engine->getSceneCount(), engine->getRenderTargetCount(), engine->getSwapChainCount()};
    };
    bool ok = true;
    try {
        {
            // Moment-map mip passes lazily create Engine-owned post-process
            // Materials, which persist until Engine teardown. Prime those
            // shared owners in a fully destroyed isolated fixture before
            // measuring our fixture's strict eight-resource lifetime baseline.
            ReceiverFixture warmup(*engine);
            warmup.init();
            for (const auto type : {filament::ShadowType::VSM, filament::ShadowType::PCSS}) {
                warmup.configure(type, false, false);
                (void)warmup.capture(true, true, true);
            }
        }
        engine->flushAndWait();
        const auto before = counts();
        {
            ReceiverFixture fixture(*engine);
            fixture.init();
            constexpr std::array types{filament::ShadowType::PCF, filament::ShadowType::VSM, filament::ShadowType::PCSS};
            constexpr std::array names{"pcf", "vsm", "pcss"};
            for (std::size_t i = 0; i < types.size(); ++i) {
                for (const bool overlap : {false, true}) {
                    for (const bool grass : {false, true}) ok &= check_receiver(fixture, types[i], names[i], overlap, grass);
                }
            }
        }
        engine->flushAndWait();
        const auto after = counts();
        const auto print_counts = [](const char* label, const auto& owners) {
            std::printf("%s owners %s (VB,IB,View,Texture,Material,Scene,RT,SwapChain)=", kTag, label);
            std::apply([](const auto... values) { ((std::printf("%zu,", values)), ...); }, owners);
            std::printf("\n");
        };
        print_counts("before", before);
        print_counts("after", after);
        ok &= before == after;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        return CaseResult::Fail;
    }
    std::printf("%s %s: actual PCF/VSM/PCSS submissions, independent reception and overlapping-card controls, owner teardown\n",
        kTag, ok ? "PASS" : "FAIL");
    return ok ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
