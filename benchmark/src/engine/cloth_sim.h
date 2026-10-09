/*
 * HeavenBench — Cloth simulation for ObjectMeshDynamic nodes
 *
 * Each ObjectMeshDynamic body in the original Heaven scene
 * (`flag.node`, `flag_small.node`) carries a baked Position-Based
 * Dynamics (PBD) network: rest particle positions, edge-length
 * constraints, bending constraints and a JointParticles list that
 * pins selected particles to a rigid parent (the flag pole).
 *
 * This module:
 *   1. Extracts that data from the xml0.node tree.
 *   2. Runs a PBD step every frame (gravity + wind force, then
 *      iterative constraint projection).
 *   3. Maps the simulated 162 particles back onto the 188 mesh
 *      vertices (multiple mesh verts share one particle at UV
 *      seams) and pushes the new vertex buffer to the GPU each
 *      frame via `update_mesh_vertices`.
 *
 * Design notes:
 *   - Particle mass is read from the `(x y z mass)` quads in the
 *     `<particles>` blob; particles with mass==0 are infinite mass
 *     (anchors) but in practice the joint-particle list pins the
 *     anchored ones and the rest are free.
 *   - We solve linear and angular constraints with Gauss-Seidel
 *     iterations (≈8 per step matches Heaven's `<num_iterations>`).
 *   - Wind is a uniform velocity (PhysicalWind in the.node) plus
 *     a time-varying gust modulation so the flag flutters.
 *   - The simulation runs independently of the FPS — when the
 *     frame budget pushes Δt above ~1/60s we substep so the cloth
 *     never explodes at low frame rates.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CLOTH_SIM_H_
#define HEAVENBENCH_SRC_ENGINE_CLOTH_SIM_H_

#include "engine/types.h"
#include "engine/mesh.h"
#include "xml0_parser.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

namespace heaven {

// ── Cloth body description ──────────────────────────────────────

struct ClothLinearConstraint {
    int   a, b;
    float rest_length;
};

struct ClothAngularConstraint {
    // p0..p3 form a hinge: triangles (p0, p1, p2) and (p1, p3, p2)
    // share edge (p1, p2) and the rest angle between them is `angle`.
    // Angle = π means the two triangles are co-planar.
    int   p0, p1, p2, p3;
    float rest_angle;
};

// One simulated triangle (3 particle indices). Stored separately
// from `bind_indices` because the cloth body authors a different
// `<triangles>` topology than the rendered mesh — the cloth's
// triangles are between the unique-position particles, while the
// mesh's triangles include UV-seam vertex duplicates.

// Used by the aerodynamic drag pass: each triangle's normal × its
// area × (n · v_rel) gives the force the wind exerts on that
// patch of cloth, distributed evenly among its 3 vertices.
struct ClothTriangle {
    int p0, p1, p2;
};

struct ClothBody {
    // Per-particle data (rest pose in world space, current state).
    std::vector<glm::vec3> rest_pos;
    std::vector<float>     inv_mass;     // 1/mass; 0 = pinned
    std::vector<glm::vec3> position;     // current
    std::vector<glm::vec3> prev_position;// for Verlet
    std::vector<int>       anchor_idx;   // particles[i] = anchor in rigid parent
    std::vector<glm::vec3> anchor_local; // anchor position in pole-local space
    std::vector<ClothLinearConstraint>  linear;
    std::vector<ClothAngularConstraint> angular;
    // Topology used by the wind drag pass — see ClothTriangle.
    // Comes from `<triangles>` inside the BodyCloth blob; falls
    // back to inferring triangles from the rendered mesh's
    // index buffer when the cloth body doesn't author one.
    std::vector<ClothTriangle> triangles;

    // Mesh vertex → particle mapping (length = mesh vertex count).
    // Multiple mesh verts may share the same particle (UV seams).
    std::vector<int> mv_to_particle;

    // Cached bind-pose mesh vertices (pre-skinning state) so we can
    // generate displaced output buffers without re-reading the source
    // mesh every frame.
    std::vector<Vertex> bind_vertices;
    std::vector<uint32_t> bind_indices;

    // Static aero / sim params from the source node.
    int   num_iterations = 8;
    float angular_restitution = 0.05f;

    // World-space frame: the body's Z-up transform in scene space.
    // Cloth particles are simulated in this frame (their authored
    // positions are in scene space relative to the parent island /
    // pole).
    glm::mat4 world_transform = glm::mat4(1.0f);

    // Wind velocity expressed in **body-local** coordinates (the
    // PhysicalWind is authored in world space; we rotate it once
    // at extraction time so step_cloth doesn't have to know about
    // the body frame).
    glm::vec3 wind_velocity = glm::vec3(0.0f);
    // Same wind, but in WORLD space. Kept around purely for
    // debug visualisation (set_wind_debug_arrows): this is the
    // ground-truth world-space wind direction the simulation
    // sees, so plotting it next to the flag tells us at a
    // glance whether the cloth is responding to the right
    // vector.
    glm::vec3 wind_velocity_world = glm::vec3(0.0f);
    // Gravity in body-local space. Same idea — extracted once
    // from world `(0,0,-9.81)` rotated by itransform's basis.
    glm::vec3 gravity_local = glm::vec3(0.0f, 0.0f, -9.81f);
    float     linear_damping = 2.0f;

    // Fixed-timestep accumulator. The PBD solver is stable only
    // when run at a small, constant `h`; rendering at 3000 FPS
    // would otherwise inject 50× more wind energy per second than
    // at 60 FPS. We accumulate render time and step the simulator
    // at a fixed cadence (kSimHz below).
    float sim_time_accum = 0.0f;
    // Slow-motion factor for visual debugging. Internal step is
    // still 60 Hz (anything else explodes); this scales the
    // wall-clock time fed into the accumulator. 60 = real-time,
    // 6 = 10× slower, 0.6 = 100× slower. The flag will appear to
    // move very slowly without being numerically unstable.
    float sim_hz = 60.0f;

    // Render-side identity so the main loop knows which GpuMesh
    // surface to push each frame.
    int       mesh_index   = -1;
    int       surface_index = 0;
    std::string node_name;
};

// ── Extraction from xml0.node files ────────────────────────────

//Look at a.node tree for an `ObjectMeshDynamic` with a
//`BodyCloth` child. If found, fill `out` and return true.

//`world_transform` is the accumulated world transform of the
//`ObjectMeshDynamic` node when it was discovered during scene
//loading (we need it to push particles to world space).

//`bind_mesh` is the static-mesh load of the source asset
//(heaven/meshes/props/flag.mesh) — we need its vertex buffer
//so we can emit Vertex[] each frame with updated positions.
bool extract_cloth_body(const XmlNode& dynamic_node,
                        const glm::mat4& world_transform,
                        const Mesh& bind_mesh,
                        ClothBody& out);

// ── Per-frame stepping ─────────────────────────────────────────

//Advance the cloth `dt` seconds. Internally substeps to keep
//each integration step ≤ 1/60s for stability. Wind gust noise
//is computed from `time` so the flag flutters even in steady wind.

//@returns true if the simulation actually stepped (and the
//vertex output should be re-uploaded), false if not
//enough time had accumulated since the last step.
bool step_cloth(ClothBody& c, float dt, float time);

//Generate a Vertex[] buffer reflecting current particle state
//for upload via `update_mesh_vertices`. The output has the same
//length as the bind mesh's surface vertices; positions come from
//the cloth particles, normals are recomputed from the simulated
//triangle fan, UVs and tangents stay at bind values.
void emit_vertices(const ClothBody& cloth, std::vector<Vertex>& out);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CLOTH_SIM_H_
