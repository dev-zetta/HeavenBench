/*
 * HeavenBench — Property test: grass blade generation never overflows
 *
 * Feature: codebase-hardening
 *   •: Grass blade generation never overflows
 *
 *
 * The CPU grass generator (engine/grass_cpu.cpp::generate_grass_blades) writes
 * `segments*2 + 1` points into a per-blade scratch array. Before  that
 * scratch was a fixed 9-element array sized for a hard-coded 4-segment
 * assumption, so any configured GrassParams::blade_segments value greater than 4
 * wrote past the end of the stack array.  sized the scratch from the
 * compile-time capacity kMaxBladePts and clamped the effective segment count to
 * `std::clamp(blade_segments, 1, kMaxBladeSegments)`.
 *
 * This property pins that fix: FOR ALL
 * blade_segments values across the full range — including values greater than
 * kMaxBladeSegments and zero/negative values — generate_grass_blades returns
 * without an out-of-bounds write (enforced when the suite is built under
 * AddressSanitizer), and the size of the returned vertex buffer is consistent
 * with the CLAMPED segment count rather than the raw, possibly-overflowing
 * configured value.
 *
 * The generator derives its per-blade triangle/vertex counts from the clamped
 * segment count:
 *
 *     segments        = clamp(blade_segments, 1, kMaxBladeSegments)
 *     tris_per_blade  = segments * 2 - 1
 *     verts_per_blade = tris_per_blade * 3
 *     output size     = blade_count * verts_per_blade
 *
 * so the reference vertex count this property checks is
 *     blade_count * (clamp(blade_segments, 1, kMaxBladeSegments) * 2 - 1) * 3.
 *
 * Random GrassRoot vectors are fed alongside the fuzzed blade_segments. Root
 * positions and the other GrassParams fields do not affect the output COUNT
 * (only the per-vertex values), so the count invariant holds for any roots; the
 * varied roots additionally exercise the generation loop body across many
 * blades under ASan.
 *
 * generate_grass_blades is GPU-free and host-testable. This file self-registers
 * with the RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point.
 */
#include <algorithm>
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

// The per-blade vertex count the generator produces for a given configured
// segment count, computed independently from the clamped segment count. This is
// the reference the property compares against (it must match the generator's own
// `verts_per_blade = (clamp(blade_segments,1,kMaxBladeSegments)*2 - 1) * 3`).
int expected_verts_per_blade(int blade_segments) {
    const int segments =
        std::clamp(blade_segments, 1, heaven::kMaxBladeSegments);
    return (segments * 2 - 1) * 3;
}

// Build a vector of random grass roots. Root positions only affect per-vertex
// values, not the output count, so any finite coordinates exercise the loop.
std::vector<heaven::GrassRoot> gen_roots(int count) {
    std::vector<heaven::GrassRoot> roots;
    roots.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(*rc::gen::inRange(-1000, 1000)) * 0.01f;
        const float y = static_cast<float>(*rc::gen::inRange(-1000, 1000)) * 0.01f;
        const float z = static_cast<float>(*rc::gen::inRange(-1000, 1000)) * 0.01f;
        roots.push_back({glm::vec3(x, y, z)});
    }
    return roots;
}

// Drive generate_grass_blades over a fuzzed (roots, blade_segments) pair and
// assert the no-overflow contract (enforced under ASan) plus
// the clamped-count invariant: the returned buffer holds
// exactly blade_count * verts_per_blade(clamped) vertices.
void check_no_overflow(const std::vector<heaven::GrassRoot>& roots,
                       int blade_segments) {
    heaven::GrassParams params;       // Shipping_Look defaults …
    params.blade_segments = blade_segments;  // … with the segment count fuzzed.

    const std::vector<heaven::GrassVertex> verts =
        heaven::generate_grass_blades(roots, /*time=*/1.5f, params);

    // Output size is consistent with the CLAMPED segment count — never with the
    // raw, possibly-overflowing configured value.
    const std::size_t expected =
        roots.size() *
        static_cast<std::size_t>(expected_verts_per_blade(blade_segments));
    RC_ASSERT(verts.size() == expected);
}

// blade_segments swept across the supported range AND beyond it: negative, zero,
// the in-range values, the capacity boundary kMaxBladeSegments, and values well
// past the capacity (which the generator must clamp rather than overflow on).
bool run_p11_full_range() {
    return rc::check(
        "Feature: codebase-hardening, Property 11: grass blade generation never "
        "overflows — blade_segments across the full range incl. "
        ">kMaxBladeSegments and 0/negative (Validates: Requirements 7.1, 7.2, "
        "7.4)",
        [] {
            const int blade_count = *rc::gen::inRange(0, 64);
            const std::vector<heaven::GrassRoot> roots = gen_roots(blade_count);

            // Range spans negatives, zero, the in-range band, the capacity, and
            // values past the capacity (2× kMaxBladeSegments + headroom).
            const int blade_segments = *rc::gen::inRange(
                -8, heaven::kMaxBladeSegments * 2 + 8);

            check_no_overflow(roots, blade_segments);
        });
}

// A focused pass on the explicit boundary values the requirement calls out, so
// the corner cases are exercised every run rather than only when RapidCheck's
// sampler happens to pick them.
bool run_p11_boundary_values() {
    return rc::check(
        "Feature: codebase-hardening, Property 11: grass blade generation never "
        "overflows — explicit boundary segment counts (Validates: Requirements "
        "7.1, 7.2, 7.4)",
        [] {
            const int blade_count = *rc::gen::inRange(1, 16);
            const std::vector<heaven::GrassRoot> roots = gen_roots(blade_count);

            const int boundaries[] = {
                -100, -1, 0, 1, 2, 4,
                heaven::kMaxBladeSegments - 1,
                heaven::kMaxBladeSegments,
                heaven::kMaxBladeSegments + 1,
                heaven::kMaxBladeSegments * 4,
                1000000,
            };
            const std::size_t idx = *rc::gen::inRange<std::size_t>(
                0, std::size(boundaries));
            check_no_overflow(roots, boundaries[idx]);
        });
}

bool run_grass_segment_properties() {
    bool ok = true;
    ok = run_p11_full_range() && ok;
    ok = run_p11_boundary_values() && ok;
    return ok;
}

const heaven::testing::Register g_register_grass_segments{
    &run_grass_segment_properties};

} // namespace
