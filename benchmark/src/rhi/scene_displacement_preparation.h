#ifndef HEAVENBENCH_SRC_RHI_SCENE_DISPLACEMENT_PREPARATION_H_
#define HEAVENBENCH_SRC_RHI_SCENE_DISPLACEMENT_PREPARATION_H_

#include "rhi/rhi.h"

namespace filament { class Engine; }
namespace heaven {
struct RhiScene;
class MeshStore;
class TextureStore;
// Startup/reload boundary after material/texture configuration and before any
// scene or depth renderable borrows the upload. No per-frame generation.
[[nodiscard]] Result<void> prepare_scene_displacement(filament::Engine& engine,
    MeshStore& meshes, TextureStore& textures, const AppConfig& config,
    RhiScene& scene, const MaterialResolver& resolve,
    const LoadingProgressCallback& progress = {});
}
#endif
