/*
 * HeavenBench — unit test: shared loader-validation helpers
 *
 * Feature: codebase-hardening
 * — Validates:
 *
 * The helpers in engine/loader_validation.h (heaven::loadval) are the single
 * source of truth for the untrusted-binary-loader checks shared across mi08 /
 * ms08 / xml0 / DDS / the tessellation cache. This file pins their exact
 * behaviour at the boundaries the loaders rely on:
 *
 *   • read_pod / read_array: a short std::fread — fewer
 *     elements returned than requested — is rejected (returns false) so a
 *     truncated file can never feed uninitialised / partially-read data into a
 *     loaded mesh or texture. Exercised against real FILE* streams backed by
 *     temp files written with known byte counts (full, one-byte-short, empty),
 *     plus the zero-element no-op that must succeed even on an empty stream.
 *
 *   • in_range: the inclusive bounds predicate behaves
 *     correctly at lo, hi, lo-1, hi+1, including the canonical loader bound
 *     constants (kMaxNameLen, kMaxElemCount, kMaxSurfaces, kMaxBones) and the
 *     signed lower bound (-1) used for bone-parent validation.
 *
 *   • fits: the buffer-relative block check is
 *     computed in 64-bit so neither offset+need nor count*size can wrap. The
 *     extreme cases (offset/need at or near UINT64_MAX) are the whole point of
 *     the helper — a naive offset+need <= size would wrap and wrongly accept an
 *     out-of-bounds block; fits must reject them.
 *
 * loadval is header-only and GPU-free, so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "engine/loader_validation.h"
#include "stdio_compat.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using namespace heaven::loadval;

// ── Temp-stream scaffolding ────────────────────────────────────

// Write `bytes` to a fresh temporary stream and rewind it for reading.
// The helper returns a binary stream that is removed automatically when closed
// (or at process exit), so there is no path to clean up. The caller closes the
// FILE* via the destructor.
struct TempStream {
    std::FILE* f = nullptr;

    explicit TempStream(const std::vector<std::uint8_t>& bytes) {
        f = heaven::test::temporary_file();
        if (f && ((!bytes.empty() &&
                   std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) ||
                  std::fseek(f, 0, SEEK_SET) != 0)) {
            std::fclose(f);
            f = nullptr;
        }
    }

    ~TempStream() {
        if (f) std::fclose(f);
    }
};

bool unit_loader_validation() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_loader_validation] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    // ── 1. read_pod: full read succeeds and yields the written value ──

    {
        const std::uint32_t written = 0xDEADBEEFu;
        std::vector<std::uint8_t> bytes(sizeof(written));
        std::memcpy(bytes.data(), &written, sizeof(written));

        TempStream ts(bytes);
        if (!ts.f) { fail("read_pod full: tmpfile() returned null"); return ok; }
        std::uint32_t out = 0;
        if (!read_pod(ts.f, out))
            fail("read_pod full: expected true on a complete read");
        if (out != written)
            fail("read_pod full: value mismatch after read");
    }

    // ── 2. read_pod: short read (fewer bytes than sizeof(T)) is rejected ──
    // — one byte short of a uint32_t.
    {
        std::vector<std::uint8_t> bytes = {0x01, 0x02, 0x03}; // 3 of 4 bytes
        TempStream ts(bytes);
        if (!ts.f) { fail("read_pod short: tmpfile() returned null"); return ok; }
        std::uint32_t out = 0;
        if (read_pod(ts.f, out))
            fail("read_pod short: expected false on a truncated read");
    }

    // ── 3. read_pod: empty stream is rejected ──

    {
        TempStream ts({});
        if (!ts.f) { fail("read_pod empty: tmpfile() returned null"); return ok; }
        std::uint16_t out = 0;
        if (read_pod(ts.f, out))
            fail("read_pod empty: expected false on an empty stream");
    }

    // ── 4. read_array: full read of n elements succeeds with correct data ──

    {
        const std::vector<std::uint32_t> src = {1u, 2u, 3u, 4u};
        std::vector<std::uint8_t> bytes(src.size() * sizeof(std::uint32_t));
        std::memcpy(bytes.data(), src.data(), bytes.size());

        TempStream ts(bytes);
        if (!ts.f) { fail("read_array full: tmpfile() returned null"); return ok; }
        std::vector<std::uint32_t> dst(src.size(), 0u);
        if (!read_array(ts.f, dst.data(), dst.size()))
            fail("read_array full: expected true reading all elements");
        if (dst != src)
            fail("read_array full: element data mismatch after read");
    }

    // ── 5. read_array: short read (n-1 elements present) is rejected ──
    // — buffer holds 3 uint32_t but we request 4.
    {
        const std::vector<std::uint32_t> src = {10u, 20u, 30u};
        std::vector<std::uint8_t> bytes(src.size() * sizeof(std::uint32_t));
        std::memcpy(bytes.data(), src.data(), bytes.size());

        TempStream ts(bytes);
        if (!ts.f) { fail("read_array short: tmpfile() returned null"); return ok; }
        std::vector<std::uint32_t> dst(4, 0u); // request one more than present
        if (read_array(ts.f, dst.data(), dst.size()))
            fail("read_array short: expected false on a truncated read");
    }

    // ── 6. read_array: requesting zero elements is a no-op success ──
    // — even on an empty stream, n==0 must succeed.
    {
        TempStream ts({});
        if (!ts.f) { fail("read_array zero: tmpfile() returned null"); return ok; }
        // Pass a null destination to make the no-op contract explicit: with
        // n==0 the helper must not dereference dst.
        if (!read_array<std::uint32_t>(ts.f, nullptr, 0))
            fail("read_array zero: expected true for a zero-element read");
    }

    // ── 7. in_range: inclusive boundaries at lo, hi, lo-1, hi+1 ──

    {
        constexpr int lo = 0;
        constexpr int hi = 100;
        if (!in_range(lo, lo, hi))      fail("in_range: lo should be inside");
        if (!in_range(hi, lo, hi))      fail("in_range: hi should be inside");
        if (!in_range(50, lo, hi))      fail("in_range: midpoint should be inside");
        if (in_range(lo - 1, lo, hi))   fail("in_range: lo-1 should be outside");
        if (in_range(hi + 1, lo, hi))   fail("in_range: hi+1 should be outside");
    }

    // ── 8. in_range: canonical loader bounds at their edges ──
    // — mirrors how the mi08/ms08 loaders validate counts.
    {
        // Name length: valid 1..kMaxNameLen.
        if (in_range(0, 1, kMaxNameLen))                 fail("in_range: name len 0 must be rejected");
        if (!in_range(1, 1, kMaxNameLen))                fail("in_range: name len 1 must be accepted");
        if (!in_range(kMaxNameLen, 1, kMaxNameLen))      fail("in_range: name len == cap must be accepted");
        if (in_range(kMaxNameLen + 1, 1, kMaxNameLen))   fail("in_range: name len > cap must be rejected");

        // Element counts: valid 0..kMaxElemCount.
        if (!in_range(0, 0, kMaxElemCount))              fail("in_range: elem count 0 must be accepted");
        if (in_range(-1, 0, kMaxElemCount))              fail("in_range: negative elem count must be rejected");
        if (!in_range(kMaxElemCount, 0, kMaxElemCount))  fail("in_range: elem count == cap must be accepted");
        if (in_range(kMaxElemCount + 1, 0, kMaxElemCount)) fail("in_range: elem count > cap must be rejected");

        // Surface count: valid 1..kMaxSurfaces.
        if (in_range(0, 1, kMaxSurfaces))                fail("in_range: surface count 0 must be rejected");
        if (!in_range(kMaxSurfaces, 1, kMaxSurfaces))    fail("in_range: surface count == cap must be accepted");
        if (in_range(kMaxSurfaces + 1, 1, kMaxSurfaces)) fail("in_range: surface count > cap must be rejected");

        // Bone parent index: valid [-1, num_bones) — signed lower bound.
        const int num_bones = 32;
        if (!in_range(-1, -1, num_bones - 1))            fail("in_range: parent -1 (root) must be accepted");
        if (!in_range(num_bones - 1, -1, num_bones - 1)) fail("in_range: parent num_bones-1 must be accepted");
        if (in_range(-2, -1, num_bones - 1))             fail("in_range: parent -2 must be rejected");
        if (in_range(num_bones, -1, num_bones - 1))      fail("in_range: parent num_bones must be rejected");

        // Bone count cap.
        if (!in_range(kMaxBones, 0, kMaxBones))          fail("in_range: bone count == cap must be accepted");
        if (in_range(kMaxBones + 1, 0, kMaxBones))       fail("in_range: bone count > cap must be rejected");
    }

    // ── 9. fits: ordinary in-bounds / out-of-bounds blocks ──

    {
        if (!fits(0, 0, 0))     fail("fits: empty block in empty buffer should fit");
        if (!fits(0, 10, 10))   fail("fits: whole buffer should fit");
        if (!fits(10, 0, 10))   fail("fits: zero-length block at end (offset==size) should fit");
        if (!fits(4, 6, 10))    fail("fits: [4,10) in 10 should fit");
        if (fits(11, 0, 10))    fail("fits: offset past end must not fit");
        if (fits(0, 11, 10))    fail("fits: need > size must not fit");
        if (fits(5, 6, 10))     fail("fits: [5,11) overruns 10 and must not fit");
    }

    // ── 10. fits: 64-bit extremes do not wrap (the helper's reason to exist) ──

    {
        constexpr std::uint64_t kMax = UINT64_MAX;

        // offset alone past a small buffer: rejected without computing offset+need.
        if (fits(kMax, kMax, 10))
            fail("fits: (UINT64_MAX, UINT64_MAX, 10) must be rejected");

        // Huge need against a small buffer: size - offset must not underflow.
        if (fits(10, kMax, 20))
            fail("fits: huge need against small buffer must be rejected");

        // The wrap trap: a naive (offset + need <= size) wraps to a tiny value
        // and would WRONGLY accept. fits must reject.
        if (fits(kMax - 5, 10, kMax))
            fail("fits: offset+need wrap case must be rejected, not accepted");

        // Exact fit at the very top of the 64-bit range must be accepted.
        if (!fits(kMax - 10, 10, kMax))
            fail("fits: exact block at the top of the range should fit");

        // One byte too many at the top must be rejected.
        if (fits(kMax - 10, 11, kMax))
            fail("fits: one byte past the top of the range must not fit");

        // Whole maximal buffer fits.
        if (!fits(0, kMax, kMax))
            fail("fits: whole maximal buffer should fit");
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_loader_validation{
    &unit_loader_validation};

} // namespace
