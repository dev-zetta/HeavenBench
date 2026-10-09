/*
 * HeavenBench — RHI-aware scene loading implementation
 */
#include "engine/scene_rhi.h"
#include "engine/render_test_scene_catalog.h"
#include "engine/mesh.h"
#include "engine/material_rhi.h"
#include "engine/path_resolver.h"
#include "engine/cpu_tessellation.h"
#include "engine/closed_displacement_seams.h"
#include "engine/tessellation_cache_archive.h"
#include "engine/camera_collision.h"
#include "engine/asset_usage_recorder.h"
#include "engine/asset_file_system.h"
#include "engine/surface_displacement_policy.h"
#include "engine/ground_relief_generator.h"
#include "engine/number_parse.h"
#include "engine/startup_timer.h"
#include "engine/boiler_smoke.h"
#include "xml0_parser.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <map>
#include <set>
#include <numbers>
#include <utility>

namespace heaven {
namespace {

// A failed load must retire only its newly uploaded mesh owners. Previously
// active scenes may still borrow other meshes from the same backend.
class SceneMeshRollback {
public:
    SceneMeshRollback(IRhiBackend& backend, RhiScene& scene)
        : backend_(backend), scene_(scene) {}
    SceneMeshRollback(const SceneMeshRollback&) = delete;
    SceneMeshRollback& operator=(const SceneMeshRollback&) = delete;
    SceneMeshRollback(SceneMeshRollback&&) = delete;
    SceneMeshRollback& operator=(SceneMeshRollback&&) = delete;
    ~SceneMeshRollback() {
        if (committed_) return;
        for (const auto& mesh : scene_.gpu_meshes) backend_.release_mesh(mesh);
    }
    void commit() { committed_ = true; }

private:
    IRhiBackend& backend_;
    RhiScene& scene_;
    bool committed_ = false;
};

// Transform parsing (same as scene.cpp)
void parse_transform(const std::string& text, float out[16]) {
    constexpr float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (std::sscanf(text.c_str(), "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
        &out[0],&out[1],&out[2],&out[3],&out[4],&out[5],&out[6],&out[7],
        &out[8],&out[9],&out[10],&out[11],&out[12],&out[13],&out[14],&out[15]) != 16)
        std::memcpy(out, id, sizeof(id));
}

void mat4_mul(const float a[16], const float b[16], float o[16]) {
    for (int r=0;r<4;r++) for (int c=0;c<4;c++) { float s=0; for (int k=0;k<4;k++) s+=a[r*4+k]*b[k*4+c]; o[r*4+c]=s; }
}

int g_next_float_group = 0;

// Per-group animation parameters (speed, amplitude) parsed from
// each `move_*` WorldExpression's script body. Populated as
// groups are minted in `walk`; consumed by the post-load pass
// that builds FloatingIsland records. Indexed by float_group;
// missing entries fall back to the heuristic
// `(group%2==0)?0.25:0.5` and amplitude=2.0 the renderer used
// before.
struct GroupAnim { float speed = 0.0f; float amplitude = 0.0f; bool valid = false; };
std::vector<GroupAnim> g_group_anim;

// Parse the move-island script body and pull out the `(time-offset)*<S>f`
// speed and the `<A>f - cos(time) * <A>f` amplitude. Returns whether
// both were found. The script is small and well-shaped (look at the
// stones probe output for the canonical form), so a couple of
// substring scans + strtof are enough — no need for a real parser.
bool parse_island_anim(const std::string& script,
                              float& speed_out, float& amp_out) {
    // Speed: look for `getTime() - offset) * <NUM>f`. The number
    // sits between the `* ` and the trailing `f`.
    auto sp = script.find(") * ");
    if (sp == std::string::npos) return false;
    sp += 4; // past `) * `
    const std::string_view speed_text{script.data() + sp, script.size() - sp};
    const auto speed = parse_number_prefix_with_consumed<float>(speed_text);
    if (!speed || speed->consumed >= speed_text.size()) return false;
    if (speed_text[speed->consumed] != 'f' &&
        speed_text[speed->consumed] != 'F') return false;

    // Amplitude: look for `<NUM>f - cos(time) * <NUM>f`. Both
    // numbers should match (Heaven's scripts always do); we just
    // read the first one before the ` - cos`.
    auto cp = script.find(" - cos(time)");
    if (cp == std::string::npos) return false;
    // Walk back from cp to the last `f` (literal suffix) preceded
    // by digits/decimal-point/sign, capturing the amplitude literal.
    size_t i = cp;
    while (i > 0 && (script[i-1] == ' ' || script[i-1] == '\t')) --i;
    if (i == 0 || (script[i-1] != 'f' && script[i-1] != 'F')) return false;
    --i; // skip the f
    size_t num_end = i;
    while (i > 0) {
        char ch = script[i-1];
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+')
            --i;
        else break;
    }
    if (i == num_end) return false;
    const auto amplitude =
        parse_number_prefix<float>(std::string_view{script}.substr(i, num_end - i));
    if (!amplitude) return false;

    speed_out = speed->value;
    amp_out   = *amplitude;
    return true;
}

// ── Particle emitter parsing ────────────────────────────────────


inline float parse_float(const std::string& s, float def = 0.0f) {
    if (s.empty()) return def;
    float v = def;
    std::sscanf(s.c_str(), "%f", &v);
    return v;
}

inline glm::vec3 parse_vec3(const std::string& s, glm::vec3 def = {0,0,0}) {
    glm::vec3 v = def;
    std::sscanf(s.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

// Extract the value of a child element (case-sensitive tag match).
// Returns empty string if not found.
inline std::string child_text(const XmlNode& node, const std::string& tag) {
    for (const auto& c : node.children)
        if (c.tag == tag) return c.text;
    return {};
}

inline ParticleEmitter parse_particle_emitter(const XmlNode& node, const float wx[16]) {
    ParticleEmitter e;
    e.name = node.get("name");

    // wx is already the emitter's full world transform — walk() composed
    // the node's own <transform> with the parent context before calling
    // us. Reading the local transform again here would double-apply it
    // (a real bug from a previous version of this code).
    e.transform = unigine_to_glm(wx);

    // Surface child carries the material name
    for (const auto& c : node.children) {
        if (c.tag == "surface") {
            e.material_name = c.get("material");
            if (auto mvd = c.get("max_visible_distance"); !mvd.empty())
                e.max_visible_distance = parse_float(mvd, e.max_visible_distance);
        }
    }

    // Emitter shape and gating
    e.emitter_type    = static_cast<int>(parse_float(child_text(node, "emitter_type"), 0.0f));
    e.emitter_enabled = parse_float(child_text(node, "emitter_enabled"), 1.0f) > 0.5f;
    e.emitter_size    = parse_vec3(child_text(node, "emitter_size"), glm::vec3(0));

    // Distributions
    e.spawn_rate      = parse_float(child_text(node, "spawn_rate"),  e.spawn_rate);
    e.life_mean       = parse_float(child_text(node, "life_mean"),     e.life_mean);
    e.life_spread     = parse_float(child_text(node, "life_spread"),   e.life_spread);
    e.velocity_mean   = parse_float(child_text(node, "velocity_mean"),   e.velocity_mean);
    e.velocity_spread = parse_float(child_text(node, "velocity_spread"), e.velocity_spread);
    e.angle_mean      = parse_float(child_text(node, "angle_mean"),    e.angle_mean);
    e.angle_spread    = parse_float(child_text(node, "angle_spread"),  e.angle_spread);
    e.rotation_mean   = parse_float(child_text(node, "rotation_mean"),   e.rotation_mean);
    e.rotation_spread = parse_float(child_text(node, "rotation_spread"), e.rotation_spread);
    e.radius_mean     = parse_float(child_text(node, "radius_mean"),   e.radius_mean);
    e.radius_spread   = parse_float(child_text(node, "radius_spread"), e.radius_spread);
    e.growth_mean     = parse_float(child_text(node, "growth_mean"),   e.growth_mean);
    e.growth_spread   = parse_float(child_text(node, "growth_spread"), e.growth_spread);
    e.gravity         = parse_vec3(child_text(node, "gravity"), glm::vec3(0));
    e.linear_damping  = parse_float(child_text(node, "linear_damping"), e.linear_damping);

    // Pool size: enough to cover the worst-case life * spawn_rate, with a
    // generous cap so a degenerate emitter can't blow up memory.
    float max_life = e.life_mean + 0.5f * e.life_spread;
    int   needed   = std::max(8, static_cast<int>(std::ceil(e.spawn_rate * max_life * 1.2f)));
    e.max_particles = std::min(needed, 256);

    return e;
}

// Strictness is confined to catalog asset fixtures. The full scene keeps its
// existing best-effort loading behavior, including optional authored references.
struct AuthoredSceneTrace {
    std::vector<std::string> nodes;
    std::set<std::string> active;
    std::string failure;

    void fail(std::string message) {
        if (failure.empty()) failure = std::move(message);
    }
    void record(const std::string& path) {
        if (std::find(nodes.begin(), nodes.end(), path) == nodes.end())
            nodes.push_back(path);
    }
};

Result<void> validate_authored_upload(const Mesh& cpu, const GpuMesh& gpu) {
    bool has_triangles = false;
    for (size_t si = 0; si < cpu.surfaces.size(); ++si) {
        const auto& surface = cpu.surfaces[si];
        if (surface.indices.empty()) continue;
        has_triangles = true;
        if (surface.vertices.empty() || si >= gpu.surfaces.size() ||
            gpu.surfaces[si].handle == 0 ||
            gpu.surfaces[si].index_count != surface.indices.size()) {
            return std::unexpected(Error("Asset fixture mesh upload failed: " +
                cpu.path + " / " + surface.name));
        }
    }
    if (!has_triangles)
        return std::unexpected(Error("Asset fixture mesh has no triangles: " + cpu.path));
    return {};
}

void walk(const XmlNode& node, const PathResolver& paths, const float px[16],
                 std::vector<NodeEntry>& out, std::vector<ParticleEmitter>& emitters,
                 std::vector<SceneLight>& lights,
                 int float_group = -1,
                 const std::string& current_node_file = "",
                 bool stone_anim = false,
                 const float* stone_dummy_world = nullptr,
                 AssetUsageRecorder* rec = nullptr,
                 const std::string& referring_file = "",
                 AssetFileSystemRef assets = std::nullopt,
                 AuthoredSceneTrace* trace = nullptr) {
    if (trace && !trace->failure.empty()) return;
    // Honour the "enabled" attribute — a node tagged enabled="0" in the
    // world file is skipped (e.g. torch_light references in heaven.world).
    // Treats the absence of the attribute as enabled (default is "1").

    // Exception: the original Heaven world disables every torch_light
    // and lamp_post_light NodeReference (`enabled="0"`), but the
    // visible benchmark shows lit torches AND fireflies near every
    // lamp post at night. The benchmark engine likely re-enables
    // them by script when the time-of-day setting flips. We mirror
    // that by force-enabling these specific references — those are
    // exactly the night-VFX containers we want to render.
    if (auto en = node.get("enabled"); en == "0") {
        bool force_enable = false;
        for (const auto& c : node.children) {
            if (c.tag != "reference" || c.text.empty()) continue;
            if (c.text.find("torch_light.node")     != std::string::npos ||
                c.text.find("lamp_post_light.node") != std::string::npos) {
                force_enable = true;
                break;
            }
        }
        if (!force_enable) return;
    }

    constexpr float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    float lx[16]; std::memcpy(lx,id,sizeof(id));
    for (const auto& c:node.children) if(c.tag=="transform"&&!c.text.empty()){parse_transform(c.text,lx);break;}
    float wx[16]; mat4_mul(lx,px,wx);
    auto t=node.get("type");
    auto name=node.get("name");

    // WorldExpression nodes named "move_*" are floating island groups
    // Also extract speed from the script text if available
    if (t=="WorldExpression" && name.find("move_") != std::string::npos) {
        int g = g_next_float_group++;
        float_group = g;
        // Pull speed/amplitude from the script body so we don't
        // have to keep them in sync with whatever Heaven authored.
        // The parse touches the `engine.game.getTime()` line (speed)
        // and the `2.0f - cos(time) * 2.0f` line (amplitude).
        for (const auto& c : node.children) {
            if (c.tag != "expression" || c.text.empty()) continue;
            float speed = 0.0f, amp = 0.0f;
            if (parse_island_anim(c.text, speed, amp)) {
                if (std::cmp_greater_equal(g, g_group_anim.size()))
                    g_group_anim.resize(g + 1);
                g_group_anim[g] = {speed, amp, true};
            }
            break;
        }
    }
    // The `stone_noise` WorldExpression drives the small rocks
    // around the floating islands with a per-position seeded sine
    // wave (see StoneAnim docs). Tag every descendant so the
    // post-load pass can populate `scene.stone_anims`. Detection is
    // by name first; fall back to the script body so a renamed
    // expression still works as long as the seeded-position
    // pattern is intact.
    if (t == "WorldExpression" && !stone_anim) {
        bool match = name == "stone_noise";
        if (!match) {
            for (const auto& c : node.children) {
                if (c.tag == "expression" &&
                    c.text.find("position.x * 13") != std::string::npos &&
                    c.text.find("setPosition") != std::string::npos) {
                    match = true;
                    break;
                }
            }
        }
        if (match) stone_anim = true;
    }

    // First ObjectDummy under the stone_noise expression: this is
    // the node whose Z the original script overwrites. Capture its
    // world transform's translation now so descendants inherit it
    // and can compute (seed, child_local_z) at NodeEntry-emission
    // time.
    float captured_dummy[3];
    if (stone_anim && t == "ObjectDummy" && stone_dummy_world == nullptr) {
        captured_dummy[0] = wx[12];
        captured_dummy[1] = wx[13];
        captured_dummy[2] = wx[14];
        stone_dummy_world = captured_dummy;
    }
    // Bridge segments and start/end dummies are part of the floating assembly
    if (float_group < 0 && t=="ObjectDummy" && 
        (name.find("bridge_") != std::string::npos || 
         name.find("start_bridge") != std::string::npos)) {
        // These are siblings of WorldExpression nodes — use the last assigned group
        // or create a new one. In practice they should all float together.
        float_group = std::max(0, g_next_float_group - 1);
    }

    if (t=="ObjectMesh" || t=="ObjectMeshSkinned") {
        std::string mp;
        for (const auto& c : node.children) {
            if (c.tag == "mesh" && !c.text.empty()) { mp = c.text; break; }
        }
        if (!mp.empty()) {
            NodeEntry e;
            e.mesh_path = mp;
            e.parent_name = name;
            std::memcpy(e.transform, wx, 64);
            e.floating_group = float_group;
            e.is_stone_anim = stone_anim;
            if (stone_anim && stone_dummy_world) {
                e.stone_dummy_x = stone_dummy_world[0];
                e.stone_dummy_y = stone_dummy_world[1];
                e.stone_dummy_z = stone_dummy_world[2];
            }
            out.push_back(e);
        } else if (trace) {
            trace->fail("Asset fixture mesh path is missing in " + current_node_file +
                        " / " + name);
        }
    }
    // ObjectMeshDynamic = physics-driven cloth (flags etc.). The
    // node carries a baked particle/joint system for the cloth
    // simulation, but the underlying renderable is just the mesh
    // file. Rendering it as a static mesh at bind pose drops the
    // animation but at least makes the flag visible — the original
    // benchmark animates it via cloth physics, which we don't
    // currently simulate.

    // The mesh path is rarely inline on this node type — instead
    // it lives as a sibling.mesh file in meshes/props/ matching
    // the.node file's basename (flag.node → meshes/props/flag.mesh).
    // Fall back to that convention when no <mesh> child is present.
    if (t=="ObjectMeshDynamic") {
        std::string mp;
        for (const auto& c : node.children) {
            if (c.tag == "mesh" && !c.text.empty()) { mp = c.text; break; }
        }
        if (mp.empty() && !current_node_file.empty()) {
            // Strip directory and extension from the.node path,
            // then look in meshes/props/ for a matching.mesh file.
            std::string base = current_node_file;
            auto sl = base.find_last_of('/');
            if (sl != std::string::npos) base = base.substr(sl + 1);
            auto dot = base.rfind('.');
            if (dot != std::string::npos) base = base.substr(0, dot);
            std::string candidate = "heaven/meshes/props/" + base + ".mesh";
            const std::string candidate_path = paths.relative_path(candidate);
            if (asset_exists(candidate_path, assets)) {
                mp = candidate;
            }
        }
        if (!mp.empty()) {
            NodeEntry e;
            e.mesh_path = mp;
            e.parent_name = name;
            std::memcpy(e.transform, wx, 64);
            e.floating_group = float_group;
            e.is_stone_anim = stone_anim;
            // Mark this entry so the post-load cloth pass can find
            // and parse the BodyCloth definition. We store the
            // .node file path so cloth_sim can re-parse it later.
            e.cloth_source_node = current_node_file;
            out.push_back(e);
        } else if (trace) {
            trace->fail("Asset fixture dynamic mesh is missing in " + current_node_file +
                        " / " + name);
        }
    }
    if (t=="NodeReference" || t=="NodeLayer") {
        std::string rp;
        for (const auto& c : node.children)
            if (c.tag == "reference" && !c.text.empty()) { rp = c.text; break; }
        if (!rp.empty()) {
            if (trace && trace->active.contains(rp)) {
                trace->fail("Asset fixture node reference cycle: " + referring_file +
                            " -> " + rp);
                return;
            }
            std::string on_disk = paths.relative_path(rp);
            auto r = xml0_parse_tree(on_disk.c_str(), assets);
            if (!r.tag.empty()) {
                // The reference resolved to a readable xml0 file: record it as
                // used and recurse with `rp` as both the cloth-source node file
                // and the referring file for any unresolved edges it contains.
                if (rec) rec->record_used(on_disk);
                if (trace) {
                    trace->record(rp);
                    trace->active.insert(rp);
                }
                walk(r, paths, wx, out, emitters, lights, float_group, rp,
                     stone_anim, stone_dummy_world, rec, rp, assets, trace);
                if (trace) trace->active.erase(rp);
            } else {
                // An empty tag means the referenced file is missing or could
                // not be parsed — record it against the file that referenced it.
                if (rec) rec->record_unresolved(rp, referring_file);
                if (trace) trace->fail("Asset fixture node is missing or invalid: " +
                                      rp + " (referenced by " + referring_file + ")");
            }
        } else if (trace && t == "NodeReference") {
            trace->fail("Asset fixture node reference is empty in " + current_node_file);
        }
    }
    if (t=="ObjectParticles") {
        ParticleEmitter e = parse_particle_emitter(node, wx);
        e.floating_group = float_group;
        emitters.push_back(std::move(e));
    }
    // LightOmni / LightSpot — Heaven authors street lamps as spot
    // lights inside heaven/nodes/lamp_post_light.node and a single
    // standalone "blue_light" LightSpot inline in heaven.world.
    // Both share the same set of child tags: <radius>, <color>,
    // <multiplier>, plus optional <inner_angle>/<outer_angle> for
    // spots. The world transform is `wx`; the spot direction is the
    // matrix's local +Z axis after rotation, which under Unigine's
    // row-vector convention sits in the third *row*'s xyz (positions
    // wx[8], wx[9], wx[10]).
    if (t=="LightOmni" || t=="LightSpot") {
        SceneLight L;
        L.kind = (t=="LightSpot") ? SceneLightKind::Spot : SceneLightKind::Omni;
        L.name = name;
        L.world_pos = {wx[12], wx[13], wx[14]};

        // Position relative to the floating group's base — this lets
        // the backend re-derive the world position each frame from
        // the island animation.
        L.local_pos = L.world_pos;
        L.floating_group = float_group;

        // Direction = local +Z after rotation. We negate so the
        // resulting vector points the same way Filament expects
        // (away from the light source, i.e. "direction of travel").
        glm::vec3 fwd_unigine{wx[8], wx[9], wx[10]};
        if (auto len2 = glm::dot(fwd_unigine, fwd_unigine); len2 > 1e-6f)
            L.direction = -glm::normalize(fwd_unigine);
        else
            L.direction = {0.0f, 0.0f, -1.0f};

        L.radius     = parse_float(child_text(node, "radius"),     L.radius);
        L.multiplier = parse_float(child_text(node, "multiplier"), L.multiplier);
        // Some authored bulbs sit inside a closed metal fixture and explicitly
        // disable their own shadow map. Preserve this per-source contract when
        // the user enables local-light shadows for the scene.
        L.shadow_eligible = parse_float(child_text(node, "shadow"), 1.0f) > 0.5f;

        // <color> is "r g b a"; alpha is unused.
        if (auto cs = child_text(node, "color"); !cs.empty()) {
            float r=1, g=1, b=1, a=1;
            std::sscanf(cs.c_str(), "%f %f %f %f", &r, &g, &b, &a);
            L.color = {r, g, b};
        }

        // Spot cone angles. Heaven's <inner_angle>/<outer_angle> are
        // in degrees, half-angles. If unspecified (some files omit
        // them), defaults give a fairly tight 35°/55° cone, which is
        // visually matches the lamp post hood reasonably well.
        if (L.kind == SceneLightKind::Spot) {
            float inner_deg = parse_float(child_text(node, "inner_angle"), 35.0f);
            float outer_deg = parse_float(child_text(node, "outer_angle"), 55.0f);
            L.inner_cone_rad = inner_deg * std::numbers::pi_v<float> / 180.0f;
            L.outer_cone_rad = outer_deg * std::numbers::pi_v<float> / 180.0f;
        }

        lights.push_back(std::move(L));
    }
    for (const auto& c:node.children) if(c.tag=="node") walk(c,paths,wx,out,emitters,lights,float_group,current_node_file,stone_anim,stone_dummy_world,rec,referring_file,assets,trace);
}

void extract(const char* fp, const PathResolver& paths, std::vector<NodeEntry>& out,
                    std::vector<ParticleEmitter>& emitters,
                    std::vector<SceneLight>& lights,
                    AssetUsageRecorder* rec = nullptr,
                    AssetFileSystemRef assets = std::nullopt,
                    AuthoredSceneTrace* trace = nullptr,
                    const std::string& source_node = "",
                    const float* parent_transform = nullptr) {
    auto root=xml0_parse_tree(fp, assets);
    if(root.tag.empty()) {
        if (trace) trace->fail("Asset fixture node is missing or invalid: " + source_node);
        return;
    }
    constexpr float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const float* parent = parent_transform ? parent_transform : id;
    if (trace) { trace->record(source_node); trace->active.insert(source_node); }
    for(const auto& c:root.children) if(c.tag=="node") walk(c,paths,parent,out,emitters,lights,-1,source_node,false,nullptr,rec,fp,assets,trace);
    for(const auto& c:root.children) if(c.tag=="editor") for(const auto& e:c.children) if(e.tag=="node") walk(e,paths,parent,out,emitters,lights,-1,source_node,false,nullptr,rec,fp,assets,trace);
    if (trace) trace->active.erase(source_node);
}

} // namespace

Result<RhiScene> load_scene_rhi(const AppConfig& config, IRhiBackend& backend,
                                AssetUsageRecorder* rec,
                                AssetFileSystemRef assets,
                                const LoadingProgressCallback& progress) {
    if (progress) progress("Reading scene and meshes", 0.0f);
    ScopedStartupTimer scene_timer("scene base load");
    RhiScene scene;
    SceneMeshRollback mesh_rollback(backend, scene);
    const bool authored_fixture = render_test_scene_uses_assets(config.render_test_scene);
    AuthoredSceneTrace authored_trace;
    std::map<std::string, int> path_to_idx;
    size_t reused_mesh_refs = 0;
    size_t static_meshes_loaded = 0;
    size_t skinned_meshes_loaded = 0;
    double static_cpu_ms = 0.0;
    double static_gpu_ms = 0.0;
    double skinned_cpu_ms = 0.0;
    double skinned_gpu_ms = 0.0;

    // Reset the file-scope floating-group state so this load does not inherit
    // counters/animation parameters accumulated by an earlier load_scene_rhi
    // call. Without this, a second load of the same scene assigns different
    // floating-group indices than the first. The first load is
    // unaffected (the globals start at these same zero/empty values).
    g_next_float_group = 0;
    g_group_anim.clear();

    // Build the single PathResolver for this load from the configured
    // Data_Root. Data_Root is validated at application init, so create()
    // succeeds in practice; we still propagate the unexpected empty-Data_Root
    // case as a load error (load_scene_rhi returns Result<RhiScene>). Every
    // asset-path construction below (world, node, parsed mesh, cloth source)
    // is resolved through this one local resolver.
    auto paths_result = PathResolver::create(config.data_dir);
    if (!paths_result) return std::unexpected(paths_result.error());
    const PathResolver& paths = *paths_result;

    auto load = [&](const std::string& rel, bool force_unique = false,
                    const std::string& referrer = "") -> int {
        // Cloth-driven instances each need their own GpuMesh so the
        // per-frame vertex updates from the cloth solver don't
        // bleed across other (static) instances of the same mesh.
        if (!force_unique) {
            if (auto it=path_to_idx.find(rel); it!=path_to_idx.end()) {
                reused_mesh_refs++;
                return it->second;
            }
        }

        std::string full = paths.relative_path(rel);

        // Check if it's a skinned mesh (.smesh)
        if (rel.size() > 6 && rel.substr(rel.size() - 6) == ".smesh") {
            auto cpu_start = startup_timer_now();
            auto sr = load_smesh(full, assets);
            if (!sr) {
                std::fprintf(stderr,"  Failed smesh: %s\n",sr.error().message.c_str());
                if (rec) rec->record_unresolved(rel, referrer);
                if (authored_fixture) authored_trace.fail("Asset fixture mesh failed: " +
                    rel + ": " + sr.error().message);
                return -1;
            }
            auto& sm = sr.value();
            if (rec) rec->record_used(full);

            // Skin at frame 0 (bind pose) to create initial GPU mesh
            Mesh bind_mesh;
            bind_mesh.name = sm.name;
            bind_mesh.path = rel;
            for (size_t si = 0; si < sm.surfaces.size(); si++) {
                auto skinned_verts = sm.skin_frame(static_cast<int>(si), 0);
                Surface surf;
                surf.name = sm.surfaces[si].name;
                surf.vertices = std::move(skinned_verts);
                surf.indices = sm.surfaces[si].indices;
                bind_mesh.surfaces.push_back(std::move(surf));
            }
            skinned_cpu_ms += startup_elapsed_ms(cpu_start);

            int idx = static_cast<int>(scene.cpu_meshes.size());

            // Animated meshes retain their source type in both configurations.
            // The renderable's gpu_skinning flag decides whether the GPU applies
            // bones; the CPU fallback updates these same buffers and preserves
            // its UV-aligned tangent frames. Non-animated meshes stay static.
            auto gpu_start = startup_timer_now();
            GpuMesh gpu = (sm.num_frames > 1)
                              ? backend.upload_skinned(sm)
                              : backend.upload_mesh(bind_mesh);
            skinned_gpu_ms += startup_elapsed_ms(gpu_start);
            if (authored_fixture) {
                if (auto uploaded = validate_authored_upload(bind_mesh, gpu); !uploaded) {
                    authored_trace.fail(uploaded.error().message);
                    backend.release_mesh(gpu);
                    return -1;
                }
            }

            // The material resolver is keyed on the RELATIVE mesh path —
            // surface names like "Object04" recur across meshes and resolve to
            // different materials per path (passing the wrong path made the
            // weathercock inherit airship_cannons' rigging texture). load_smesh
            // sets sm.path to the FULL resolved path, so upload_skinned(sm)
            // tags the GpuMesh with that full path; pin it back to `rel` so the
            // GPU-skinned path resolves the same materials as the CPU bind-pose
            // path (bind_mesh.path == rel).
            gpu.path = rel;

            // Fast-spinning rotors must NOT cast the directional sun shadow.
            // The airship's port_propeller spins quickly; a per-frame skinned
            // shadow map sweeps a hard blade shadow across the nearby building
            // wall (the "rotating blades on the wall" artifact, worst at a low
            // sun where the blade's shadow is long). A real propeller at speed
            // motion-blurs into a faint translucent disc, so dropping the cast
            // is closer to the intended look than a hard sweeping shadow. The
            // slow rotors (mill_wings, weathercock) keep their shadows — their
            // sweep is gentle and reads correctly.
            if (rel.find("port_propeller") != std::string::npos) {
                gpu.cast_shadows = false;
            }

            // Store as animated object if it has animation frames
            if (sm.num_frames > 1) {
                AnimatedObject anim;
                anim.smesh = std::move(sm);
                anim.mesh_index = idx;
                anim.anim_speed = 30.0f; // 30 fps animation
                // Per-surface "last uploaded frame_f" — start at a
                // sentinel that always triggers the first upload.
                anim.last_frame_f.assign(anim.smesh.surfaces.size(), -1e30f);
                scene.animated.push_back(std::move(anim));
            }

            scene.gpu_meshes.push_back(std::move(gpu));
            scene.cpu_meshes.push_back(std::move(bind_mesh));
            if (!force_unique) path_to_idx[rel] = idx;
            skinned_meshes_loaded++;
            return idx;
        }

        // Regular static mesh
        auto cpu_start = startup_timer_now();
        auto r = load_mi08(full, assets);
        if (!r) {
            std::fprintf(stderr,"  Failed: %s\n",r.error().message.c_str());
            if (rec) rec->record_unresolved(rel, referrer);
            if (authored_fixture) authored_trace.fail("Asset fixture mesh failed: " +
                rel + ": " + r.error().message);
            return -1;
        }
        auto& m = r.value(); m.path=rel; m.name=rel.substr(rel.rfind('/')+1);
        static_cpu_ms += startup_elapsed_ms(cpu_start);
        if (rec) rec->record_used(full);
        int idx = static_cast<int>(scene.cpu_meshes.size());
        auto gpu_start = startup_timer_now();
        auto gpu = backend.upload_mesh(m);
        if (authored_fixture) {
            if (auto uploaded = validate_authored_upload(m, gpu); !uploaded) {
                authored_trace.fail(uploaded.error().message);
                backend.release_mesh(gpu);
                return -1;
            }
        }
        scene.gpu_meshes.push_back(std::move(gpu));
        static_gpu_ms += startup_elapsed_ms(gpu_start);
        scene.cpu_meshes.push_back(std::move(m));
        // Only register the dedupe entry when this load is shared.
        // Cloth-driven instances skip the cache so their GpuMesh
        // can be updated in isolation.
        if (!force_unique) path_to_idx[rel] = idx;
        static_meshes_loaded++;
        return idx;
    };

    std::vector<NodeEntry> entries;
    size_t bridge_start = 0, bridge_end = 0;

    // Best-effort referrer for a mesh that fails to load: the explicit node
    // file when one is configured, otherwise the world file.
    std::string scene_source = config.node_file.empty()
                                   ? paths.world_path()
                                   : config.node_file;

    {
        ScopedStartupTimer timer("scene xml extraction");
        if (authored_fixture) {
            const auto roots = render_test_scene_asset_nodes(config.render_test_scene);
            if (roots.empty()) return std::unexpected(Error("Asset fixture has no catalog nodes: " +
                                                            config.render_test_scene));
            for (const auto root : roots) {
                const std::string logical(root);
                const auto path = paths.relative_path(logical);
                const auto translation = render_test_scene_asset_translation(
                    config.render_test_scene, root);
                float parent[16] = {1,0,0,0,0,1,0,0,0,0,1,0,
                                    translation[0],translation[1],translation[2],1};
                const auto before = entries.size();
                extract(path.c_str(), paths, entries, scene.particle_emitters,
                        scene.lights, rec, assets, &authored_trace, logical, parent);
                if (!authored_trace.failure.empty())
                    return std::unexpected(Error(authored_trace.failure));
                if (rec) rec->record_used(path);
                std::printf("  Asset fixture node: %s (+%zu mesh instances)\n",
                            logical.c_str(), entries.size() - before);
            }
            if (entries.empty()) return std::unexpected(Error("Asset fixture has no mesh instances: " +
                                                              config.render_test_scene));
            scene.source_node_files = std::move(authored_trace.nodes);
        } else if (!config.node_file.empty()) {
            if (rec) rec->record_used(config.node_file);
            extract(config.node_file.c_str(), paths, entries, scene.particle_emitters,
                    scene.lights, rec, assets);
            std::printf("  Node file: %zu mesh instances\n", entries.size());
        } else {
            auto wp = paths.world_path();
            std::printf("  Loading world: %s\n", wp.c_str());
            if (rec) rec->record_used(wp);
            extract(wp.c_str(), paths, entries, scene.particle_emitters,
                    scene.lights, rec, assets);
            std::printf("  World file: %zu mesh instances\n", entries.size());

            // Load bridges.node explicitly — the world file's NodeLayer contains
            // bridge endpoints inline but not the bridge span segments
            auto bridges_path = *paths.node_path("bridges.node");
            if (rec) rec->record_used(bridges_path);
            bridge_start = entries.size();
            extract(bridges_path.c_str(), paths, entries, scene.particle_emitters,
                    scene.lights, rec, assets);
            bridge_end = entries.size();
            if (bridge_end > bridge_start)
                std::printf("  Bridges: +%zu mesh instances from bridges.node\n",
                            bridge_end - bridge_start);
        }
    }

    // Track entries that are cloth-driven so we can build their
    // ClothBody after all meshes have been loaded (we need the
    // bind-pose Mesh data, the GpuMesh handle and the world
    // transform).
    struct PendingCloth {
        int instance_index;
        int mesh_index;
        std::string source_node;
        glm::mat4 world_transform;
    };
    std::vector<PendingCloth> pending_cloths;

    {
        ScopedStartupTimer timer("scene mesh load + upload");
        size_t loaded_entries = 0;
        for (const auto& e : entries) {
            ++loaded_entries;
            if (progress) progress("Reading mesh " + std::to_string(loaded_entries) +
                "/" + std::to_string(entries.size()),
                0.2f * static_cast<float>(loaded_entries) / static_cast<float>(entries.size()));
            // Cloth-driven instances need their own private mesh so the
            // per-frame vertex updates don't corrupt other instances of
            // the same mesh path.
            bool unique = !e.cloth_source_node.empty();
            std::string mesh_referrer = e.cloth_source_node.empty() ? scene_source
                                                                    : e.cloth_source_node;
            int mi = load(e.mesh_path, unique, mesh_referrer);
            if (authored_fixture && mi < 0)
                return std::unexpected(Error(authored_trace.failure));
            if (mi >= 0) {
                int inst_idx = static_cast<int>(scene.instances.size());
                scene.instances.push_back({mi, unigine_to_glm(e.transform)});

            // Tag instances that are children of WorldExpression "move_*" nodes
            if (e.floating_group >= 0) {
                FloatingIsland fi;
                fi.instance_index = inst_idx;
                fi.base_transform = unigine_to_glm(e.transform);
                fi.group = e.floating_group;
                // Prefer the script-parsed values; fall back to the
                // historical (group%2)? heuristic + amp=2 if the
                // script body wasn't shaped the way we expect (this
                // keeps modded scenes alive even when the parser
                // can't latch onto the literal).
                if (std::cmp_less(e.floating_group, g_group_anim.size()) &&
                    g_group_anim[e.floating_group].valid) {
                    fi.speed     = g_group_anim[e.floating_group].speed;
                    fi.amplitude = g_group_anim[e.floating_group].amplitude;
                } else {
                    fi.speed     = (e.floating_group % 2 == 0) ? 0.25f : 0.5f;
                    fi.amplitude = 2.0f;
                }
                scene.floating_islands.push_back(fi);
            }

            // Stones driven by the `stone_noise` WorldExpression.
            // Speed/amplitude come straight from the script body
            // (see StoneAnim docs); the seed and dummy_z come from
            // the *parent ObjectDummy* (the node the script
            // actually iterates over) — captured at walk time as
            // `stone_dummy_*`. The rendered mesh sits at a per-
            // instance Z offset above/below the dummy
            // (`child_local_z`); we preserve that offset every
            // frame on top of the script-computed dummy_z.
            if (e.is_stone_anim) {
                StoneAnim sa;
                sa.instance_index = inst_idx;
                sa.base_transform = unigine_to_glm(e.transform);
                sa.dummy_z       = e.stone_dummy_z;
                sa.child_local_z = sa.base_transform[3].z - e.stone_dummy_z;
                sa.seed = e.stone_dummy_x * 13.0f + e.stone_dummy_y;
                sa.speed = 0.5f;
                sa.amplitude = 6.0f;
                scene.stone_anims.push_back(sa);
            }

            // Stash cloth-eligible entries for the post-load pass.
            if (!e.cloth_source_node.empty()) {
                PendingCloth pc;
                pc.instance_index = inst_idx;
                pc.mesh_index = mi;
                pc.source_node = e.cloth_source_node;
                pc.world_transform = unigine_to_glm(e.transform);
                pending_cloths.push_back(std::move(pc));
            }
            }
        }
        std::printf("  Mesh timings: static CPU %.2f ms, static GPU upload %.2f ms, "
                    "skinned CPU %.2f ms, skinned GPU upload %.2f ms\n",
                    static_cpu_ms, static_gpu_ms, skinned_cpu_ms, skinned_gpu_ms);
        std::printf("  Mesh cache: %zu static, %zu skinned, %zu reused instance refs\n",
                    static_meshes_loaded, skinned_meshes_loaded, reused_mesh_refs);
    }

    {
        ScopedStartupTimer timer("scene post-process metadata");
        for (const auto& m : scene.cpu_meshes)
            for (const auto& s : m.surfaces) {
                scene.total_vertices += static_cast<uint64_t>(s.vertices.size());
                scene.total_triangles += static_cast<uint64_t>(s.indices.size()) / 3;
            }

        // Collect surface names from skinned meshes — these surfaces also exist
        // in the parent static mesh and should not be rendered twice.
        // We mark the static mesh's instances that share surfaces with skinned
        // meshes by removing those surfaces from the GPU mesh.
        if (!scene.animated.empty()) {
            std::set<std::string> skinned_names;
            for (const auto& anim : scene.animated) {
                // Collect surface names from the skinned mesh
                for (const auto& surf : anim.smesh.surfaces)
                    skinned_names.insert(surf.name);
                // Also use the smesh filename (without extension) as a surface name to match
                std::string smesh_name = anim.smesh.name;
                // Strip path and extension
                auto slash = smesh_name.rfind('/');
                if (slash != std::string::npos) smesh_name = smesh_name.substr(slash + 1);
                auto dot = smesh_name.rfind('.');
                if (dot != std::string::npos) smesh_name = smesh_name.substr(0, dot);
                skinned_names.insert(smesh_name);
                // Also try the path-based name
                std::string path_name = anim.smesh.path;
                slash = path_name.rfind('/');
                if (slash != std::string::npos) path_name = path_name.substr(slash + 1);
                dot = path_name.rfind('.');
                if (dot != std::string::npos) path_name = path_name.substr(0, dot);
                skinned_names.insert(path_name);
            }
            // Hide static surfaces that duplicate skinned mesh geometry
            for (auto& gpu_mesh : scene.gpu_meshes) {
                bool is_skinned = false;
                for (const auto& anim : scene.animated)
                    if (anim.mesh_index >= 0 && &scene.gpu_meshes[anim.mesh_index] == &gpu_mesh)
                        { is_skinned = true; break; }
                if (is_skinned) continue;

                for (auto& surf : gpu_mesh.surfaces) {
                    if (skinned_names.contains(surf.name)) {
                        std::printf("    Hiding static surface '%s' in mesh '%s' (skinned duplicate)\n",
                                    surf.name.c_str(), gpu_mesh.name.c_str());
                        surf.index_count = 0;
                    }
                }
            }
        }

        // ── Synthetic emitter: airship steam boiler chimney ────────────
        // The original Heaven scene shows smoke rising from the airship's
        // steam-boiler chimney, but that emitter is NOT in any of the
        // extracted.node files (the boiler ships as a single ObjectMesh
        // with no ObjectParticles child). To match the original visual we
        // synthesise a smoke emitter at the boiler instance's chimney top.

        // This is the one place in the renderer where particle data does
        // not come from the binary; the fallback is gated on actually
        // finding the boiler mesh in the scene.
        {
            const std::string boiler_path = "heaven/meshes/airship/airship_steam_boiler.mesh";
            for (size_t i = 0; i < scene.cpu_meshes.size(); ++i) {
                const auto& m = scene.cpu_meshes[i];
                if (m.path != boiler_path) continue;

                // The boiler mesh is offset from its origin. Derive the
                // chimney opening from its top rim and transform that point.
                for (const auto& inst : scene.instances) {
                    if (!std::cmp_equal(inst.mesh_index, i)) continue;
                    const auto emitter_transform = boiler_smoke_transform(m, inst.transform);
                    if (!emitter_transform) continue;

                    ParticleEmitter e;
                    e.name          = "steam_boiler_smoke_synthetic";
                    e.material_name = "fire_smoke_white";

                    e.transform = *emitter_transform;

                    e.emitter_enabled = true;
                    e.emitter_type    = 0;          // box
                    // Chimney-bore-sized spawn area (~10cm), so smoke leaves
                    // the chimney as a tight column rather than a wide puff.
                    e.emitter_size    = glm::vec3(0.05f, 0.05f, 0.0f);
                    e.spawn_rate      = 18.0f;
                    e.life_mean       = 4.0f;
                    e.life_spread     = 1.5f;
                    e.velocity_mean   = 1.2f;
                    e.velocity_spread = 0.3f;
                    e.angle_mean      = 0.0f;
                    e.angle_spread    = 0.10f;       // tight upward cone
                    e.rotation_mean   = 0.0f;
                    e.rotation_spread = 90.0f;
                    // Tiny initial radius — smoke broadens as it rises via
                    // the size_ramp + growth term in the shader.
                    e.radius_mean     = 0.10f;
                    e.radius_spread   = 0.03f;
                    e.growth_mean     = 0.55f;       // 0.55 m/s expansion
                    e.growth_spread   = 0.15f;
                    e.gravity         = glm::vec3(0.0f, 0.0f, 0.6f); // gentle uplift
                    e.linear_damping  = 0.4f;
                    e.max_visible_distance = 200.0f;
                    e.max_particles   = 96;

                    scene.particle_emitters.push_back(std::move(e));
                }
            }
        }
    }

    std::printf("  Loaded %zu unique meshes, %zu instances, "
                "%llu vertices, %llu triangles\n",
                scene.cpu_meshes.size(), scene.instances.size(),
                static_cast<unsigned long long>(scene.total_vertices),
                static_cast<unsigned long long>(scene.total_triangles));
    scene.base_vertices = scene.total_vertices;
    scene.base_triangles = scene.total_triangles;
    if (!scene.animated.empty())
        std::printf("  Animated objects: %zu (skinned meshes with bone animation)\n", scene.animated.size());
    if (!scene.floating_islands.empty()) {
        std::printf("  Floating islands: %zu (sine wave animation)\n", scene.floating_islands.size());
        // Per-group authored params (speed, amplitude). Useful when
        // adding a new map that lays out new move_* expressions —
        // confirms the parser latched on rather than silently
        // falling back to the heuristic.
        std::map<int, std::pair<float, float>> seen;
        for (const auto& fi : scene.floating_islands)
            seen[fi.group] = {fi.speed, fi.amplitude};
        for (auto& [g, sp] : seen) {
            std::printf("    group %d: speed=%.3f amplitude=%.2f%s\n",
                        g, sp.first, sp.second,
                        (std::cmp_less(g, g_group_anim.size()) &&
                         g_group_anim[g].valid) ? " (parsed)" : " (fallback)");
        }
    }

    if (!scene.stone_anims.empty())
        std::printf("  Stone anims: %zu (per-position seeded noise)\n",
                    scene.stone_anims.size());
    if (!scene.particle_emitters.empty()) {
        std::printf("  Particle emitters: %zu (from binary nodes + synthetic)\n",
                    scene.particle_emitters.size());
    }

    // Set up bridge rope chains from bridge segment instances
    {
        ScopedStartupTimer timer("scene bridge setup");
        // Helper: find the nearest floating group to a world position.
        // Returns -1 if the position is on the main (static) island.
        // Uses nearest-instance matching for accuracy.
        auto find_nearest_group = [&](glm::vec3 pos) -> int {
            int best_group = -1;
            float best_dist = 28.0f;

            for (const auto& fi : scene.floating_islands) {
                glm::vec3 p = glm::vec3(fi.base_transform[3]);
                float d = glm::length(pos - p);
                if (d < best_dist) {
                    best_dist = d;
                    best_group = fi.group;
                }
            }
            return best_group;
        };

        // Bridge chains with known anchor positions (from bridges.node ObjectDummy nodes)
        struct ChainDef {
            glm::vec3 start, end;
            int num_segments;
        };

        // From the bridges.node data:
        // Bridge 1: main island → floating island (6 segments)
        // Bridge 2: floating island → floating island (18 segments)
        // Bridge 3: main island → floating island (11 segments)
        std::vector<ChainDef> chain_defs = {
            { glm::vec3(7.17f, 40.60f, 16.11f), glm::vec3(29.79f, 55.64f, 16.59f), 6 },
            { glm::vec3(104.24f, -8.57f, 49.62f), glm::vec3(56.63f, 46.56f, 15.91f), 18 },
            { glm::vec3(-50.74f, -41.65f, -3.38f), glm::vec3(-94.68f, -64.66f, -6.98f), 11 },
        };

        std::set<size_t> used_entries; // Track which bridge entries are already assigned

        for (auto& def : chain_defs) {
            // Find segment instances matching this chain by proximity
            struct SegInfo { int instance_idx; glm::vec3 pos; int seg_num; glm::mat4 base_xform; size_t entry_idx; };
            std::vector<SegInfo> segs;

            for (size_t ei = bridge_start; ei < bridge_end; ei++) {
                if (used_entries.contains(ei)) continue; // Already assigned to another chain
                const auto& e = entries[ei];
                if (e.mesh_path.find("bridge_part.mesh") == std::string::npos) continue;

                glm::mat4 xform = unigine_to_glm(e.transform);
                glm::vec3 pos = glm::vec3(xform[3]);

                float d_start = glm::length(pos - def.start);
                float d_end = glm::length(pos - def.end);
                float chain_len = glm::length(def.end - def.start);

                if (d_start + d_end < chain_len * 1.8f) {
                    int inst_idx = -1;
                    float best = 1e10f;
                    for (size_t i = 0; i < scene.instances.size(); i++) {
                        float d = glm::length(glm::vec3(scene.instances[i].transform[3]) - pos);
                        if (d < best) { best = d; inst_idx = static_cast<int>(i); }
                    }
                    if (inst_idx >= 0 && best < 1.0f) {
                        segs.push_back({inst_idx, pos, 0, xform, ei});
                    }
                }
            }

            if (segs.empty()) continue;

            // Sort segments by distance from start
            for (auto& s : segs) {
                s.seg_num = static_cast<int>(glm::length(s.pos - def.start) * 100);
            }
            std::sort(segs.begin(), segs.end(), [](const SegInfo& a, const SegInfo& b) {
                return a.seg_num < b.seg_num;
            });

            // Build chain
            BridgeChain bc;

            for (auto& s : segs) {
                bc.segment_instance_indices.push_back(s.instance_idx);
                bc.segment_base_transforms.push_back(s.base_xform);
                used_entries.insert(s.entry_idx); // Mark as used

                // Remove bridge segments from floating islands list
                scene.floating_islands.erase(
                    std::remove_if(scene.floating_islands.begin(), scene.floating_islands.end(),
                        [&](const FloatingIsland& fi) { return fi.instance_index == s.instance_idx; }),
                    scene.floating_islands.end());
            }

            // Match each anchor to its nearest floating group by proximity
            BridgeSystem::ChainAnchors ca;
            ca.start_base = def.start;
            ca.end_base = def.end;
            ca.start_group = find_nearest_group(def.start);
            ca.end_group = find_nearest_group(def.end);
            scene.bridge_system.anchors.push_back(ca);
            scene.bridge_system.chains.push_back(std::move(bc));

            std::printf("    Chain %zu: %zu segments, start_group=%d, end_group=%d\n",
                        scene.bridge_system.chains.size(), segs.size(),
                        ca.start_group, ca.end_group);
        }

        if (!scene.bridge_system.chains.empty()) {
            std::printf("  Initializing Jolt Physics for %zu bridge chains...\n",
                        scene.bridge_system.chains.size());
            scene.bridge_system.init_physics();
        }
    }

    // ── Cloth bodies (flags) ───────────────────────────────────

    // For every NodeEntry tagged as `cloth_source_node`, re-parse
    // the source.node and pull its BodyCloth definition. We
    // build one ClothBody per dynamic-mesh instance; the per-frame
    // step in main.cpp pushes simulated vertices to the matching
    // GpuMesh surface via update_mesh_vertices.
    if (!pending_cloths.empty()) {
        ScopedStartupTimer timer("scene cloth setup");
        std::printf("  Cloth bodies pending: %zu\n", pending_cloths.size());
        for (const auto& pc : pending_cloths) {
            // Re-parse the source.node so we can find the
            // ObjectMeshDynamic and its BodyCloth child.
            std::string src_path = paths.relative_path(pc.source_node);
            XmlNode root = xml0_parse_tree(src_path.c_str(), assets);
            // Locate the ObjectMeshDynamic child anywhere in the tree.
            std::function<const XmlNode*(const XmlNode&)> find_dyn =
                [&](const XmlNode& n) -> const XmlNode* {
                if (n.tag == "node" && n.get("type") == "ObjectMeshDynamic")
                    return &n;
                for (const auto& c : n.children)
                    if (auto p = find_dyn(c)) return p;
                return nullptr;
            };
            const XmlNode* dyn = find_dyn(root);
            if (!dyn) {
                std::fprintf(stderr,
                    "  cloth: no ObjectMeshDynamic in %s\n", src_path.c_str());
                if (authored_fixture) return std::unexpected(Error(
                    "Asset fixture cloth node is invalid: " + pc.source_node));
                continue;
            }

            ClothBody cb;
            cb.mesh_index    = pc.mesh_index;
            cb.surface_index = 0;
            if (!extract_cloth_body(*dyn, pc.world_transform,
                                    scene.cpu_meshes[pc.mesh_index], cb)) {
                if (authored_fixture) return std::unexpected(Error(
                    "Asset fixture cloth body is invalid: " + pc.source_node));
                continue;
            }
            std::printf("    Cloth '%s': %zu particles, %zu linear, %zu angular, %zu anchors\n",
                        cb.node_name.c_str(),
                        cb.position.size(),
                        cb.linear.size(),
                        cb.angular.size(),
                        cb.anchor_idx.size());
            scene.cloths.push_back(std::move(cb));
        }
    }

    if (authored_fixture) {
        const bool visible = std::any_of(scene.gpu_meshes.begin(), scene.gpu_meshes.end(),
            [](const GpuMesh& mesh) {
                return std::any_of(mesh.surfaces.begin(), mesh.surfaces.end(),
                    [](const GpuSurface& surface) { return surface.handle && surface.index_count; });
            });
        if (scene.instances.empty() || !scene.total_triangles || !visible)
            return std::unexpected(Error("Asset fixture has no uploaded geometry: " +
                                          config.render_test_scene));
    }
    if (progress) progress("Base scene ready", 0.2f);
    mesh_rollback.commit();
    return scene;
}

// ── Tessellation-aware scene loading ────────────────────────────

Result<RhiScene> load_scene_rhi(const AppConfig& config, IRhiBackend& backend,
                                 RhiMaterialDatabase& materials,
                                 const std::string& cache_dir,
                                 CameraCollision* collision,
                                 AssetFileSystemRef assets,
                                 const LoadingProgressCallback& progress) {
    ScopedStartupTimer scene_timer("scene load wrapper");

    // First load the scene normally
    auto result = load_scene_rhi(config, backend, nullptr, assets, progress);
    if (!result) return result;

    auto& scene = result.value();
    SceneMeshRollback mesh_rollback(backend, scene);

    // Temporary bridge: construct a local PathResolver from the configured
    // Data_Root so the material displacement lookups below can take a
    // PathResolver (changed their signatures). Data_Root was already
    // validated at application init, so create() succeeds here in practice; we
    // still propagate the unexpected empty-Data_Root case as a load error.
    // finalizes this by threading a PathResolver into load_scene_rhi
    // proper, replacing this local construction.
    auto paths_result = PathResolver::create(config.data_dir);
    if (!paths_result) return std::unexpected(paths_result.error());
    const PathResolver& paths = *paths_result;

    if (render_test_scene_uses_assets(config.render_test_scene)) {
        bool visible = false;
        for (const auto& gpu : scene.gpu_meshes) {
            for (const auto& surface : gpu.surfaces) {
                if (!surface.index_count) continue;
                // Use the existing inactive-surface / authored-hidden policy
                // before requiring an actual definition for a visible surface.
                if (materials.resolve(gpu.path, surface.name, paths, backend).hidden) continue;
                if (!materials.get_material_for_surface(gpu.path, surface.name))
                    return std::unexpected(Error("Asset fixture material is unresolved: " +
                        gpu.path + " / " + surface.name));
                visible = true;
            }
        }
        if (!visible) return std::unexpected(Error("Asset fixture has no visible material surfaces: " +
                                                  config.render_test_scene));
    }

    // Build the camera collision world NOW, from the base
    // (pre-tessellation) geometry. The tessellation pass below
    // replaces surface vertex/index data in place with up to 64×
    // the triangle count; building Jolt MeshShapes from that would
    // add ~10 s to startup and make every sphere-cast crawl over
    // displaced micro-detail. Collision only needs the coarse
    // collision hull, so the base mesh is exactly right.
    if (collision) {
        if (progress) progress("Building camera collision", 0.22f);
        ScopedStartupTimer timer("camera collision build");
        collision->build(scene.cpu_meshes, scene.instances);
    }

    // Map the user-facing tessellation parameters to topology and GPU
    // displacement controls, matching the original Heaven benchmark UI:
    //   - scale   → multiplies the GPU displacement amount
    //   - factor  → scales the CPU-generated near-camera topology density
    //   - distance→ scales each material's authored fade range
    //   - enabled → master on/off
    if (!config.tessellation.enabled) {
        std::printf("  CPU tessellation: disabled\n");
        mesh_rollback.commit();
        return result;
    }

    ScopedStartupTimer tess_timer("CPU tessellation total");

    // Unigine's quality factor scales the authored tessellation factor, not
    // the number of recursive subdivision passes. Filament cannot adapt
    // topology per camera, so baking the full 16-segment hardware maximum into
    // every surface permanently produces a 256x worst case. Three dyadic
    // levels retain the documented Heaven normal-quality density (64x worst
    // case) while the density map and authored edge length still stop quieter
    // regions earlier.
    constexpr int max_subs_h = kMaxTessellationSubdivisions;
    auto subdivisions_for_factor = [](float factor, int max_levels) {
        const float maximum_factor = static_cast<float>(1u << max_levels);
        const float clamped = std::clamp(factor, 0.0f, maximum_factor);
        int levels = 0;
        while (levels < max_levels &&
               static_cast<float>(1u << levels) < clamped) {
            ++levels;
        }
        return levels;
    };

    std::printf("  CPU tessellation: scale=%.2f factor=%.2f distance=%.2fx "
                "(max subs=%d, cache=v%u)\n",
                config.tessellation.scale,
                config.tessellation.factor,
                config.tessellation.distance,
                max_subs_h,
                kTessellationArchiveVersion);

    TessellationCacheArchive cache;
    auto cache_result = TessellationCacheArchive::open(
        cache_dir, assets, paths.cache_dir());
    if (cache_result) {
        cache = std::move(*cache_result);
    } else {
        std::fprintf(stderr,
                     "  CPU tessellation cache disabled for this load: %s\n",
                     cache_result.error().message.c_str());
    }

    // Generate neutral topology for surfaces with authored tessellation.
    int tess_count = 0;
    int enhanced_terrain_count = 0;
    int ground_relief_count = 0;
    int cache_hits = 0;
    int skinned_cache_hits = 0;
    double static_reupload_ms = 0.0;
    double skinned_reupload_ms = 0.0;
    std::size_t closed_seam_segments = 0;
    std::size_t closed_seam_vertices = 0;
    double closed_seam_maximum_width = 0;
    double closed_seam_maximum_extent = 0;
    double closed_seam_maximum_slope = 0;
    std::size_t closed_seam_triangle_evaluations = 0;
    std::size_t closed_seam_exact_ratio_repairs = 0;
    std::size_t closed_seam_rounding_stalls = 0;

    ClosedSeamHeightSampler seam_height_sampler(assets);
    auto static_tess_start = startup_timer_now();
    for (size_t mi = 0; mi < scene.cpu_meshes.size(); mi++) {
        if (progress) progress("Loading tessellation " + std::to_string(mi + 1) +
            "/" + std::to_string(scene.cpu_meshes.size()),
            0.25f + 0.6f * static_cast<float>(mi) / static_cast<float>(scene.cpu_meshes.size()));
        auto& mesh = scene.cpu_meshes[mi];
        const std::string mesh_source_path = paths.relative_path(mesh.path);
        bool mesh_modified = false;

        // Skip skinned meshes — tessellation adds vertices that
        // wouldn't have valid bone weights, and re-uploading would
        // also discard the pre-skinned bind pose. The static-mesh-
        // hiding pass below has already removed any duplicated
        // skinned surfaces from non-skinned meshes.
        bool is_skinned_mesh = false;
        for (const auto& anim : scene.animated) {
            if (std::cmp_equal(anim.mesh_index, mi)) {
                is_skinned_mesh = true;
                break;
            }
        }
        if (is_skinned_mesh) continue;

        // Collect from authored source boundaries, before any neutral archive
        // replaces a surface. Hidden/far-LOD or unresolved surfaces cannot
        // establish a flat neighbor. No texture upload is needed for this query.
        std::vector<SeamSurfaceKind> seam_kinds(mesh.surfaces.size(), SeamSurfaceKind::Inactive);
        std::vector<double> seam_offset_bounds(mesh.surfaces.size(), 0);
        std::vector<std::string> seam_height_paths(mesh.surfaces.size());
        std::vector<std::array<float, 4>> seam_height_transforms(mesh.surfaces.size());
        double minimum_instance_scale = std::numeric_limits<double>::infinity();
        for (const auto& instance : scene.instances) if (std::cmp_equal(instance.mesh_index, mi)) {
            minimum_instance_scale = std::min(minimum_instance_scale,
                closed_seam_minimum_spatial_scale(instance.transform));
        }
        std::size_t gpu_surface_index = 0;
        for (std::size_t si = 0; si < mesh.surfaces.size(); ++si) {
            const auto& source = mesh.surfaces[si];
            if (source.vertices.empty() || source.indices.empty()) continue;
            // MeshStore preserves nonempty source order, including duplicate
            // names. Do not use a same-named visible surface for a hidden one.
            const auto gi = gpu_surface_index++;
            if (gi >= scene.gpu_meshes[mi].surfaces.size() ||
                scene.gpu_meshes[mi].surfaces[gi].name != source.name ||
                scene.gpu_meshes[mi].surfaces[gi].index_count == 0 ||
                !materials.is_surface_active(mesh.path, source.name)) continue;
            const auto* definition = materials.get_material_for_surface(mesh.path, source.name);
            if (!definition) continue;
            const auto policy = resolve_surface_displacement_policy(config, paths,
                mesh.path, source.name, definition->name);
            const bool displaced = (definition->is_tessellation && definition->tess_length > 0) ||
                policy.has_value();
            seam_kinds[si] = displaced ? SeamSurfaceKind::Displaced : SeamSurfaceKind::Flat;
            if (displaced && minimum_instance_scale > 0 && std::isfinite(minimum_instance_scale)) {
                const double amplitude = policy ? policy->displacement_scale :
                    (definition->tess_scale_set ? definition->tess_scale_param : 0.5f);
                // Height and magnitude are bounded per vertex after neutral
                // tessellation. Keep physical amplitude separate from source
                // instance stretch; camera fade cannot increase this offset.
                seam_offset_bounds[si] = std::abs(amplitude * config.tessellation.scale) /
                    minimum_instance_scale;
                seam_height_paths[si] = policy ? policy->height_path :
                    materials.get_displacement_path(mesh.path, source.name, paths, "_h");
                std::copy(std::begin(definition->base_transform), std::end(definition->base_transform),
                    seam_height_transforms[si].begin());
            }
        }
        const auto closed_seams = collect_closed_displacement_seams(mesh, seam_kinds);
        closed_seam_segments += closed_seams.matched_segments;
        const bool seam_metric_supported = minimum_instance_scale > 0 && std::isfinite(minimum_instance_scale);
        if (closed_seams.matched_segments != 0 && !seam_metric_supported) {
            std::fprintf(stderr, "  Closed displacement seams retained for '%s': unsupported instance metric\n",
                mesh.path.c_str());
        }

        for (std::size_t si = 0; si < mesh.surfaces.size(); ++si) {
            auto& surf = mesh.surfaces[si];
            const uint64_t source_vertex_count = surf.vertices.size();
            const uint64_t source_index_count = surf.indices.size();
            // Ordinary geometric displacement happens only for materials that
            // derive from a `*_tessellation*` base (they alone carry the
            // engine's TCS/TES stages) AND that author a non-zero
            // `tessellation_length` (the target edge length used by the
            // control shader; default 0 disables tessellation). Plain
            // materials ship a
            // `parallax`/`height` texture for parallax-occlusion mapping
            // only — the original engine renders them flat. Gating on the
            // material class (instead of "has a height texture") stops us
            // from warping the dozens of POM-only stone/tile surfaces that
            // used to tear and fold. Massive Foliage's three specifically
            // identified island-ground surfaces are the deliberate exception:
            // that optional enhancement supplies its own bounded topology and
            // height parameters while retaining the original renderables.
            const MaterialDef* def =
                materials.get_material_for_surface(mesh.path, surf.name);
            if (def == nullptr) continue;
            const auto displacement_policy =
                resolve_surface_displacement_policy(
                    config, paths, mesh.path, surf.name, def->name);
            const bool enhanced_terrain = displacement_policy.has_value();
            if (!enhanced_terrain && !def->is_tessellation) continue;
            if (!enhanced_terrain && !(def->tess_length > 0.0f)) continue;

            // `_h` is sampled by the GPU for camera-dependent displacement;
            // `_t` controls the neutral topology generated here.
            std::string height_path = enhanced_terrain
                ? displacement_policy->height_path
                : materials.get_displacement_path(
                    mesh.path, surf.name, paths, "_h");
            std::string density_path = enhanced_terrain
                ? displacement_policy->density_path
                : materials.get_displacement_path(
                    mesh.path, surf.name, paths, "_t");
            // `mesh_base_tessellation.dds` is Unigine's inherited white
            // default for s_texture_16, not an authored density map. Model it
            // directly so core/heaven root resolution cannot accidentally
            // turn the default into a missing-file tessellation opt-out.
            if (density_path.find("mesh_base_tessellation.dds") !=
                std::string::npos) {
                density_path.clear();
            }
            const float authored_factor = enhanced_terrain
                ? displacement_policy->tessellation_factor
                : (def->tess_factor > 0.0f ? def->tess_factor : 1.0f);
            const float maximum_factor = enhanced_terrain
                ? (displacement_policy->kind ==
                           SurfaceDisplacementKind::VillageGround
                       ? 32.0f : 30.0f)
                : 15.0f;
            const float density_factor = std::clamp(
                authored_factor * std::max(config.tessellation.factor, 0.0f),
                0.0f, maximum_factor);
            // control_base.shader multiplies each edge by
            // saturate(edge_length * s_material_tessellation_distance.z).
            // That packed component is the reciprocal of the authored target
            // edge length, not the independently-authored displacement scale.
            const float tessellation_length = enhanced_terrain
                ? displacement_policy->tessellation_length : def->tess_length;
            const float edge_scale = 1.0f / tessellation_length;
            const int maximum_subdivisions = enhanced_terrain
                ? displacement_policy->max_subdivisions : max_subs_h;
            int subdivisions = subdivisions_for_factor(
                density_factor, maximum_subdivisions);

            if (height_path.empty() ||
                !asset_exists(height_path, assets)) continue;
            // A missing `_t` is the tessellation base material's authored
            // white default, not an opt-out. cpu_tessellate_surface() models
            // it as a uniform density of one.
            if (!density_path.empty() &&
                !asset_exists(density_path, assets)) continue;

            // Skip the inherited 16×16 `mesh_base_*` placeholder
            // heightmaps — they have no detail and just slightly
            // jitter vertices off the bind surface. A tessellation
            // material that only inherits the placeholder height map
            // (no authored displacement texture) should be left flat.
            if (height_path.find("mesh_base_parallax") != std::string::npos ||
                height_path.find("mesh_base_tessellation") != std::string::npos ||
                height_path.find("mesh_base_height") != std::string::npos) {
                continue;
            }

            // Apply the per-material `base_transform` to the UV used for
            // height sampling — this matches the original engine. Its
            // vertex shader computes
            //   texcoord.xy = uv * base_transform.xy + base_transform.zw
            // and that SAME transformed texcoord flows through
            // s_texcoord_0 into the TES (evaluate_base.shader), which
            // samples the displacement map with it. Skipping it means
            // surfaces with a non-identity transform (fort_steps =
            // (2,1,-0.5,0), fort_merlon = (2,1,-1,0), ruins_column =
            // (1,2,0,-0.5), fort_wall = (0.5,1,0.25,0)) displace against
            // the wrong region of the height map.
            const std::span<const float> mat_base_transform{
                def->base_transform};
            TessellationCacheParameters cache_parameters{
                .density_path = density_path,
                .height_path = height_path,
                .density_factor = density_factor,
                .edge_scale = edge_scale,
                .subdivisions = subdivisions,
                .base_transform = {
                    mat_base_transform[0], mat_base_transform[1],
                    mat_base_transform[2], mat_base_transform[3]},
                .boundary_fade = displacement_policy
                    ? displacement_policy->boundary_fade : 0.0f,
                .surface_policy_version = displacement_policy
                    ? displacement_policy->policy_version : 0,
            };
            if (displacement_policy && displacement_policy->kind ==
                    SurfaceDisplacementKind::VillageGround) {
                cache_parameters.source_normal_path =
                    paths.relative_path(kGroundNormalRelativePath);
                cache_parameters.generator_revision = std::format(
                    "{}:{}:{}:{}:{}", kGroundHeightGeneratorId,
                    kGroundHeightGeneratorVersion, kDeepBumpRevision,
                    kPocketFftRevision, kBcdecRevision);
            }

            // Skip tessellation for foliage surfaces — they're flat billboard
            // quads that get destroyed by vertex displacement
            if (surf.name.find("branch") != std::string::npos ||
                surf.name.find("leaf") != std::string::npos ||
                surf.name.find("leaves") != std::string::npos ||
                surf.name.find("fir_") != std::string::npos ||
                surf.name.find("_fir") != std::string::npos) continue;

            // Try loading from cache
            if (cache.load(surf, mesh_source_path, surf.name,
                           cache_parameters)) {
                cache_hits++;
                tess_count++;
                if (enhanced_terrain) enhanced_terrain_count++;
                if (displacement_policy &&
                    displacement_policy->kind ==
                        SurfaceDisplacementKind::VillageGround) {
                    ++ground_relief_count;
                }
                mesh_modified = true;
                continue;
            }

            // CPU tessellate
            if (progress) progress("Building tessellation: " + surf.name,
                0.25f + 0.6f * static_cast<float>(mi) / static_cast<float>(scene.cpu_meshes.size()));
            size_t orig_tris = surf.indices.size() / 3;
            if (!cpu_tessellate_surface(
                    surf, density_path,
                    TessellationTopologyParameters{
                        .density_factor = density_factor,
                        .edge_scale = edge_scale,
                        .subdivisions = subdivisions,
                        .base_transform = {
                            mat_base_transform[0], mat_base_transform[1],
                            mat_base_transform[2], mat_base_transform[3]},
                        .boundary_fade = displacement_policy
                            ? displacement_policy->boundary_fade : 0.0f,
                    },
                    assets)) {
                continue;
            }
            size_t new_tris = surf.indices.size() / 3;
            // Save to cache
            cache.stage(surf, mesh_source_path, surf.name,
                        cache_parameters,
                        source_vertex_count, source_index_count);

            tess_count++;
            if (enhanced_terrain) enhanced_terrain_count++;
            if (displacement_policy &&
                displacement_policy->kind ==
                    SurfaceDisplacementKind::VillageGround) {
                ++ground_relief_count;
            }
            mesh_modified = true;

            if (tess_count <= 5) {
                std::printf("    Tessellated '%s': %zu → %zu tris (factor=%.2f)\n",
                            surf.name.c_str(), orig_tris, new_tris, density_factor);
            }
        }

        // Apply once, after every neutral cache load/build. A disabled density
        // split can still leave shader displacement active on original vertices.
        // Neutral archives remain reusable; upload/prepared keys see this field.
        for (std::size_t si = 0; si < mesh.surfaces.size(); ++si) {
            if (!seam_metric_supported || closed_seams.surfaces[si].empty()) continue;
            const auto heights = seam_height_sampler.bounds(mesh.surfaces[si], seam_height_paths[si],
                seam_height_transforms[si], config.texture_quality);
            if (heights.universal_vertices != 0) {
                std::fprintf(stderr, "  Closed displacement height bound fallback '%s'/'%s': %zu/%zu vertices use unsigned envelope\n",
                    mesh.path.c_str(), mesh.surfaces[si].name.c_str(), heights.universal_vertices,
                    mesh.surfaces[si].vertices.size());
            }
            std::vector<double> offsets(mesh.surfaces[si].vertices.size());
            for (std::size_t vi = 0; vi < offsets.size(); ++vi)
                offsets[vi] = heights.maximum_height_delta[vi] * seam_offset_bounds[si] *
                    std::abs(static_cast<double>(mesh.surfaces[si].vertices[vi].displacement_normal_scale));
            ClosedSeamFadeReport fade_report;
            const auto changed = apply_closed_displacement_seams(mesh.surfaces[si],
                closed_seams.surfaces[si], closed_seams.fade_distance, closed_seams.matching_tolerance,
                0, &fade_report, offsets);
            closed_seam_triangle_evaluations += fade_report.evaluated_triangles;
            closed_seam_exact_ratio_repairs += fade_report.exact_ratio_relaxations;
            closed_seam_rounding_stalls += fade_report.rounding_stalls;
            if (!closed_seams.surfaces[si].empty() && !fade_report.slope_limit_met) {
                std::fprintf(stderr, "  Closed displacement seams retained for '%s'/'%s': local slope limit unresolved (%zu triangles, work limit %s)\n",
                    mesh.path.c_str(), mesh.surfaces[si].name.c_str(), fade_report.unresolved_triangles,
                    fade_report.work_limit_exhausted ? "exhausted" : "not exhausted");
                continue;
            }
            closed_seam_maximum_width = std::max(closed_seam_maximum_width, fade_report.fade_distance);
            closed_seam_maximum_extent = std::max(closed_seam_maximum_extent, fade_report.affected_distance);
            closed_seam_maximum_slope = std::max(closed_seam_maximum_slope, fade_report.maximum_added_slope);
            closed_seam_vertices += changed;
            if (changed != 0) mesh_modified = true;
        }
        // Re-upload the mesh if topology or a displacement constraint changed.
        if (mesh_modified) {
            auto upload_start = startup_timer_now();
            auto replacement = backend.upload_mesh(mesh);
            backend.release_mesh(scene.gpu_meshes[mi]);
            scene.gpu_meshes[mi] = std::move(replacement);
            static_reupload_ms += startup_elapsed_ms(upload_start);
        }
    }
    std::printf("  [startup] %-34s %8.2f ms\n",
                "CPU tessellation static", startup_elapsed_ms(static_tess_start));

    if (tess_count > 0) {
        std::printf("  CPU tessellation: %d surfaces (%d from cache), "
                    "%d enhanced terrain (%d village ground), static reupload %.2f ms\n",
                    tess_count, cache_hits, enhanced_terrain_count,
                    ground_relief_count, static_reupload_ms);
    }
    if (closed_seam_vertices != 0) {
        std::printf("  Closed displacement seams: %zu source segments, %zu constrained vertices, "
                    "maximum fade %.3f m, affected extent %.3f m, added slope %.3f, %zu triangle evaluations, %zu exact-ratio repairs, %zu rounding stalls\n",
                    closed_seam_segments, closed_seam_vertices,
                    closed_seam_maximum_width, closed_seam_maximum_extent,
                    closed_seam_maximum_slope, closed_seam_triangle_evaluations,
                    closed_seam_exact_ratio_repairs, closed_seam_rounding_stalls);
    }

    // ── Skinned-mesh tessellation ────────────────────────────────

    // Skinned meshes (gears, weathercock, mill wings, propeller)
    // also have tessellation/heightmap entries in their materials.
    // Without subdividing them, the GPU `_h` displacement has too few
    // vertices to reproduce the lab gears' tooth detail. The `_t` map
    // supplies topology density; it is never treated as height.

    // The algorithm differs from the static path: we operate on
    // SkinnedSurface (which carries bone weights), interpolate
    // weights/UVs/normals barycentrically into each new midpoint,
    // and let the runtime per-frame skinning then transform the
    // dense vertex set the same way it does the bind pose. After
    // tessellation we re-skin frame 0 to refresh the bind-pose
    // Surface uploaded to the GPU.
    int sk_tess = 0;
    auto skinned_tess_start = startup_timer_now();
    for (auto& anim : scene.animated) {
        if (progress) progress("Loading animated tessellation", 0.9f);
        if (anim.mesh_index < 0 ||
            std::cmp_greater_equal(anim.mesh_index, scene.cpu_meshes.size())) {
            continue;
        }
        auto& mesh = scene.cpu_meshes[anim.mesh_index];
        bool mesh_modified = false;

        for (size_t si = 0; si < anim.smesh.surfaces.size() &&
                            si < mesh.surfaces.size(); ++si) {
            auto& sk_surf = anim.smesh.surfaces[si];
            const auto& mesh_surf_name = mesh.surfaces[si].name;

            // Same data-driven gate as the static path: only
            // `*_tessellation*` materials with an authored target edge length
            // receive dense topology and GPU displacement; POM-only materials
            // remain flat.
            const MaterialDef* def =
                materials.get_material_for_surface(mesh.path, mesh_surf_name);
            if (def == nullptr) continue;
            if (!def->is_tessellation) continue;
            if (!(def->tess_length > 0.0f)) continue;

            // GPU displacement samples the `height` (`_h`) map; `_t`
            // controls only the CPU-generated topology density.
            std::string height_path = materials.get_displacement_path(
                mesh.path, mesh_surf_name, paths, "_h");
            std::string density_path = materials.get_displacement_path(
                mesh.path, mesh_surf_name, paths, "_t");
            if (density_path.find("mesh_base_tessellation.dds") !=
                std::string::npos) {
                density_path.clear();
            }
            const float density_factor =
                std::clamp(
                    (def->tess_factor > 0.0f ? def->tess_factor : 1.0f) *
                    std::max(config.tessellation.factor, 0.0f),
                    0.0f, 15.0f);
            const float edge_scale = 1.0f / def->tess_length;
            int subdivisions = subdivisions_for_factor(
                density_factor, max_subs_h);
            if (height_path.empty() ||
                !asset_exists(height_path, assets)) continue;
            if (!density_path.empty() &&
                !asset_exists(density_path, assets)) continue;

            // Skip placeholder heightmaps inherited from `mesh_base`
            // — they're 16×16 stubs that produce uniform displacement
            // and just drag the bind-pose mesh slightly off its
            // original surface without adding any detail.
            if (height_path.find("mesh_base_parallax")     != std::string::npos ||
                height_path.find("mesh_base_tessellation") != std::string::npos ||
                height_path.find("mesh_base_height")       != std::string::npos) {
                continue;
            }

            // Apply base_transform to the height sample (engine-faithful;
            // see the static-surface block above).
            const std::span<const float> sk_base_transform{
                def->base_transform};
            const TessellationCacheParameters sk_cache_parameters{
                .density_path = density_path,
                .height_path = height_path,
                .density_factor = density_factor,
                .edge_scale = edge_scale,
                .subdivisions = subdivisions,
                .base_transform = {
                    sk_base_transform[0], sk_base_transform[1],
                    sk_base_transform[2], sk_base_transform[3]},
            };
            // Foliage skip — not relevant for the skinned set today
            // but kept for symmetry with the static path.
            if (mesh_surf_name.find("branch") != std::string::npos ||
                mesh_surf_name.find("leaf")   != std::string::npos ||
                mesh_surf_name.find("leaves") != std::string::npos ||
                mesh_surf_name.find("fir_")   != std::string::npos ||
                mesh_surf_name.find("_fir")   != std::string::npos)
                continue;

            const uint64_t source_vertex_count = sk_surf.vertices.size();
            const uint64_t source_index_count = sk_surf.indices.size();
            if (cache.load_skinned(
                    sk_surf, anim.smesh.path, mesh_surf_name,
                    sk_cache_parameters)) {
                ++cache_hits;
                ++skinned_cache_hits;
                ++sk_tess;
                mesh_modified = true;
                continue;
            }

            size_t orig_tris = sk_surf.indices.size() / 3;
            if (progress) progress("Building animated tessellation: " + mesh_surf_name, 0.9f);
            if (!cpu_tessellate_skinned_surface(
                    sk_surf, density_path,
                    TessellationTopologyParameters{
                        .density_factor = density_factor,
                        .edge_scale = edge_scale,
                        .subdivisions = subdivisions,
                        .base_transform = {
                            sk_base_transform[0], sk_base_transform[1],
                            sk_base_transform[2], sk_base_transform[3]},
                    }, assets)) {
                continue;
            }
            size_t new_tris = sk_surf.indices.size() / 3;
            cache.stage_skinned(
                sk_surf, anim.smesh.path, mesh_surf_name,
                sk_cache_parameters,
                source_vertex_count, source_index_count);
            sk_tess++;
            mesh_modified = true;

            if (sk_tess <= 5) {
                std::printf("    Tessellated skinned '%s': %zu → %zu tris (factor=%.2f)\n",
                            mesh_surf_name.c_str(), orig_tris, new_tris,
                            density_factor);
            }
        }

        // Re-skin frame 0 to refresh the bind-pose Surface that the
        // GPU buffer was sized for, and re-upload so subsequent
        // per-frame `update_mesh_vertices` calls have the right
        // capacity.
        if (mesh_modified) {
            // Keep the CPU bind-pose Mesh (scene.cpu_meshes) consistent with
            // the tessellated topology by re-skinning frame 0 into its
            // surfaces. This is needed for the --no-gpu-skinning re-upload
            // path (right vertex capacity) and any CPU-side consumer of the
            // dense vertex set.
            for (size_t si = 0; si < anim.smesh.surfaces.size() &&
                                si < mesh.surfaces.size(); ++si) {
                auto skinned_verts = anim.smesh.skin_frame(static_cast<int>(si), 0);
                mesh.surfaces[si].vertices = std::move(skinned_verts);
                mesh.surfaces[si].indices  = anim.smesh.surfaces[si].indices;
            }

            // Preserve animated source ownership after tessellation, including
            // the CPU fallback. New midpoint bone weights are retained; only
            // the renderable flag decides whether the GPU applies those bones.
            if (anim.smesh.num_frames > 1) {
                // upload_skinned tags the GpuMesh with anim.smesh.path (the
                // FULL resolved path) and resets cast_shadows to the default;
                // preserve the relative path the material resolver is keyed on
                // and the cast-shadow decision made at initial load (e.g. the
                // port_propeller's disabled cast) so the re-upload doesn't
                // mis-resolve materials or reintroduce the rotor shadow.
                const std::string keep_path =
                    scene.gpu_meshes[anim.mesh_index].path;
                const bool keep_cast =
                    scene.gpu_meshes[anim.mesh_index].cast_shadows;
                auto upload_start = startup_timer_now();
                auto replacement = backend.upload_skinned(anim.smesh);
                backend.release_mesh(scene.gpu_meshes[anim.mesh_index]);
                scene.gpu_meshes[anim.mesh_index] = std::move(replacement);
                skinned_reupload_ms += startup_elapsed_ms(upload_start);
                scene.gpu_meshes[anim.mesh_index].path        = keep_path;
                scene.gpu_meshes[anim.mesh_index].cast_shadows = keep_cast;
            } else {
                auto upload_start = startup_timer_now();
                auto replacement = backend.upload_mesh(mesh);
                backend.release_mesh(scene.gpu_meshes[anim.mesh_index]);
                scene.gpu_meshes[anim.mesh_index] = std::move(replacement);
                skinned_reupload_ms += startup_elapsed_ms(upload_start);
            }
        }
    }
    std::printf("  [startup] %-34s %8.2f ms\n",
                "CPU tessellation skinned", startup_elapsed_ms(skinned_tess_start));
    if (sk_tess > 0) {
        std::printf(
            "  CPU tessellation (skinned): %d surfaces (%d from cache), "
            "reupload %.2f ms\n",
            sk_tess, skinned_cache_hits, skinned_reupload_ms);
    }

    // Re-apply skinned mesh surface hiding (tessellation re-upload may have reset it)
    if (!scene.animated.empty()) {
        ScopedStartupTimer timer("tessellation surface hiding");
        std::set<std::string> skinned_names;
        for (const auto& anim : scene.animated) {
            for (const auto& surf : anim.smesh.surfaces)
                skinned_names.insert(surf.name);
            std::string name = anim.smesh.path;
            auto slash = name.rfind('/'); if (slash != std::string::npos) name = name.substr(slash+1);
            auto dot = name.rfind('.'); if (dot != std::string::npos) name = name.substr(0, dot);
            skinned_names.insert(name);
            name = anim.smesh.name;
            slash = name.rfind('/'); if (slash != std::string::npos) name = name.substr(slash+1);
            dot = name.rfind('.'); if (dot != std::string::npos) name = name.substr(0, dot);
            skinned_names.insert(name);
        }
        for (auto& gpu_mesh : scene.gpu_meshes) {
            bool is_skinned = false;
            for (const auto& anim : scene.animated)
                if (anim.mesh_index >= 0 && &scene.gpu_meshes[anim.mesh_index] == &gpu_mesh)
                    { is_skinned = true; break; }
            if (is_skinned) continue;
            for (auto& surf : gpu_mesh.surfaces)
                if (skinned_names.contains(surf.name))
                    surf.index_count = 0;
        }
    }

    scene.total_vertices = 0;
    scene.total_triangles = 0;
    for (const auto& mesh : scene.cpu_meshes) {
        for (const auto& surface : mesh.surfaces) {
            scene.total_vertices += surface.vertices.size();
            scene.total_triangles += surface.indices.size() / 3;
        }
    }
    std::printf("  Tessellated geometry: %llu vertices, %llu triangles "
                "(base: %llu vertices, %llu triangles)\n",
                static_cast<unsigned long long>(scene.total_vertices),
                static_cast<unsigned long long>(scene.total_triangles),
                static_cast<unsigned long long>(scene.base_vertices),
                static_cast<unsigned long long>(scene.base_triangles));

    if (progress) progress("Saving tessellation cache", 0.95f);
    if (auto published = cache.finish(); !published) {
        std::fprintf(stderr,
                     "  CPU tessellation cache write failed (nonfatal): %s\n",
                     published.error().message.c_str());
    }

    if (render_test_scene_uses_assets(config.render_test_scene)) {
        for (size_t mi = 0; mi < scene.cpu_meshes.size(); ++mi) {
            // Static counterparts of animated surfaces are intentionally hidden
            // after both uploads; require live handles rather than re-enabling them.
            const auto& cpu = scene.cpu_meshes[mi];
            const auto& gpu = scene.gpu_meshes[mi];
            for (size_t si = 0; si < cpu.surfaces.size(); ++si) {
                if (cpu.surfaces[si].indices.empty()) continue;
                if (si >= gpu.surfaces.size() || !gpu.surfaces[si].handle)
                    return std::unexpected(Error("Asset fixture tessellated mesh upload failed: " +
                        cpu.path + " / " + cpu.surfaces[si].name));
            }
        }
    }

    if (progress) progress("CPU scene preparation complete", 1.0f);
    mesh_rollback.commit();
    return result;
}

} // namespace heaven
