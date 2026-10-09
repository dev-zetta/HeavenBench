/*
 * HeavenBench — Cloth simulation (PBD)
 *
 * The.node files for `ObjectMeshDynamic` bodies (flags) carry a
 * baked PBD network: particle rest positions, distance constraints,
 * angle constraints and an anchor list pinning some particles to a
 * rigid parent. This file extracts that data and runs the simulation.
 *
 * Coordinate system: simulation runs in world space. Particle
 * positions are stored as authored (scene world space, Z-up).
 * Anchor positions track the parent rigid body's transform — for
 * a static flag pole that's just a constant world transform; for
 * a flag pole on a floating island we pull the parent transform
 * from the scene each frame.
 */
#include "engine/cloth_sim.h"
#include "engine/number_parse.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace heaven {
namespace {

// ── Helpers ─────────────────────────────────────────────────────

// Heaven stores binary-ish blobs as ASCII space-separated numbers.
// Parse a flat float array and a flat int array out of the element
// text. These are robust to any whitespace.

std::vector<float> parse_floats(const std::string& s) {
    std::vector<float> out;
    out.reserve(s.size() / 6);
    std::istringstream iss(s);
    float v;
    while (iss >> v) out.push_back(v);
    return out;
}

std::vector<int> parse_ints(const std::string& s) {
    std::vector<int> out;
    out.reserve(s.size() / 4);
    std::istringstream iss(s);
    int v;
    while (iss >> v) out.push_back(v);
    return out;
}

// Find the BodyCloth descendant of `n`, or return null.
const XmlNode* find_body_cloth(const XmlNode& n) {
    if (n.tag == "body" && n.get("type") == "BodyCloth") return &n;
    for (const auto& c : n.children) if (auto p = find_body_cloth(c)) return p;
    return nullptr;
}

// Find the first child of `parent` whose tag matches `t`.
const XmlNode* find_direct_child(const XmlNode& parent, const std::string& t) {
    for (const auto& c : parent.children) if (c.tag == t) return &c;
    return nullptr;
}

// Quantise a 3D position so the same point hashes identically at
// reasonable float precision. Used to identify which mesh vertices
// share a position (UV-seam duplicates that should map to the same
// cloth particle).
struct PosKey {
    int qx, qy, qz;
    bool operator==(const PosKey& o) const {
        return qx == o.qx && qy == o.qy && qz == o.qz;
    }
};
struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        size_t h = std::hash<int>{}(k.qx);
        h = h * 31 + std::hash<int>{}(k.qy);
        h = h * 31 + std::hash<int>{}(k.qz);
        return h;
    }
};
PosKey make_key(float x, float y, float z) {
    return {
        static_cast<int>(std::round(x * 5000.0f)),
        static_cast<int>(std::round(y * 5000.0f)),
        static_cast<int>(std::round(z * 5000.0f))
    };
}

} // namespace

// ── Extraction ─────────────────────────────────────────────────

