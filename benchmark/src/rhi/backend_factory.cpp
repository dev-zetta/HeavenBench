/*
 * HeavenBench — Backend factory
 *
 * Filament is the only supported backend today. The legacy
 * hand-rolled OpenGL 4.5 backend was removed once Filament
 * reached feature parity. The factory shape is kept (rather
 * than hard-wiring the FilamentBackend constructor at every
 * call site) so adding a new backend later doesn't churn
 * application code.
 */
#include "rhi/rhi.h"
#include "rhi/filament_backend.h"

namespace heaven {

Result<std::unique_ptr<IRhiBackend>> create_backend(
        GraphicsApi api, WindowHandle& window, const AppConfig& config) {
    switch (api) {
    case GraphicsApi::Filament: {
        auto backend = std::make_unique<FilamentBackend>(window.window, config);
        if (!backend->initialized()) {
            return std::unexpected(Error(backend->initialization_error()));
        }
        return backend;
    }
    }
    return std::unexpected(Error("Unknown graphics API"));
}

} // namespace heaven
