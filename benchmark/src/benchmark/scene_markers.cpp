/*
 * HeavenBench — Scene markers (clip.track scene-start times)
 */
#include "benchmark/scene_markers.h"
#include "engine/asset_usage_recorder.h"
#include "engine/asset_file_system.h"
#include "xml0_parser.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace heaven {

// ── Scene-index selection ───────────────────────────────────────

size_t SceneMarkers::scene_index_at(float t) const {
    if (times.empty()) {
        return 0;
    }

    // upper_bound gives the first marker strictly greater than t; stepping back
    // one yields the latest marker whose time does not exceed t. When t precedes
    // the first marker the iterator is already at begin(), so the index is 0

    auto it = std::upper_bound(times.begin(), times.end(), t);
    if (it == times.begin()) {
        return 0;
    }

    size_t index = static_cast<size_t>((it - times.begin()) - 1);

    // Defensive clamp to [0, count()-1].
    if (index >= times.size()) {
        index = times.size() - 1;
    }
    return index;
}

// ── Marker normalization ────────────────────────────────────────

std::vector<float> normalize_marker_times(std::vector<float> times) {
    std::sort(times.begin(), times.end());
    return times;
}

// ── clip.track parsing helpers ──────────────────────────────────

namespace {

// Returns true when the node has at least one direct <key> child.
bool has_key_children(const XmlNode& node) {
    return std::ranges::any_of(node.children, [](const auto& child) {
        return child.tag == "key";
    });
}

// Locate the node that actually holds the scene-start keys. In clip.track the
// first <parameter type="track"> does not carry <key> children directly; its
// scene references live one level down in a <keys_n> sub-node (alongside the
// <keys_t>/<keys_w> sub-tracks we must not read). Other track parameters carry
// <key> children directly. Searching the parameter's subtree for the first node
// with direct <key> children handles both layouts and picks the scene-name keys
// because <keys_n> is the first child of the first parameter.
const XmlNode* find_key_holder(const XmlNode& node) {
    if (has_key_children(node)) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const XmlNode* found = find_key_holder(child)) {
            return found;
        }
    }
    return nullptr;
}

// Parse a float that may be empty/malformed without throwing. Returns false on
// failure so the caller can treat the track as unparseable.
bool parse_float(const std::string& str, float& out) {
    if (str.empty()) {
        out = 0.0f;
        return true;
    }
    try {
        size_t consumed = 0;
        out = std::stof(str, &consumed);
        return consumed > 0;
    } catch (...) {
        return false;
    }
}

} // namespace

// ── Public loader ───────────────────────────────────────────────

std::optional<SceneMarkers> load_scene_markers(const std::string& track_dir,
                                               AssetUsageRecorder* rec,
                                               AssetFileSystemRef assets) {
    namespace fs = std::filesystem;

    const fs::path clip_path = fs::path(track_dir) / "clip.track";
    const std::string clip_str = clip_path.string();

    // Missing file.
    if (!asset_exists(clip_str, assets)) {
        std::fprintf(stderr,
                     "Scene markers: clip.track not found at %s\n",
                     clip_str.c_str());
        return std::nullopt;
    }

    // xml0_parse_tree returns an empty root (empty tag) when the file cannot be
    // parsed as xml0.
    XmlNode root = xml0_parse_tree(clip_str.c_str(), assets);
    if (root.tag.empty()) {
        std::fprintf(stderr,
                     "Scene markers: failed to parse %s as xml0\n",
                     clip_str.c_str());
        return std::nullopt;
    }

    // The clip.track file was successfully opened and parsed as xml0: record it
    // as a used.track asset for the packer. No-op on normal
    // runs.
    if (rec) rec->record_used(clip_str);

    // The Scene_Markers come from the first <parameter>'s scene-start keys.
    const XmlNode* first_param = nullptr;
    for (const auto& child : root.children) {
        if (child.tag == "parameter") {
            first_param = &child;
            break;
        }
    }
    if (first_param == nullptr) {
        std::fprintf(stderr,
                     "Scene markers: no <parameter> element in %s\n",
                     clip_str.c_str());
        return std::nullopt;
    }

    const XmlNode* key_holder = find_key_holder(*first_param);
    if (key_holder == nullptr) {
        std::fprintf(stderr,
                     "Scene markers: first parameter in %s has no key children\n",
                     clip_str.c_str());
        return std::nullopt;
    }

    // Collect the time attribute of each scene-start key.
    std::vector<float> times;
    for (const auto& key : key_holder->children) {
        if (key.tag != "key") {
            continue;
        }
        float t = 0.0f;
        if (!parse_float(key.get("time"), t)) {
            std::fprintf(stderr,
                         "Scene markers: malformed key time \"%s\" in %s\n",
                         key.get("time").c_str(), clip_str.c_str());
            return std::nullopt;
        }
        times.push_back(t);
    }

    // Zero markers.
    if (times.empty()) {
        std::fprintf(stderr,
                     "Scene markers: zero markers extracted from %s\n",
                     clip_str.c_str());
        return std::nullopt;
    }

    SceneMarkers markers;
    markers.times = normalize_marker_times(std::move(times));
    return markers;
}

} // namespace heaven