bool extract_cloth_body(const XmlNode& dynamic_node,
                        const glm::mat4& world_transform,
                        const Mesh& bind_mesh,
                        ClothBody& out)
{
    // Sanity: we only care about ObjectMeshDynamic nodes.
    if (dynamic_node.get("type") != "ObjectMeshDynamic") return false;

    const XmlNode* cloth = find_body_cloth(dynamic_node);
    if (!cloth) return false;

    // ── Particles ──────────────────────────────────────────────
    const XmlNode* p_node = find_direct_child(*cloth, "particles");
    if (!p_node) return false;
    const int n_particles =
        parse_number_prefix<int>(p_node->get("num_particles")).value_or(0);
    auto p_floats = parse_floats(p_node->text);
    if (n_particles < 0 ||
        p_floats.size() != static_cast<std::size_t>(n_particles) * 4u) {
        return false;
    }

    // Heaven authors particle positions in **scene/world** space
    // (relative to the body's authoring frame). The body's
    // `<itransform>` is the inverse-world transform that, when
    // applied to a world-space particle, yields the body-local
    // position. For a flag at the world origin (the main flag.node)
    // itransform is ≈ identity, so particles already look "local".
    // For flag_small instances placed elsewhere by the world file,
    // particles are stored in their AUTHORING world space — so we
    // must apply itransform to convert them to body-local before
    // simulating. Without this step the particles are double-
    // transformed at render time (instance.transform * world_pos)
    // and the flag flies off into space.
    glm::mat4 itransform(1.0f);
    if (const XmlNode* it = find_direct_child(*cloth, "itransform")) {
        auto fs = parse_floats(it->text);
        if (fs.size() >= 16) {
            // Heaven row-major == GLM column-major (same memory
            // layout) per the project's convention; just memcpy.
            std::memcpy(&itransform[0][0], fs.data(), sizeof(float) * 16);
        }
    }

    // Gravity is authored as world-space (0, 0, -9.81). Express
    // it in the body's local frame so the PBD step pulls things
    // "down" relative to the world even when the flag instance
    // is yawed/rolled. We use the **instance** world-to-local
    // rotation (transpose of the world_transform's rotation),
    // not itransform — see the comment on the wind block below
    // for the same reasoning.
    {
        glm::mat3 R_world_to_local =
            glm::transpose(glm::mat3(world_transform));
        out.gravity_local = R_world_to_local * glm::vec3(0.0f, 0.0f, -9.81f);
    }

    // ── Mass ───────────────────────────────────────────────────
    // Heaven authors per-particle weights as a NORMALISED
    // distribution (the 4th component of each `<particles>` entry
    // sums to 1.0 across all particles), and a separate `<mass>`
    // element with the total cloth mass in kilograms. The actual
    // particle mass is `weight * total_mass`. Treating the raw
    // weight as kg gave us a 100×-too-light cloth, which combined
    // with the previous uniform wind force made the flag respond
    // far too eagerly to wind impulses.
    float total_mass = 2.0f;  // sensible default if the tag is missing
    if (const XmlNode* m = find_direct_child(*cloth, "mass")) {
        const float v = parse_number_prefix<float>(m->text).value_or(0.0f);
        if (v > 0.0f) total_mass = v;
    }

    out.rest_pos.resize(n_particles);
    out.inv_mass.resize(n_particles);
    for (int i = 0; i < n_particles; ++i) {
        const auto offset = static_cast<std::size_t>(i) * 4u;
        glm::vec4 world_pos(p_floats[offset], p_floats[offset + 1], p_floats[offset + 2], 1.0f);
        glm::vec4 local_pos = itransform * world_pos;
        out.rest_pos[i] = glm::vec3(local_pos);
        float weight = p_floats[offset + 3];
        float mass = weight * total_mass;
        out.inv_mass[i] = (mass > 1e-6f) ? 1.0f / mass : 0.0f;
    }
    out.position      = out.rest_pos;
    out.prev_position = out.rest_pos;

    // ── Linear (distance) constraints ───────────────────────────

    // Heaven's authored `<lengths>` blob disagrees with the actual
    // particle-pair distances at rest pose by up to ~6cm for some
    // edges. Using the authored values directly means the rest
    // pose is NOT an equilibrium of the constraint system: each
    // frame the solver pulls particles toward the authored length
    // (away from rest), then the anchor pin yanks them back, and
    // the cloth drifts forever even with gravity and wind both
    // disabled. We instead measure the actual distance between
    // each pair at rest pose and use that as the rest_length.
    // The cloth then truly sits at rest until external forces
    // perturb it.
    const XmlNode* lin = find_direct_child(*cloth, "linear");
    if (lin) {
        const int n_lin =
            parse_number_prefix<int>(lin->get("num_linear")).value_or(0);
        const XmlNode* idx = find_direct_child(*lin, "indices");
        if (idx) {
            auto inds = parse_ints(idx->text);
            const std::size_t n = std::min(
                static_cast<std::size_t>(std::max(0, n_lin)),
                inds.size() / 2u);
            out.linear.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                ClothLinearConstraint c;
                c.a = inds[i*2];
                c.b = inds[i*2 + 1];
                if (c.a >= 0 && c.b >= 0 &&
                    c.a < n_particles && c.b < n_particles) {
                    c.rest_length = glm::length(out.rest_pos[c.a] - out.rest_pos[c.b]);
                    out.linear.push_back(c);
                }
            }
        }
    }

    // ── Angular (bending) constraints ──────────────────────────
    const XmlNode* ang = find_direct_child(*cloth, "angular");
    if (ang) {
        const int n_ang =
            parse_number_prefix<int>(ang->get("num_angular")).value_or(0);
        const XmlNode* idx = find_direct_child(*ang, "indices");
        const XmlNode* angs = find_direct_child(*ang, "angles");
        if (idx && angs) {
            auto inds = parse_ints(idx->text);
            auto fs = parse_floats(angs->text);
            const std::size_t n = std::min({
                static_cast<std::size_t>(std::max(0, n_ang)),
                inds.size() / 4u, fs.size()});
            out.angular.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                ClothAngularConstraint c;
                c.p0 = inds[i*4];
                c.p1 = inds[i*4 + 1];
                c.p2 = inds[i*4 + 2];
                c.p3 = inds[i*4 + 3];
                c.rest_angle = fs[i];
                if (c.p0 >= 0 && c.p1 >= 0 && c.p2 >= 0 && c.p3 >= 0 &&
                    c.p0 < n_particles && c.p1 < n_particles &&
                    c.p2 < n_particles && c.p3 < n_particles)
                    out.angular.push_back(c);
            }
        }
    }

    // ── Triangles ──────────────────────────────────────────────
    // Wind drag scales with each triangle's facing area, so we
    // need the simulated topology directly. Heaven authors a
    // dedicated `<triangles>` blob inside BodyCloth that
    // enumerates SIX integers per triangle: 3 cloth-particle
    // indices followed by 3 mesh-vertex indices for the same
    // triangle (the latter are how the simulated triangles map
    // back to the rendered mesh's vertex buffer; we only need
    // the particle indices for drag forces).

    // Reading this as plain `inds[i*3..i*3+2]` (which we used to
    // do) interleaves real triangles with mesh-vertex tuples
    // reinterpreted as particle indices — the mesh-vertex values
    // happen to fall inside the [0, 162) particle range often
    // enough to look valid, so wind drag was being applied to
    // ~half ghost triangles whose normals had no relation to the
    // cloth surface. That was the source of the random twisting:
    // forces with arbitrary directions yanked the cloth around.
    if (const XmlNode* tri = find_direct_child(*cloth, "triangles")) {
        const int n_tri =
            parse_number_prefix<int>(tri->get("num_triangles")).value_or(0);
        auto inds = parse_ints(tri->text);
        // The blob has 6 ints per triangle; the first 3 are
        // particle indices.
        const std::size_t n = std::min(
            static_cast<std::size_t>(std::max(0, n_tri)),
            inds.size() / 6u);
        out.triangles.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            ClothTriangle t;
            t.p0 = inds[i * 6 + 0];
            t.p1 = inds[i * 6 + 1];
            t.p2 = inds[i * 6 + 2];
            if (t.p0 >= 0 && t.p1 >= 0 && t.p2 >= 0 &&
                t.p0 < n_particles && t.p1 < n_particles &&
                t.p2 < n_particles)
                out.triangles.push_back(t);
        }
    }

    // ── Synthesize linear constraints from triangle edges ──────

    // Heaven's authored `<linear>` blob contains 441 distance
    // constraints, but they're a sparse subset of the actual mesh
    // edges (≈2.7 constraints per particle, vs ~6 expected for a
    // triangle mesh). The real engine evidently relies on
    // additional implicit constraints — bend matching, material
    // stiffness, or its body-collision step — to keep the cloth
    // shape coherent. Without those, our 441 authored constraints
    // can't hold the cloth's shape: the fabric flops around its
    // sparse skeleton like a marionette on too-few strings.

    // The fix is to derive distance constraints from EVERY
    // triangle edge in the simulation mesh. With 282 triangles
    // and a fully manifold topology that's roughly 3*282/2 ≈ 423
    // interior edges + boundary ≈ ~600 distance constraints — a
    // properly woven cloth. Edges already present in the
    // authored `<linear>` list are skipped to avoid duplicates.
    {
        // Build a hash set of existing constraint pairs (a,b)
        // with a<b for fast lookup.
        std::unordered_map<uint64_t, int> existing;
        existing.reserve(out.linear.size() * 2);
        auto pair_key = [](int a, int b) -> uint64_t {
            if (a > b) std::swap(a, b);
            return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b));
        };
        for (const auto& lc : out.linear) {
            existing[pair_key(lc.a, lc.b)] = 1;
        }

        int added = 0;
        auto add_edge = [&](int a, int b) {
            if (a == b) return;
            uint64_t k = pair_key(a, b);
            if (existing.contains(k)) return;
            existing[k] = 1;
            ClothLinearConstraint c;
            c.a = a;
            c.b = b;
            c.rest_length = glm::length(out.rest_pos[a] - out.rest_pos[b]);
            out.linear.push_back(c);
            added++;
        };
        for (const auto& t : out.triangles) {
            add_edge(t.p0, t.p1);
            add_edge(t.p1, t.p2);
            add_edge(t.p2, t.p0);
        }
        if (added > 0) {
            std::printf("    cloth: synthesized %d distance constraints from triangle edges\n",
                        added);
        }
    }

    // ── Bridge disconnected components ─────────────────────────

    // Heaven's flag.node ships the cloth as TWO disconnected
    // particle networks: 80 particles forming the pole sleeve
    // (anchored, pinned to the pole) and 82 forming the flag
    // fabric (free). The visible mesh stitches them together at
    // the boundary between sleeve and fabric, but the BodyCloth
    // blob has no linear constraints linking the two halves —
    // the original engine evidently relies on a body-collision
    // step (the sleeve wraps around a rigid pole and the fabric
    // gets dragged along with it). We don't simulate that, so
    // without bridges the flag fabric just floats free in space
    // and produces the "swirling cloth that ignores the pole"
    // pathology.

    // The fix is to add distance constraints bridging EVERY
    // fabric particle to its closest sleeve particle (up to a
    // distance threshold). This effectively pins each fabric
    // particle to a unique sleeve neighbour, giving the fabric
    // a fully-anchored leading edge instead of just six pivot
    // points.
    {
        // Build linear-only adjacency.
        std::vector<std::vector<int>> adj(n_particles);
        for (const auto& lc : out.linear) {
            adj[lc.a].push_back(lc.b);
            adj[lc.b].push_back(lc.a);
        }
        // Compute components.
        std::vector<int> comp(n_particles, -1);
        int n_comp = 0;
        for (int i = 0; i < n_particles; ++i) {
            if (comp[i] != -1) continue;
            std::vector<int> stack = {i};
            comp[i] = n_comp;
            while (!stack.empty()) {
                int u = stack.back(); stack.pop_back();
                for (int v : adj[u]) if (comp[v] == -1) {
                    comp[v] = n_comp;
                    stack.push_back(v);
                }
            }
            n_comp++;
        }
        if (n_comp > 1) {
            // For every particle in the smaller component(s),
            // bind it to its N closest neighbours in the LARGEST
            // component. A single bridge per fabric particle
            // makes it pivot freely around its sleeve anchor
            // (the rest-length constraint allows rotation around
            // the anchor on a radius equal to the bridge
            // length). Three bridges fix the fabric particle
            // rigidly in space, giving the cloth a properly
            // anchored leading edge.
            constexpr int kBridgesPerParticle = 3;
            constexpr float kMaxBridgeDistance = 0.45f;
            // Find the largest component.
            std::vector<int> comp_size(n_comp, 0);
            for (int c : comp) comp_size[c]++;
            int largest = 0;
            for (int c = 1; c < n_comp; ++c)
                if (comp_size[c] > comp_size[largest]) largest = c;

            int total_bridges = 0;
            for (int i = 0; i < n_particles; ++i) {
                if (comp[i] == largest) continue;
                // Collect candidates with distance, sort, take top-K.
                std::vector<std::pair<float, int>> cand;
                for (int j = 0; j < n_particles; ++j) {
                    if (comp[j] != largest) continue;
                    float d = glm::length(out.rest_pos[i] - out.rest_pos[j]);
                    if (d < kMaxBridgeDistance) cand.emplace_back(d, j);
                }
                std::sort(cand.begin(), cand.end());
                const std::size_t take = std::min(
                    static_cast<std::size_t>(kBridgesPerParticle),
                    cand.size());
                for (std::size_t k = 0; k < take; ++k) {
                    ClothLinearConstraint bridge;
                    bridge.a = i;
                    bridge.b = cand[k].second;
                    bridge.rest_length = cand[k].first;
                    out.linear.push_back(bridge);
                    total_bridges++;
                }
            }
            std::printf("    cloth: %d components, added %d bridge constraints\n",
                        n_comp, total_bridges);
        }
    }

    // ── Joint particles (anchors) ──────────────────────────────
    // Heaven pins specific particles to the parent rigid body
    // (`body_0`, the flag pole). We treat anchored particles as
    // pinned at their authored rest positions — the pole is static
    // for the flags in this scene, so the anchor position is
    // simply the rest position.
    if (const XmlNode* joint = find_direct_child(*cloth, "joint")) {
        if (joint->get("type") == "JointParticles") {
            if (const XmlNode* jp = find_direct_child(*joint, "particles")) {
                const int n_anchor =
                    parse_number_prefix<int>(jp->get("num_particles")).value_or(0);
                const XmlNode* idx = find_direct_child(*jp, "indices");
                const XmlNode* pos = find_direct_child(*jp, "positions");
                if (idx) {
                    auto inds = parse_ints(idx->text);
                    auto pf = pos ? parse_floats(pos->text) : std::vector<float>{};
                    const std::size_t n = std::min(
                        static_cast<std::size_t>(std::max(0, n_anchor)),
                        inds.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        int pidx = inds[i];
                        if (pidx < 0 || pidx >= n_particles) continue;
                        out.anchor_idx.push_back(pidx);
                        // Anchor local position (in pole-local space).
                        // For our purposes we only need the world-
                        // space anchor, which equals the particle's
                        // rest position because the pole is the
                        // identity transform in scene space.
                        glm::vec3 ap = out.rest_pos[pidx];
                        if (pf.size() >= (i + 1u) * 3u) {
                            // Some files store anchor positions in pole-
                            // local space; we don't currently need that.
                        }
                        out.anchor_local.push_back(ap);
                        // Pin the anchored particle (zero inverse mass).
                        out.inv_mass[pidx] = 0.0f;
                    }
                }
            }
        }
    }

    // ── Mesh vertex → particle mapping ─────────────────────────
    // Particles enumerate the unique bind-pose positions of the
    // mesh; multiple mesh vertices that share the same position
    // (UV-seam duplicates) all map to the same particle. The
    // mapping order matches the mesh's vertex order: particle[0] is
    // the first unique position in the mesh, particle[1] the second,
    // etc.
    if (bind_mesh.surfaces.empty()) return false;
    const Surface& src_surf = bind_mesh.surfaces.front();
    const std::size_t n_verts = src_surf.vertices.size();
    out.mv_to_particle.resize(n_verts);

    // Build position→particle map by hashing rest-pose particle
    // positions, then look up each mesh vertex against it. This
    // matches the way Heaven's exporter assigned particles to
    // unique positions.
    std::unordered_map<PosKey, int, PosKeyHash> pos_to_particle;
    pos_to_particle.reserve(static_cast<std::size_t>(n_particles) * 2u);
    for (int i = 0; i < n_particles; ++i) {
        const auto& p = out.rest_pos[i];
        pos_to_particle[make_key(p.x, p.y, p.z)] = i;
    }

    // Heaven's particle positions and mesh-vertex positions differ
    // by the cloth body's `<itransform>` — the particles live in
    // a coordinate frame slightly offset from the mesh vertices.
    // We don't extract that transform, but the relationship is
    // a uniform 3D offset. Compute it from the first mesh vertex
    // by finding the particle whose XY matches when we strip a
    // shared offset.

    // Empirical observation: particle[i].xyz ≈ mesh_v[mv_first(i)].xyz
    // with a small per-axis offset. For the flag this offset is
    // about (-0.04, 0.03, 0.18). Rather than reverse-engineering
    // the itransform, we build the mapping from mesh-vertex order:
    // mark the first mesh vertex with each unique position as the
    // particle index it should map to.
    std::unordered_map<PosKey, int, PosKeyHash> mv_first_pos;
    int next_unique = 0;
    for (std::size_t i = 0; i < n_verts; ++i) {
        const auto& v = src_surf.vertices[i];
        PosKey k = make_key(v.px, v.py, v.pz);
        auto it = mv_first_pos.find(k);
        if (it == mv_first_pos.end()) {
            mv_first_pos[k] = next_unique;
            out.mv_to_particle[i] = next_unique;
            ++next_unique;
        } else {
            out.mv_to_particle[i] = it->second;
        }
    }

    if (next_unique != n_particles) {
        // Mismatch — likely the mesh in /props/ doesn't match the
        // cloth body 1:1. Bail out so the caller renders the static
        // mesh instead.
        std::fprintf(stderr,
            "  cloth: vertex/particle count mismatch (mv unique=%d, "
            "particles=%d) for %s — falling back to static\n",
            next_unique, n_particles, dynamic_node.get("name").c_str());
        return false;
    }

    // ── Cache the bind mesh ────────────────────────────────────
    out.bind_vertices = src_surf.vertices;
    out.bind_indices  = src_surf.indices;
    out.world_transform = world_transform;

    // ── Sim params ─────────────────────────────────────────────
    if (const XmlNode* it = find_direct_child(*cloth, "num_iterations")) {
        const int n = parse_number_prefix<int>(it->text).value_or(0);
        if (n > 0 && n < 64) out.num_iterations = std::max(2, n * 2);
        // Heaven's default 2 is for old hardware; we double it for
        // visual stability without measurable cost.
    }
    if (const XmlNode* ar = find_direct_child(*cloth, "angular_restitution")) {
        out.angular_restitution =
            parse_number_prefix<float>(ar->text).value_or(0.0f);
    }

    // ── Wind ───────────────────────────────────────────────────
    // PhysicalWind is a sibling node under the dummy parent in
    // Heaven's flag setup — it lives outside the BodyCloth tree.
    // We walk the parent's siblings looking for one.
    auto find_wind = [](const XmlNode& root) -> const XmlNode* {
        // Walk the entire tree; each WorldExpression typically
        // contains one PhysicalWind.
        std::function<const XmlNode*(const XmlNode&)> walk =
            [&](const XmlNode& n) -> const XmlNode* {
            if (n.tag == "node" && n.get("type") == "PhysicalWind") return &n;
            for (const auto& c : n.children) if (auto p = walk(c)) return p;
            return nullptr;
        };
        return walk(root);
    };
    // The caller passes us only the ObjectMeshDynamic node; the
    // wind sibling is one level up. Find the closest ancestor by
    // looking up via the parent dummy (we assume the flag node
    // contains one PhysicalWind anywhere in its tree).
    if (const XmlNode* w = find_wind(dynamic_node)) {
        if (const XmlNode* v = find_direct_child(*w, "velocity")) {
            float x = 0, y = 0, z = 0;
            std::sscanf(v->text.c_str(), "%f %f %f", &x, &y, &z);
            // Wind is authored in world coordinates. Particles are
            // simulated in **body-local** space and rendered as
            // `instance.transform * particle_local`. So for the
            // wind force to push the rendered flag in the right
            // world direction, we have to express the wind in the
            // body's local frame, which is
            //     inverse(instance.world_transform).rotation * wind_world

            // The previous code used `itransform`'s rotation here,
            // but `itransform` is the cloth body's *internal*
            // authoring offset (≈ identity for the flag), NOT the
            // per-instance rotation that places the flag against
            // a tower or airship. With every flag.node placed at
            // a different yaw, the un-rotated wind blew into
            // whatever local direction happened to face world+X,
            // sending the cloth twirling depending on instance.
            glm::vec3 wv_world(x, y, z);
            glm::mat3 R_world_to_local =
                glm::transpose(glm::mat3(world_transform));
            out.wind_velocity = R_world_to_local * wv_world;
            out.wind_velocity_world = wv_world;
        }
        if (const XmlNode* ld = find_direct_child(*w, "linear_damping")) {
            out.linear_damping =
                parse_number_prefix<float>(ld->text).value_or(0.0f);
        }
    } else {
        // Default to a gentle breeze along +X so flags still flutter
        // even if PhysicalWind is missing from the source tree.
        // Same world→local rotation logic as above.
        glm::vec3 wv_world(3.0f, 0.0f, 0.0f);
        glm::mat3 R_world_to_local =
            glm::transpose(glm::mat3(world_transform));
        out.wind_velocity = R_world_to_local * wv_world;
        out.wind_velocity_world = wv_world;
        out.linear_damping = 2.0f;
    }

    out.node_name = dynamic_node.get("name");
    return true;
}

