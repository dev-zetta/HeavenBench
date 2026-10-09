/*
 * HeavenBench — RHI-aware scene loading
 */
#ifndef HEAVENBENCH_SRC_ENGINE_SCENE_RHI_H_
#define HEAVENBENCH_SRC_ENGINE_SCENE_RHI_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"
#include "engine/skinned_mesh.h"
#include "engine/rope_sim.h"
#include "engine/cloth_sim.h"
#include "engine/loading_progress.h"
#include "rhi/rhi.h"

namespace heaven {

class RhiMaterialDatabase;
class CameraCollision;
class AssetUsageRecorder;
struct AnimatedObject {
    SkinnedMesh smesh;
    GpuMesh     gpu_mesh;     // current frame's GPU data
    Instance    instance;     // world transform
    float       anim_speed;   // frames per second
    int         mesh_index;   // index into scene gpu_meshes

    // Per-surface tracker: the frame_f value the GPU buffer
    // currently reflects. Used by the per-frame skinning loop to
    // skip redundant CPU skinning + GPU upload at high render
    // rates. The underlying animation is 30 fps; rendering at
    // 3000 fps means we'd otherwise skin ~100x per source frame
    // for no visible gain. We re-skin only when frame_f advanced
    // by more than `kReskinFrameDelta` since the last upload.

    // Initialized to a very negative value so the first frame
    // always uploads (post-tessellation re-skin happens at
    // load time, but the AnimatedObject we keep here is for
    // the live path).
    std::vector<float> last_frame_f;

    // Per-object GPU-skinning decision. When GPU skinning
    // is requested globally, an Animated_Object whose skinned upload/enable did
    // not take effect (e.g. its bone count exceeded Filament's 255-bone limit,
    // so MeshStore left the surfaces unmarked and SceneBuilder built a plain,
    // un-skinned renderable) has no skinning-enabled renderable. Such an object
    // must animate through the CPU_Skinning_Path or it would freeze at its rest
    // pose. The decision is only knowable once the persistent scene is built
    // (the first render_frame), so the main loop resolves it lazily via
    // IRhiBackend::gpu_skinning_active() and caches it here, emitting a one-time
    // diagnostic on fallback. `gpu_skin_resolved` stays false (and
    // `gpu_skin_active` is unused) when GPU skinning is disabled, so the CPU
    // path is byte-for-byte unchanged in that configuration.
    bool gpu_skin_resolved = false; // whether the decision below has been made
    bool gpu_skin_active   = false; // true ⇒ GPU-skinned; false ⇒ CPU fallback
};

struct FloatingIsland {
    int instance_index;       // index into scene.instances
    glm::mat4 base_transform; // original transform from world file
    float speed;              // animation speed (0.25 or 0.5)
    float amplitude;          // vertical amplitude (2.0)
    int group;                // floating group index (from WorldExpression walk)
};

// Per-stone seeded animation, mirroring heaven.world's
// `stone_noise` WorldExpression. Each stone's authored world (x,y)
// produces a deterministic seed; world Z each frame is

//     dummy_z = sin(time * speed + seed) * amplitude * sin(seed)
//     mesh_z  = dummy_z + child_local_z

// where `seed = dummy.x*13 + dummy.y`, amplitude=6, speed=0.5 and
// the seed/dummy_xy come from the *parent ObjectDummy* (the node
// the script's `getChild(i).getPosition()` actually returns), not
// from the rendered mesh — those can be tens of metres apart on
// the Z axis. Without this distinction the rocks would all
// collapse to ±5m around z=0 and clip into the islands.
struct StoneAnim {
    int       instance_index; // index into scene.instances
    glm::mat4 base_transform; // authored world transform — Z is overwritten per frame
    float     dummy_z;        // z component the script overwrites
    float     child_local_z;  // mesh_z - dummy_z (preserved each frame)
    float     seed;           // dummy.x*13 + dummy.y (same as the original script)
    float     speed;          // 0.5 from the script
    float     amplitude;      // 6.0 from the script
};

struct RhiScene {
    // Logical authored roots and nested node references actually parsed for a
    // compact asset fixture. Empty for the ordinary world / --node paths.
    std::vector<std::string> source_node_files;
    std::vector<Mesh>     cpu_meshes;
    std::vector<GpuMesh>  gpu_meshes;
    std::vector<Instance> instances;
    std::vector<AnimatedObject> animated;
    std::vector<FloatingIsland> floating_islands;
    std::vector<StoneAnim>       stone_anims;
    std::vector<ParticleEmitter> particle_emitters;
    std::vector<SceneLight>      lights;
    std::vector<ClothBody>       cloths;
    BridgeSystem bridge_system;
    // Unique CPU mesh-resource geometry. Base counts describe the authored
    // meshes before tessellation; total counts describe the topology uploaded
    // after the optional tessellation pass.
    uint64_t base_vertices   = 0;
    uint64_t base_triangles  = 0;
    uint64_t total_vertices  = 0;
    uint64_t total_triangles = 0;
};

//Load a scene using the RHI backend for GPU uploads.

//When `rec` is non-null, every asset file the resolution path opens (the
//world file, the explicit `bridges.node`, an explicit `config.node_file`,
//each `NodeReference`/`NodeLayer` target, and each mesh) is reported into the
//recorder; references that cannot be located or read are recorded as
//unresolved. A null `rec` (every normal render run) leaves behavior
//byte-for-byte unchanged with no added cost beyond a null check.
[[nodiscard]] Result<RhiScene> load_scene_rhi(const AppConfig& config, IRhiBackend& backend,
                                               AssetUsageRecorder* rec = nullptr,
                                               AssetFileSystemRef assets = std::nullopt,
                                               const LoadingProgressCallback& progress = {});

//Load a scene with CPU tessellation for surfaces that have displacement maps.
//Tessellated meshes are cached to disk in cache_dir.

//If `collision` is non-null, the camera collision world is built from the
//*base* (pre-tessellation) geometry — collision doesn't need the displaced
//micro-detail, and building Jolt MeshShapes from the tessellated mesh
//(up to 64× the triangles) would add seconds to startup and slow every cast.
[[nodiscard]] Result<RhiScene> load_scene_rhi(const AppConfig& config, IRhiBackend& backend,
                                               RhiMaterialDatabase& materials,
                                               const std::string& cache_dir,
                                               CameraCollision* collision = nullptr,
                                               AssetFileSystemRef assets = std::nullopt,
                                               const LoadingProgressCallback& progress = {});

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_SCENE_RHI_H_
