/*
 * HeavenBench — headless pixel-difference helper tests.
 *
 * These synthetic RGBA8 buffers verify the one-code-value tolerance and
 * size-mismatch rejection. They do not assess full-scene visual parity.
 * The application now supports diagnostic --capture with --camera-tour-time
 * or --camera-pose, and scripts/visual_audit.py can collect real scene views.
 * No accepted full-scene reference image set is bundled or automatically
 * compared by this test, the smoke harness, or production --benchmark.
 */
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "benchmark/pixel_parity.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

constexpr const char* kTag = "[integ_shipping_look_parity]";

// A small RGBA8 image filled with a deterministic gradient so the comparison is
// exercised over varied channel values (not a single flat colour).
std::vector<std::uint8_t> make_image(std::size_t width, std::size_t height) {
    std::vector<std::uint8_t> img(width * height * 4);
    for (std::size_t px = 0; px < width * height; ++px) {
        const std::size_t base = px * 4;
        img[base + 0] = static_cast<std::uint8_t>((px * 7)  & 0xFF); // R
        img[base + 1] = static_cast<std::uint8_t>((px * 13) & 0xFF); // G
        img[base + 2] = static_cast<std::uint8_t>((px * 29) & 0xFF); // B
        img[base + 3] = 255;                                         // A
    }
    return img;
}

bool integ_shipping_look_parity() {
    bool ok = true;

    constexpr std::size_t kW = 16;
    constexpr std::size_t kH = 12;

    // ── The tolerance constant single-sources  ─────────────
    // "no colour channel of any pixel differs... by more than 1 of 255 levels"
    // ⇒ the max allowed per-channel delta is exactly 1. Pin it so the bar can
    // never be silently widened.
    if (heaven::kShippingLookMaxChannelDelta != 1) {
        std::printf("%s FAILED: kShippingLookMaxChannelDelta == %d, expected 1 "
                    "(R9.4 'more than 1 of 255 levels')\n", kTag,
                    heaven::kShippingLookMaxChannelDelta);
        ok = false;
    }

    const std::vector<std::uint8_t> reference = make_image(kW, kH);

    // ── Identical buffers ⇒ max delta 0 ⇒ within tolerance (PASS) ──
    // An exact synthetic copy must report zero drift; this says nothing
    // about the pixels produced by a particular feature configuration.
    {
        std::vector<std::uint8_t> captured = reference; // exact copy
        const heaven::PixelDiff d =
            heaven::compare_rgba8(captured, reference, kW, kH);
        if (!d.size_match || d.max_channel_delta != 0 ||
            d.differing_pixels != 0 ||
            !heaven::within_shipping_look_tolerance(d)) {
            std::printf("%s FAILED: identical buffers should give max delta 0, "
                        "0 differing pixels, and be within tolerance "
                        "(size_match=%d max_delta=%d differing=%zu within=%d)\n",
                        kTag, d.size_match, d.max_channel_delta,
                        d.differing_pixels,
                        heaven::within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    // ── A single channel off by exactly 1 ⇒ max delta 1 ⇒ within (PASS) ──
    // The bound is inclusive of 1 level, so a one-level drift in a single
    // channel of a single pixel still matches the Shipping_Look.
    {
        std::vector<std::uint8_t> captured = reference;
        // Bump the green channel of pixel 5 by exactly one level (guard against
        // the gradient already sitting at 255 for this index).
        const std::size_t idx = 5 * 4 + 1;
        captured[idx] = static_cast<std::uint8_t>(
            reference[idx] <= 254 ? reference[idx] + 1 : reference[idx] - 1);
        const heaven::PixelDiff d =
            heaven::compare_rgba8(captured, reference, kW, kH);
        if (!d.size_match || d.max_channel_delta != 1 ||
            d.differing_pixels != 1 ||
            !heaven::within_shipping_look_tolerance(d)) {
            std::printf("%s FAILED: a single channel off by 1 should give max "
                        "delta 1 over 1 pixel and stay within tolerance "
                        "(size_match=%d max_delta=%d differing=%zu within=%d)\n",
                        kTag, d.size_match, d.max_channel_delta,
                        d.differing_pixels,
                        heaven::within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    // ── A channel off by 2 ⇒ max delta 2 ⇒ exceeds tolerance (FAIL) ──
    // Two levels of drift is "more than 1 of 255" and must be rejected — this is
    // the regression the bar exists to catch.
    {
        std::vector<std::uint8_t> captured = reference;
        const std::size_t idx = 9 * 4 + 2; // blue channel of pixel 9
        captured[idx] = static_cast<std::uint8_t>(
            reference[idx] <= 253 ? reference[idx] + 2 : reference[idx] - 2);
        const heaven::PixelDiff d =
            heaven::compare_rgba8(captured, reference, kW, kH);
        if (!d.size_match || d.max_channel_delta != 2 ||
            heaven::within_shipping_look_tolerance(d)) {
            std::printf("%s FAILED: a channel off by 2 should give max delta 2 "
                        "and EXCEED the tolerance (size_match=%d max_delta=%d "
                        "within=%d — a >1-level drift must fail R9.4)\n",
                        kTag, d.size_match, d.max_channel_delta,
                        heaven::within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    // ── Size mismatch ⇒ size_match false ⇒ treated as FAIL ──────
    // A captured frame whose dimensions differ from the reference is not
    // comparable; that is a parity failure, never a silent pass.
    {
        const std::vector<std::uint8_t> smaller = make_image(kW, kH - 1);
        const heaven::PixelDiff d =
            heaven::compare_rgba8(smaller, reference, kW, kH);
        if (d.size_match || heaven::within_shipping_look_tolerance(d)) {
            std::printf("%s FAILED: a size mismatch must report size_match=false "
                        "and fail the tolerance (size_match=%d within=%d)\n",
                        kTag, d.size_match,
                        heaven::within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    if (ok) {
        std::printf("%s RGBA8 max-per-channel-delta comparison and the "
                    "1-of-255 tolerance for the R9.4 Shipping_Look bar verified "
                    "headlessly; full-scene visual parity needs separate "
                    "capture review and accepted reference images.\n", kTag);
    }
    return ok;
}

const heaven::testing::Register reg_integ_shipping_look_parity{
    integ_shipping_look_parity};

} // namespace
