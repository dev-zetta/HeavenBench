/*
 * HeavenBench — Procedural sky IBL probe
 *
 * Generates the Z-up procedural sky cubemap that drives Filament's
 * IndirectLight reflections (and, when the visible sky-sphere feature
 * is disabled, the background Skybox).
 *
 * This is deliberately NOT part of SkyFeature: the cubemap is the
 * scene's ambient/reflection environment and must exist regardless of
 * whether the optional sky-sphere is enabled. It is a pure generator —
 * (engine, lighting) → Texture* — owned by whoever calls it (the
 * Filament backend's setup_lighting manages its lifetime and binds it
 * to IndirectLight / Skybox).
 */
#ifndef HEAVENBENCH_SRC_RHI_SKY_IBL_H_
#define HEAVENBENCH_SRC_RHI_SKY_IBL_H_

#include "rhi/rhi.h"   // LightingParams

namespace filament {
class Engine;
class Texture;
}

namespace heaven {

// Build a Z-up procedural sky cubemap. Each face is rendered by
// sampling the world-space direction at every texel, computing a
// horizon→zenith gradient, painting a sun disc where the direction
// matches `lighting.sun_dir`, and overlaying a low-frequency cloud
// noise layer. The result is a full HDR sky in 6 faces of FACE×FACE
// RGBA8 texels — properly oriented for our Z-up world, so Filament's
// Skybox sampler renders it without rotation or seams.

// The caller owns the returned texture and must destroy it via the
// same engine.

// `include_clouds` gates the procedural cloud-noise overlay. It must
// track the old cloud-quad feature toggle (`FeatureToggles::clouds`):
// this cubemap is the IBL reflection/ambient probe, so baking clouds
// into it makes those clouds appear in every reflective surface (e.g.
// the airship's metal hull) even when the cloud feature is disabled.
// With clouds off the probe stays a clean horizon→zenith gradient.
[[nodiscard]] filament::Texture* build_sky_cubemap(
    filament::Engine& engine, const LightingParams& lighting,
    bool include_clouds = false);

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_SKY_IBL_H_
