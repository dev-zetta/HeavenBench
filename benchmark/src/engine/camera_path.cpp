/*
 * HeavenBench — Camera path playback
 */
#include "engine/camera_path.h"
#include "engine/asset_usage_recorder.h"
#include "engine/asset_file_system.h"
#include "engine/number_parse.h"
#include "xml0_parser.h"

#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

namespace heaven {
namespace {

// ── Parse a single track file for position/rotation keys ────────

// One animated scalar channel. Besides the (time, value) pairs we keep the
// per-key Bezier handles and key type so playback can reproduce the original
// Tracker's cubic-spline interpolation rather than a straight lerp.

// `handles` is the authored vec4 (in_dx, in_dy, out_dx, out_dy): the incoming
// and outgoing tangent handles in (Δtime, Δvalue) relative to the key. `type`
// is the Unigine key type (0=CONSTANT, 1=LINEAR, 2=SMOOTH, 3=BREAK, 4=AUTO);
// SMOOTH/BREAK/AUTO use the Bezier path (see core/.../tracker_parameter.h
// get_track_value_float).
struct TrackKeys {
    std::vector<float>     times;
    std::vector<float>     values;
    std::vector<glm::vec4> handles;  // (in_dx, in_dy, out_dx, out_dy)
    std::vector<int>       types;    // KEY_* enum
};

// Non-throwing float parse; returns `fallback` when the field is empty or
// malformed, otherwise the parsed value. Well-formed input parses identically
// to std::stof, so valid track data yields the same numbers.
[[nodiscard]] float parse_float_or(const std::string& s, float fallback) {
    return parse_number_prefix<float>(s).value_or(fallback);
}

// Parse a "x y z w" attribute into a vec4 (missing components default to 0).
glm::vec4 parse_vec4(const std::string& str) {
    glm::vec4 v(0.0f);
    if (str.empty()) return v;
    std::sscanf(str.c_str(), "%f %f %f %f", &v.x, &v.y, &v.z, &v.w);
    return v;
}

// Evaluate a cubic Bezier defined by 4 control points whose X coordinates are
// monotonically non-decreasing, solving for the parameter u where X(u) == x,
// then returning Y(u). This mirrors the engine's bezier(t, v, time): a key
// segment's control points are
//   X = (t0, t0+out_dx0, t1+in_dx1, t1)   (clamped to [t0, t1])
//   Y = (v0, v0+out_dy0, v1+in_dy1, v1)
// so the value follows the authored tangent handles smoothly. X is monotonic
// over the clamped control points, so a short bisection inverts it robustly.
float bezier_solve(const glm::vec4& X, const glm::vec4& Y, float x) {
    auto cubic = [](const glm::vec4& c, float u) {
        const float mu = 1.0f - u;
        return mu * mu * mu * c.x + 3.0f * mu * mu * u * c.y +
               3.0f * mu * u * u * c.z + u * u * u * c.w;
    };

    const float x0 = X.x;
    const float x3 = X.w;
    if (x <= x0) return Y.x;
    if (x >= x3) return Y.w;

    // Bisection on u in [0, 1] to invert the (monotonic) time-Bezier. ~24
    // iterations give sub-millisecond time precision over any realistic span.
    float lo = 0.0f, hi = 1.0f;
    for (int iter = 0; iter < 24; ++iter) {
        const float mid = 0.5f * (lo + hi);
        if (cubic(X, mid) < x) lo = mid; else hi = mid;
    }
    return cubic(Y, 0.5f * (lo + hi));
}

TrackKeys parse_track_keys(const XmlNode& param_node) {
    TrackKeys keys;

    for (const auto& child : param_node.children) {
        if (child.tag == "key") {
            float t = 0.0f;
            float v = 0.0f;
            auto time_str = child.get("time");
            auto text = child.text;
            t = parse_float_or(time_str, t);
            v = parse_float_or(text, v);
            keys.times.push_back(t);
            keys.values.push_back(v);
            keys.handles.push_back(parse_vec4(child.get("handles")));
            // Default authored key type is SMOOTH (2) — the camera tracks omit
            // an explicit type on smooth keys and only tag the boundary reset
            // keys with type="1" (LINEAR). Honour an explicit type when present.
            const std::string type_str = child.get("type");
            keys.types.push_back(
                type_str.empty()
                    ? 2
                    : parse_number_prefix<int>(type_str).value_or(0));
        }
    }
    return keys;
}

float lerp_keys(const TrackKeys& keys, float t) {
    const size_t n = keys.times.size();
    if (n == 0) return 0.0f;
    if (n == 1) return keys.values[0];
    if (t <= keys.times.front()) return keys.values.front();
    if (t >= keys.times.back()) return keys.values.back();

    // Find the surrounding key pair [i, i+1].
    for (size_t i = 0; i + 1 < n; i++) {
        const float t0 = keys.times[i];
        const float t1 = keys.times[i + 1];
        if (t < t0 || t > t1) continue;
        if (t1 == t0) return keys.values[i];

        const float v0 = keys.values[i];
        const float v1 = keys.values[i + 1];
        const int   k0_type = keys.types[i];

        // CONSTANT holds the left value; LINEAR is a straight lerp; everything
        // else (SMOOTH/BREAK/AUTO) is the cubic Bezier defined by the adjoining
        // tangent handles — matching the engine's get_track_value_float.
        if (k0_type == 0) {
            return v0;
        }
        if (k0_type == 1) {
            const float k = (t - t0) / (t1 - t0);
            return v0 + k * (v1 - v0);
        }

        const glm::vec4 h0 = keys.handles[i];      // (in_dx, in_dy, out_dx, out_dy)
        const glm::vec4 h1 = keys.handles[i + 1];
        const glm::vec4 X(t0,
                          glm::clamp(t0 + h0.z, t0, t1),
                          glm::clamp(t1 + h1.x, t0, t1),
                          t1);
        const glm::vec4 Y(v0, v0 + h0.w, v1 + h1.y, v1);
        return bezier_solve(X, Y, t);
    }
    return keys.values.back();
}

// The original benchmark drives DOF through two reusable preset tracks. Each
// preset contains the complete Unigine near/far blur parameter set over a
// normalized focus-control timeline. Camera tracks bind one or both presets,
// providing an animated local time (keys_t) and blend weight (keys_w).
struct DofControlTrack {
    TrackKeys focus_distance;
    TrackKeys near_focal_range;
    TrackKeys near_blur_range;
    TrackKeys near_blur_radius;
    TrackKeys far_focal_range;
    TrackKeys far_blur_range;
    TrackKeys far_blur_radius;
    TrackKeys near_blur_power;
    TrackKeys bokeh_far_size;
    bool loaded = false;
};

DofControlTrack parse_dof_control_track(
    const std::string& file, AssetUsageRecorder* rec = nullptr,
    AssetFileSystemRef assets = std::nullopt) {
    DofControlTrack control;
    const XmlNode root = xml0_parse_tree(file.c_str(), assets);
    if (root.tag.empty()) {
        std::fprintf(stderr, "DOF control track: failed to parse %s\n", file.c_str());
        return control;
    }

    if (rec) rec->record_used(file);

    for (const auto& param : root.children) {
        if (param.tag != "parameter") continue;
        const std::string type = param.get("type");
        TrackKeys* destination = nullptr;
        if (type == "render.dof.focalDistance") {
            destination = &control.focus_distance;
        } else if (type == "render.dof.nearFocalRange") {
            destination = &control.near_focal_range;
        } else if (type == "render.dof.nearBlurRange") {
            destination = &control.near_blur_range;
        } else if (type == "render.dof.nearBlurRadius") {
            destination = &control.near_blur_radius;
        } else if (type == "render.dof.farFocalRange") {
            destination = &control.far_focal_range;
        } else if (type == "render.dof.farBlurRange") {
            destination = &control.far_blur_range;
        } else if (type == "render.dof.farBlurRadius") {
            destination = &control.far_blur_radius;
        } else if (type == "render.dof.nearBlurPower") {
            destination = &control.near_blur_power;
        } else if (type == "render.hdr.bokeh.farSize") {
            destination = &control.bokeh_far_size;
        }
        if (destination != nullptr) {
            *destination = parse_track_keys(param);
        }
    }

    control.loaded = !control.focus_distance.times.empty();
    return control;
}

AuthoredDofState sample_dof_control(
    const DofControlTrack& control, float time) {
    AuthoredDofState state;
    state.enabled          = control.loaded;
    state.focus_distance   = lerp_keys(control.focus_distance, time);
    state.near_focal_range = lerp_keys(control.near_focal_range, time);
    state.near_blur_range  = lerp_keys(control.near_blur_range, time);
    state.near_blur_radius = lerp_keys(control.near_blur_radius, time);
    state.far_focal_range  = lerp_keys(control.far_focal_range, time);
    state.far_blur_range   = lerp_keys(control.far_blur_range, time);
    state.far_blur_radius  = lerp_keys(control.far_blur_radius, time);
    state.near_blur_power  = lerp_keys(control.near_blur_power, time);
    state.bokeh_far_size   = lerp_keys(control.bokeh_far_size, time);
    return state;
}

// ── Parse a single camera track into per-axis position/rotation keys ─

struct DofTrackBinding {
    enum class Preset { F2, F16 };

