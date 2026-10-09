/*
 * HeavenBench — Wind debug arrows feature
 *
 * Migrated verbatim out of FilamentBackend::set_wind_debug_arrows.
 */
#include "rhi/features/wind_arrows_feature.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"   // glm_to_filament
#include "rhi/materials_data.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>

#include <utils/EntityManager.h>

#include <math/vec3.h>

#include <glm/glm.hpp>

#include <cstring>
#include <cmath>

namespace heaven {

void WindArrowsFeature::init(RenderContext& /*ctx*/, const AppConfig& config) {
    enabled_ = config.wind_debug;
    // No GPU resources up front — the cuboid VB/IB and material are
    // built lazily on the first set_arrows() that has work to do, just
    // like the original backend code.
}

void WindArrowsFeature::set_arrows(std::span<const IRhiBackend::WindArrow> arrows) {
    pending_.assign(arrows.begin(), arrows.end());
}

void WindArrowsFeature::update(RenderContext& ctx, const FrameContext& /*frame*/) {
    apply(ctx, pending_);
}

// ── Wind debug arrows (cloth simulation visualization) ─────────

// Lazy-builds a unit cuboid (1 m along +X, 0.06 m square cross-
// section) shared by every arrow renderable. Each arrow is
// repositioned and re-oriented per frame to point in the
// world-space wind direction at its origin. Visual meaning:

//   - origin: where the arrow's TAIL sits (anchor centroid of
//     the cloth body).
//   - direction.length(): wind speed × gust factor (m/s). The
//     arrow stretches to represent it visually.
//   - color: brightness encodes whether this is the "tracked"
//     cloth (the one diagnostics dump for) or another one.

// Calling this function with an empty span destroys all arrows
// and disables the debug pass for the rest of the frame.
void WindArrowsFeature::apply(RenderContext& ctx,
                              std::span<const IRhiBackend::WindArrow> arrows) {
    auto* engine_ = ctx.engine;
    auto* scene_  = ctx.scene;
    if (!engine_) return;

    // Nothing to do and nothing built yet — don't allocate the shared
    // cuboid / material until arrows actually arrive. This preserves the
    // original behaviour where set_wind_debug_arrows() was only called
    // (and resources only built) when `--wind-debug` was enabled.
    if (arrows.empty() && arrows_.empty()) return;

    // Lazy resource init — cube VB/IB and material are shared
    // across every arrow. Build once and reuse.
    if (!material_) {
        material_ = filament::Material::Builder()
            .package(k_heaven_debug_arrow_filamat,
                     k_heaven_debug_arrow_filamat_size)
            .build(*engine_);
    }
    if (!material_) return;
    if (!vb_ || !ib_) {
        // Unit cuboid: 1 m in +X, 0.06 m square in YZ. Centred at
        // origin, so we can scale by (length, 1, 1) and translate
        // by `origin + 0.5 * direction` to put the tail at origin.
        struct V { float x, y, z; float u, v; };
        constexpr float L = render_tuning::wind_arrows.shaft_length;
        constexpr float W = render_tuning::wind_arrows.half_width;
        const V verts[8] = {
            { 0,  -W, -W, 0, 0}, { L, -W, -W, 1, 0}, { L,  W, -W, 1, 1}, { 0,  W, -W, 0, 1},
            { 0,  -W,  W, 0, 0}, { L, -W,  W, 1, 0}, { L,  W,  W, 1, 1}, { 0,  W,  W, 0, 1},
        };
        const uint16_t idx[36] = {
            0,1,2, 0,2,3,    // -Z
            4,6,5, 4,7,6,    // +Z
            0,5,1, 0,4,5,    // -Y
            3,2,6, 3,6,7,    // +Y
            0,3,7, 0,7,4,    // start cap (-X)
            1,5,6, 1,6,2,    // end cap (+X)
        };
        vb_ = filament::VertexBuffer::Builder()
            .vertexCount(8).bufferCount(1)
            .attribute(filament::VertexAttribute::POSITION, 0,
                       filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(V))
            .attribute(filament::VertexAttribute::UV0, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2, 12, sizeof(V))
            .build(*engine_);
        auto* vbuf = new V[8];
        std::memcpy(vbuf, verts, sizeof(verts));
        vb_->setBufferAt(*engine_, 0,
            filament::VertexBuffer::BufferDescriptor(vbuf, sizeof(verts),
                [](void* b, size_t, void*){ delete[] static_cast<V*>(b); }));

        ib_ = filament::IndexBuffer::Builder()
            .indexCount(36)
            .bufferType(filament::IndexBuffer::IndexType::USHORT)
            .build(*engine_);
        auto* ibuf = new uint16_t[36];
        std::memcpy(ibuf, idx, sizeof(idx));
        ib_->setBuffer(*engine_,
            filament::IndexBuffer::BufferDescriptor(ibuf, sizeof(idx),
                [](void* b, size_t, void*){ delete[] static_cast<uint16_t*>(b); }));
    }

    // Resize the entity pool to match the number of arrows the
    // caller wants. Build new entities lazily, destroy excess.
    while (arrows_.size() < arrows.size()) {
        DebugArrow a;
        a.entity = utils::EntityManager::get().create();
        a.mi = material_->createInstance();
        // Per-arrow color: cycle through a small distinct palette
        // so we can tell flag #0 from flag #3 visually.
        const auto& c =
            render_tuning::wind_arrows.palette[
                arrows_.size() % render_tuning::wind_arrows.palette.size()];
        a.mi->setParameter("color", filament::math::float3{c.x, c.y, c.z});
        filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      vb_, ib_, 0, 36)
            .material(0, a.mi)
            .castShadows(false)
            .receiveShadows(false)
            .culling(false)
            .priority(7)
            .boundingBox(box_from_min_max(
                {-render_tuning::wind_arrows.bounds_extent,
                 -render_tuning::wind_arrows.bounds_extent,
                 -render_tuning::wind_arrows.bounds_extent},
                {render_tuning::wind_arrows.bounds_extent,
                 render_tuning::wind_arrows.bounds_extent,
                 render_tuning::wind_arrows.bounds_extent}))
            .build(*engine_, a.entity);
        engine_->getTransformManager().create(a.entity);
        scene_->addEntity(a.entity);
        arrows_.push_back(a);
    }
    while (arrows_.size() > arrows.size()) {
        auto& a = arrows_.back();
        scene_->remove(a.entity);
        engine_->destroy(a.entity);
        utils::EntityManager::get().destroy(a.entity);
        if (a.mi) engine_->destroy(a.mi);
        arrows_.pop_back();
    }

