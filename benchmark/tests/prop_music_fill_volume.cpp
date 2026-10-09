// Feature: background-music-playback,: Volume scales amplitude linearly, with silence at zero and identity at one
/*
 * HeavenBench — Property test for the music fill volume scaling
 *
 * Feature: background-music-playback,: Volume scales amplitude
 * linearly, with silence at zero and identity at one
 *
 *
 * fill_block(pcm, cursor, volume, out) is the pure, SDL-free core of the audio
 * callback (engine/types.h). It operates at the SAMPLE level: `cursor` is an
 * index into `pcm` in samples, reduced `% pcm.size()` on entry and left in
 * [0, pcm.size()) on exit. For each of `out.size()` output samples it writes
 *
 *     out[i] = clamp(lround(float(pcm[(start + i) % n]) * volume), -32768, 32767)
 *
 * advancing a wrapping read position so looping is gap-free, then stores the
 * final position back into `cursor`.
 *
 * This property exercises the amplitude-scaling contract: the
 * Music_Player outputs the track at an amplitude linearly proportional to the
 * Music_Volume, with volume 1.0 reproducing the unmodified track and volume
 * 0.0 producing silence. For arbitrary non-empty PCM buffers, an arbitrary
 * starting cursor, an arbitrary output length (including lengths that wrap the
 * buffer several times), and an arbitrary volume v in [0, 1], the test asserts:
 *
 *   • Linear scaling — every output sample equals round(input * v) clamped to
 *     the int16 range, where the input is the cyclically-indexed source sample.
 *     The expectation is computed independently of fill_block (the same closed
 *     form, re-derived from `pcm`, `start`, and `v`), never by calling
 *     fill_block to produce its own oracle.
 *   • Silence at zero — when v == 0.0 every output sample is exactly 0.
 *   • Identity at one — when v == 1.0 every output sample equals the
 *     (cyclically indexed) source sample unchanged: lround(s * 1.0f) == s for
 *     every int16 s (all are exactly representable as float and round-trip),
 *     and the clamp is a no-op since s is already in range.
 *   • The cursor is left in the half-open range [0, n) after the fill.
 *
 * The volume generator deliberately blends the two boundary values (0.0 and
 * with a dense interior grid over [0, 1] so the silence and identity
 * branches are reached on most runs while the general linear law is checked on
 * every run.
 *
 * The logic is pure (engine/types.h is header-only, no GPU/SDL/Filament deps),
 * so it runs on the host. This translation unit self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point; no second main() is defined.
 */
#include <algorithm>
#include <cmath>
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

// The int16 saturation bounds fill_block clamps each scaled sample into.
constexpr long k_int16_min = -32768;
constexpr long k_int16_max = 32767;

// Independent oracle for one output sample: the closed-form contract of
// fill_block, re-derived from the source rather than by calling fill_block.
int16_t expected_sample(int16_t in, float volume) {
    const long scaled = std::lround(static_cast<float>(in) * volume);
    const long bounded = std::clamp<long>(scaled, k_int16_min, k_int16_max);
    return static_cast<int16_t>(bounded);
}

// A volume in [0, 1], blending the two boundary values with a dense interior
// grid so the silence (0.0) and identity (1.0) branches are exercised often.
rc::Gen<float> volume_gen() {
    return rc::gen::weightedOneOf<float>({
        {1, rc::gen::just(0.0f)},
        {1, rc::gen::just(1.0f)},
        {3, rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                         [](int g) { return static_cast<float>(g) / 1'000'000.0f; })},
    });
}

bool run_music_fill_volume_property() {
    // RapidCheck's default is 100 successful iterations (maxSuccess = 100),
    // which meets the "≥ 100 iterations" requirement; raise via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: background-music-playback, Property 2: Volume scales amplitude "
        "linearly, with silence at zero and identity at one "
        "(Validates: Requirements 3.3)",
        [] {
            // An arbitrary non-empty interleaved PCM buffer of int16 samples.
            const std::vector<int16_t> pcm = *rc::gen::nonEmpty(
                rc::gen::container<std::vector<int16_t>>(
                    rc::gen::arbitrary<int16_t>()));
            const std::size_t n = pcm.size();

            // Arbitrary starting cursor; fill_block reduces it `% n` on entry,
            // so deriving the entry position the same way keeps the oracle
            // overflow-free regardless of how large `start` is.
            const std::size_t start = *rc::gen::arbitrary<std::size_t>();
            const std::size_t pos0 = start % n;

            // An output length that may wrap the buffer several times (and may
            // be zero), so the cyclic indexing and gap-free wrap are covered.
            const std::size_t out_len =
                *rc::gen::inRange<std::size_t>(0, 4 * n + 8);

            const float v = *volume_gen();

            // Run the function under test on a fresh output buffer.
            std::size_t cursor = start;
            std::vector<int16_t> out(out_len, static_cast<int16_t>(0x7BCD));
            heaven::fill_block(pcm, cursor, v,
                               std::span<int16_t>(out.data(), out.size()));

            // Linear scaling: every output sample matches the independently
            // derived round(input * v) clamped to int16.
            for (std::size_t i = 0; i < out_len; ++i) {
                const int16_t in = pcm[(pos0 + i) % n];
                RC_ASSERT(out[i] == expected_sample(in, v));

                // Silence at zero and identity at one, stated directly against
                // the source sample so the boundary contract is explicit.
                if (v == 0.0f) {
                    RC_ASSERT(out[i] == static_cast<int16_t>(0));
                } else if (v == 1.0f) {
                    RC_ASSERT(out[i] == in);
                }
            }

            // The cursor is always left in the half-open range [0, n).
            RC_ASSERT(cursor < n);
            RC_ASSERT(cursor == (pos0 + out_len) % n);
        });
}

const heaven::testing::Register g_register_music_fill_volume{
    &run_music_fill_volume_property};

} // namespace
