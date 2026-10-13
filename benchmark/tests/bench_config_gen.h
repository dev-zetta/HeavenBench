// Feature: in-ui-configuration — shared RapidCheck BenchConfig generator
/*
 * HeavenBench — shared host-test generator for a randomized BenchConfig.
 *
 * This header generalizes the inline `gen_governed_config()` originally written
 * in tests/prop_catalog_seed.cpp into a single reusable generator the In-UI
 * Configuration property/unit tests (tasks 4.4–4.12, 5.x) share. It randomizes
 * EVERY catalog-governed BenchConfig field across its full valid range, plus
 * boundary values and out-of-range inputs, so the controller's clamp / reject /
 * route paths are exercised uniformly. All generated floats are finite (NaN is
 * deliberately excluded so variant value-equality and round-trip comparisons in
 * consumer tests are well defined; the few helpers that sanitize NaN are
 * covered by the dedicated catalog-validation property instead).
 *
 * IMPORTANT (RapidCheck idiom, matching the codebase): the generators below
 * call `*rc::gen::...`, which is only valid INSIDE a running property. They are
 * therefore plain `inline` free functions that each consuming test calls inside
 * its own `rc::check(...)` lambda — exactly the established
 * prop_catalog_seed.cpp pattern — NOT `rc::Gen<>` factories usable at namespace
 * scope. Being `inline` and token-identical across every TU that includes this
 * header, they are ODR-safe to share.
 *
 * Sub-1.0 magnitudes: several persisted floats (rain intensity, music volume,
 * tessellation scale/factor, foliage density) naturally take values whose
 * magnitude is below 1.0, and the in-range draw plus an explicit sub-1.0 branch
 * guarantee such values appear — this is what exercises the save/load
 * round-trip's "1e-6 absolute tolerance below magnitude 1.0" branch (Property
 * 15 /).
 *
 * Validity note: because the generator intentionally also emits boundary and
 * out-of-range values, consumers that need a strictly in-range BenchConfig
 * (e.g. an EXACT save/load round-trip) should first normalize each governed
 * field through the Settings Catalog's apply_to() — the same shared validators
 * load/CLI use — so the comparison is against the post-clamp value. Routing,
 * dirty-state, and reload tests can consume the raw generated config directly,
 * since the controller clamps deterministically on apply.
 *
 * GPU-free: depends only on engine/cli_parser.h (BenchConfig), engine/types.h,
 * RapidCheck, and the standard library. References no Filament symbol, so it
 * links into the Filament-free heaven_benchmark_tests host target.
 */
#ifndef HEAVENBENCH_TESTS_BENCH_CONFIG_GEN_H_
#define HEAVENBENCH_TESTS_BENCH_CONFIG_GEN_H_

#include <cmath>
#include <string>

#include <rapidcheck.h>

#include "engine/cli_parser.h"  // heaven::BenchConfig (AppConfig + benchmark fields)
#include "engine/types.h"       // QualityLevel, the validation bounds/helpers

