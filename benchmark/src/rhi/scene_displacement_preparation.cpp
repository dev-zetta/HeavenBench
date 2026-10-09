#include "benchmark/microprofiler.h"
#include "rhi/scene_displacement_preparation.h"
#include "rhi/displacement_surface_preparer.h"
#include "rhi/materials_data.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"
#include "rhi/prepared_displacement_cache.h"
#include "engine/scene_rhi.h"
#include "engine/displacement_surface_priority.h"
#include "engine/window_material_ref.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <unordered_set>

namespace heaven {
Result<void> prepare_scene_displacement(filament::Engine& engine,
        MeshStore& meshes, TextureStore& textures, const AppConfig& config,
        RhiScene& scene, const MaterialResolver& resolve,
        const LoadingProgressCallback& progress) {
    struct ReleasePacking {
        MeshStore& meshes;
        ~ReleasePacking() { meshes.discard_original_packing(); }
    } release{meshes};
    if (!config.tessellation.enabled || config.tessellation.scale <= 0) return {};
    std::unique_ptr<HeightSampleBatch> height_batch;
    std::string height_batch_failure;
    std::unordered_set<int> deforming;
    for (const auto& object : scene.animated) deforming.insert(object.mesh_index);
    for (const auto& cloth : scene.cloths) deforming.insert(cloth.mesh_index);
    std::vector<std::vector<glm::mat4>> sharing(scene.cpu_meshes.size());
    for (const auto& instance : scene.instances)
        if (instance.mesh_index >= 0 && std::cmp_less(instance.mesh_index, sharing.size()))
            sharing[instance.mesh_index].push_back(instance.transform);
    // Bound the actual authored population, including every sharing instance.
    // The scene's exported geometry totals continue to count unique meshes.
    constexpr uint64_t maximum_rendered_added_triangles = 27'000'000;
    constexpr uint64_t maximum_scene_patch_evaluations = 128'000'000;
    constexpr uint32_t maximum_surface_triangles = 3'000'000;
    constexpr uint32_t maximum_refinement_attempts = 8;
    constexpr uint64_t maximum_scene_sample_vertices = 4'000'000;
    constexpr uint64_t maximum_frozen_source_bytes = 1024ull * 1024 * 1024;
    constexpr uint64_t source_vertex_bytes = sizeof(BoundedDisplacementVertex) + sizeof(HeightSampleValue) +
        2 * sizeof(filament::math::float3) + 2 * sizeof(filament::math::float2) +
        sizeof(filament::math::short4) + sizeof(filament::math::float4) + sizeof(mesh_displacement::Footprint);
    uint64_t added_triangles = 0, rendered_added_triangles = 0, changed = 0, fallbacks = 0;
    uint64_t patch_evaluations = 0;
    uint64_t sampled_vertex_requests = 0, frozen_source_bytes = 0;
    uint32_t refinement_attempts = 0;
    const auto started = std::chrono::steady_clock::now();
    struct PendingSurface {
        size_t mesh, surface, source;
        PreparedDisplacementSurface sampled;
        DisplacementSurfacePriority priority;
    };
    std::vector<PendingSurface> pending;
    const size_t count = std::min(scene.cpu_meshes.size(), scene.gpu_meshes.size());
    for (size_t m = 0; m < count; ++m) {
        if (progress) progress("Sampling displacement " + std::to_string(m + 1) +
            "/" + std::to_string(count), 0.4f * static_cast<float>(m) / static_cast<float>(count));
        const auto& source_mesh = scene.cpu_meshes[m];
        auto& gpu = scene.gpu_meshes[m];
        if (deforming.contains(static_cast<int>(m)) || sharing[m].empty()) continue;
        // Upload records source indices explicitly, including omitted empty
        // surfaces and repeated names.
        for (size_t s = 0; s < gpu.surfaces.size(); ++s) {
            auto& surface = gpu.surfaces[s];
            if (!surface.index_count) continue;
            const auto source_index = meshes.source_surface_index(surface.handle);
            if (!source_index || *source_index >= source_mesh.surfaces.size())
                return std::unexpected(Error{"Static preparation source surface is missing"});
            const auto* source = &source_mesh.surfaces[*source_index];
            const auto material = resolve(gpu.path, surface.name);
            auto* height = textures.get(material.height);
            const auto descriptor = geometry_displacement(material, height != nullptr,
                config.tessellation.scale, config.tessellation.distance);
            if (!descriptor.enabled || material.hidden) continue;
            if (window_ref::is_window_material(material.material_name, material.parent_chain)) {
                ++fallbacks;
                std::printf("  Bounded displacement: %s/%s retained (manual window height route)\n",
                    gpu.name.c_str(), surface.name.c_str());
                continue;
            }
            if (!height_batch && height_batch_failure.empty()) {
                auto batch = HeightSampleBatch::create(engine,
                    {k_heaven_height_sample_filamat, k_heaven_height_sample_filamat_size});
                if (batch) height_batch = std::move(*batch);
                else height_batch_failure = batch.error().message;
            }
            if (!height_batch) {
                ++fallbacks;
                std::printf("  Bounded displacement: %s/%s retained (native sampler: %s)\n",
                    gpu.name.c_str(), surface.name.c_str(), height_batch_failure.c_str());
                continue;
            }
            const uint64_t source_bytes = source->vertices.size() * source_vertex_bytes +
                source->indices.size() * sizeof(uint32_t);
            if (source->vertices.size() > maximum_scene_sample_vertices - sampled_vertex_requests ||
                    source_bytes > maximum_frozen_source_bytes - frozen_source_bytes) {
                ++fallbacks;
                std::printf("  Bounded displacement: %s/%s retained (scene native-sampling/source-byte budget)\n",
                    gpu.name.c_str(), surface.name.c_str());
                continue;
            }
            auto packed = meshes.take_original_packing(surface.handle, source->vertices, source->indices);
            if (!packed) {
                ++fallbacks;
                std::printf("  Bounded displacement: %s/%s retained (%s)\n", gpu.name.c_str(),
                    surface.name.c_str(), packed.error().message.c_str());
                continue;
            }
            DisplacementSurfacePreparer preparer(*height_batch);
            StaticDisplacementSurfaceRequest request;
            request.vertices = source->vertices;
            request.indices = source->indices;
            request.uploaded_source = &*packed;
            request.instance_transforms = sharing[m];
            request.static_vertices = true;
            request.selected_height = height;
            request.displacement = descriptor;
            std::copy(std::begin(material.base_transform), std::end(material.base_transform),
                request.base_transform.begin());
            sampled_vertex_requests += source->vertices.size();
            auto sampled = preparer.sample_source(request);
            if (!sampled || !sampled->source_ready) {
                ++fallbacks;
                const std::string reason = sampled ? sampled->reason : sampled.error().message;
                std::printf("  Bounded displacement: %s/%s retained (%s)\n", gpu.name.c_str(),
                    surface.name.c_str(), reason.c_str());
                continue;
            }
            auto priority = analyze_displacement_surface_priority({sampled->source_vertices,
                sampled->source_triangles, sampled->uniform_scale, sampled->displacement.amplitude});
            if (!priority) {
                ++fallbacks;
                std::printf("  Bounded displacement: %s/%s retained (priority: %s)\n",
                    gpu.name.c_str(), surface.name.c_str(), priority.error().message.c_str());
                continue;
            }
            frozen_source_bytes += source_bytes;
            pending.push_back({m, s, *source_index, std::move(*sampled), *priority});
        }
    }
    // Prioritize total displaced angular energy in the actual rendered
    // population. A repeated source affects all borrowers, while one target
    // serves them together. Source order breaks ties deterministically.
    std::stable_sort(pending.begin(), pending.end(), [&](const auto& a, const auto& b) {
        return a.priority.priority_energy * sharing[a.mesh].size() >
            b.priority.priority_energy * sharing[b.mesh].size();
    });
    const double sampling_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("Bounded displacement: %zu sampled candidates in %.3f s, %llu charged vertex requests "
        "(ceiling %llu), %llu frozen array bytes (ceiling %llu)\n",
        pending.size(), sampling_seconds, static_cast<unsigned long long>(sampled_vertex_requests),
        static_cast<unsigned long long>(maximum_scene_sample_vertices),
        static_cast<unsigned long long>(frozen_source_bytes),
        static_cast<unsigned long long>(maximum_frozen_source_bytes));
    for (auto& candidate : pending) {
        if (progress) progress("Preparing displaced surfaces", 0.4f +
            0.6f * static_cast<float>(&candidate - pending.data()) / static_cast<float>(pending.size()));
        const auto m = candidate.mesh;
        auto& gpu = scene.gpu_meshes[m];
        auto& surface = gpu.surfaces[candidate.surface];
        const auto* source = &scene.cpu_meshes[m].surfaces[candidate.source];
        const uint64_t population = sharing[m].size();
        const auto old_triangles = surface.index_count / 3;
        const uint64_t available_added =
            (maximum_rendered_added_triangles - rendered_added_triangles) / population;
        const uint64_t limit = std::min({uint64_t(maximum_surface_triangles),
            uint64_t(old_triangles) * 128, uint64_t(old_triangles) + available_added});
        std::printf("  Displacement priority: %s/%s rank %.9g, per-triangle score %.9g, energy %.6g, "
            "rotation RMS %.3f deg, %llu instances\n", gpu.name.c_str(), surface.name.c_str(),
            candidate.priority.priority_energy * static_cast<double>(population),
            candidate.priority.priority_per_source_triangle, candidate.priority.priority_energy,
            candidate.priority.height_rotation_rms_degrees,
            static_cast<unsigned long long>(population));
        if (candidate.priority.priority_energy <= 1e-16 ||
                limit < uint64_t(old_triangles) + std::max<uint64_t>(1, old_triangles / 2) ||
                patch_evaluations >= maximum_scene_patch_evaluations ||
                refinement_attempts >= maximum_refinement_attempts) {
            ++fallbacks;
            std::printf("  Bounded displacement: %s/%s retained (scene preparation budget "
                "or no displaced angular energy)\n", gpu.name.c_str(), surface.name.c_str());
            candidate.sampled = {};
            continue;
        }
        DisplacementPreparationPolicy policy;
        policy.geometry.maximum_triangles = static_cast<uint32_t>(limit);
        // Keep small targets within the original working-set estimate; only
        // dense targets need the larger estimate validated for the roof.
        policy.geometry.maximum_work_bytes = limit > 1'500'000
            ? 2ull * 1024 * 1024 * 1024 : 1024ull * 1024 * 1024;
        policy.geometry.maximum_patch_evaluations = 96'000'000;
        policy.geometry.maximum_patch_evaluations = std::min(
            policy.geometry.maximum_patch_evaluations,
            maximum_scene_patch_evaluations - patch_evaluations);
        DisplacementSurfacePreparer preparer(*height_batch, policy);
        ++refinement_attempts;
        // Native sampling has already frozen every input. Cache I/O and the
        // pure CPU target builder need no Engine/texture access. The owner
        // continues drawing and pumping events until the worker is joined.
        const auto cache_directory = default_prepared_displacement_cache_directory(
            config.tessellation.cache_file);
        std::atomic<int> phase = 0;
        auto* trace_recorder = profile::recorder();
        const auto trace_context = trace_recorder ? trace_recorder->frame_context() : profile::Context{};
        const auto trace_flow = trace_recorder && trace_recorder->active() ? profile::Recorder::steady_now_ns() : 0;
        if (trace_flow) trace_recorder->flow(trace_flow, profile::FlowPhase::Begin, "Displacement refinement", trace_context);
        auto refine = [sampled = std::move(candidate.sampled), &preparer, policy,
                       cache_directory = cache_directory, &phase, trace_context,
                       trace_recorder, trace_flow]() mutable -> Result<PreparedDisplacementSurface> {
            profile::ContextScope inherited_context(trace_context);
            if (trace_flow) {
                trace_recorder->set_thread_name("Displacement worker");
                trace_recorder->flow(trace_flow, profile::FlowPhase::Step, "Displacement refinement", trace_context);
            }
            profile::Scope scope("Bounded displacement refinement", "Tessellation");
            auto cached = load_prepared_displacement_cache(cache_directory, sampled, policy);
            if (!cached) std::fprintf(stderr, "Prepared displacement cache read failed: %s\n",
                cached.error().message.c_str());
            if (cached && *cached) {
                std::printf("  Prepared displacement cache: hit\n");
                return std::move(sampled);
            }
            phase.store(1);
            auto result = preparer.refine_source(std::move(sampled));
            if (result && result->ready()) {
                phase.store(2);
                if (auto saved = store_prepared_displacement_cache(cache_directory, *result, policy); !saved)
                    std::fprintf(stderr, "Prepared displacement cache write failed: %s\n",
                        saved.error().message.c_str());
            }
            return result;
        };
        Result<PreparedDisplacementSurface> prepared;
        if (progress) {
            auto future = std::async(std::launch::async, std::move(refine));
            std::exception_ptr cancelled;
            while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                const auto step = phase.load();
                const std::string status = std::string(step == 0 ? "Reading displacement cache: " :
                    step == 1 ? "Refining displacement: " : "Saving displacement cache: ") +
                    gpu.name + "/" + surface.name;
                try { progress(status, -1.0f); }
                catch (...) { if (!cancelled) cancelled = std::current_exception(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            prepared = future.get();
            // The worker no longer borrows the preparer/native batch before
            // cancellation unwinds their owner-thread scope.
            if (cancelled) std::rethrow_exception(cancelled);
        } else prepared = refine();
        if (trace_flow) trace_recorder->flow(trace_flow, profile::FlowPhase::End, "Displacement refinement", trace_context);
        // Failed targets may not expose counters. Charge their entire quota
        // conservatively, so unsupported sources cannot prolong startup.
        patch_evaluations += prepared && prepared->geometry.metrics.patch_evaluations
            ? prepared->geometry.metrics.patch_evaluations : policy.geometry.maximum_patch_evaluations;
        if (!prepared || !prepared->ready()) {
            ++fallbacks;
            const std::string reason = prepared ? prepared->reason : prepared.error().message;
            std::printf("  Bounded displacement: %s/%s retained (%s)\n", gpu.name.c_str(),
                surface.name.c_str(), reason.c_str());
            continue;
        }
        const bool moves = std::any_of(prepared->geometry.nodes.begin(),
            prepared->geometry.nodes.end(), [](const auto& node) {
                return glm::dot(node.correction, node.correction) > 1e-20;
            });
        if (!prepared->geometry.metrics.accepted_components || !moves) {
            ++fallbacks;
            std::printf("  Bounded displacement: %s/%s retained (no accepted smoothing)\n",
                gpu.name.c_str(), surface.name.c_str());
            continue;
        }
        const uint64_t next_triangles = prepared->geometry.triangles.size();
        const uint64_t next_added = next_triangles > old_triangles ? next_triangles - old_triangles : 0;
        if (next_added > (maximum_rendered_added_triangles - rendered_added_triangles) / population) {
            ++fallbacks;
            std::printf("  Bounded displacement: %s/%s retained (instance-weighted geometry budget)\n",
                gpu.name.c_str(), surface.name.c_str());
            continue;
        }
        auto vertex_count = meshes.replace_displacement_surface(surface, source->vertices, *prepared);
        if (!vertex_count) return std::unexpected(vertex_count.error());
        const auto new_triangles = surface.index_count / 3;
        added_triangles += next_added;
        rendered_added_triangles += next_added * population;
        scene.total_triangles = scene.total_triangles - old_triangles + new_triangles;
        scene.total_vertices = scene.total_vertices - source->vertices.size() + *vertex_count;
        gpu.bbox_min = glm::min(gpu.bbox_min, surface.bbox_min);
        gpu.bbox_max = glm::max(gpu.bbox_max, surface.bbox_max);
        ++changed;
        const auto& metrics = prepared->geometry.metrics;
        std::printf("  Bounded displacement: %s/%s %u -> %u triangles, "
            "movement %.3f mm, normal error %.3f deg, %s\n",
            gpu.name.c_str(), surface.name.c_str(), old_triangles, new_triangles,
            metrics.maximum_sampled_movement * 1000,
            metrics.maximum_sampled_normal_error_degrees,
            prepared->requested_quality_met ? "quality met" : "budget limited; quality unmet");
        std::printf("    Components: %llu accepted, %llu rejected (%llu original triangles retained), "
            "sampled quality misses %llu (%.3f%% of evaluated area)\n",
            static_cast<unsigned long long>(metrics.accepted_components),
            static_cast<unsigned long long>(metrics.rejected_components),
            static_cast<unsigned long long>(metrics.rejected_source_triangles),
            static_cast<unsigned long long>(metrics.sampled_quality_failures),
            metrics.failing_area_fraction * 100);
        for (const auto& rejected : prepared->geometry.component_fallbacks)
            std::printf("    Retained %zu source triangles: %s\n",
                rejected.source_parents.size(), rejected.diagnostic.c_str());
    }
    std::printf("Bounded displacement: %llu prepared surfaces, %llu fallbacks, "
        "%llu added unique triangles, %llu added rendered triangles (ceiling %llu)\n",
        static_cast<unsigned long long>(changed), static_cast<unsigned long long>(fallbacks),
        static_cast<unsigned long long>(added_triangles),
        static_cast<unsigned long long>(rendered_added_triangles),
        static_cast<unsigned long long>(maximum_rendered_added_triangles));
    std::printf("Bounded displacement preparation: %u target attempts, %llu charged patch evaluations "
        "(ceiling %llu), %.3f s total\n", refinement_attempts,
        static_cast<unsigned long long>(patch_evaluations),
        static_cast<unsigned long long>(maximum_scene_patch_evaluations),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    if (progress) progress("Scene ready", 1.0f);
    return {};
}
} // namespace heaven
