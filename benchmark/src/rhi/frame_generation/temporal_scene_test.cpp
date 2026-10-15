/* Native temporal-input oracle. Requires a working Vulkan device, no assets. */
#include "rhi/frame_generation/temporal_scene.h"
#include "rhi/frame_generation/temporal_camera.h"
#include "rhi/frame_generation/temporal_grass.h"
#include "rhi/frame_generation/temporal_grass_pixel_oracle.h"
#include "rhi/frame_generation/frame_generation_output.h"
#include "rhi/filament_util.h"
#include "rhi/grass_tangent_upload.h"
#include "rhi/materials_data.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"
#include "engine/camera.h"
#include "engine/skinned_mesh.h"

#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Frustum.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/Material.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/View.h>
#include <filament/VertexBuffer.h>
#include <backend/PixelBufferDescriptor.h>
#include <backend/VulkanCommandContext.h>
#include <math/half.h>
#include <utils/EntityManager.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven {
namespace {
constexpr uint32_t kExtent = 192;
constexpr glm::vec4 kCenter{4.0f, 0.0f, 0.0f, 1.0f};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void verify_culling_projection_contract() {
    const auto finite = glm::perspective(glm::radians(60.0f),1.0f,.5f,2000.0f);
    const auto infinite = glm::infinitePerspective(glm::radians(60.0f),1.0f,.5f);
    const auto valid_planes = [](const glm::mat4& projection) {
        const filament::Frustum frustum(glm_to_filament(projection));
        std::array<filament::math::float4,6> planes{};
        frustum.getNormalizedPlanes(planes.data());
        bool valid = true;
        for (const auto& plane : planes)
            for (int component = 0; component < 4; ++component)
                valid &= std::isfinite(plane[component]);
        return valid;
    };
    const filament::Frustum finite_frustum(glm_to_filament(finite));
    require(valid_planes(finite),"Finite culling projection has nonfinite planes");
    require(finite_frustum.intersects(filament::Box{{0,0,-4},{1,1,1}}),
        "Finite culling projection rejects the visible reference box");
    // Negative control catches passing the infinite rendering projection to
    // the finite-frustum API, independent of platform-specific NaN sign bits.
    require(!valid_planes(infinite),"Infinite culling negative control unexpectedly has finite planes");
    std::puts("[temporal_scene_test] finite-culling-contract PASS; infinite-projection negative control produces nonfinite planes");
}

Mesh plane() {
    Mesh mesh;
    mesh.name = "temporal-oracle";
    Surface surface;
    surface.name = "plane";
    constexpr std::array positions{glm::vec3{4,-1,-1}, glm::vec3{4,1,-1},
        glm::vec3{4,1,1}, glm::vec3{4,-1,1}};
    constexpr std::array uvs{glm::vec2{0,0}, glm::vec2{1,0}, glm::vec2{1,1}, glm::vec2{0,1}};
    for (size_t i = 0; i < positions.size(); ++i) {
        Vertex v{};
        v.px = positions[i].x; v.py = positions[i].y; v.pz = positions[i].z;
        v.nx = -1.0f; v.u = uvs[i].x; v.v = uvs[i].y;
        v.tx = 0; v.ty = 1; v.tz = 0; v.tw = 1;
        surface.vertices.push_back(v);
    }
    surface.indices = {0,2,1,0,3,2};
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

SkinnedMesh skin(const Mesh& source) {
    SkinnedMesh mesh;
    mesh.name = "temporal-bone-oracle";
    mesh.bones.push_back({"one-rigid-bone", -1});
    SkinnedSurface surface;
    surface.name = "plane";
    surface.indices = source.surfaces.front().indices;
    for (const auto& v : source.surfaces.front().vertices) {
        SkinnedVertex sv{};
        sv.px = v.px; sv.py = v.py; sv.pz = v.pz; sv.nx = v.nx;
        sv.u = v.u; sv.v = v.v;
        sv.bone_weights[0] = 1.0f;
        surface.vertices.push_back(sv);
    }
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

struct Projection {
    glm::vec2 uv;
    float depth;
};
// Independent CPU projection of a known planar reference point. Every
// exercised motion is parallel to this plane, so its exact projected motion
// and depth are constant over the interior; no shader metadata is decoded.
Projection project(const glm::mat4& clip, const glm::mat4& model,
                   const glm::vec4& local = kCenter) {
    const auto p = clip * model * local;
    require(p.w > 0.0f, "Oracle point lies behind the camera");
    return {{p.x / p.w * .5f + .5f, -p.y / p.w * .5f + .5f},
        .5f - .5f * p.z / p.w};
}

struct NativeWrite {
    glm::vec4 color{};
    bool discard = false;
    std::atomic_bool called{false};
    std::atomic_bool supported{false};
};

void native_write(void* user, const void* native_context) noexcept {
    auto& probe = *static_cast<NativeWrite*>(user);
    if (native_context) {
        const auto& context = *static_cast<const filament::backend::VulkanCommandContext*>(native_context);
        const auto& image = context.images[0];
        if (context.discard && context.commandBuffer && image.image &&
            image.format == VK_FORMAT_R32G32B32A32_SFLOAT &&
            (image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && image.layout == VK_IMAGE_LAYOUT_GENERAL) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = barrier.newLayout = image.layout;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            bluevk::vkCmdPipelineBarrier(context.commandBuffer,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            const VkClearColorValue color{{probe.color.x,probe.color.y,probe.color.z,probe.color.w}};
            bluevk::vkCmdClearColorImage(context.commandBuffer,image.image,image.layout,&color,1,
                &barrier.subresourceRange);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            bluevk::vkCmdPipelineBarrier(context.commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            // Deliberately fail only after recording a real image mutation.
            // A discarded secondary must leave the input image unchanged.
            *context.discard = probe.discard;
            probe.supported.store(true);
        }
    }
    probe.called.store(true);
}

class Fixture {
public:
    ~Fixture() {
        if (!engine_) return;
        engine_->flushAndWait();
        destroy_read_targets();
        destroy_grass();
        if (native_texture_) engine_->destroy(native_texture_);
        output_.reset();
        temporal_.reset();
        meshes_.reset();
        textures_.reset();
        if (motion_) engine_->destroy(motion_);
        if (masked_) engine_->destroy(masked_);
        if (grass_motion_) engine_->destroy(grass_motion_);
        if (core_.scene) engine_->destroy(core_.scene);
        if (core_.camera) {
            engine_->destroyCameraComponent(camera_entity_);
            utils::EntityManager::get().destroy(camera_entity_);
        }
        if (core_.renderer) engine_->destroy(core_.renderer);
        if (swapchain_) engine_->destroy(swapchain_);
        filament::Engine::destroy(&engine_);
    }

    void init() {
        engine_ = filament::Engine::create(filament::Engine::Backend::VULKAN);
        require(engine_, "Vulkan Engine creation failed");
        core_.engine = engine_;
        core_.renderer = engine_->createRenderer();
        core_.scene = engine_->createScene();
        camera_entity_ = utils::EntityManager::get().create();
        core_.camera = engine_->createCamera(camera_entity_);
        swapchain_ = engine_->createSwapChain(kExtent, kExtent);
        require(core_.renderer && core_.scene && core_.camera && swapchain_, "Headless core creation failed");
        core_.renderer->setDisplayInfo({.refreshRate = 0.0f});
        core_.renderer->setFrameRateOptions({.interval = 0});
        projection_ = glm::infinitePerspective(glm::radians(60.0f), 1.0f, .5f);
        const auto culling = glm::perspective(glm::radians(60.0f),1.0f,.5f,2000.0f);
        core_.camera->setCustomProjection(filament::math::mat4(glm_to_filament(projection_)),
            filament::math::mat4(glm_to_filament(culling)),.5,2000);
        verify_projection_conversion();
        camera.position = {0,0,0}; camera.yaw = 0; camera.pitch = 0; camera.fov = 60;
        meshes_ = std::make_unique<MeshStore>(*engine_);
        textures_ = std::make_unique<TextureStore>(*engine_);
        textures_->create_placeholders();
        motion_ = filament::Material::Builder().package(k_heaven_motion_filamat,
            k_heaven_motion_filamat_size).build(*engine_);
        masked_ = filament::Material::Builder().package(k_heaven_motion_masked_filamat,
            k_heaven_motion_masked_filamat_size).build(*engine_);
        require(motion_ && masked_, "Motion packages unavailable");
        config_.tessellation.enabled = false;
        config_.features.gpu_skinning = true;
    }

    void setup(bool skinned = false, bool masked = false) {
        engine_->flushAndWait();
        destroy_read_targets();
        destroy_grass();
        output_.reset();
        temporal_.reset();
        camera.position = {0,0,0}; camera.yaw = 0; camera.pitch = 0;
        source_ = plane();
        auto uploaded = skinned ? meshes_->upload_skinned(skin(source_)) : meshes_->upload(source_);
        require(uploaded.surfaces.size() == 1, "Oracle mesh upload failed");
        // A planar mesh has zero X extent; retain its exact placement in the
        // proxy's conservative bounds rather than depending on volume guards.
        uploaded.bbox_min = {3.99f,-1,-1}; uploaded.bbox_max = {4.01f,1,1};
        gpu_meshes_ = {uploaded};
        instances_ = {{0, glm::mat4(1.0f)}};
        temporal_ = std::make_unique<TemporalScene>(core_);
        output_ = std::make_unique<FrameGenerationOutput>(core_);
        require(output_->ensure(kExtent,kExtent) && temporal_->ensure(kExtent,kExtent), "Temporal targets unavailable");
        require(temporal_->build(*meshes_,*textures_,instances_,gpu_meshes_,
            [masked](const auto&,const auto&) {
                GpuMaterial material;
                material.alpha_ref = masked ? .5f : 0.0f;
                return material;
            },config_,motion_,masked_), "Temporal proxy construction failed");
        depth_read_ = read_target(output_->depth_texture());
        motion_read_ = read_target(output_->motion_texture());
        packed_read_ = read_target(temporal_->packed_texture());
        require(depth_read_ && motion_read_ && packed_read_, "Readback targets unavailable");
        if (skinned) bones(glm::mat4(1.0f));
    }

    void setup_grass() {
        engine_->flushAndWait();
        destroy_read_targets();
        destroy_grass();
        output_.reset();
        temporal_.reset();
        camera.position = {0,0,0}; camera.yaw = 0; camera.pitch = 0;
        instances_ = {{-1, glm::mat4(1.0f)}};
        gpu_meshes_.clear();
        temporal_ = std::make_unique<TemporalScene>(core_);
        output_ = std::make_unique<FrameGenerationOutput>(core_);
        require(output_->ensure(kExtent,kExtent) && temporal_->ensure(kExtent,kExtent),
            "Grass temporal targets unavailable");
        require(temporal_->build(*meshes_,*textures_,instances_,gpu_meshes_,
            [](const auto&,const auto&) { return GpuMaterial{}; },config_,motion_,masked_),
            "Grass shared temporal scene construction failed");
        if (!grass_motion_) grass_motion_ = filament::Material::Builder()
            .package(k_heaven_motion_grass_filamat,k_heaven_motion_grass_filamat_size).build(*engine_);
        require(grass_motion_,"Production grass-motion package unavailable");

        // Actual production blade generation and vertex layout, with wider
        // controlled ribbons so a 192-pixel image contains strict interiors.
        // No per-frame vertex upload or CPU wind deformation is performed.
        GrassParams parameters;
        parameters.blade_width = .8f;
        parameters.blade_width_variance = 0.0f;
        parameters.blade_height = 1.5f;
        parameters.blade_height_variance = 0.0f;
        parameters.blade_bend_max = 0.0f;
        parameters.wind_amplitude = 0.0f;
        std::vector<GrassRoot> roots;
        for (float y : {-1.45f,-.7f,0.0f,.7f,1.45f}) roots.push_back({{4.0f,y,-.85f}});
        grass_vertices_ = generate_grass_blades(roots,0.0f,parameters);
        require(grass_vertices_.size() == roots.size()*GRASS_VERTS_PER_BLADE,
            "Production grass geometry has unexpected topology");
        const auto count = static_cast<uint32_t>(grass_vertices_.size());
        grass_vb_ = filament::VertexBuffer::Builder().vertexCount(count).bufferCount(2)
            .attribute(filament::VertexAttribute::POSITION,0,filament::VertexBuffer::AttributeType::FLOAT3,
                offsetof(GrassVertex,px),sizeof(GrassVertex))
            .attribute(filament::VertexAttribute::UV0,0,filament::VertexBuffer::AttributeType::FLOAT2,
                offsetof(GrassVertex,u),sizeof(GrassVertex))
            .attribute(filament::VertexAttribute::COLOR,0,filament::VertexBuffer::AttributeType::FLOAT4,
                offsetof(GrassVertex,r),sizeof(GrassVertex))
            .attribute(filament::VertexAttribute::TANGENTS,1,filament::VertexBuffer::AttributeType::SHORT4,0,0)
            .normalized(filament::VertexAttribute::TANGENTS).build(*engine_);
        grass_ib_ = filament::IndexBuffer::Builder().indexCount(count)
            .bufferType(filament::IndexBuffer::IndexType::UINT).build(*engine_);
        require(grass_vb_ && grass_ib_,"Grass source buffers unavailable");
        grass_vb_->setBufferAt(*engine_,0,{grass_vertices_.data(),grass_vertices_.size()*sizeof(GrassVertex)});
        upload_grass_tangent_frames(*engine_,*grass_vb_,grass_vertices_);
        std::vector<uint32_t> indices(count);
        std::iota(indices.begin(),indices.end(),0u);
        grass_ib_->setBuffer(*engine_,{indices.data(),indices.size()*sizeof(uint32_t)});
        grass_diffuse_ = filament::Texture::Builder().width(128).height(128).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).build(*engine_);
        require(grass_diffuse_,"Grass cutout atlas unavailable");
        std::vector<std::array<uint8_t,4>> atlas(size_t{128}*128,{128,192,96,255});
        for (unsigned y = 32; y < 96; ++y)
            for (unsigned x = 32; x < 96; ++x) atlas[y*128+x][3] = 0;
        grass_diffuse_->setImage(*engine_,0,{atlas.data(),atlas.size()*sizeof(atlas.front()),
            filament::Texture::Format::RGBA,filament::Texture::Type::UBYTE});
        grass_source_ = utils::EntityManager::get().create();
        grass_patch_ = {.source_entity = grass_source_, .vertices = grass_vb_, .indices = grass_ib_,
            .diffuse = grass_diffuse_, .instance_index = 0, .index_count = count,
            .bounds = {{4.0f,0.0f,0.0f},{2.0f,3.0f,3.0f}},
            .diffuse_uv = {1.0f,1.0f,0.0f,0.0f},
            .wind_tuning = {1.7f,2.9f,.55f,.25f}, .culling = false, .visible = true};
        grass_ = std::make_unique<TemporalGrass>(core_,temporal_->scene(),grass_motion_);
        const filament::math::float3 sh{.1f,.1f,.1f};
        grass_ibl_ = filament::IndirectLight::Builder().irradiance(1,&sh).intensity(1.0f).build(*engine_);
        require(grass_ibl_,"Grass wind-basis IBL unavailable");
        core_.scene->setIndirectLight(grass_ibl_);
        grass_wind_basis(glm::mat3(1.0f));
        depth_read_ = read_target(output_->depth_texture());
        motion_read_ = read_target(output_->motion_texture());
        packed_read_ = read_target(temporal_->packed_texture());
        require(depth_read_ && motion_read_ && packed_read_,"Grass readback targets unavailable");
        // BufferDescriptor inputs above remain owned until all uploads finish.
        engine_->flushAndWait();
    }

    void grass_wind_basis(const glm::mat3& rotation) {
        require(grass_ibl_,"Grass IBL was not initialized");
        grass_wind_to_user_ = rotation;
        grass_ibl_->setRotation(filament::math::mat3f{
            filament::math::float3{rotation[0].x,rotation[0].y,rotation[0].z},
            filament::math::float3{rotation[1].x,rotation[1].y,rotation[1].z},
            filament::math::float3{rotation[2].x,rotation[2].y,rotation[2].z}});
    }

    [[nodiscard]] temporal_grass_oracle::Frame grass_frame(float time) const {
        return {glm::dmat4(clip()),glm::dmat4(instances_.front().transform),
            glm::dmat3(grass_wind_to_user_),double(time)};
    }

    void reject_grass_attempt(float time) {
        core_.camera->setModelMatrix(glm_to_filament(glm::inverse(camera.view_matrix())));
        require(temporal_->prepare(camera,instances_,runtime_clip()),"Rejected grass scene prepare failed");
        require(grass_->prepare(std::span(&grass_patch_,1),instances_,time,runtime_clip()),
            "Rejected grass proxy prepare failed");
        // Deliberately neither render nor commit: accepted time/model/basis
        // must survive this otherwise valid, prepared application attempt.
    }

    void invalidate_grass() {
        temporal_->invalidate_history();
        grass_->invalidate_history();
    }

    void grass_visible(bool visible) { grass_patch_.visible = visible; }

    [[nodiscard]] glm::mat4 clip() const { return projection_ * camera.view_matrix(); }
    [[nodiscard]] glm::mat4 runtime_clip() const {
        // Exercise the exact application seam, while clip() remains an
        // independent GL reference for the expected native depth/motion.
        const auto scaling=core_.camera->getScaling();
        const auto shift=core_.camera->getShift();
        return temporal_gl_projection(core_.camera->getProjectionMatrix(),
            {float(scaling.x),float(scaling.y)},{float(shift.x),float(shift.y)}) * camera.view_matrix();
    }
    void model(const glm::mat4& value) { instances_.front().transform = value; }
    void bones(const glm::mat4& value) {
        const std::array palette{value};
        require(temporal_->set_bones(0,palette), "Current bone upload rejected");
    }
    void deform(float y) {
        auto vertices = source_.surfaces.front().vertices;
        for (auto& vertex : vertices) vertex.py += y;
        const auto handle = gpu_meshes_.front().surfaces.front().handle;
        require(temporal_->set_positions(handle,vertices), "Current position snapshot rejected");
        require(meshes_->update_vertices(handle,vertices.data(),vertices.size()).has_value(), "Current vertex upload rejected");
    }
    void reject_prepared_attempt() {
        require(temporal_->prepare(camera,instances_,runtime_clip()), "Rejected attempt prepare failed");
        // This models beginFrame rejection: no render/endFrame/commit occurs.
    }
    void invalidate() { temporal_->invalidate_history(); }

    void verify_native_isolation() {
        // Filament texture usage is a scoped bitmask.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::SAMPLEABLE) |
            std::to_underlying(filament::Texture::Usage::UPLOADABLE) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC) |
            std::to_underlying(filament::Texture::Usage::BLIT_DST));
        native_texture_ = filament::Texture::Builder().width(kExtent).height(kExtent).levels(1)
            .usage(usage).format(filament::Texture::InternalFormat::RGBA32F).build(*engine_);
        require(native_texture_, "Native isolation texture unavailable");
        native_read_ = read_target(native_texture_);
        require(native_read_, "Native isolation readback target unavailable");
        const glm::vec4 initial{.25f,.5f,.75f,1.0f};
        std::vector<glm::vec4> pixels(static_cast<size_t>(kExtent)*kExtent,initial);
        native_texture_->setImage(*engine_,0,{pixels.data(),pixels.size()*sizeof(glm::vec4),
            filament::Texture::Format::RGBA,filament::Texture::Type::FLOAT});
        engine_->flushAndWait();
        const auto attempt = [&](const char* label, const glm::vec4& write,
                                 const glm::vec4& expected, bool discard) {
            NativeWrite probe{.color = write, .discard = discard};
            std::atomic_bool done{false};
            bool accepted = false;
            for (int retry = 0; retry < 8 && !accepted; ++retry) {
                accepted = core_.renderer->beginFrame(swapchain_);
                if (!accepted) engine_->flushAndWait();
            }
            require(accepted, "Native isolation beginFrame rejected all bounded attempts");
            core_.renderer->nativeCommand(native_texture_,nullptr,nullptr,nullptr,native_write,&probe);
            read(native_read_,pixels.data(),pixels.size()*sizeof(glm::vec4),
                filament::backend::PixelDataFormat::RGBA,done);
            core_.renderer->endFrame();
            engine_->flushAndWait();
            require(probe.called && probe.supported && done,
                "Native isolation callback, discard pointer or readback unavailable");
            float max_error = 0.0f;
            for (const auto& pixel : pixels)
                for (int channel = 0; channel < 4; ++channel) {
                    require(std::isfinite(pixel[channel]), "Nonfinite native isolation pixel");
                    max_error = std::max(max_error,std::abs(pixel[channel]-expected[channel]));
                }
            std::printf("[temporal_scene_test] %s pixels=%zu max_error=%.8g discard=%d\n",
                label,pixels.size(),max_error,discard);
            require(max_error < 1e-6f,"Native command discard/commit mutated unexpected pixels");
        };
        const glm::vec4 seed{.125f,.375f,.625f,.875f};
        attempt("native-partial-write-discard",seed,initial,true);
        attempt("native-successful-seed-commits",seed,seed,false);
        attempt("native-discard-preserves-successful-seed",glm::vec4(1.0f),seed,true);
    }

    void capture(const char* label, const glm::vec2& motion, float expected_depth,
                 bool history_valid, bool accept = true, bool sky_only = false,
                 bool wrong_sky_history = false) {
        core_.camera->setModelMatrix(glm_to_filament(glm::inverse(camera.view_matrix())));
        require(temporal_->prepare(camera,instances_,runtime_clip()), "Temporal prepare failed");
        const auto& proxy_camera = temporal_->view()->getCamera();
        const filament::Frustum frustum(filament::math::mat4f(
            proxy_camera.getCullingProjectionMatrix() * proxy_camera.getViewMatrix()));
        std::array<filament::math::float4,6> planes{};
        frustum.getNormalizedPlanes(planes.data());
        for (const auto& plane : planes)
            for (int component = 0; component < 4; ++component)
                require(std::isfinite(plane[component]),"Temporal culling plane is nonfinite");
        output_->prepare(temporal_->packed_texture(),kExtent,kExtent,false,
            {temporal_->current_clip(), wrong_sky_history ? temporal_->current_clip() :
                temporal_->previous_clip(), temporal_->history_valid()});
        std::vector<float> depths(static_cast<size_t>(kExtent)*kExtent);
        // Request RG16F's native component type. Filament's generic HALF to
        // FLOAT readback conversion clamps negative values to zero; decoding
        // the native halves here preserves the signed motion being tested.
        std::vector<std::array<uint16_t,2>> half_vectors(static_cast<size_t>(kExtent)*kExtent);
        std::vector<glm::vec4> packed(static_cast<size_t>(kExtent)*kExtent);
        std::array<std::atomic_bool,3> done{};
        bool accepted = false;
        for (int attempt = 0; attempt < 8 && !accepted; ++attempt) {
            accepted = core_.renderer->beginFrame(swapchain_);
            if (!accepted) engine_->flushAndWait();
        }
        require(accepted, "Headless beginFrame rejected all bounded attempts");
        core_.renderer->render(temporal_->view());
        output_->render_extract();
        read(depth_read_,depths.data(),depths.size()*sizeof(float),filament::backend::PixelDataFormat::R,done[0]);
        read(motion_read_,half_vectors.data(),half_vectors.size()*sizeof(half_vectors.front()),
            filament::backend::PixelDataFormat::RG,done[1],filament::backend::PixelDataType::HALF);
        read(packed_read_,packed.data(),packed.size()*sizeof(glm::vec4),filament::backend::PixelDataFormat::RGBA,done[2]);
        core_.renderer->endFrame();
        engine_->flushAndWait();
        require(done[0] && done[1] && done[2], "Native pixel callbacks incomplete");
        std::vector<glm::vec2> vectors;
        vectors.reserve(half_vectors.size());
        for (const auto& value : half_vectors) vectors.emplace_back(
            float(filament::math::makeHalf(value[0])),float(filament::math::makeHalf(value[1])));
        size_t packed_coverage = 0;
        float packed_depth_max = 0.0f, extracted_depth_max = 0.0f;
        for (size_t i = 0; i < packed.size(); ++i) {
            packed_coverage += packed[i].z != 0.0f;
            packed_depth_max = std::max(packed_depth_max,packed[i].z);
            extracted_depth_max = std::max(extracted_depth_max,depths[i]);
        }
        std::printf("[temporal_scene_test] %s renderables=%zu visible=%d proxies=%zu packed_coverage=%zu packed_depth_max=%.8g extracted_depth_max=%.8g\n",
            label,temporal_->scene()->getRenderableCount(),temporal_->view()->getVisibleRenderableCount(),
            temporal_->coverage().proxy_primitives,packed_coverage,packed_depth_max,extracted_depth_max);
        size_t interior = 0, background = 0;
        float max_depth_error = 0.0f, max_motion_error = 0.0f, sky_error = 0.0f;
        float expected_sky_max = 0.0f;
        // Independent pinhole-ray oracle: construct an eye-space direction
        // from the known 60-degree lens and rotate it into world space. This
        // does not invert the clip matrix or subtract homogeneous endpoints.
        const auto world_from_eye = glm::mat3(glm::inverse(camera.view_matrix()));
        const auto previous_sky_clip = projection_ * previous_sky_camera_.view_matrix();
        for (uint32_t y = 1; y + 1 < kExtent; ++y) {
            for (uint32_t x = 1; x + 1 < kExtent; ++x) {
                const size_t index = static_cast<size_t>(y)*kExtent+x;
                if (depths[index] == 0.0f) {
                    ++background;
                    const glm::vec2 uv{(float(x)+.5f)/float(kExtent),
                        (float(y)+.5f)/float(kExtent)};
                    const auto tangent = std::tan(glm::radians(30.0f));
                    const auto ray = world_from_eye * glm::vec3{
                        (2.0f*uv.x-1.0f)*tangent,(1.0f-2.0f*uv.y)*tangent,-1.0f};
                    const auto prior = previous_sky_clip * glm::vec4(ray,0.0f);
                    const auto expected = history_valid && prior.w > 1e-8f ?
                        glm::vec2(prior)/prior.w*glm::vec2(.5f,-.5f)+.5f-uv : glm::vec2{};
                    require(std::isfinite(vectors[index].x) && std::isfinite(vectors[index].y),
                        "Sky motion is nonfinite");
                    sky_error = std::max(sky_error,glm::length(vectors[index]-expected));
                    expected_sky_max = std::max(expected_sky_max,glm::length(expected));
                    continue;
                }
                bool inside = true;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        inside &= depths[static_cast<size_t>(int(y)+dy)*kExtent+uint32_t(int(x)+dx)] > 0.0f;
                if (!inside) continue;
                ++interior;
                require(std::isfinite(depths[index]) && std::isfinite(vectors[index].x) &&
                    std::isfinite(vectors[index].y), "Nonfinite native temporal input");
                if (!sky_only) {
                    max_depth_error = std::max(max_depth_error,std::abs(depths[index]-expected_depth));
                    max_motion_error = std::max(max_motion_error,glm::length(vectors[index]-motion));
                }
                require(glm::length(vectors[index]-glm::vec2(packed[index])) < 4e-4f,
                    "RG16F extract differs from signed RGBA32F source motion");
                require(std::abs(packed[index].w-(history_valid?1.0f:0.0f)) < 1e-5f,
                    "Packed validity differs from accepted history");
            }
        }
        std::printf("[temporal_scene_test] %s interior=%zu background=%zu depth_error=%.8g motion_error=%.8g expected=(%.6f,%.6f)\n",
            label,interior,background,max_depth_error,max_motion_error,motion.x,motion.y);
        const size_t center = static_cast<size_t>(kExtent/2)*kExtent+kExtent/2;
        std::printf("[temporal_scene_test] %s center packed=(%.8g,%.8g,%.8g,%.8g) extracted=(%.8g,%.8g)\n",
            label,packed[center].x,packed[center].y,packed[center].z,packed[center].w,
            vectors[center].x,vectors[center].y);
        require(interior >= 256 && background >= 128, "Temporal raster coverage missing or unexpectedly full-screen");
        require(max_depth_error < 2e-5f, "R32F depth differs from independent projection");
        require(max_motion_error < 4e-4f, "RG16F signed motion differs from independent projection");
        std::printf("[temporal_scene_test] %s sky_pixels=%zu sky_error=%.8g expected_sky_max=%.8g\n",
            label,background,sky_error,expected_sky_max);
        require(sky_error < 4e-4f,"Dense sky motion differs from independent pinhole-ray projection");
        if (accept) {
            temporal_->commit_accepted();
            previous_sky_camera_ = camera;
        }
    }

    void capture_grass(const char* label, float time,
                       const temporal_grass_oracle::Frame& previous,
                       bool history_valid, bool wrong_previous_time = false,
                       bool require_time_control = false) {
        namespace oracle = temporal_grass_oracle;
        core_.camera->setModelMatrix(glm_to_filament(glm::inverse(camera.view_matrix())));
        require(temporal_->prepare(camera,instances_,runtime_clip()),"Grass temporal scene prepare failed");
        require(grass_->prepare(std::span(&grass_patch_,1),instances_,time,runtime_clip()),
            "Production grass proxy prepare failed");
        require(temporal_->coverage().proxy_primitives == 0,"Unexpected mesh in grass-only target");
        require(grass_->coverage().ranges == 1 && grass_->coverage().unavailable_ranges == 0,
            "Grass source range unavailable");
        require(grass_->coverage().visible_ranges == size_t(grass_patch_.visible) &&
            grass_->coverage().incomplete_history_ranges == size_t(grass_patch_.visible && !history_valid),
            "Grass range history accounting differs from explicit reference state");
        const auto current = grass_frame(time);
        auto reference_previous = previous;
        if (wrong_previous_time) reference_previous.time = current.time;
        const auto expected = oracle::raster(grass_vertices_,current,reference_previous,
            glm::dvec4(grass_patch_.wind_tuning),glm::dvec4(grass_patch_.diffuse_uv),kExtent,history_valid);
        auto incorrect_previous = previous;
        incorrect_previous.time = current.time;
        const auto incorrect = oracle::raster(grass_vertices_,current,incorrect_previous,
            glm::dvec4(grass_patch_.wind_tuning),glm::dvec4(grass_patch_.diffuse_uv),kExtent,history_valid);
        output_->prepare(temporal_->packed_texture(),kExtent,kExtent,false);
        std::vector<float> depths(static_cast<size_t>(kExtent)*kExtent);
        std::vector<std::array<uint16_t,2>> half_vectors(static_cast<size_t>(kExtent)*kExtent);
        std::vector<glm::vec4> packed(static_cast<size_t>(kExtent)*kExtent);
        std::array<std::atomic_bool,3> done{};
        bool accepted = false;
        for (int retry = 0; retry < 8 && !accepted; ++retry) {
            accepted = core_.renderer->beginFrame(swapchain_);
            if (!accepted) engine_->flushAndWait();
        }
        require(accepted,"Grass beginFrame rejected all bounded attempts");
        core_.renderer->render(temporal_->view());
        output_->render_extract();
        read(depth_read_,depths.data(),depths.size()*sizeof(float),filament::backend::PixelDataFormat::R,done[0]);
        read(motion_read_,half_vectors.data(),half_vectors.size()*sizeof(half_vectors.front()),
            filament::backend::PixelDataFormat::RG,done[1],filament::backend::PixelDataType::HALF);
        read(packed_read_,packed.data(),packed.size()*sizeof(glm::vec4),filament::backend::PixelDataFormat::RGBA,done[2]);
        core_.renderer->endFrame();
        engine_->flushAndWait();
        require(done[0] && done[1] && done[2],"Grass pixel callbacks incomplete");
        size_t opaque = 0, cutout = 0, background = 0, wrong_time_rejections = 0, cutout_mismatches = 0;
        double depth_error = 0.0, motion_error = 0.0, packed_error = 0.0, wrong_time_error = 0.0;
        for (size_t i = 0; i < packed.size(); ++i) {
            for (int channel = 0; channel < 4; ++channel)
                require(std::isfinite(packed[i][channel]),"Nonfinite grass packed pixel");
            require(std::isfinite(depths[i]),"Nonfinite grass extracted depth");
            const glm::dvec2 extracted{float(filament::math::makeHalf(half_vectors[i][0])),
                float(filament::math::makeHalf(half_vectors[i][1]))};
            require(std::isfinite(extracted.x) && std::isfinite(extracted.y),"Nonfinite grass extracted motion");
            if (!grass_patch_.visible) {
                require(glm::length(packed[i]) < 1e-7f && depths[i] == 0.0f && glm::length(extracted) == 0.0,
                    "Hidden grass range still writes temporal pixels");
                continue;
            }
            const auto& pixel = expected[i];
            if (pixel.coverage == oracle::Coverage::opaque) {
                ++opaque;
                depth_error = std::max(depth_error,std::abs(double(depths[i])-pixel.depth));
                depth_error = std::max(depth_error,std::abs(double(packed[i].z)-pixel.depth));
                motion_error = std::max(motion_error,glm::length(extracted-pixel.motion));
                packed_error = std::max(packed_error,glm::length(glm::dvec2(packed[i])-pixel.motion));
                require(std::abs(packed[i].w-(history_valid?1.0f:0.0f)) < 1e-6f,
                    "Grass packed validity differs from explicit accepted history");
                const double incorrect_error = glm::length(glm::dvec2(packed[i])-incorrect[i].motion);
                wrong_time_error = std::max(wrong_time_error,incorrect_error);
                wrong_time_rejections += incorrect_error > 0.001;
            } else if (pixel.coverage == oracle::Coverage::cutout) {
                ++cutout;
                if (glm::length(packed[i]) >= 1e-7f || depths[i] != 0.0f || glm::length(extracted) != 0.0) {
                    if (cutout_mismatches < 12)
                        std::printf("[temporal_scene_test] %s cutout mismatch pixel=(%zu,%zu) reference_uv=(%.8g,%.8g) packed=(%.8g,%.8g,%.8g,%.8g) depth=%.8g extracted=(%.8g,%.8g)\n",
                            label,i%kExtent,i/kExtent,pixel.uv.x,pixel.uv.y,packed[i].x,packed[i].y,
                            packed[i].z,packed[i].w,depths[i],extracted.x,extracted.y);
                    ++cutout_mismatches;
                }
            } else if (pixel.coverage == oracle::Coverage::outside && depths[i] == 0.0f) {
                ++background;
            }
        }
        std::printf("[temporal_scene_test] %s grass_opaque=%zu cutout=%zu background=%zu cutout_mismatches=%zu depth_error=%.8g packed_motion_error=%.8g extracted_motion_error=%.8g wrong_previous_time_error=%.8g rejected_pixels=%zu valid=%d visible=%d\n",
            label,opaque,cutout,background,cutout_mismatches,depth_error,packed_error,motion_error,wrong_time_error,
            wrong_time_rejections,history_valid,grass_patch_.visible);
        if (grass_patch_.visible) {
            require(cutout_mismatches == 0,"Transparent atlas constant-alpha pixel quad writes grass depth/motion");
            require(opaque >= 96 && cutout >= 24 && background >= 128,
                "Grass oracle lacks independent opaque/cutout/background pixel coverage");
            require(depth_error < 2e-5,"Grass native depth differs from independent wind/projected raster");
            require(packed_error < 3e-5 && motion_error < 4e-4,
                "Grass native motion differs from independent accepted wind/model/camera reference");
            if (require_time_control)
                require(wrong_time_rejections >= 32 && wrong_time_error > 0.003,
                    "Wrong previous wind time does not falsify enough grass pixels");
        }
        temporal_->commit_accepted();
        grass_->commit_accepted();
    }

    Camera camera;
private:
    void destroy_grass() {
        // Proxies borrow feature geometry/atlas and the temporal scene. Retire
        // borrowers first, as the production feature shutdown path does.
        grass_.reset();
        if (grass_ibl_) {
            if (core_.scene) core_.scene->setIndirectLight(nullptr);
            engine_->destroy(grass_ibl_);
        }
        grass_ibl_ = nullptr;
        if (grass_vb_) engine_->destroy(grass_vb_);
        if (grass_ib_) engine_->destroy(grass_ib_);
        if (grass_diffuse_) engine_->destroy(grass_diffuse_);
        if (grass_source_) utils::EntityManager::get().destroy(grass_source_);
        grass_vb_ = nullptr;
        grass_ib_ = nullptr;
        grass_diffuse_ = nullptr;
        grass_source_ = {};
        grass_vertices_.clear();
    }
    void verify_projection_conversion() {
        struct Case {
            const char* name;
            glm::mat4 projection;
            float near_plane;
            float far_plane;
            bool infinite = false;
            bool orthographic = false;
        };
        const std::array cases{
            Case{"finite",glm::perspective(glm::radians(63.0f),1.6f,.25f,512.0f),.25f,512.0f},
            Case{"infinite",glm::infinitePerspective(glm::radians(57.0f),1.3f,.5f),.5f,2000.0f,true},
            Case{"off-axis",glm::frustum(-.2f,.35f,-.125f,.25f,.25f,512.0f),.25f,512.0f},
            Case{"orthographic",glm::ortho(-3.0f,5.0f,-2.0f,4.0f,.25f,512.0f),.25f,512.0f,false,true}};
        for (const auto& item : cases) {
            const auto finite = glm::perspective(glm::radians(57.0f),1.3f,item.near_plane,item.far_plane);
            core_.camera->setCustomProjection(filament::math::mat4(glm_to_filament(item.projection)),
                filament::math::mat4(glm_to_filament(item.infinite ? finite : item.projection)),
                item.near_plane,item.far_plane);
            core_.camera->setScaling({1.25,.75});
            core_.camera->setShift({.08,-.12});
            glm::mat4 image_transform(1.0f);
            image_transform[0][0]=1.25f; image_transform[1][1]=.75f;
            image_transform[3][0]=.16f; image_transform[3][1]=-.24f;
            const auto expected=image_transform*item.projection;
            const auto public_projection=core_.camera->getProjectionMatrix();
            const auto raw=temporal_gl_projection(public_projection);
            const auto scaling=core_.camera->getScaling();
            const auto shift=core_.camera->getShift();
            const auto recovered=temporal_gl_projection(public_projection,
                {float(scaling.x),float(scaling.y)},{float(shift.x),float(shift.y)});
            const auto expected_culling=image_transform*(item.infinite ? finite : item.projection);
            const auto culling=temporal_gl_projection(core_.camera->getCullingProjectionMatrix(),
                {float(scaling.x),float(scaling.y)},{float(shift.x),float(shift.y)});
            float error=0.0f;
            for(int column=0;column<4;++column)
                for(int row=0;row<4;++row) {
                    require(std::abs(raw[column][row]-item.projection[column][row])<2e-6f,
                        "Public camera projection no longer returns the original GL matrix");
                    require(std::abs(culling[column][row]-expected_culling[column][row])<2e-6f,
                        "Finite culling projection helper changed image scale/shift");
                    error=std::max(error,std::abs(recovered[column][row]-expected[column][row]));
                }
            require(error<2e-6f,"Public camera projection helper changed GL depth or image scale/shift");
            // These expectations use physical near/far/distance, rather than
            // passing either projection back through the shader's formula.
            constexpr float distance=4.0f;
            const float expected_depth=item.infinite ? item.near_plane/distance :
                (item.orthographic ? (item.far_plane-distance)/(item.far_plane-item.near_plane) :
                item.near_plane*(item.far_plane-distance)/(distance*(item.far_plane-item.near_plane)));
            const auto point=recovered*glm::vec4{0,0,-distance,1};
            require(std::abs((.5f-.5f*point.z/point.w)-expected_depth)<2e-6f,
                "Recovered camera depth differs from physical near/far/distance");
            std::printf("[temporal_scene_test] public-camera-%s GL recovery error=%.8g depth=%.8g\n",
                item.name,error,expected_depth);
        }
        core_.camera->setScaling({1.0,1.0});
        core_.camera->setShift({0.0,0.0});
        const auto culling=glm::perspective(glm::radians(60.0f),1.0f,.5f,2000.0f);
        core_.camera->setCustomProjection(filament::math::mat4(glm_to_filament(projection_)),
            filament::math::mat4(glm_to_filament(culling)),.5,2000);
        require(std::abs(project(projection_,glm::mat4(1.0f),glm::vec4{0,0,-4,1}).depth-.125f)<1e-7f,
            "Native oracle reference depth differs from near/distance .5/4");
    }
    filament::RenderTarget* read_target(filament::Texture* texture) {
        return filament::RenderTarget::Builder().texture(filament::RenderTarget::AttachmentPoint::COLOR0,texture).build(*engine_);
    }
    void destroy_read_targets() {
        for (auto** target : {&depth_read_,&motion_read_,&packed_read_,&native_read_}) {
            if (*target) engine_->destroy(*target);
            *target = nullptr;
        }
    }
    void read(filament::RenderTarget* target, void* data, size_t bytes,
              filament::backend::PixelDataFormat format, std::atomic_bool& done,
              filament::backend::PixelDataType type = filament::backend::PixelDataType::FLOAT) {
        core_.renderer->readPixels(target,0,0,kExtent,kExtent,filament::backend::PixelBufferDescriptor(
            data,bytes,format,type,
            [](void*,size_t,void* value) { static_cast<std::atomic_bool*>(value)->store(true); },&done));
    }
    filament::Engine* engine_ = nullptr;
    BackendCore core_;
    utils::Entity camera_entity_;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Material* motion_ = nullptr;
    filament::Material* masked_ = nullptr;
    filament::Material* grass_motion_ = nullptr;
    filament::RenderTarget* depth_read_ = nullptr;
    filament::RenderTarget* motion_read_ = nullptr;
    filament::RenderTarget* packed_read_ = nullptr;
    filament::RenderTarget* native_read_ = nullptr;
    filament::Texture* native_texture_ = nullptr;
    std::unique_ptr<MeshStore> meshes_;
    std::unique_ptr<TextureStore> textures_;
    std::unique_ptr<TemporalScene> temporal_;
    std::unique_ptr<FrameGenerationOutput> output_;
    std::unique_ptr<TemporalGrass> grass_;
    filament::VertexBuffer* grass_vb_ = nullptr;
    filament::IndexBuffer* grass_ib_ = nullptr;
    filament::Texture* grass_diffuse_ = nullptr;
    filament::IndirectLight* grass_ibl_ = nullptr;
    utils::Entity grass_source_;
    TemporalGrassPatch grass_patch_;
    std::vector<GrassVertex> grass_vertices_;
    glm::mat3 grass_wind_to_user_{1.0f};
    Mesh source_;
    std::vector<GpuMesh> gpu_meshes_;
    std::vector<Instance> instances_;
    AppConfig config_;
    glm::mat4 projection_{1.0f};
    Camera previous_sky_camera_;
};

void run_sky(Fixture& fixture, bool wrong_history = false) {
    fixture.setup();
    fixture.capture("sky-first-invalid",{},.125f,false,true,true);
    fixture.camera.yaw = 7.0f;
    fixture.camera.pitch = -3.0f;
    fixture.capture("sky-camera-rotation",{},0,true,true,true,wrong_history);
    fixture.camera.position = {.1f,-.2f,.08f};
    fixture.capture("sky-translation-has-no-parallax",{},0,true,true,true);
    fixture.camera.yaw = 18.0f;
    fixture.reject_prepared_attempt();
    fixture.camera.yaw = 9.0f;
    fixture.capture("sky-rejected-camera-retains-accepted-history",{},0,true,true,true);
    fixture.invalidate();
    fixture.camera.pitch = -1.0f;
    fixture.capture("sky-cut-resets-history",{},0,false,true,true);
    fixture.camera.pitch = -2.0f;
    fixture.capture("sky-return-resumes-history",{},0,true,true,true);
}

void run_grass(Fixture& fixture, bool wrong_previous_time = false) {
    fixture.setup_grass();
    const glm::mat4 identity(1.0f);
    fixture.grass_wind_basis(glm::mat3(glm::rotate(identity,.35f,glm::vec3{0,0,1})));
    auto previous = fixture.grass_frame(.15f);
    fixture.capture_grass("grass-first-invalid",.15f,previous,false);
    fixture.capture_grass("grass-stationary",.15f,previous,true);
    fixture.capture_grass("grass-wind-time",1.2f,previous,true,wrong_previous_time,true);
    previous = fixture.grass_frame(1.2f);
    fixture.model(glm::translate(identity,glm::vec3{0,.18f,.09f}) *
        glm::rotate(identity,.06f,glm::vec3{0,0,1}));
    fixture.capture_grass("grass-model-transform",1.2f,previous,true);
    previous = fixture.grass_frame(1.2f);
    fixture.grass_wind_basis(glm::mat3(glm::rotate(identity,.8f,glm::vec3{0,0,1})));
    fixture.capture_grass("grass-accepted-wind-basis",1.55f,previous,true,false,true);
    previous = fixture.grass_frame(1.55f);

    fixture.model(glm::translate(identity,glm::vec3{0,.9f,.4f}));
    fixture.grass_wind_basis(glm::mat3(glm::rotate(identity,-.6f,glm::vec3{1,0,0})));
    fixture.reject_grass_attempt(3.1f);
    fixture.model(glm::translate(identity,glm::vec3{0,.24f,.12f}));
    fixture.grass_wind_basis(glm::mat3(glm::rotate(identity,.55f,glm::vec3{0,0,1})));
    fixture.capture_grass("grass-rejected-time-model-basis-retains-accepted-history",1.85f,
        previous,true,false,true);
    previous = fixture.grass_frame(1.85f);
    fixture.camera.position = {0,-.13f,.08f};
    fixture.capture_grass("grass-camera-translation",2.0f,previous,true);
    previous = fixture.grass_frame(2.0f);
    fixture.invalidate_grass();
    fixture.capture_grass("grass-reset-invalidates-history",2.35f,previous,false);
    previous = fixture.grass_frame(2.35f);
    fixture.capture_grass("grass-reset-resumes-accepted-history",2.8f,previous,true,false,true);
    previous = fixture.grass_frame(2.8f);
    fixture.grass_visible(false);
    fixture.capture_grass("grass-hidden-range",2.9f,previous,false);
    previous = fixture.grass_frame(2.9f);
    fixture.grass_visible(true);
    fixture.capture_grass("grass-return-invalidates-hidden-history",3.05f,previous,false);
}

void run() {
    Fixture fixture;
    fixture.init();
    for (bool masked : {false,true}) {
        fixture.setup(false,masked);
        const glm::mat4 identity(1.0f);
        auto previous = project(fixture.clip(),identity);
        fixture.capture(masked?"masked-first-invalid":"first-invalid",{},previous.depth,false);
        fixture.capture(masked?"masked-stationary":"stationary",{},previous.depth,true);
        auto model = glm::translate(identity,glm::vec3{0,.25f,.125f});
        fixture.model(model);
        auto current = project(fixture.clip(),model);
        require((previous.uv-current.uv).x > 0 && (previous.uv-current.uv).y > 0,"Known positive object motion sign changed");
        fixture.capture("object-translation",previous.uv-current.uv,current.depth,true);
        previous = current;
        fixture.model(glm::translate(identity,glm::vec3{0,.8f,.6f}));
        fixture.reject_prepared_attempt();
        model = glm::translate(identity,glm::vec3{0,.4f,.2f});
        fixture.model(model);
        current = project(fixture.clip(),model);
        fixture.capture("rejected-attempt-retains-accepted-history",previous.uv-current.uv,current.depth,true);
        previous = current;
        fixture.camera.position.y = .2f;
        current = project(fixture.clip(),model);
        require((previous.uv-current.uv).x < 0,"Known negative camera motion sign changed");
        fixture.capture("camera-translation",previous.uv-current.uv,current.depth,true);
        fixture.invalidate();
        fixture.capture("cut-invalidates-history",{},current.depth,false);
    }
    for (float w : {.5f,2.0f}) {
        fixture.setup();
        glm::mat4 model(1.0f); model[3][3] = w;
        fixture.model(model);
        auto previous = project(fixture.clip(),model);
        fixture.capture("positive-homogeneous-W-first",{},previous.depth,false);
        model[3].y = .25f; fixture.model(model);
        const auto current = project(fixture.clip(),model);
        fixture.capture("positive-homogeneous-W-motion",previous.uv-current.uv,current.depth,true);
    }
    fixture.setup(true);
    const glm::mat4 identity(1.0f);
    auto previous = project(fixture.clip(),identity);
    fixture.capture("GPU-bones-first",{},previous.depth,false);
    auto bone = glm::translate(identity,glm::vec3{0,-.2f,.1f});
    fixture.bones(bone);
    auto current = project(fixture.clip(),bone);
    fixture.capture("GPU-bones-translation",previous.uv-current.uv,current.depth,true);
    fixture.setup();
    fixture.deform(0.0f);
    previous = project(fixture.clip(),identity);
    fixture.capture("CPU-positions-first",{},previous.depth,false);
    fixture.deform(.125f);
    current = project(fixture.clip(),identity,kCenter+glm::vec4{0,.125f,0,0});
    fixture.capture("CPU-positions-translation",previous.uv-current.uv,current.depth,true);
    run_grass(fixture);
    run_sky(fixture);
    fixture.verify_native_isolation();
}
} // namespace
} // namespace heaven

