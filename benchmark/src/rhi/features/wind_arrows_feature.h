/*
 * HeavenBench — Wind debug arrows feature
 *
 * A debug visualisation for the cloth simulation: one thin elongated
 * cuboid per cloth body, oriented along the wind direction the cloth
 * experiences in WORLD space. The arrow's length encodes wind speed ×
 * gust; this is the ground-truth way to confirm that wind and the
 * cloth's simulation frame agree. If the arrow points one way and the
 * flag billows the other, there's a body-local-vs-world transform
 * mismatch.
 *
 * Unlike the backend's other former responsibilities, the wind arrows
 * are a toggleable, optional, visible scene-graph effect that owns its
 * own renderables — so it is a render feature, not a backend collaborator.
 * It only owns renderables once arrows arrive via set_arrows(); the main
 * loop pushes them per frame only when `--wind-debug` is on.
 *
 * Migrated verbatim out of FilamentBackend::set_wind_debug_arrows.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_WIND_ARROWS_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_WIND_ARROWS_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>

#include <utils/Entity.h>

#include <span>
#include <vector>

namespace heaven {

class WindArrowsFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "wind-arrows"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void destroy(RenderContext& ctx);

    // Per-frame arrow list from the main loop (cloth wind vectors).
    // Stored and applied in the next update(). An empty span destroys
    // all arrows.
    void set_arrows(std::span<const IRhiBackend::WindArrow> arrows);

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    void apply(RenderContext& ctx, std::span<const IRhiBackend::WindArrow> arrows);

    struct DebugArrow {
        utils::Entity               entity;
        filament::MaterialInstance* mi = nullptr;
    };
    std::vector<DebugArrow>      arrows_;
    std::vector<IRhiBackend::WindArrow> pending_;
    filament::Material*          material_ = nullptr;
    filament::VertexBuffer*      vb_       = nullptr;
    filament::IndexBuffer*       ib_       = nullptr;
    bool                         enabled_arrows_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_WIND_ARROWS_FEATURE_H_
