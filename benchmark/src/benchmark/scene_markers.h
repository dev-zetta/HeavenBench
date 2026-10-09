/*
 * HeavenBench — Scene markers (clip.track scene-start times)
 *
 * Loads the ordered scene start-time markers from clip.track and answers
 * "which scene index is active at playback time t". Pure CPU: depends only on
 * the standard library and the header-only xml0 parser, with no Filament, SDL,
 * or ImGui dependency, so it compiles into the host-runnable test target.
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_SCENE_MARKERS_H_
#define HEAVENBENCH_SRC_BENCHMARK_SCENE_MARKERS_H_

#include "engine/asset_file_system.h"

#include <optional>
#include <string>
#include <vector>

namespace heaven {

class AssetUsageRecorder;

// Loaded representation of clip.track's first parameter's keys.
struct SceneMarkers {
    std::vector<float> times;  // non-decreasing scene start times, seconds

    [[nodiscard]] size_t count() const { return times.size(); }

    // Scene_Index: index of the latest marker whose time <= t, or 0 when t
    // precedes the first marker. Returns 0 for an empty
    // marker list (defensive; load failure is handled before a run starts).
    [[nodiscard]] size_t scene_index_at(float t) const;
};

// Parse clip.track in track_dir. The first <parameter> supplies the markers;
// its scene-start keys may be nested one level (clip.track's <keys_n>) or be
// direct <key> children. Returns std::nullopt (with a logged reason) when the
// file is missing, unparseable, or has zero markers.

// When `rec` is non-null (the used-asset packer), the clip.track file is
// recorded as a used.track asset once it is successfully opened and parsed
// When null (every normal render run) behavior is
// byte-for-byte unchanged.
[[nodiscard]] std::optional<SceneMarkers>
load_scene_markers(const std::string& track_dir,
                   AssetUsageRecorder* rec = nullptr,
                   AssetFileSystemRef assets = std::nullopt);

// Pure, separately testable: ensure non-decreasing order.
// Returns a sorted permutation of the input.
[[nodiscard]] std::vector<float> normalize_marker_times(std::vector<float> times);

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_SCENE_MARKERS_H_
