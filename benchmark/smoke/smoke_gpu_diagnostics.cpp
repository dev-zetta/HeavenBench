// Actual-API invocation query oracle: one unclipped triangle, one draw, many fragments.
// The existing test-only unlit raster package is reused; production material bytes stay intact.
#include "smoke_cases.h"
#include "benchmark/microprofiler.h"
#include "benchmark/microprofile_report.h"
#include "rhi/filament_backend.h"
#include "rhi/mesh_store.h"
#include "rhi/profile_bridge.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <backend/platforms/VulkanPlatform.h>
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
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifndef HEAVEN_SMOKE_VRS_UNLIT_PACKAGE
#define HEAVEN_SMOKE_VRS_UNLIT_PACKAGE ""
#endif

namespace heaven::smoke {
namespace {
constexpr std::uint32_t kSize = 128;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
bool requested() {
    const char* value = std::getenv("HEAVEN_SMOKE_GPU_DIAGNOSTICS");
    return value && std::string_view(value) == "1";
}

class DiagnosticFixture {
public:
    explicit DiagnosticFixture(filament::Engine& engine) : engine_(engine), meshes_(engine) {}
    DiagnosticFixture(const DiagnosticFixture&) = delete;
    DiagnosticFixture& operator=(const DiagnosticFixture&) = delete;
    ~DiagnosticFixture() {
        // Fixture-only drain before detaching the observer and before deleting query borrowers.
        engine_.flushAndWait();
        bridge_.reset();
        if (view_) {
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
            view_->setRenderTarget(nullptr);
        }
        if (object_) {
            if (scene_) scene_->remove(object_);
            engine_.destroy(object_);
            utils::EntityManager::get().destroy(object_);
        }
        engine_.destroy(view_);
        engine_.destroy(scene_);
        engine_.destroy(target_);
        engine_.destroy(depth_);
        engine_.destroy(color_);
        engine_.destroy(instance_);
        engine_.destroy(material_);
        if (cameraEntity_) {
            engine_.destroyCameraComponent(cameraEntity_);
            utils::EntityManager::get().destroy(cameraEntity_);
        }
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }
    void init() {
        bridge_ = std::make_unique<ProfileBridge>(recorder_);
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        cameraEntity_ = utils::EntityManager::get().create();
        auto* camera = engine_.createCamera(cameraEntity_);
        require(renderer_ && swapchain_ && scene_ && view_ && camera, "fixture owners unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1, .1, 10);
        camera->lookAt({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
        camera->setExposure(1.0f);
        // Texture usage is a documented combinable flag set.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        require(color_ && depth_, "fixture textures unavailable");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        require(target_, "fixture render target unavailable");
        view_->setScene(scene_);
        view_->setCamera(camera);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        std::ifstream input(HEAVEN_SMOKE_VRS_UNLIT_PACKAGE, std::ios::binary);
        require(input.is_open(), "existing test-only unlit package unavailable");
        const std::vector<char> package{std::istreambuf_iterator<char>(input), {}};
        require(!package.empty(), "test-only material is empty");
        material_ = filament::Material::Builder().package(package.data(), package.size()).build(engine_);
        require(material_, "test-only material creation failed");
        instance_ = material_->createInstance();
        require(instance_, "test-only material instance creation failed");
        instance_->setParameter("pattern", 1);
        Mesh mesh;
        mesh.path = mesh.name = "gpu-diagnostics-one-triangle";
        Surface surface;
        surface.name = "unclipped-triangle";
        for (const auto point : std::array<glm::vec2, 3>{{{-.8f, -.8f}, {.8f, -.8f}, {0, .8f}}}) {
            Vertex vertex{};
            vertex.px = point.x; vertex.py = point.y;
            vertex.nz = vertex.tx = vertex.tw = vertex.displacement_normal_scale = 1;
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2};
        mesh.surfaces.push_back(std::move(surface));
        const auto gpu = meshes_.upload(mesh);
        require(gpu.surfaces.size() == 1, "triangle upload failed");
        const auto buffers = meshes_.find(gpu.surfaces.front().handle);
        require(buffers.vb && buffers.ib, "triangle buffers unavailable");
        object_ = utils::EntityManager::get().create();
        require(filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, buffers.vb, buffers.ib, 0, 3)
            .material(0, instance_).boundingBox({{0, 0, 0}, {.8f, .8f, .01f}})
            .culling(false).castShadows(false).receiveShadows(false).build(engine_, object_) ==
                filament::RenderableManager::Builder::Success, "triangle renderable creation failed");
        scene_->addEntity(object_);
    }
    void draw(std::uint64_t attempt) {
        const profile::Context context{attempt, attempt, 0, 0, 0, recorder_.session_id()};
        bridge_->update(context);
        renderer_->setProfilerContext(utils::profiling::getContext());
        for (int retry = 0; retry < 16; ++retry) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_);
            renderer_->endFrame();
            engine_.flushAndWait();
            return;
        }
        throw std::runtime_error("diagnostic fixture frame repeatedly rejected");
    }
    std::vector<std::uint8_t> pixels() {
        std::vector<std::uint8_t> result(static_cast<std::size_t>(kSize) * kSize * 4);
        std::atomic_bool completed{false};
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(result.data(), result.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &completed));
        engine_.flushAndWait();
        require(completed.load(), "diagnostic fixture readback incomplete");
        return result;
    }
    profile::Snapshot capture(std::uint64_t first) {
        recorder_.begin_capture();
        if (engine_.getBackend() == filament::Engine::Backend::VULKAN) {
            utils::profiling::Context context{};
            context.capture_id = recorder_.session_id();
            static_cast<filament::backend::VulkanPlatform*>(engine_.getPlatform())
                ->requestGpuDiagnosticsSnapshot(context);
        }
        for (std::uint64_t i = first; i < first + 12; ++i) draw(i);
        auto result = recorder_.snapshot();
        recorder_.set_active(false);
        return result;
    }
    TraceCapabilities capabilities() const { return bridge_->capabilities(); }
    std::uintptr_t view_id() const { return reinterpret_cast<std::uintptr_t>(view_); }
private:
    filament::Engine& engine_;
    MeshStore meshes_;
    profile::Recorder recorder_;
    std::unique_ptr<ProfileBridge> bridge_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::Material* material_ = nullptr;
    filament::MaterialInstance* instance_ = nullptr;
    utils::Entity cameraEntity_, object_;
};
} // namespace