namespace heaven::testing {

// Draw an arbitrary FINITE float (any magnitude, either sign). Used for fields
// whose stored value is not bound-checked on its own; the suchThat guard keeps
// NaN/Inf out so consumer comparisons stay well defined.
[[nodiscard]] inline float gen_finite_float() {
    return *rc::gen::suchThat<float>([](float f) { return std::isfinite(f); });
}

// Draw a finite float biased to cover, for a nominal valid range [lo, hi]:
//   • both boundary values exactly (lo, hi),
//   • a sub-1.0 magnitude in (-1, 1) (the round-trip tolerance branch),
//   • out-of-range values below lo and above hi (clamp/reject branches),
//   • and — most often — a uniform in-range value on a 1000-step grid.
// Every result is finite. The in-range branch dominates (~50%) so consumers
// that normalize boundary/out-of-range inputs still see a broad valid spread.
[[nodiscard]] inline float gen_ranged_float(double lo, double hi) {
    if (hi < lo) std::swap(lo, hi);
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    const int kind = *rc::gen::inRange(0, 10);
    switch (kind) {
        case 0:  // lower boundary
            return static_cast<float>(lo);
        case 1:  // upper boundary
            return static_cast<float>(hi);
        case 2: {  // sub-1.0 magnitude in (-1, 1)
            const int n = *rc::gen::inRange(-999999, 1000000);
            return static_cast<float>(n) / 1.0e6f;
        }
        case 3: {  // below the range (out of range)
            const int mult = *rc::gen::inRange(1, 4);
            return static_cast<float>(lo - span * static_cast<double>(mult));
        }
        case 4: {  // above the range (out of range)
            const int mult = *rc::gen::inRange(1, 4);
            return static_cast<float>(hi + span * static_cast<double>(mult));
        }
        default: {  // uniform in-range on a fixed grid
            const int steps = 1000;
            const int n = *rc::gen::inRange(0, steps + 1);
            return static_cast<float>(
                lo + (hi - lo) * static_cast<double>(n) / static_cast<double>(steps));
        }
    }
}

// Draw an int biased to cover boundary and out-of-range values around [lo, hi]
// as well as the interior. Mirrors gen_ranged_float for the integer knobs.
[[nodiscard]] inline int gen_ranged_int(int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    const int span = (hi > lo) ? (hi - lo) : 1;
    const int kind = *rc::gen::inRange(0, 8);
    switch (kind) {
        case 0:  return lo;            // lower boundary
        case 1:  return hi;            // upper boundary
        case 2:  return lo - *rc::gen::inRange(1, span + 1);  // below range
        case 3:  return hi + *rc::gen::inRange(1, span + 1);  // above range
        default: return *rc::gen::inRange(lo, hi + 1);        // interior
    }
}

// Draw one of the three valid ordered quality levels.
[[nodiscard]] inline QualityLevel gen_quality_level() {
    return *rc::gen::element(QualityLevel::Low, QualityLevel::Medium,
                             QualityLevel::High);
}

// Build a BenchConfig with every catalog-governed field independently
// randomized. Fields the catalog does not govern keep their defaults. Call this
// INSIDE an rc::check(...) lambda (it dereferences RapidCheck generators).
[[nodiscard]] inline BenchConfig gen_bench_config() {
    BenchConfig cfg;

    // ── Camera (Live; not persisted in config_schema) ──
    cfg.features.physical_camera  = *rc::gen::arbitrary<bool>();
    cfg.physical_camera.aperture =
        gen_ranged_float(PhysicalCameraConfig::k_aperture_lo,
                         PhysicalCameraConfig::k_aperture_hi);
    cfg.physical_camera.shutter_speed =
        gen_ranged_float(PhysicalCameraConfig::k_shutter_lo,
                         PhysicalCameraConfig::k_shutter_hi);
    cfg.physical_camera.sensitivity =
        gen_ranged_float(PhysicalCameraConfig::k_iso_lo,
                         PhysicalCameraConfig::k_iso_hi);

    // ── Environment (Live) ──
    cfg.time_of_day              = gen_ranged_float(0.0, 24.0);
    cfg.bloom_strength           = gen_ranged_float(0.0, 1.0);
    cfg.weather.rain_intensity   = gen_ranged_float(0.0, 1.0);
    cfg.weather.dry_down_seconds = gen_ranged_float(0.5, 30.0);
    cfg.features.rain            = *rc::gen::arbitrary<bool>();
    cfg.features.wet_reflections = *rc::gen::arbitrary<bool>();
    cfg.features.sky             = *rc::gen::arbitrary<bool>();

    // ── Tessellation (Reload) ── unbounded floats; nominal ranges keep them
    // finite while still emitting sub-1.0 magnitudes (defaults are 0.7 / 0.8).
    cfg.tessellation.enabled  = *rc::gen::arbitrary<bool>();
    cfg.tessellation.scale    = gen_ranged_float(0.0, 2.0);
    cfg.tessellation.factor   = gen_ranged_float(0.0, 2.0);
    cfg.tessellation.distance = gen_ranged_float(0.0, 200.0);

    // ── Settings (Reload) ──
    cfg.width                = gen_ranged_int(320, 7680);
    cfg.height               = gen_ranged_int(240, 4320);
    cfg.fullscreen           = *rc::gen::arbitrary<bool>();
    cfg.hdr                  = *rc::gen::arbitrary<bool>();
    cfg.hdr_peak_nits        = gen_ranged_float(0.0, 10000.0);
    cfg.hdr_paper_white_nits = gen_ranged_float(0.0, 1000.0);
    cfg.cloth_sim_hz         = gen_ranged_float(0.0, 240.0);

    // ── Quality — feature toggles ──
    cfg.features.grass               = *rc::gen::arbitrary<bool>();
    cfg.features.particles           = *rc::gen::arbitrary<bool>();
    cfg.features.god_rays            = *rc::gen::arbitrary<bool>();
    cfg.features.volumetric_fog      = *rc::gen::arbitrary<bool>();
    cfg.features.clouds              = *rc::gen::arbitrary<bool>();
    cfg.features.lens_flares         = *rc::gen::arbitrary<bool>();
    cfg.features.volumetric_clouds   = *rc::gen::arbitrary<bool>();
    cfg.features.ssgi                = *rc::gen::arbitrary<bool>();
    cfg.features.massive_foliage     = *rc::gen::arbitrary<bool>();
    cfg.features.ground_relief       = *rc::gen::arbitrary<bool>();
    cfg.features.soft_shadows        = *rc::gen::arbitrary<bool>();
    cfg.features.contact_shadows     = *rc::gen::arbitrary<bool>();
    cfg.features.point_light_shadows = *rc::gen::arbitrary<bool>();
    cfg.features.reflections         = *rc::gen::arbitrary<bool>();
    cfg.wind_debug                   = *rc::gen::arbitrary<bool>();
    cfg.features.msaa                = *rc::gen::arbitrary<bool>();
    cfg.features.refraction          = *rc::gen::arbitrary<bool>();
    cfg.features.clear_coat          = *rc::gen::arbitrary<bool>();
    cfg.features.anisotropic         = *rc::gen::arbitrary<bool>();
    cfg.features.subsurface          = *rc::gen::arbitrary<bool>();
    cfg.features.cloth               = *rc::gen::arbitrary<bool>();
    cfg.features.transparent_shadows = *rc::gen::arbitrary<bool>();
    cfg.features.colored_penumbra    = *rc::gen::arbitrary<bool>();
    cfg.features.specular_aa         = *rc::gen::arbitrary<bool>();
    cfg.heat_haze_enabled            = *rc::gen::arbitrary<bool>();

    // ── Quality — knobs ──
    cfg.quality.soft_shadow_level    = gen_quality_level();
    cfg.quality.contact_shadow_level = gen_quality_level();
    cfg.quality.shadow_light_budget =
        gen_ranged_int(0, cfg.quality.shadow_light_budget_max);
    cfg.quality.cloud_quality        = gen_quality_level();
    cfg.quality.ssgi_quality         = gen_quality_level();
    cfg.quality.foliage_density =
        gen_ranged_float(1.0, static_cast<double>(cfg.quality.foliage_density_max));
    // MSAA accepts EXACTLY {1,2,4}; draw from the valid set plus a few rejected
    // values so consumers see both the accept and reject-and-retain branches.
    cfg.quality.msaa_samples = *rc::gen::element(1, 2, 4, 0, 3, 8);
    cfg.quality_preset_label = *rc::gen::arbitrary<std::string>();
    cfg.texture_quality = static_cast<TextureQuality>(
        *rc::gen::inRange(0, 3));
    cfg.frame_generation = static_cast<FrameGenerationMode>(*rc::gen::inRange(0, 3));

    // ── Sound ──
    cfg.music.enabled = *rc::gen::arbitrary<bool>();
    cfg.music.volume  = gen_ranged_float(0.0, 1.0);

    return cfg;
}

} // namespace heaven::testing

#endif // HEAVENBENCH_TESTS_BENCH_CONFIG_GEN_H_