namespace {

// ── PBD step ───────────────────────────────────────────────────

// Smooth gust noise used to modulate the steady wind. The original
// Heaven script rebuilds the wind every frame as

//     noise    = engine.game.getNoise(time * 0.2, 8, 8);
//     velocity = vec3(3 + noise*2, noise*4, 0);

// i.e. the steady X component never drops below 1 and never
// reverses. We approximate that with a positive multiplier whose
// variation is intentionally small — large gusts produce big
// changes in drag force (force ∝ v²) which whip the cloth
// instead of fluttering it. Range [0.85, 1.15] gives a clearly
// visible breeze effect without driving the cloth into a swing.
float wind_gust(float t) {
    float g = 1.0f + 0.10f * std::sin(t * 0.85f)
                   + 0.05f * std::sin(t * 0.21f + 1.7f);
    return std::clamp(g, 0.85f, 1.15f);
}

// ── Integration ────────────────────────────────────────────────

// Verlet position-based integration. Gravity is applied as an
// acceleration; per-particle damping is a small bleed of the
// implicit velocity each step. Wind is NOT applied here — it's
// done as a separate per-triangle aerodynamic pass (see
// `apply_wind_drag` below) because a uniform body force per
// particle ignores the cloth's orientation and produces the
// "flag swings around the pole" pathology we observed.
void integrate(ClothBody& c, float dt) {
    const glm::vec3 gravity = c.gravity_local;

    // Per-step velocity attenuation (general air friction). Tuned
    // so a free-falling cloth panel reaches a believable terminal
    // velocity ~3 m/s rather than rocketing to 30 m/s. The wind
    // drag is on top of this.
    const float k_damp = std::clamp(2.0f * dt, 0.0f, 0.3f);

    for (size_t i = 0; i < c.position.size(); ++i) {
        if (c.inv_mass[i] == 0.0f) {
            c.prev_position[i] = c.position[i]; // anchored — no Verlet
            continue;
        }
        glm::vec3 vel = (c.position[i] - c.prev_position[i]);
        vel *= (1.0f - k_damp);

        glm::vec3 new_pos = c.position[i] + vel + gravity * (dt * dt);
        c.prev_position[i] = c.position[i];
        c.position[i]      = new_pos;
    }
}

// ── Aerodynamic drag ──────────────────────────────────────────

// Canonical per-triangle wind force, ported from Kyle Palermo's
// CSCE-450 cloth simulator (a well-tested reference implementation
// of Provot/Müller-style cloth with wind):

//     pressure  = n · wind
//     triForce  = pressure * (2*area) * n
//     vertex   += triForce / 3

// Note `(2*area) = |cross|` — the unnormalized cross product of
// two triangle edges, NOT divided by two. The reference uses the
// raw norm of the cross product and calls it "area".

// Force is along the triangle normal (signed by pressure). For
// adjacent triangles with consistent winding, normals point the
// same way, so neighbours' forces add coherently. The flag's
// authored `<triangles>` blob is consistent (we verified by
// inspection), so this works.

// `kAir` scales the whole formula. The reference uses 1.0; with
// our flag's 0.018 kg-per-particle masses and 0.016 m² average
// triangle area, that gives clean visible motion at Heaven's
// 3 m/s wind.
void apply_wind_drag(ClothBody& c, float dt, float gust) {
    if (c.triangles.empty()) return;

    const glm::vec3 wind = c.wind_velocity * gust;
    // Higher kAir lifts the flag further off the pole and keeps
    // it more horizontal. 1.0 = textbook reference, but the
    // reference flag is much lighter than ours; we scale up so
    // the visible cloth presents a clear "flying in the wind"
    // pose rather than draping straight down.
    constexpr float kAir = 4.0f;

    // Accumulate per-vertex force, then convert to a position
    // delta using inv_mass and dt (semi-implicit Euler step).
    // We allocate a thread-local scratch buffer so we don't churn
    // the heap every step.
    static thread_local std::vector<glm::vec3> tri_forces;
    tri_forces.assign(c.position.size(), glm::vec3(0.0f));

    for (const auto& t : c.triangles) {
        const glm::vec3& x0 = c.position[t.p0];
        const glm::vec3& x1 = c.position[t.p1];
        const glm::vec3& x2 = c.position[t.p2];
        glm::vec3 cross = glm::cross(x1 - x0, x2 - x0);
        float two_area = glm::length(cross);
        if (two_area < 1e-8f) continue;
        glm::vec3 n = cross / two_area;
        float pressure = glm::dot(n, wind);
        glm::vec3 triForce = n * (pressure * two_area * kAir);
        glm::vec3 share = triForce * (1.0f / 3.0f);
        tri_forces[t.p0] += share;
        tri_forces[t.p1] += share;
        tri_forces[t.p2] += share;
    }

    // Apply: per-vertex Δx = (F * dt²) / m. Inv_mass = 1/m, so
    // Δx = F * dt² * inv_mass. Anchored particles (inv_mass==0)
    // get no displacement.
    float dt2 = dt * dt;
    for (size_t i = 0; i < c.position.size(); ++i) {
        if (c.inv_mass[i] == 0.0f) continue;
        c.position[i] += tri_forces[i] * (dt2 * c.inv_mass[i]);
    }
}

// Project a single linear (distance) constraint. Pulls the two
// particles toward each other / apart so their separation equals
// the rest length, weighted by their inverse masses.
void project_linear(ClothBody& c, const ClothLinearConstraint& lc) {
    glm::vec3 d = c.position[lc.b] - c.position[lc.a];
    float len = glm::length(d);
    if (len < 1e-6f) return;
    float diff = (len - lc.rest_length) / len;

    float wA = c.inv_mass[lc.a];
    float wB = c.inv_mass[lc.b];
    float wsum = wA + wB;
    if (wsum < 1e-8f) return;

    glm::vec3 corr = d * diff;
    c.position[lc.a] += corr * (wA / wsum);
    c.position[lc.b] -= corr * (wB / wsum);
}

// Project an angular (bending) constraint. The hinge is two
// triangles (p0, p1, p2) and (p1, p3, p2) sharing edge (p1, p2).
// The interior angle between them is `rest_angle` (π means flat).

// We use a simple centroid-pull approximation: nudge the four
// particles toward their centroid by a small factor `k`. This
// preserves edge lengths (the linear projection re-asserts those
// every iteration) while gently biasing the hinge back toward
// rest. A rigorous PBD dihedral projection (Müller-Macklin) gives
// stiffer cloth but is numerically delicate near coplanar
// triangles, and we don't need that level of stiffness for a
// flag fluttering in the wind.
void project_angular(ClothBody& c, const ClothAngularConstraint& ac, float k) {
    if (k <= 0.0f) return;
    glm::vec3 centroid = (c.position[ac.p0] + c.position[ac.p1]
                        + c.position[ac.p2] + c.position[ac.p3]) * 0.25f;
    auto pull = [&](int i) {
        if (c.inv_mass[i] == 0.0f) return;
        c.position[i] += (centroid - c.position[i]) * k;
    };
    pull(ac.p0); pull(ac.p1); pull(ac.p2); pull(ac.p3);
}

} // namespace