CaseResult run_gpu_diagnostics_case(const CaseContext& ctx) {
    if (!requested()) return CaseResult::Skip;
    try {
        auto& backend = static_cast<FilamentBackend&>(ctx.backend);
        require(backend.init_pipelines(ctx.config).has_value(), "backend pipeline initialization failed");
        auto& engine = *backend.debug_render_context().engine;
        DiagnosticFixture fixture(engine);
        fixture.init();
        for (std::uint64_t i = 1; i <= 3; ++i) fixture.draw(i);
        const auto off = fixture.pixels();
        std::size_t visible_pixels = 0;
        for (std::size_t i = 0; i < off.size(); i += 4) {
            if (std::max(off[i], off[i + 1]) > 20) ++visible_pixels;
        }
        require(visible_pixels > 100 && off[0] == 0 && off[1] == 0 && off[2] == 0,
            "independent triangle/background color oracle failed");
        const auto snapshot = fixture.capture(101);
        const auto on = fixture.pixels();
        require(off == on, "recording changed actual triangle pixels");
        const auto capabilities = fixture.capabilities();
        std::size_t available = 0, unavailable = 0;
        bool oracle = false;
        for (const auto& sample : snapshot.gpu_samples) {
            if (sample.context.view_id != fixture.view_id()) continue;
            if (sample.pipeline_statistics_status != profile::SampleStatus::Available) {
                ++unavailable;
                require(!sample.pipeline_statistics_reason.empty(), "missing query-unavailable reason");
                continue;
            }
            ++available;
            if (sample.command_count != 1 || sample.primitive_count != 1 ||
                    sample.fragment_shader_invocations < 64) continue;
            require(sample.input_assembly_vertices == 3 && sample.input_assembly_primitives == 1 &&
                    sample.vertex_shader_invocations == 3 && sample.clipping_invocations == 1 &&
                    sample.clipping_primitives == 1, "native query differs from unclipped triangle oracle");
            oracle = true;
            std::printf("[smoke_gpu_diagnostics] pass=%s commands=%llu IA=%llu/%llu VS=%llu clip=%llu/%llu FS=%llu\n",
                sample.name.c_str(), static_cast<unsigned long long>(sample.command_count),
                static_cast<unsigned long long>(sample.input_assembly_vertices),
                static_cast<unsigned long long>(sample.input_assembly_primitives),
                static_cast<unsigned long long>(sample.vertex_shader_invocations),
                static_cast<unsigned long long>(sample.clipping_invocations),
                static_cast<unsigned long long>(sample.clipping_primitives),
                static_cast<unsigned long long>(sample.fragment_shader_invocations));
        }
        require(capabilities.pipeline_statistics_enabled ? oracle : available == 0 && unavailable > 0,
            "actual invocation coverage or explicit unsupported coverage missing");
        std::set<std::uint64_t> pipelines;
        std::size_t statistics = 0, uses = 0, losses = 0;
        for (const auto& event : snapshot.pipeline_events) {
            if (event.kind == profile::PipelineEventKind::Pipeline) pipelines.insert(event.pipeline_id);
            if (event.kind == profile::PipelineEventKind::Statistic) ++statistics;
            if (event.kind == profile::PipelineEventKind::Loss) ++losses;
        }
        for (const auto& event : snapshot.pipeline_events) {
            if (event.kind != profile::PipelineEventKind::Use) continue;
            ++uses;
            require(event.pipeline_id && event.program_id && event.pass_id,
                "native pipeline use lacks identity");
            require(pipelines.contains(event.pipeline_id) || losses > 0,
                "native pipeline use lacks retained compiler metadata or explicit loss");
        }
        if (capabilities.executable_info_enabled) {
            require(uses > 0 && ((!pipelines.empty() && statistics > 0) || losses > 0),
                "compiler metadata replay after inactive warmup is missing");
        } else require(uses == 0 && pipelines.empty(), "unsupported compiler capture fabricated metadata");
        const auto next = fixture.capture(1001);
        require(!next.gpu_samples.empty() && std::ranges::all_of(next.gpu_samples, [&](const auto& sample) {
            return sample.context.capture_id == next.session_id && sample.context.attempt_id >= 1001;
        }), "new capture imported stale query identities");
        if (capabilities.executable_info_enabled && !pipelines.empty()) {
            require(std::ranges::any_of(next.pipeline_events, [](const auto& event) {
                return event.kind == profile::PipelineEventKind::Pipeline;
            }), "new capture did not replay cached compiler metadata");
        }
        if (const char* output = std::getenv("HEAVEN_SMOKE_GPU_DIAGNOSTICS_OUTPUT")) {
            profile::ReportWriter writer;
            require(writer.submit(snapshot, std::filesystem::path(output)), "diagnostic export rejected");
            writer.wait();
            require(writer.status().state == profile::ReportState::Complete, "diagnostic export failed");
        }
        std::printf("[smoke_gpu_diagnostics] actual=%zu unavailable=%zu compiler=%zu statistics=%zu uses=%zu loss=%zu pixels=exact: PASS\n",
            available, unavailable, pipelines.size(), statistics, uses, losses);
        return CaseResult::Pass;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[smoke_gpu_diagnostics] FAIL: %s\n", error.what());
        return CaseResult::Fail;
    }
}
} // namespace heaven::smoke