int main(int argc, char** argv) try {
    heaven::verify_culling_projection_contract();
    if (argc > 1) {
        const std::string_view option(argv[1]);
        heaven::require(argc == 2 && (option == "--culling-only" || option == "--grass-only" ||
            option == "--grass-wrong-previous-time" || option == "--sky-only" ||
            option == "--sky-wrong-previous-camera"),
            "Usage: heaven_temporal_scene_test [--culling-only|--grass-only|--grass-wrong-previous-time|--sky-only|--sky-wrong-previous-camera]");
        if (option != "--culling-only") {
            heaven::Fixture fixture;
            fixture.init();
            if (option == "--sky-only" || option == "--sky-wrong-previous-camera") {
                heaven::run_sky(fixture,option == "--sky-wrong-previous-camera");
                std::puts("[temporal_scene_test] PASS: dense sky rotation/translation, rejected camera and reset");
            } else {
                heaven::run_grass(fixture,option == "--grass-wrong-previous-time");
                std::puts("[temporal_scene_test] PASS: production grass pixel oracle, wind/model/camera history, cutout, rejected attempt and reset");
            }
        }
        return 0;
    }
    heaven::run();
    std::puts("[temporal_scene_test] PASS: native R32F/RG16F/validity, signed camera/object motion, accepted history, masked coverage, positive W, GPU bones, CPU positions, production grass wind/mask/history and isolated native command discard/commit");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr,"[temporal_scene_test] FAIL: %s\n",error.what());
    return 1;
} catch (...) {
    std::fputs("[temporal_scene_test] FAIL: unknown exception\n",stderr);
    return 1;
}
