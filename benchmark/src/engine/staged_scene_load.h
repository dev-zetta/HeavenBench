/* HeavenBench — CPU scene staging and owner-thread upload handoff. */
#ifndef HEAVENBENCH_SRC_ENGINE_STAGED_SCENE_LOAD_H_
#define HEAVENBENCH_SRC_ENGINE_STAGED_SCENE_LOAD_H_

#include "engine/loading_progress.h"
#include "engine/scene_rhi.h"

namespace heaven {

// CPU worker only. The material database and optional collision world must be
// private to this load, and the borrowed asset namespace must remain alive and
// immutable until the worker has joined. Only one loader may run at a time:
// this function serializes other staged loads, but ordinary load_scene_rhi
// callers must not execute concurrently with it.
//
// The returned GPU-shaped metadata contains synthetic handles, not resources.
// Do not render it or pass those handles to a real backend's release_mesh.
// Material resolution uses nonzero texture sentinels; clear_resolved_cache()
// is required before resolving materials against the real backend.
[[nodiscard]] Result<RhiScene> stage_scene_load(
        const AppConfig& config, RhiMaterialDatabase& materials,
        const std::string& cache_file, CameraCollision* collision = nullptr,
        AssetFileSystemRef assets = std::nullopt,
        const LoadingProgressCallback& progress = {});

// Owner/UI thread only, outside a render pass. The callback is invoked between
// whole mesh uploads so the caller can service SDL/lifecycle events or throw
// to cancel. A single upload is not a hard millisecond-bounded operation.
// All new actual owners are retired on error/cancellation, leaving the staged
// scene untouched. Success installs only real handles and preserves authored
// path, caster policy and hidden-surface decisions. The caller still owns final
// scene/material/context commit and retirement of its previous live scene.
[[nodiscard]] Result<void> upload_staged_scene(
        RhiScene& scene, IRhiBackend& backend, const AppConfig& config,
        const LoadingProgressCallback& progress = {});

} // namespace heaven

#endif
