/*
 * HeavenBench — Deterministic real-scene CPU benchmark phase
 */
#include "benchmark/cpu_benchmark.h"

#include "engine/camera.h"
#include "engine/camera_collision.h"
#include "engine/camera_path.h"
#include "engine/cloth_sim.h"
#include "engine/material_rhi.h"
#include "engine/null_backend.h"
#include "engine/scene_rhi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <barrier>
#include <numeric>
#include <thread>
#include <vector>

namespace heaven {
namespace {

constexpr float kCpuFixedDt =
    1.0f / static_cast<float>(kCpuSimulationRate);
constexpr int   kWarmupTicks = 120;

Result<void> prepare_cpu_assets(const AppConfig& config,
                                AssetFileSystemRef& assets,
                                std::optional<AssetFileSystem>& owned_assets) {
    if (!assets) {
        auto opened = AssetFileSystem::open(config.data_dir);
        if (!opened) return std::unexpected(opened.error());
        owned_assets.emplace(std::move(*opened));
        assets = std::cref(*owned_assets);
    }
    auto paths = PathResolver::create(config.data_dir);
    if (!paths) return std::unexpected(paths.error());
    const std::string source = config.node_file.empty()
        ? paths->world_path() : config.node_file;
    auto stream = open_asset_stream(source, assets);
    if (!stream) return std::unexpected(stream.error());
    if ((*stream)->size() == 0) {
        return std::unexpected(Error{"CPU benchmark scene file is empty: " + source});
    }
    return {};
}

Result<void> validate_cpu_scene(const RhiScene& scene) {
    // The general scene loader is deliberately best-effort. A benchmark must
    // not turn missing meshes or an empty world into an exceptionally high
    // score for ticking only its camera and weather state.
    if (scene.cpu_meshes.empty() || scene.instances.empty() || scene.total_triangles == 0) {
        return std::unexpected(Error{
            "CPU benchmark scene has no usable mesh instances or triangles"});
    }
    return {};
}

struct CpuWorldRunner {
    RhiScene& scene;
    CameraCollision& collision;
    const CameraPath& path;
    WeatherState weather;
    Camera camera;
    float simulation_time = 0.0f;
    std::vector<int> dirty_instances;
    std::vector<Vertex> cloth_vertices;
    double checksum = 0.0;

    CpuWorldRunner(RhiScene& scene_in, CameraCollision& collision_in,
                   const CameraPath& path_in, const WeatherConfig& weather_config)
        : scene(scene_in),
          collision(collision_in),
          path(path_in),
          weather(weather_config) {}

