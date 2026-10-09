// Feature: background-music-playback,: Looping playback is gap-free and the cursor stays in range
/*
 * HeavenBench — Property test for gap-free music looping
 *
 * Feature: background-music-playback,: Looping playback is gap-free
 * and the cursor stays in range
 *
 *
 * fill_block(pcm, cursor, volume, out) (engine/types.h) is the pure core of
 * the SDL audio callback: it copies interleaved samples from the immutable
 * decoded track starting at `cursor`, scales each by `volume`, and wraps the
 * read position back to 0 at end-of-track — all within a single call, so the
 * sample following the last is the first with NO zero-sample inserted at the
 * wrap point. This is what makes looping audibly seamless.
 *
 * This property threads ONE cursor variable across a sequence of fill requests
 * (exactly as the real callback is invoked repeatedly with whatever buffer
 * sizes SDL hands it) and pins volume at 1.0 so the looping behaviour is
 * isolated from amplitude scaling (covers volume separately). With
 * the total fill length forced to meet or exceed the buffer length, at least
 * one wrap is guaranteed, so the no-gap-at-the-seam guarantee is actually
 * exercised.
 *
 * Two things are asserted, both against expectations derived INDEPENDENTLY of
 * the function under test (cyclic indexing, not a second call to fill_block):
 *
 *   1. The concatenation of every output block equals the buffer repeated
 *      end-to-end from the initial cursor: concat[i] == pcm[(start + i) % n].
 *      If any zero-gap were inserted at a wrap point the indices would slip and
 *      this comparison would fail.
 *
 *   2. After EVERY individual fill_block call the cursor is left in the
 *      half-open range [0, n) — never == n, never past the end.
 *
 * The buffer is non-empty (the empty-pcm zero-fill path is a separate contract
 * branch, not the looping one). Block sizes may be zero (a no-op fill that must
 * still leave the cursor in range) and the final block is sized to guarantee
 * the total reaches at least n so a wrap always occurs.
 *
 * The logic is pure (engine/types.h is header-only, no GPU/SDL/decoder deps),
 * so it runs on the host. This TU self-registers with the RapidCheck harness
 * in test_main.cpp via the heaven::testing::Register pattern and is picked up
 * by the file(GLOB) in benchmark/CMakeLists.txt — no entry-point edit.
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h"

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

bool run_music_fill_loop_property() {
    // RapidCheck's default is 100 successful iterations (maxSuccess = 100),
    // which meets the "≥ 100 iterations" requirement; raise via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: background-music-playback, Property 4: Looping playback is "
        "gap-free and the cursor stays in range (Validates: Requirements 1.2)",
        [] {
            // A non-empty decoded track of interleaved 16-bit samples. The
            // empty-buffer zero-fill path is a different contract branch, so it
            // is deliberately excluded here.
            const auto pcm = *rc::gen::nonEmpty(
                rc::gen::container<std::vector<int16_t>>(
                    rc::gen::arbitrary<int16_t>()));
            const std::size_t n = pcm.size();

            // An arbitrary starting read position inside the track. fill_block
            // takes the entry cursor modulo n, so the loop output is the track
            // rotated to begin at `start`.
            const std::size_t start =
                static_cast<std::size_t>(*rc::gen::inRange<std::size_t>(0, n));

            // A sequence of fill-request sizes, each possibly zero (a no-op
            // fill that must still leave the cursor in range). Block sizes are
            // bounded relative to n so several wraps can occur within one run.
            auto blocks = *rc::gen::container<std::vector<std::size_t>>(
                rc::gen::inRange<std::size_t>(0, 3 * n + 1));

            // Guarantee the total fill length meets or exceeds the buffer
            // length so at least one wrap is exercised every iteration: append
            // a final block sized to cover whatever the random blocks left
            // short of n (and at minimum one full n when they already reached
            // it, so a seam is always crossed).
            std::size_t planned = 0;
            for (const std::size_t b : blocks) {
                planned += b;
            }
            blocks.push_back(planned < n ? (n - planned) : n);

            // Thread a single cursor across every call, exactly as the SDL
            // callback is driven, and concatenate all emitted samples.
            std::size_t cursor = start;
            std::vector<int16_t> concat;
            for (const std::size_t b : blocks) {
                std::vector<int16_t> out(b, static_cast<int16_t>(0));
                heaven::fill_block(pcm, cursor, 1.0f, out);

                // The cursor is left in the half-open range [0, n) after EVERY
                // individual fill — never == n, never past the end (size_t is
                // unsigned, so the lower bound is structural).
                RC_ASSERT(cursor < n);

                concat.insert(concat.end(), out.begin(), out.end());
            }

            // The construction forces at least one full loop's worth of output.
            RC_ASSERT(concat.size() >= n);

            // Gap-free loop: the concatenated stream is the track repeated
            // end-to-end from `start`, with no zero-sample wedged at any wrap
            // point. The expectation is the cyclic index (start + i) % n,
            // computed without calling fill_block, so the test cannot mirror a
            // bug in the function under test. Volume is 1.0, so each emitted
            // sample equals its source sample unchanged.
            for (std::size_t i = 0; i < concat.size(); ++i) {
                const std::int16_t expected = pcm[(start + i) % n];
                RC_ASSERT(concat[i] == expected);
            }
        });
}

const heaven::testing::Register g_register_music_fill_loop{
    &run_music_fill_loop_property};

} // namespace