    Preset    preset = Preset::F16;
    TrackKeys time;
    TrackKeys weight;
};

// One camera track's animated channels. Each camera_*.track is authored on a
// normalized [0, 1] local timeline (its root max_time is 1); the clip timeline
// maps global seconds onto that local position.
struct CameraTrackChannels {
    TrackKeys pos_x, pos_y, pos_z;
    TrackKeys rot_x, rot_y, rot_z;  // Euler ZYX, degrees
    TrackKeys time_of_day;          // nested day_cycle.track local-hour channel
    std::vector<DofTrackBinding> dof_bindings;
    bool      has_time_of_day = false;
    bool      loaded = false;
};

CameraTrackChannels parse_camera_track(const std::string& file,
                                              AssetUsageRecorder* rec = nullptr,
                                              AssetFileSystemRef assets = std::nullopt) {
    CameraTrackChannels ch;

    XmlNode root = xml0_parse_tree(file.c_str(), assets);
    if (root.tag.empty()) {
        std::fprintf(stderr, "Camera track: failed to parse %s\n", file.c_str());
        return ch;
    }

    // The file was successfully opened and parsed as xml0: record it as a used
    // .track asset for the packer. No-op on normal runs.
    if (rec) rec->record_used(file);

    // The camera parameters carry the original Unigine Tracker types:
    //   type="node.position"     — keys_x/keys_y/keys_z are world X/Y/Z (m)
    //   type="node.rotationZYX"  — keys_x/keys_y/keys_z are Euler angles in
    //                              DEGREES, applied as Rz(z)*Ry(y)*Rx(x)
    //                              (see core/.../tracker/parameters/
    //                              node.parameters: rotationZYX `set`).

    // Each benchmark camera track also embeds a nested type="track" parameter
    // whose keys_n points at heaven/tracks/day_cycle.track and whose keys_t is
    // the authored time-of-day hour for this scene (constant or ramped). Capture
    // that channel here so benchmark playback can reproduce the original scene
    // lighting changes instead of using one global CLI hour.
    for (const auto& param : root.children) {
        if (param.tag != "parameter") continue;

        const std::string ptype = param.get("type");
        const bool is_position = ptype == "node.position";
        const bool is_rotation = ptype == "node.rotationZYX";
        const bool is_nested_track = ptype == "track";
        if (!is_position && !is_rotation && !is_nested_track) continue;

        if (is_nested_track) {
            std::string referenced_track;
            const XmlNode* keys_t = nullptr;
            const XmlNode* keys_w = nullptr;
            for (const auto& sub : param.children) {
                if (sub.tag == "keys_n") {
                    for (const auto& key : sub.children) {
                        if (!key.text.empty()) {
                            referenced_track =
                                std::filesystem::path(key.text).filename().string();
                            break;
                        }
                    }
                } else if (sub.tag == "keys_t") {
                    keys_t = &sub;
                } else if (sub.tag == "keys_w") {
                    keys_w = &sub;
                }
            }
            if (referenced_track == "day_cycle.track" && keys_t != nullptr) {
                ch.time_of_day = parse_track_keys(*keys_t);
                ch.has_time_of_day = !ch.time_of_day.times.empty();
            } else if ((referenced_track == "dof_control2.track" ||
                        referenced_track == "dof_control16.track") &&
                       keys_t != nullptr && keys_w != nullptr) {
                DofTrackBinding binding;
                binding.preset = referenced_track == "dof_control2.track"
                    ? DofTrackBinding::Preset::F2
                    : DofTrackBinding::Preset::F16;
                binding.time = parse_track_keys(*keys_t);
                binding.weight = parse_track_keys(*keys_w);
                ch.dof_bindings.push_back(std::move(binding));
            }
            continue;
        }

        for (const auto& keys_node : param.children) {
            if (keys_node.tag == "keys_x") {
                auto k = parse_track_keys(keys_node);
                if (is_position) ch.pos_x = k; else ch.rot_x = k;
            } else if (keys_node.tag == "keys_y") {
                auto k = parse_track_keys(keys_node);
                if (is_position) ch.pos_y = k; else ch.rot_y = k;
            } else if (keys_node.tag == "keys_z") {
                auto k = parse_track_keys(keys_node);
                if (is_position) ch.pos_z = k; else ch.rot_z = k;
            }
        }
    }

    ch.loaded = !ch.pos_x.times.empty() || !ch.rot_x.times.empty();
    return ch;
}

// Sample a camera track at local (normalized) time `s` in [0, 1] and write the
// interpolated camera keyframe pose.
CameraKeyframe sample_camera_track(const CameraTrackChannels& ch, float s) {
    CameraKeyframe kf;
    kf.position = glm::vec3(lerp_keys(ch.pos_x, s),
                            lerp_keys(ch.pos_y, s),
                            lerp_keys(ch.pos_z, s));

    // Build the orientation from the Euler ZYX angles (degrees). The original
    // engine applies Rz * Ry * Rx (intrinsic), so compose the axis-angle
    // quaternions in that order.
    const float ex = glm::radians(lerp_keys(ch.rot_x, s));
    const float ey = glm::radians(lerp_keys(ch.rot_y, s));
    const float ez = glm::radians(lerp_keys(ch.rot_z, s));
    const glm::quat qx = glm::angleAxis(ex, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat qy = glm::angleAxis(ey, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat qz = glm::angleAxis(ez, glm::vec3(0.0f, 0.0f, 1.0f));
    kf.rotation = glm::normalize(qz * qy * qx);
    kf.fov = 60.0f;
    if (ch.has_time_of_day) {
        kf.time_of_day = lerp_keys(ch.time_of_day, s);
        kf.has_time_of_day = true;
    }
    return kf;
}

AuthoredDofState sample_camera_dof(
    const CameraTrackChannels& ch, float s,
    const DofControlTrack& control2, const DofControlTrack& control16) {
    AuthoredDofState result;
    float total_weight = 0.0f;
    float reciprocal_aperture_sum = 0.0f;

    auto accumulate = [&](const AuthoredDofState& sample,
                          float weight, float preset_aperture) {
        if (!sample.enabled || !std::isfinite(weight) || weight <= 1e-6f) return;
        result.focus_distance     += sample.focus_distance * weight;
        result.near_focal_range   += sample.near_focal_range * weight;
        result.near_blur_range    += sample.near_blur_range * weight;
        result.near_blur_radius   += sample.near_blur_radius * weight;
        result.far_focal_range    += sample.far_focal_range * weight;
        result.far_blur_range     += sample.far_blur_range * weight;
        result.far_blur_radius    += sample.far_blur_radius * weight;
        result.near_blur_power    += sample.near_blur_power * weight;
        result.bokeh_far_size     += sample.bokeh_far_size * weight;
        // Tracker blends the actual DOF parameters linearly. Aperture diameter,
        // and therefore physical CoC strength, is proportional to 1/f-number,
        // so the equivalent f-number of the f/2:f/16 blend is the weighted
        // harmonic mean rather than a linear interpolation of 2 and 16.
        reciprocal_aperture_sum += weight / preset_aperture;
        total_weight += weight;
    };

    // AuthoredDofState has meaningful interactive defaults, but the accumulator
    // must start from zero before taking the normalized Tracker blend.
    result.focus_distance = 0.0f;
    result.desired_aperture = 0.0f;

    for (const DofTrackBinding& binding : ch.dof_bindings) {
        const float local_time = lerp_keys(binding.time, s);
        const float weight = lerp_keys(binding.weight, s);
        if (binding.preset == DofTrackBinding::Preset::F2) {
            accumulate(sample_dof_control(control2, local_time), weight, 2.0f);
        } else {
            accumulate(sample_dof_control(control16, local_time), weight, 16.0f);
        }
    }

    if (total_weight <= 1e-6f) return AuthoredDofState{};

    const float inv_weight = 1.0f / total_weight;
    result.enabled            = true;
    result.focus_distance    *= inv_weight;
    result.near_focal_range  *= inv_weight;
    result.near_blur_range   *= inv_weight;
    result.near_blur_radius  *= inv_weight;
    result.far_focal_range   *= inv_weight;
    result.far_blur_range    *= inv_weight;
    result.far_blur_radius   *= inv_weight;
    result.near_blur_power   *= inv_weight;
    result.bokeh_far_size    *= inv_weight;
    result.desired_aperture   =
        reciprocal_aperture_sum > 1e-6f
            ? total_weight / reciprocal_aperture_sum
            : 16.0f;
    return result;
}

// ── clip.track scene sequencing ─────────────────────────────────

// One scene of the benchmark: it starts at `start_time` on the master clip
// timeline and plays `file`'s camera track. The normalized local position
// within the track is read from clip.track's keys_t channel as a function of
// global time.
struct ClipScene {
    float       start_time = 0.0f;   // seconds on the master clip timeline
    std::string file;                // camera_*.track filename (relative path)
};

// Parsed clip.track: the ordered scenes (keys_n), the global→local position
// channel (keys_t), the master timeline duration, and the scene-cut fade
// (render.fadeColor alpha over global time).
struct ClipTrack {
    std::vector<ClipScene> scenes;   // keys_n, in time order
    TrackKeys              pos_t;     // keys_t — normalized [0,1] over global time
    std::vector<glm::vec2> fade;      // (time, alpha) from render.fadeColor
    float                  duration = 0.0f;  // max_time
    bool                   loaded = false;
};

ClipTrack parse_clip_track(const std::string& clip_file,
                                  AssetUsageRecorder* rec = nullptr,
                                  AssetFileSystemRef assets = std::nullopt) {
    ClipTrack clip;

    XmlNode root = xml0_parse_tree(clip_file.c_str(), assets);
    if (root.tag.empty()) {
        std::fprintf(stderr, "Clip track: failed to parse %s\n", clip_file.c_str());
        return clip;
    }

    // The clip.track file was successfully opened and parsed: record it as a
    // used.track asset for the packer. No-op on normal runs.
    if (rec) rec->record_used(clip_file);

    auto max_time_str = root.get("max_time");
    clip.duration = parse_float_or(max_time_str, clip.duration);

    // The first parameter (type="track") sequences the scenes: its <keys_n>
    // sub-node holds one key per scene (time = scene start on the master
    // timeline, text = camera track path), and its <keys_t> sub-node holds the
    // normalized [0,1] local playback position as a function of global time
    // (reset to 0 at each scene boundary). A second parameter
    // (type="render.fadeColor") carries the scene-cut fade-to-black: RGBA keys
    // whose alpha ramps to 1 just before each boundary and back to 0 after.
    const XmlNode* track_param = nullptr;
    const XmlNode* fade_param  = nullptr;
    for (const auto& child : root.children) {
        if (child.tag != "parameter") continue;
        const std::string ptype = child.get("type");
        if (ptype == "track" && track_param == nullptr) {
            track_param = &child;
        } else if (ptype == "render.fadeColor" && fade_param == nullptr) {
            fade_param = &child;
        }
    }
    if (track_param == nullptr) {
        std::fprintf(stderr, "Clip track: no <parameter type=\"track\"> in %s\n",
                     clip_file.c_str());
        return clip;
    }

    for (const auto& sub : track_param->children) {
        if (sub.tag == "keys_n") {
            for (const auto& key : sub.children) {
                if (key.tag != "key") continue;
                ClipScene scene;
                auto ts = key.get("time");
                scene.start_time = parse_float_or(ts, scene.start_time);
                scene.file = key.text;  // e.g. "heaven/tracks/camera_09.track"
                if (!scene.file.empty()) clip.scenes.push_back(std::move(scene));
            }
        } else if (sub.tag == "keys_t") {
            clip.pos_t = parse_track_keys(sub);
        }
    }

    std::sort(clip.scenes.begin(), clip.scenes.end(),
              [](const ClipScene& a, const ClipScene& b) {
                  return a.start_time < b.start_time;
              });

    // Scene-cut fade-to-black: read the alpha (4th component) of each
    // render.fadeColor RGBA key. The color is always black; only the alpha
    // matters for the mask.
    if (fade_param != nullptr) {
        for (const auto& key : fade_param->children) {
            if (key.tag != "key") continue;
            float ts = 0.0f;
            auto ts_str = key.get("time");
            ts = parse_float_or(ts_str, ts);
            const glm::vec4 rgba = parse_vec4(key.text);
            clip.fade.emplace_back(ts, rgba.w);
        }
        std::sort(clip.fade.begin(), clip.fade.end(),
                  [](const glm::vec2& a, const glm::vec2& b) {
                      return a.x < b.x;
                  });
    }

    clip.loaded = !clip.scenes.empty();
    return clip;
}

} // namespace

// ── Load the camera path, sequenced by clip.track ───────────────

CameraPath load_camera_path(const std::string& track_dir, AssetUsageRecorder* rec,
                            AssetFileSystemRef assets) {
    CameraPath path;
    namespace fs = std::filesystem;

    if (!asset_exists((fs::path(track_dir) / "clip.track").string(), assets)) {
        std::fprintf(stderr, "Track directory not found: %s\n", track_dir.c_str());
        return path;
    }

    // clip.track is the master timeline: it names which camera track plays in
    // each of the 26 scenes (keys_n) and the normalized local position within
    // that track as a function of global time (keys_t). Without it we cannot
    // sequence the scenes, so the path stays empty and the benchmark refuses to
    // start.
    const std::string clip_file = (fs::path(track_dir) / "clip.track").string();
    ClipTrack clip = parse_clip_track(clip_file, rec, assets);
    if (!clip.loaded) {
        std::fprintf(stderr, "Camera path: clip.track unusable at %s\n",
                     clip_file.c_str());
        return path;
    }

    // Resolve and parse each scene's camera track once. The keys_n text is a
    // data-dir-relative path like "heaven/tracks/camera_09.track"; the tracks
    // live alongside clip.track, so resolve by basename inside track_dir.
    std::vector<CameraTrackChannels> scene_tracks(clip.scenes.size());
    for (size_t i = 0; i < clip.scenes.size(); ++i) {
        const std::string basename =
            fs::path(clip.scenes[i].file).filename().string();
        const std::string full = (fs::path(track_dir) / basename).string();
        scene_tracks[i] = parse_camera_track(full, rec, assets);
        if (!scene_tracks[i].loaded) {
            std::fprintf(stderr,
                         "Camera path: scene %zu track %s unusable\n",
                         i, full.c_str());
        }
    }

    // Load only the indirectly referenced authored DOF presets. Keeping this
    // dependency-driven avoids spurious missing-file diagnostics for synthetic
    // camera-path fixtures that contain no DOF bindings, while ensuring the
    // used-asset packer records every preset the real clip graph consumes.
    bool needs_dof_control2 = false;
    bool needs_dof_control16 = false;
    for (const CameraTrackChannels& track : scene_tracks) {
        for (const DofTrackBinding& binding : track.dof_bindings) {
            if (binding.preset == DofTrackBinding::Preset::F2) {
                needs_dof_control2 = true;
            } else {
                needs_dof_control16 = true;
            }
        }
    }

    DofControlTrack dof_control2;
    DofControlTrack dof_control16;
    if (needs_dof_control2) {
        dof_control2 = parse_dof_control_track(
            (fs::path(track_dir) / "dof_control2.track").string(), rec, assets);
    }
    if (needs_dof_control16) {
        dof_control16 = parse_dof_control_track(
            (fs::path(track_dir) / "dof_control16.track").string(), rec, assets);
    }

    // Bake the master timeline into evenly-spaced keyframes. At each global
    // time t we find the active scene (latest start_time <= t), compute the
    // scene-local normalized position s in [0, 1] from keys_t, and sample that
    // scene's camera track at s. This reproduces the original Tracker, which
    // evaluates clip.track at the global clip time and drives the camera node
    // from the selected track at the keys_t position.
    const float duration = clip.duration > 0.0f
        ? clip.duration
        : clip.scenes.back().start_time;  // fallback if max_time missing
    constexpr float kStep = 0.05f;  // 20 keyframes per second

    auto active_scene = [&](float t) -> size_t {
        size_t idx = 0;
        for (size_t i = 0; i < clip.scenes.size(); ++i) {
            if (clip.scenes[i].start_time <= t) idx = i; else break;
        }
        return idx;
    };

    // Scene starts are discontinuities: clip.track changes the selected nested
    // camera immediately at the exact authored timestamp. Keep those timestamps
    // authoritative and discard a nearby regular-grid sample instead. The old
    // tolerance-based std::unique did the opposite whenever floating-point
    // accumulation put the 20 Hz sample a few microseconds before a cut, leaving
    // the previous scene active until the following keyframe and causing a
    // delayed camera/DOF pop.
    std::vector<float> discontinuities;
    discontinuities.reserve(clip.scenes.size() + 1);
    discontinuities.push_back(duration);
    for (const ClipScene& scene : clip.scenes) {
        discontinuities.push_back(
            glm::clamp(scene.start_time, 0.0f, duration));
    }
    std::sort(discontinuities.begin(), discontinuities.end());
    discontinuities.erase(
        std::unique(discontinuities.begin(), discontinuities.end()),
        discontinuities.end());

    std::vector<float> sample_times = discontinuities;
    const int grid_count =
        static_cast<int>(std::floor(duration / kStep));
    for (int i = 0; i <= grid_count; ++i) {
        const float t = static_cast<float>(i) * kStep;
        const bool near_discontinuity = std::any_of(
            discontinuities.begin(), discontinuities.end(),
            [t](float boundary) {
                return std::abs(t - boundary) < 0.0005f;
            });
        if (!near_discontinuity) sample_times.push_back(t);
    }
    std::sort(sample_times.begin(), sample_times.end());

    for (float t : sample_times) {
        const size_t si = active_scene(t);
        const CameraTrackChannels& ch = scene_tracks[si];

        // Normalized local position within the active scene's track. keys_t is
        // authored to ramp 0→~1 across each scene's span (with a reset key at
        // the boundary), so sampling it at the global time gives the correct
        // local position; clamp defensively to [0, 1].
        float s = lerp_keys(clip.pos_t, t);
        s = glm::clamp(s, 0.0f, 1.0f);

        CameraKeyframe kf = sample_camera_track(ch, s);
        kf.authored_dof = sample_camera_dof(
            ch, s, dof_control2, dof_control16);
        kf.time  = t;
        kf.scene = static_cast<int>(si);
        path.keyframes.push_back(kf);
    }

    path.duration  = duration;
    path.fade_keys = clip.fade;
    std::printf("  Camera path: %zu keyframes, %.1f seconds, %zu scenes "
                "(clip.track sequenced, %zu fade keys)\n",
                path.keyframes.size(), path.duration, clip.scenes.size(),
                path.fade_keys.size());

    return path;
}

// ── Interpolation ───────────────────────────────────────────────

// Shared interpolation core. The caller guarantees a non-empty keyframe list
// and a time value already mapped into the path's range (wrapped for looping,
// clamped for non-looping). Keeping this in one place means the looping and
// non-looping evaluators perform identical per-segment interpolation and
// cannot drift apart.
void CameraPath::evaluate_core(float t, glm::vec3& out_pos, glm::quat& out_rot, float& out_fov) const {
    // Find surrounding keyframes
    size_t i = 0;
    for (; i + 1 < keyframes.size(); i++) {
        if (keyframes[i + 1].time > t) break;
    }

    if (i + 1 >= keyframes.size()) {
        out_pos = keyframes.back().position;
        out_rot = keyframes.back().rotation;
        out_fov = keyframes.back().fov;
        return;
    }

    const auto& a = keyframes[i];
    const auto& b = keyframes[i + 1];

    // Do NOT blend across a scene cut. When the surrounding keyframes belong to
    // different clip.track scenes, the original benchmark snaps the camera to
    // the new scene's pose (and masks the discontinuity with a fade-to-black),
    // rather than swinging the camera between two unrelated viewpoints. Hold
    // the keyframe of whichever scene is active at time t.
    if (a.scene != b.scene) {
        const CameraKeyframe& hold = (t < b.time) ? a : b;
        out_pos = hold.position;
        out_rot = hold.rotation;
        out_fov = hold.fov;
        return;
    }

    float frac = (t - a.time) / (b.time - a.time);
    frac = glm::clamp(frac, 0.0f, 1.0f);

    out_pos = glm::mix(a.position, b.position, frac);
    out_rot = glm::slerp(a.rotation, b.rotation, frac);
    out_fov = glm::mix(a.fov, b.fov, frac);
}

float CameraPath::time_of_day_core(float t, float fallback) const {
    size_t i = 0;
    for (; i + 1 < keyframes.size(); i++) {
        if (keyframes[i + 1].time > t) break;
    }

    if (i + 1 >= keyframes.size()) {
        const CameraKeyframe& k = keyframes.back();
        return k.has_time_of_day ? k.time_of_day : fallback;
    }

    const auto& a = keyframes[i];
    const auto& b = keyframes[i + 1];

    if (a.scene != b.scene) {
        const CameraKeyframe& hold = (t < b.time) ? a : b;
        return hold.has_time_of_day ? hold.time_of_day : fallback;
    }

    if (!a.has_time_of_day || !b.has_time_of_day) {
        if (a.has_time_of_day) return a.time_of_day;
        if (b.has_time_of_day) return b.time_of_day;
        return fallback;
    }

    float frac = (t - a.time) / (b.time - a.time);
    frac = glm::clamp(frac, 0.0f, 1.0f);
    return glm::mix(a.time_of_day, b.time_of_day, frac);
}

namespace {

AuthoredDofState mix_dof(
    const AuthoredDofState& a, const AuthoredDofState& b, float fraction) {
    if (!a.enabled) return b;
    if (!b.enabled) return a;

    AuthoredDofState result;
    result.enabled            = true;
    result.focus_distance     = glm::mix(a.focus_distance, b.focus_distance, fraction);
    result.near_focal_range   = glm::mix(a.near_focal_range, b.near_focal_range, fraction);
    result.near_blur_range    = glm::mix(a.near_blur_range, b.near_blur_range, fraction);
    result.near_blur_radius   = glm::mix(a.near_blur_radius, b.near_blur_radius, fraction);
    result.far_focal_range    = glm::mix(a.far_focal_range, b.far_focal_range, fraction);
    result.far_blur_range     = glm::mix(a.far_blur_range, b.far_blur_range, fraction);
    result.far_blur_radius    = glm::mix(a.far_blur_radius, b.far_blur_radius, fraction);
    result.near_blur_power    = glm::mix(a.near_blur_power, b.near_blur_power, fraction);
    result.bokeh_far_size     = glm::mix(a.bokeh_far_size, b.bokeh_far_size, fraction);
    result.desired_aperture   = glm::mix(
        a.desired_aperture, b.desired_aperture, fraction);
    return result;
}

} // namespace

AuthoredDofState CameraPath::depth_of_field_core(float t) const {
    size_t i = 0;
    for (; i + 1 < keyframes.size(); ++i) {
        if (keyframes[i + 1].time > t) break;
    }

    if (i + 1 >= keyframes.size()) {
        return keyframes.back().authored_dof;
    }

    const CameraKeyframe& a = keyframes[i];
    const CameraKeyframe& b = keyframes[i + 1];
    if (a.scene != b.scene) {
        return (t < b.time ? a : b).authored_dof;
    }

    const float span = b.time - a.time;
    if (span <= 0.0f) return a.authored_dof;
    const float fraction = glm::clamp((t - a.time) / span, 0.0f, 1.0f);
    return mix_dof(a.authored_dof, b.authored_dof, fraction);
}

void CameraPath::evaluate(float t, glm::vec3& out_pos, glm::quat& out_rot, float& out_fov) const {
    if (keyframes.empty()) return;

    // Loop time into [0, duration) before delegating to the shared core.
    if (duration > 0.0f) {
        t = std::fmod(t, duration);
        if (t < 0.0f) t += duration;
    }

    evaluate_core(t, out_pos, out_rot, out_fov);
}

float CameraPath::time_of_day(float t, float fallback) const {
    if (keyframes.empty()) return fallback;

    if (duration > 0.0f) {
        t = std::fmod(t, duration);
        if (t < 0.0f) t += duration;
    }

    return time_of_day_core(t, fallback);
}

AuthoredDofState CameraPath::depth_of_field(float t) const {
    if (keyframes.empty()) return {};

    if (duration > 0.0f) {
        t = std::fmod(t, duration);
        if (t < 0.0f) t += duration;
    }
    return depth_of_field_core(t);
}

void CameraPath::evaluate_clamped(float t, glm::vec3& out_pos, glm::quat& out_rot, float& out_fov) const {
    if (keyframes.empty()) return;

    // Non-looping: clamp the time to [0, duration] so t <= 0 yields the first
    // keyframe and t >= duration holds the final keyframe. The shared core then
    // performs the same per-segment interpolation as the looping path.
    t = glm::clamp(t, 0.0f, duration);

    evaluate_core(t, out_pos, out_rot, out_fov);
}

float CameraPath::time_of_day_clamped(float t, float fallback) const {
    if (keyframes.empty()) return fallback;

    t = glm::clamp(t, 0.0f, duration);
    return time_of_day_core(t, fallback);
}

AuthoredDofState CameraPath::depth_of_field_clamped(float t) const {
    if (keyframes.empty()) return {};

    t = glm::clamp(t, 0.0f, duration);
    return depth_of_field_core(t);
}

namespace {

// Convert an interpolated camera state onto the free-fly Camera fields.
void apply_state(Camera& camera, const glm::vec3& pos, const glm::quat& rot, float fov) {
    camera.position = pos;
    camera.fov = fov;

    // The authored camera node looks along its local -Z axis (Z-up world). The
    // rotationZYX quaternion maps -Z to the world view direction; the node's
    // pitch channel sits near 90°, which puts -Z on the horizon (looking across
    // the scene, aligned with the flight direction). The previously-used +X / -Y
    // axes are the node's right / down vectors, so the view never tilted with
    // the path. Derive yaw/pitch from the correct forward vector.
    glm::vec3 front = glm::normalize(rot * glm::vec3(0.0f, 0.0f, -1.0f));
    camera.yaw   = glm::degrees(std::atan2(front.y, front.x));
    camera.pitch = glm::degrees(std::asin(glm::clamp(front.z, -1.0f, 1.0f)));
}

} // namespace

void CameraPath::apply(float t, Camera& camera) const {
    if (keyframes.empty()) return;
    glm::vec3 pos = keyframes.front().position;
    glm::quat rot = keyframes.front().rotation;
    float fov = keyframes.front().fov;
    evaluate(t, pos, rot, fov);
    apply_state(camera, pos, rot, fov);
    camera.authored_dof = depth_of_field(t);
}

void CameraPath::apply_clamped(float t, Camera& camera) const {
    if (keyframes.empty()) return;
    glm::vec3 pos = keyframes.front().position;
    glm::quat rot = keyframes.front().rotation;
    float fov = keyframes.front().fov;
    evaluate_clamped(t, pos, rot, fov);
    apply_state(camera, pos, rot, fov);
    camera.authored_dof = depth_of_field_clamped(t);
}

float CameraPath::fade_alpha(float t) const {
    if (fade_keys.empty()) return 0.0f;
    if (t <= fade_keys.front().x) return glm::clamp(fade_keys.front().y, 0.0f, 1.0f);
    if (t >= fade_keys.back().x)  return glm::clamp(fade_keys.back().y, 0.0f, 1.0f);

    // Linear interpolation between the surrounding alpha keys. The original
    // render.fadeColor keys are linear ramps (fade out → hold black → fade in),
    // so a straight lerp reproduces the transition mask faithfully.
    for (size_t i = 0; i + 1 < fade_keys.size(); ++i) {
        const glm::vec2& k0 = fade_keys[i];
        const glm::vec2& k1 = fade_keys[i + 1];
        if (t < k0.x || t > k1.x) continue;
        if (k1.x == k0.x) return glm::clamp(k0.y, 0.0f, 1.0f);
        const float f = (t - k0.x) / (k1.x - k0.x);
        return glm::clamp(k0.y + f * (k1.y - k0.y), 0.0f, 1.0f);
    }
    return 0.0f;
}

} // namespace heaven