    void tick() {
        simulation_time += kCpuFixedDt;
        const float path_time = path.duration > 0.0f
            ? std::fmod(simulation_time, path.duration)
            : 0.0f;
        path.apply(path_time, camera);

        dirty_instances.clear();
        std::map<int, float> group_offsets;

        for (auto& island : scene.floating_islands) {
            const float phase = simulation_time * island.speed;
            const float z_offset =
                island.amplitude - std::cos(phase) * island.amplitude;
            const glm::mat4 transform =
                glm::translate(glm::mat4(1.0f),
                               glm::vec3(0.0f, 0.0f, z_offset)) *
                island.base_transform;
            if (scene.instances[island.instance_index].transform != transform) {
                scene.instances[island.instance_index].transform = transform;
                dirty_instances.push_back(island.instance_index);
            }
            group_offsets[island.group] = z_offset;
        }

        for (auto& stone : scene.stone_anims) {
            const float dummy_z =
                std::sin(simulation_time * stone.speed + stone.seed) *
                stone.amplitude * std::sin(stone.seed);
            glm::mat4 transform = stone.base_transform;
            transform[3].z = dummy_z + stone.child_local_z;
            if (scene.instances[stone.instance_index].transform != transform) {
                scene.instances[stone.instance_index].transform = transform;
                dirty_instances.push_back(stone.instance_index);
            }
        }

        collision.update_moving(
            scene.instances, dirty_instances, camera.position);

        if (scene.bridge_system.update(kCpuFixedDt, group_offsets)) {
            scene.bridge_system.apply_to_instances(
                scene.instances, &dirty_instances);
        }

        std::sort(dirty_instances.begin(), dirty_instances.end());
        dirty_instances.erase(
            std::unique(dirty_instances.begin(), dirty_instances.end()),
            dirty_instances.end());

        for (const auto& animated : scene.animated) {
            const float frame = animated.smesh.num_frames > 0
                ? std::fmod(simulation_time * animated.anim_speed,
                            static_cast<float>(animated.smesh.num_frames))
                : 0.0f;
            const auto matrices =
                animated.smesh.bone_matrices_interpolated(frame);
            if (!matrices.empty()) {
                checksum += static_cast<double>(matrices.front()[3].x) +
                            static_cast<double>(matrices.back()[3].z);
            }
        }

        for (auto& cloth : scene.cloths) {
            if (!step_cloth(cloth, kCpuFixedDt, simulation_time)) continue;
            emit_vertices(cloth, cloth_vertices);
            if (!cloth_vertices.empty()) {
                checksum +=
                    static_cast<double>(cloth_vertices.front().px) +
                    static_cast<double>(cloth_vertices.back().pz);
            }
        }

        weather.advance(kCpuFixedDt);
        checksum += static_cast<double>(camera.position.x) +
                    static_cast<double>(camera.position.y) +
                    static_cast<double>(dirty_instances.size()) * 0.001;
    }
};

} // namespace

Result<CpuBenchmarkMetrics> run_cpu_benchmark(
    const AppConfig& config,
    RhiMaterialDatabase& materials,
    const std::string& cache_dir,
    const CameraPath& camera_path,
    double measurement_seconds,
    AssetFileSystemRef assets) {
    if (!(measurement_seconds > 0.0) || !std::isfinite(measurement_seconds)) {
        return std::unexpected(
            Error{"CPU benchmark measurement duration must be positive"});
    }

    std::optional<AssetFileSystem> owned_assets;
    if (auto prepared = prepare_cpu_assets(config, assets, owned_assets); !prepared) {
        return std::unexpected(prepared.error());
    }
    NullBackend null_backend;
    CameraCollision collision;
    Result<RhiScene> loaded = load_scene_rhi(
        config, null_backend, materials, cache_dir, &collision, assets);
    if (!loaded) return std::unexpected(loaded.error());
    if (auto valid = validate_cpu_scene(*loaded); !valid) return std::unexpected(valid.error());

    RhiScene scene = std::move(*loaded);
    for (auto& cloth : scene.cloths) {
        cloth.sim_hz = config.cloth_sim_hz;
    }

    CpuWorldRunner runner(scene, collision, camera_path, config.weather);
    for (int i = 0; i < kWarmupTicks; ++i) runner.tick();

    using Clock = std::chrono::steady_clock;
    std::vector<double> tick_times_ms;
    tick_times_ms.reserve(100000);

    const auto measurement_start = Clock::now();
    double elapsed_seconds = 0.0;
    do {
        const auto tick_start = Clock::now();
        runner.tick();
        const auto tick_end = Clock::now();
        tick_times_ms.push_back(
            std::chrono::duration<double, std::milli>(
                tick_end - tick_start).count());
        elapsed_seconds =
            std::chrono::duration<double>(
                tick_end - measurement_start).count();
    } while (elapsed_seconds < measurement_seconds);

    CpuBenchmarkMetrics result;
    result.scene_meshes = scene.cpu_meshes.size();
    result.scene_instances = scene.instances.size();
    result.timing = summarize_timings(tick_times_ms);
    result.updates_per_second = elapsed_seconds > 0.0
        ? static_cast<double>(tick_times_ms.size()) / elapsed_seconds
        : 0.0;
    result.checksum = runner.checksum;
    return result;
}

Result<CpuStressMetrics> run_cpu_stress_benchmark(
    const AppConfig& config,
    RhiMaterialDatabase& materials,
    const std::string& cache_dir,
    const CameraPath& camera_path,
    AssetFileSystemRef assets) {
    CpuStressMetrics result;
    result.enabled = config.cpu_stress.enabled;
    if (!result.enabled) return result;
    if (!(config.cpu_stress.duration_seconds > 0.0f) ||
        !std::isfinite(config.cpu_stress.duration_seconds)) {
        return std::unexpected(
            Error{"CPU stress duration must be positive"});
    }

    // Every world is loaded before threads start. The local mount, or caller's
    // borrowed mount, stays alive until all workers join and this call returns.
    std::optional<AssetFileSystem> owned_assets;
    if (auto prepared = prepare_cpu_assets(config, assets, owned_assets); !prepared) {
        return std::unexpected(prepared.error());
    }
    const unsigned detected = std::max(1u, std::thread::hardware_concurrency());
    const int workers = std::clamp(
        config.cpu_stress.workers == 0
            ? static_cast<int>(detected)
            : config.cpu_stress.workers,
        1, 256);

    struct World {
        NullBackend backend;
        CameraCollision collision;
        RhiScene scene;
    };
    std::vector<std::unique_ptr<World>> worlds;
    worlds.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        auto world = std::make_unique<World>();
        Result<RhiScene> loaded = load_scene_rhi(
            config, world->backend, materials, cache_dir, &world->collision, assets);
        if (!loaded) return std::unexpected(loaded.error());
        if (auto valid = validate_cpu_scene(*loaded); !valid) return std::unexpected(valid.error());
        if (i == 0) {
            result.scene_meshes = loaded->cpu_meshes.size();
            result.scene_instances = loaded->instances.size();
        } else if (result.scene_meshes != loaded->cpu_meshes.size() ||
                   result.scene_instances != loaded->instances.size()) {
            return std::unexpected(Error{"CPU stress workers loaded different scene workloads"});
        }
        world->scene = std::move(*loaded);
        for (auto& cloth : world->scene.cloths)
            cloth.sim_hz = config.cloth_sim_hz;
        worlds.push_back(std::move(world));
    }

    result.workers = workers;
    result.duration_seconds = config.cpu_stress.duration_seconds;
    result.worker_updates_per_second.resize(static_cast<size_t>(workers));
    result.checksums.resize(static_cast<size_t>(workers));
    std::barrier start_barrier(workers);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            World& world = *worlds[static_cast<size_t>(i)];
            CpuWorldRunner runner(
                world.scene, world.collision, camera_path, config.weather);
            for (int tick = 0; tick < kWarmupTicks; ++tick) runner.tick();
            start_barrier.arrive_and_wait();

            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            uint64_t updates = 0;
            double elapsed = 0.0;
            do {
                runner.tick();
                ++updates;
                elapsed = std::chrono::duration<double>(
                    Clock::now() - start).count();
            } while (elapsed < config.cpu_stress.duration_seconds);

            result.worker_updates_per_second[static_cast<size_t>(i)] =
                elapsed > 0.0 ? static_cast<double>(updates) / elapsed : 0.0;
            result.checksums[static_cast<size_t>(i)] = runner.checksum;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    result.aggregate_updates_per_second = std::accumulate(
        result.worker_updates_per_second.begin(),
        result.worker_updates_per_second.end(), 0.0);
    return result;
}

} // namespace heaven
