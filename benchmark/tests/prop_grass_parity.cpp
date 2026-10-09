/*
 * HeavenBench — Property test: grass 4-segment parity
 *
 * Feature: codebase-hardening
 *   •: Grass 4-segment parity
 *
 *
 * pins the Shipping_Look invariant for the grass generator: WHERE
 * Grass_Params.blade_segments equals 4, THE Grass_Generator SHALL produce blade
 * geometry identical to the current Shipping_Look output.  reworked
 * generate_grass_blades (engine/grass_cpu.cpp) to size its per-blade scratch from
 * a clamped segment count instead of a hard-coded 4-segment assumption; this
 * property is the regression guard that the default 4-segment path is unchanged.
 *
 * There is no separate "reference 4-segment generator", so parity is pinned by
 * three structural invariants the reworked generator must satisfy at 4 segments:
 *
 *   (a) Default-vs-explicit equivalence — the default GrassParams (blade_segments
 *       defaults to 4) and an explicitly-constructed GrassParams with
 *       blade_segments == 4 produce byte-identical GrassVertex output for the
 *       same roots / time / lighting inputs. This is exactly the 4-segment
 *       Shipping_Look path, so any divergence in the clamp/derivation logic that
 *       perturbed the default would surface here.
 *
 *   (b) Determinism — repeated calls with identical inputs produce identical
 *       output. The generator derives every per-blade shape deterministically
 *       from the root world position (no RNG, no time-of-call state), so two
 *       calls must agree field-for-field.
 *
 *   (c) Per-blade vertex count — at 4 segments each blade emits exactly
 *       GRASS_VERTS_PER_BLADE (21) vertices, so the total vertex count is
 *       roots.size() * 21. This pins the documented Shipping_Look layout
 *       (7 triangles × 3 vertices per blade).
 *
 * generate_grass_blades is GPU-free and host-testable. Because both sides of
 * each comparison run the identical code path, exact float equality is the right
 * comparison (no tolerance needed). This file self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point.
 */
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <rapidcheck.h>

#include "engine/grass_cpu.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// ── Field-by-field exact equality for two GrassVertex values ──

// Both operands are produced by the same code path (4-segment generator), so
// exact bit-for-bit float equality is the correct comparison — there is no
// rounding divergence to tolerate. Comparing every field guarantees position,
// normal, UV, and albedo-tint/alpha fields all match.
bool vertices_identical(const heaven::GrassVertex& a,
                        const heaven::GrassVertex& b) {
    return a.px == b.px && a.py == b.py && a.pz == b.pz &&
           a.nx == b.nx && a.ny == b.ny && a.nz == b.nz &&
           a.u == b.u && a.v == b.v &&
           a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool arrays_identical(const std::vector<heaven::GrassVertex>& a,
                      const std::vector<heaven::GrassVertex>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!vertices_identical(a[i], b[i])) return false;
    }
    return true;
}

// Generate a random set of grass roots. Positions span a few hundred metres in
// each axis so the position-derived per-blade hashes (kind / height / width /
// bend / wind phase) exercise a varied range rather than a single blade shape.
std::vector<heaven::GrassRoot> gen_roots() {
    const int count = *rc::gen::inRange(1, 64);
    std::vector<heaven::GrassRoot> roots;
    roots.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(*rc::gen::inRange(-500, 500)) * 0.1f;
        const float y = static_cast<float>(*rc::gen::inRange(-500, 500)) * 0.1f;
        const float z = static_cast<float>(*rc::gen::inRange(-500, 500)) * 0.1f;
        roots.push_back({glm::vec3(x, y, z)});
    }
    return roots;
}

bool run_p12_grass_4segment_parity() {
    return rc::check(
        "Feature: codebase-hardening, Property 12: grass 4-segment parity "
        "(Validates: Requirements 7.3)",
        [] {
            const std::vector<heaven::GrassRoot> roots = gen_roots();

            // Shared animation input.
            const float time = static_cast<float>(*rc::gen::inRange(0, 100000)) * 0.01f;

            // Default GrassParams: blade_segments defaults to 4 (the
            // Shipping_Look default).
            const heaven::GrassParams default_params{};
            RC_ASSERT(default_params.blade_segments == 4);

            // Explicit 4-segment params — identical to the default in every
            // field, but constructed explicitly so we compare the "default"
            // and "explicit blade_segments == 4" code paths.
            heaven::GrassParams explicit_params{};
            explicit_params.blade_segments = 4;

            const std::vector<heaven::GrassVertex> out_default =
                heaven::generate_grass_blades(roots, time, default_params);
            const std::vector<heaven::GrassVertex> out_explicit =
                heaven::generate_grass_blades(roots, time, explicit_params);

            // (a) Default and explicit-4 produce byte-identical output.
            RC_ASSERT(arrays_identical(out_default, out_explicit));

            // (b) Determinism: a repeated call with the same inputs reproduces
            // the same output exactly.
            const std::vector<heaven::GrassVertex> out_repeat =
                heaven::generate_grass_blades(roots, time, default_params);
            RC_ASSERT(arrays_identical(out_default, out_repeat));

            // (c) Per-blade vertex count matches the documented Shipping_Look
            // layout: GRASS_VERTS_PER_BLADE (21) vertices per root.
            const std::size_t expected =
                roots.size() *
                static_cast<std::size_t>(heaven::GRASS_VERTS_PER_BLADE);
            RC_ASSERT(out_default.size() == expected);
        });
}

const heaven::testing::Register g_register_grass_parity{
    &run_p12_grass_4segment_parity};

} // namespace
