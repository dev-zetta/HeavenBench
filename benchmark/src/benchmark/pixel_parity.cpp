/*
 * HeavenBench — Shipping_Look pixel-parity comparison (pure, GPU-free)
 *
 * Implements the RGBA8 max-per-channel-difference comparison the Shipping_Look
 * parity bar  is defined over. See pixel_parity.h for the rationale and
 * the headless/GPU split.
 */
#include "benchmark/pixel_parity.h"

#include <algorithm>
#include <cstdlib>

namespace heaven {

PixelDiff compare_rgba8(std::span<const std::uint8_t> a,
                        std::span<const std::uint8_t> b,
                        std::size_t width, std::size_t height) {
    PixelDiff out;

    // The two buffers must each hold exactly width*height*4 RGBA8 bytes and be
    // the same length. A mismatch means the images are not comparable, which
    // treats as a FAILURE — leave size_match false and report no delta so
    // callers (via within_shipping_look_tolerance) reject it rather than reading
    // past either buffer.
    const std::size_t expected = width * height * 4;
    if (a.size() != expected || b.size() != expected) {
        return out;
    }
    out.size_match = true;

    // Walk every channel of every pixel, tracking the maximum absolute
    // difference (0..255) and how many pixels differ in at least one channel.
    int max_delta = 0;
    std::size_t differing = 0;
    for (std::size_t px = 0; px < width * height; ++px) {
        const std::size_t base = px * 4;
        bool pixel_differs = false;
        for (std::size_t c = 0; c < 4; ++c) {
            const int delta = std::abs(static_cast<int>(a[base + c]) -
                                       static_cast<int>(b[base + c]));
            max_delta = std::max(delta, max_delta);
            if (delta != 0) pixel_differs = true;
        }
        if (pixel_differs) ++differing;
    }

    out.max_channel_delta = max_delta;
    out.differing_pixels  = differing;
    return out;
}

PixelDiff compare_rgba8(const std::uint8_t* a, std::size_t a_len,
                        const std::uint8_t* b, std::size_t b_len,
                        std::size_t width, std::size_t height) {
    return compare_rgba8(std::span<const std::uint8_t>(a, a_len),
                         std::span<const std::uint8_t>(b, b_len),
                         width, height);
}

bool within_shipping_look_tolerance(const PixelDiff& diff) {
    // A size mismatch is never within tolerance (the images were not
    // comparable). Otherwise the captured frame matches the Shipping_Look only
    // when no channel of any pixel drifts past the bound of 1 of 255.
    return diff.size_match &&
           diff.max_channel_delta <= kShippingLookMaxChannelDelta;
}

} // namespace heaven