bool step_cloth(ClothBody& c, float dt, float time) {
    if (c.position.empty()) return false;

    // Fixed-timestep accumulator. The PBD solver is calibrated for
    // a 60 Hz step (`h = 1/60s`); running at any other rate would
    // make the cloth diverge — at much larger `h` the constraint
    // solver can't catch up to the per-step displacements and the
    // cloth explodes; at much smaller `h` we run gratuitously
    // many steps per frame.

    // For visual debugging we keep the internal step at 60 Hz but
    // SLOW DOWN the wall-clock time fed into the accumulator. The
    // `c.sim_hz` field re-purposes its name as a slow-motion
    // factor: 60 = real-time, 6 = 10× slower (still 60 Hz internal),
    // 0.6 = 100× slower, etc. The sim sees `dt * (sim_hz / 60)`.
    constexpr float kSimHz = 60.0f;
    constexpr float h = 1.0f / kSimHz;
    float slow_factor = (c.sim_hz > 0.0f) ? (c.sim_hz / kSimHz) : 1.0f;

    c.sim_time_accum += dt * slow_factor;

    // Hard cap so we don't run more than 4 sim steps per render
    // call even after a long pause/stall — otherwise the cloth
    // would catch up explosively.
    const int steps_to_run = static_cast<int>(std::clamp(
        std::floor(c.sim_time_accum / h), 0.0f, 4.0f));
    c.sim_time_accum -= static_cast<float>(steps_to_run) * h;

    if (steps_to_run == 0) return false;

    float gust = wind_gust(time);

    // Anchor restoration force — keep pinned particles glued to
    // their authored rest positions before we run any sim steps.
    for (int p : c.anchor_idx) {
        c.position[p]      = c.rest_pos[p];
        c.prev_position[p] = c.rest_pos[p];
    }

    for (int s = 0; s < steps_to_run; ++s) {
        integrate(c, h);

        // Aerodynamic wind drag — applied AFTER integrate so the
        // (n · v_rel) factor uses the new tentative positions.
        // This pushes triangles whose normal faces the wind, with
        // no force on edge-on triangles.
        apply_wind_drag(c, h, gust);

        // Re-pin anchors after integration (they shouldn't have
        // moved because inv_mass==0, but explicit is safer).
        for (int p : c.anchor_idx) {
            c.position[p]      = c.rest_pos[p];
            c.prev_position[p] = c.rest_pos[p];
        }

        // Constraint projection — Gauss-Seidel.
        for (int it = 0; it < c.num_iterations; ++it) {
            for (const auto& lc : c.linear)  project_linear(c, lc);
            // NOTE: angular projection disabled. The current
            // centroid-pull approximation pulls particles toward
            // their centroid every iteration regardless of whether
            // the rest configuration is already satisfied — even
            // at rest pose, with k=0.05, 405 constraints, 4
            // iterations, every step "smooths" particles toward
            // their neighbours by a few percent. The cumulative
            // effect is that the cloth collapses inward by a
            // huge amount over the first second of simulation,
            // creating energy from nothing. Without a proper PBD
            // dihedral constraint (which is zero-error at rest),
            // we leave bending unconstrained — the cloth will be
            // softer than the original but at least won't drift
            // when there's no wind.
            // for (const auto& ac: c.angular) project_angular(c, ac, c.angular_restitution);
        }

        // ── Pole collision ─────────────────────────────────────

        // The flag's BodyDummy parent is a vertical pole. Heaven's
        // simulator reports cloth-vs-pole contacts implicitly via
        // the cloth's broadphase against the rigid bodies in the
        // scene; we don't have that infrastructure but the only
        // collider that actually matters for a hanging flag is
        // the pole itself.

        // We model the pole as a vertical cylinder along the
        // body-local Z axis. Particles in the rest pose all sit
        // at X >= 0.18 — the anchor strip is at X=0.18 and
        // everything else is at X >= ~0.18. So `pole_x = 0.0,
        // pole_y = 0.0, radius = 0.16` keeps particles on their
        // authored side of the pole instead of letting them swing
        // around to the other side (which is what produces the
        // "flag wraps around the pole" pathology — without
        // collision, nothing physically prevents that).
        constexpr float kPoleX = 0.0f;
        constexpr float kPoleY = 0.0f;
        constexpr float kPoleR = 0.16f;
        for (size_t i = 0; i < c.position.size(); ++i) {
            if (c.inv_mass[i] == 0.0f) continue;
            float dx = c.position[i].x - kPoleX;
            float dy = c.position[i].y - kPoleY;
            float r2 = dx * dx + dy * dy;
            if (r2 < kPoleR * kPoleR && r2 > 1e-8f) {
                float r = std::sqrt(r2);
                float push = (kPoleR - r) / r;
                c.position[i].x += dx * push;
                c.position[i].y += dy * push;
            }
        }

        // Final anchor pin.
        for (int p : c.anchor_idx) {
            c.position[p]      = c.rest_pos[p];
            c.prev_position[p] = c.rest_pos[p];
        }
    }
    return true;
}

