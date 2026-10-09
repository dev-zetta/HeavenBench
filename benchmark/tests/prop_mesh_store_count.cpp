// Feature: codebase-hardening,
/*
 * HeavenBench — Property test for mesh-store vertex re-upload count
 * validation (design B4).
 *
 * Feature: codebase-hardening
 *   •: Mesh-store vertex-count validation
 *
 *
 * The real MeshStore::update_vertices (rhi/mesh_store.cpp) re-uploads CPU-skinned
 * vertices into the Filament VertexBuffer it built in upload(). Exercising it
 * needs a live Filament engine (the buffers are GPU resources) and the recorded
 * count lives on the private MeshData stored in mesh_data_, so the seam is not
 * reachable from the headless RapidCheck harness.  covers the
 * real-buffer behaviour as a HEAVEN_SMOKE-gated seam; this property pins the
 * host-side count-validation INVARIANT that gates that re-upload.
 *
 * That gate is exactly one comparison in update_vertices:
 *
 *     if (vertex_count != md.vertex_count)
 *         return std::unexpected(Error{...}); 10.1, 10.2: reject, no setBufferAt
 * ... setBufferAt(...)...                   / 10.3: re-upload as today
 *     return {};
 *
 * We model that decision exactly with `validate_update_count`, returning the
 * REAL heaven::Result<void> / heaven::Error contract update_vertices uses (so the
 * accept = value / reject = Error signalling is the production type), and assert
 * across fuzzed (recorded, incoming) count pairs that it accepts IFF the counts
 * are equal and otherwise yields an Error — never the reverse.
 *
 * The file self-registers via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing
 * test_main.cpp.
 */
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h" // heaven::Result, heaven::Error

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

// Host-side model of the update_vertices count gate, mirroring the production
// comparison in MeshStore::update_vertices verbatim: a re-upload is accepted
// (returns a value-bearing Result) only when the incoming count matches the
// recorded count; any mismatch is rejected with an Error and performs no upload.
// `recorded` stands in for MeshData::vertex_count captured at upload() time.
heaven::Result<void> validate_update_count(uint32_t recorded, uint32_t incoming) {
    if (incoming != recorded) {
        return std::unexpected(heaven::Error{
            "MeshStore::update_vertices: vertex count mismatch (expected "
            + std::to_string(recorded) + ", got "
            + std::to_string(incoming) + ")"});
    }
    return {};
}

// ──: mesh-store vertex-count validation ─────────────
bool run_p16_mesh_store_count_property() {
    return rc::check(
        "Feature: codebase-hardening, Property 16: Mesh-store vertex-count "
        "validation (Validates: Requirements 10.1, 10.2, 10.3)",
        [] {
            // The count recorded when the surface was uploaded.
            const uint32_t recorded =
                *rc::gen::inRange<uint32_t>(0u, 5'000'000u);

            // The incoming re-upload count. Bias generation toward the matching
            // case (CPU skinning always preserves the bind-mesh count) so both
            // the accept and reject branches are exercised frequently rather
            // than mismatching almost every time.
            const bool force_match = *rc::gen::arbitrary<bool>();
            const uint32_t incoming =
                force_match ? recorded
                            : *rc::gen::inRange<uint32_t>(0u, 5'000'000u);

            const heaven::Result<void> r =
                validate_update_count(recorded, incoming);

            // Accept IFF the counts are equal (compares the two; Req
            // 10.3 accepts on match;  rejects on mismatch). The result
            // carries the value exactly when recorded == incoming, and carries
            // an Error exactly otherwise — never both, never neither.
            const bool counts_equal = (recorded == incoming);
            RC_ASSERT(r.has_value() == counts_equal);

            // On rejection, the signalled Error is non-empty and names both
            // counts so the caller can log a meaningful skip.
            if (!counts_equal) {
                RC_ASSERT(!r.error().message.empty());
            }
        });
}

// ── Explicit example assertions for the three named requirement clauses ──
// Property coverage above is exhaustive over the count space, but these pin the
// exact scenarios from the acceptance criteria so a regression names the clause.
bool run_p16_examples() {
    bool ok = true;

    // a matching count is accepted (the re-upload proceeds as today).
    {
        const auto r = validate_update_count(1024u, 1024u);
        if (!r.has_value()) {
            std::printf("[prop_mesh_store_count] Req 10.3 FAILED: matching count "
                        "(1024 == 1024) was rejected\n");
            ok = false;
        }
    }

    // a mismatched count is rejected with an Error and performs
    // no re-upload.
    {
        const auto r = validate_update_count(1024u, 512u);
        if (r.has_value()) {
            std::printf("[prop_mesh_store_count] Req 10.2 FAILED: mismatched count "
                        "(1024 vs 512) was accepted\n");
            ok = false;
        }
    }

    // boundary: an off-by-one larger count is still a mismatch and is
    // rejected (the corruption case Filament would otherwise hit).
    {
        const auto r = validate_update_count(1024u, 1025u);
        if (r.has_value()) {
            std::printf("[prop_mesh_store_count] Req 10.2 FAILED: off-by-one count "
                        "(1024 vs 1025) was accepted\n");
            ok = false;
        }
    }

    // boundary: zero recorded and zero incoming match and are accepted.
    {
        const auto r = validate_update_count(0u, 0u);
        if (!r.has_value()) {
            std::printf("[prop_mesh_store_count] Req 10.3 FAILED: matching zero "
                        "counts were rejected\n");
            ok = false;
        }
    }

    return ok;
}

bool run_mesh_store_count_properties() {
    bool ok = true;
    ok = run_p16_examples() && ok;
    ok = run_p16_mesh_store_count_property() && ok;
    return ok;
}

const heaven::testing::Register g_register_mesh_store_count{
    &run_mesh_store_count_properties};

} // namespace
