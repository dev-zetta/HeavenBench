/*
 * HeavenBench — Backend core handle
 *
 * A non-owning view of the Filament core objects the FilamentBackend
 * creates and owns (engine, scene, view, renderer, camera). It is handed
 * to the backend's collaborator subsystems (mesh / texture / material
 * stores, scene builder, light spawner, lighting, depth pre-pass) so each
 * one can reach the core objects it needs without taking five raw
 * pointers individually — and without depending on the feature-facing
 * RenderContext (which also carries an IResourceProvider and data_dir
 * that the infrastructure subsystems don't need).
 *
 * The pointers are owned by FilamentBackend; subsystems must never
 * create or destroy them.
 */
#ifndef HEAVENBENCH_SRC_RHI_BACKEND_CORE_H_
#define HEAVENBENCH_SRC_RHI_BACKEND_CORE_H_

namespace filament {
class Engine;
class Scene;
class View;
class Renderer;
class Camera;
}

namespace heaven {

struct BackendCore {
    filament::Engine*   engine   = nullptr;
    filament::Scene*    scene    = nullptr;
    filament::View*     view     = nullptr;
    filament::Renderer* renderer = nullptr;
    filament::Camera*   camera   = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_BACKEND_CORE_H_