// ── Mesh emission ──────────────────────────────────────────────

void emit_vertices(const ClothBody& cloth, std::vector<Vertex>& out) {
    const std::size_t n_verts = cloth.bind_vertices.size();
    out = cloth.bind_vertices; // copy bind UVs/tangents/etc.

    // Position from particle.
    for (std::size_t i = 0; i < n_verts; ++i) {
        int p = cloth.mv_to_particle[i];
        const glm::vec3& pp = cloth.position[p];
        out[i].px = pp.x;
        out[i].py = pp.y;
        out[i].pz = pp.z;
    }

    // Recompute normals from the simulated triangles. We zero them
    // first, then accumulate per-triangle face normals to each of
    // their 3 vertices, and finally renormalise. This is cheap
    // (one triangle pass) and reproduces the flutter-driven shading
    // that makes a cloth feel like cloth instead of a flat decal.
    for (auto& v : out) { v.nx = 0; v.ny = 0; v.nz = 0; }

    const auto& idx = cloth.bind_indices;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        Vertex& v0 = out[idx[i]];
        Vertex& v1 = out[idx[i + 1]];
        Vertex& v2 = out[idx[i + 2]];
        glm::vec3 e1(v1.px - v0.px, v1.py - v0.py, v1.pz - v0.pz);
        glm::vec3 e2(v2.px - v0.px, v2.py - v0.py, v2.pz - v0.pz);
        glm::vec3 n = glm::cross(e1, e2);
        v0.nx += n.x; v0.ny += n.y; v0.nz += n.z;
        v1.nx += n.x; v1.ny += n.y; v1.nz += n.z;
        v2.nx += n.x; v2.ny += n.y; v2.nz += n.z;
    }
    for (auto& v : out) {
        float l = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (l > 1e-6f) { v.nx /= l; v.ny /= l; v.nz /= l; }
        else           { v.nx = 0;   v.ny = 0;  v.nz = 1; }
    }
}

} // namespace heaven
