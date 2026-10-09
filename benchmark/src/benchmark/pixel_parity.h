/*
 * HeavenBench — Shipping_Look pixel-parity comparison (pure, GPU-free)
 *
 * WHEN all new Render_Features
 * introduced by these changes are disabled, THE Renderer SHALL produce rendered
 * output for each camera position on the benchmark path that matches the
 * Shipping_Look such that "no colour channel of any pixel differs by more than
 * 1 of 255 levels".
 *
 * The genuine parity bar is a GPU pixel capture: with `--no-rain
 * --no-wet-reflections` and rain intensity 0, frames captured at sampled
 * benchmark-path camera positions are compared against a pre-recorded
 * Shipping_Look reference image set. That capture + reference comparison needs a
 * real GPU + display + the full Heaven scene + the reference set, so it runs in
 * the HEAVEN_SMOKE-gated harness (and the production --benchmark path), NOT in
 * headless CI.
 *
 * What CAN run headlessly — and what this helper isolates — is the pure RGBA8
 * comparison the parity bar is defined over: the maximum per-channel absolute
 * difference across every pixel, and the "within 1 of 255" tolerance test. By
 * keeping that math in a GPU-free helper (compiled into BOTH the renderer/smoke
 * targets via the src/benchmark glob AND the headless heaven_benchmark_tests
 * target), the comparison logic the on-hardware bar relies on is locked down by
 * a deterministic unit test without needing a GPU. Performance measurements
 * similarly separate GPU rendering from the headless windowing helper
 * (measure_perf_window in stats.{h,cpp}) plus a gated full-run path.
 *
 * Pure and free of any Filament / SDL / GPU dependency.
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_PIXEL_PARITY_H_
#define HEAVENBENCH_SRC_BENCHMARK_PIXEL_PARITY_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace heaven {

// The Shipping_Look parity tolerance, single-sourced from: "no colour
// channel of any pixel differs... by more than 1 of 255 levels". A captured
// frame matches the reference only when the maximum per-channel absolute
// difference over every RGBA8 pixel is at most this many 0..255 levels. Pinning
// it as a named constant (rather than a scattered literal) keeps the bar
// traceable to the requirement and prevents it being silently widened.
inline constexpr int kShippingLookMaxChannelDelta = 1;

// Result of comparing two RGBA8 image buffers.
struct PixelDiff {
    // True only when both buffers have the expected size (width*height*4 bytes)
    // AND are the same length as each other. A size mismatch is a FAILURE: the
    // two images are not comparable, so callers must treat size_match==false as
    // "not within tolerance" rather than a pass. When false, the delta/pixel
    // fields below are left at 0 because no per-pixel comparison was performed.
    bool        size_match        = false;
    // The maximum per-channel absolute difference (in 0..255 levels) found over
    // every channel of every pixel. 0 means the buffers are bit-identical.
    int         max_channel_delta = 0;
    // How many pixels differ in at least one channel (diagnostic only).
    std::size_t differing_pixels  = 0;
};

// Compare two RGBA8 image buffers of `width`×`height` pixels (4 bytes/pixel, in
// R,G,B,A order) and return the maximum per-channel absolute difference over all
// pixels. `size_match` is false (and the comparison is skipped) unless both
// spans are exactly `width*height*4` bytes and the same length — a size
// mismatch is a parity FAILURE, never silently ignored. Pure: no GPU/Filament
// dependency, so the comparison the Shipping_Look bar relies on is exercised
// headlessly.
[[nodiscard]] PixelDiff compare_rgba8(std::span<const std::uint8_t> a,
                                      std::span<const std::uint8_t> b,
                                      std::size_t width, std::size_t height);

// Convenience pointer overload for callers that hold raw frame buffers (e.g. a
// Filament readPixels destination). `a_len` / `b_len` are the buffers' byte
// lengths so the size-mismatch check  still holds. Forwards to the span
// overload.
[[nodiscard]] PixelDiff compare_rgba8(const std::uint8_t* a, std::size_t a_len,
                                      const std::uint8_t* b, std::size_t b_len,
                                      std::size_t width, std::size_t height);

// True when a captured frame matches the Shipping_Look reference within the
// tolerance: the buffers were comparable (size_match) AND no colour channel
// of any pixel differs by more than kShippingLookMaxChannelDelta (1) of 255
// levels. A size mismatch therefore fails the tolerance, as required.
[[nodiscard]] bool within_shipping_look_tolerance(const PixelDiff& diff);

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_PIXEL_PARITY_H_