    // Per-frame: orient and scale each arrow.
    auto& tcm = engine_->getTransformManager();
    for (size_t i = 0; i < arrows.size(); ++i) {
        const auto& wa = arrows[i];
        glm::vec3 dir = wa.direction;
        float len = glm::length(dir);
        if (len < render_tuning::wind_arrows.minimum_direction_length) {
            // Degenerate — collapse the arrow so it's invisible
            // but the entity stays alive for next frame.
            glm::mat4 m(1.0f);
            m[0][0] = m[1][1] = m[2][2] = 0.0f;
            tcm.setTransform(tcm.getInstance(arrows_[i].entity),
                             glm_to_filament(m));
            continue;
        }
        glm::vec3 x_axis = dir / len;
        // Build any orthonormal basis with x_axis as +X.
        glm::vec3 up =
            std::abs(x_axis.z) <
                    render_tuning::wind_arrows.basis_parallel_threshold
                       ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        glm::vec3 y_axis = glm::normalize(glm::cross(up, x_axis));
        glm::vec3 z_axis = glm::cross(x_axis, y_axis);

        glm::mat4 m(1.0f);
        m[0] = glm::vec4(x_axis * len, 0.0f);   // length encodes magnitude
        m[1] = glm::vec4(y_axis,        0.0f);
        m[2] = glm::vec4(z_axis,        0.0f);
        m[3] = glm::vec4(wa.origin,     1.0f);
        tcm.setTransform(tcm.getInstance(arrows_[i].entity),
                         glm_to_filament(m));
    }
    enabled_arrows_ = !arrows.empty();
}

void WindArrowsFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    // When toggled off, tear down the arrow renderables; when toggled
    // back on they rebuild from the next set_arrows()/update().
    if (!on) {
        for (auto& a : arrows_) {
            ctx.scene->remove(a.entity);
            ctx.engine->destroy(a.entity);
            utils::EntityManager::get().destroy(a.entity);
            if (a.mi) ctx.engine->destroy(a.mi);
        }
        arrows_.clear();
        enabled_arrows_ = false;
    }
}

void WindArrowsFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    for (auto& a : arrows_) {
        ctx.scene->remove(a.entity);
        engine_->destroy(a.entity);
        utils::EntityManager::get().destroy(a.entity);
        if (a.mi) engine_->destroy(a.mi);
    }
    arrows_.clear();
    if (vb_) { engine_->destroy(vb_); vb_ = nullptr; }
    if (ib_) { engine_->destroy(ib_); ib_ = nullptr; }
    if (material_) { engine_->destroy(material_); material_ = nullptr; }
}

} // namespace heaven
