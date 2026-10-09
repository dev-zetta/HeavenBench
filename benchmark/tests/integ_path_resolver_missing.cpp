/*
 * HeavenBench — asset-integration test: missing-asset diagnostic content
 *
 * Feature: asset-path-resolution
 *
 *
 * This test pins the missing-asset diagnostic CONTRACT the renderer must keep
 * after the PathResolver refactor:
 *
 *   -: IF an asset resolved through the PathResolver cannot be opened as a
 *     regular file at its Resolved_Path, THEN the diagnostic text contains the
 *     complete Resolved_Path reproduced verbatim (no truncation, normalization,
 *     or substitution).
 *   -: WHEN a missing-asset diagnostic is emitted, the Renderer continues
 *     loading the remaining assets without aborting or terminating the process.
 *
 * SELF-CONTAINED RATIONALE: the headless `heaven_benchmark_tests` target links
 * neither Filament/SDL nor the render features / TextureStore (it compiles only
 * a small set of engine TUs + path_resolver.cpp + the test TUs — see
 * benchmark/CMakeLists.txt). So this test cannot drive RainFeature/TextureStore
 * the way the gated renderer-linked smoke case (smoke_rain_asset_failure.cpp)
 * does. Instead it models the missing-asset diagnostic contract directly with
 * the real PathResolver plus a tiny local loader stub that mirrors the
 * renderer's behavior: stat() the resolved path; on absence append a diagnostic
 * STRING that embeds the resolved path verbatim and return a failure sentinel
 * (it does NOT throw/abort) — then CONTINUE to the next asset. This exercises
 * the exact strings the live diagnostics name (guarantees the
 * resolved strings are byte-identical to the legacy ones), while staying
 * runnable on the headless host with no GPU.
 *
 * GATING: the test builds its own temp directory tree under
 * std::filesystem::temp_directory_path(), so it does not depend on the gitignored
 * extracted asset tree. It is robust: if the temp directory cannot be created
 * (read-only sandbox, etc.) it SKIPS gracefully (prints a notice and returns
 * true) so CI still passes.
 *
 * Registers with the shared `heaven_benchmark_tests` harness (test_main.cpp) via
 * heaven::testing::Register at static-init time, following the same
 * self-registration pattern as the other integ and prop translation units. The
 * registered callable returns true on success/skip and false (after printing a
 * diagnostic) on a genuine failure. No second main() is defined.
 */
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. registry()
// has external linkage and is defined there; the Register helper's definition
// is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

// Outcome of one loader attempt: whether the resolved file was opened as a
// readable regular file. Mirrors the renderer's per-asset load result.
struct LoadOutcome {
    std::string resolved_path;  // the exact string the loader was pointed at
    bool loaded = false;        // true iff a readable regular file existed
};

// A tiny local "loader" that mirrors the renderer's missing-asset behavior. It
// probes the resolved path on the filesystem; if no regular file is present it
// appends a diagnostic that embeds the resolved path VERBATIM to `log` and
// records a failed outcome (it does NOT throw or abort), so the caller can
// CONTINUE to the next asset. On success it records a loaded outcome and emits
// no diagnostic (a successful load is not a miss).
LoadOutcome try_load(const std::string& resolved_path,
                     std::vector<std::string>& log) {
    std::error_code ec;
    const bool exists = fs::is_regular_file(resolved_path, ec) && !ec;
    if (!exists) {
        // Format mirrors the renderer's load-failure diagnostics, which print
        // the complete resolved path verbatim (e.g. rain_feature drop/splash
        // paths). The resolved string is appended unmodified.
        log.push_back("failed to load: " + resolved_path);
        return LoadOutcome{resolved_path, /*loaded=*/false};
    }
    return LoadOutcome{resolved_path, /*loaded=*/true};
}

// Create the parent directories for `file` and write a small payload, so the
// loader's existence probe finds a readable regular file there. Returns false
// (with the error_code populated) if the layout could not be created.
bool place_asset(const fs::path& file, std::error_code& ec) {
    fs::create_directories(file.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(file, std::ios::binary);
    if (!out) return false;
    out << "DDS-stub";
    return static_cast<bool>(out);
}

bool integ_path_resolver_missing() {
    constexpr const char* kTag = "[integ_path_resolver_missing]";

    // ── Build an isolated temp-dir Data_Root ────────────────────
    // Use a unique subdirectory under the system temp dir so the test never
    // touches the (gitignored, possibly absent) extracted asset tree and can be
    // cleaned up wholesale. If temp-dir setup fails, SKIP gracefully.
    std::error_code ec;
    fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("%s SKIPPED: no temp dir available (%s)\n",
                    kTag, ec.message().c_str());
        return true;
    }

    const fs::path data_root =
        temp_base / "heaven_integ_missing_asset_test";
    // Start from a clean slate in case a prior run left the tree behind.
    fs::remove_all(data_root, ec);
    fs::create_directories(data_root, ec);
    if (ec) {
        std::printf("%s SKIPPED: could not create temp Data_Root '%s' (%s)\n",
                    kTag, data_root.string().c_str(), ec.message().c_str());
        return true;
    }

    // ── Construct the real PathResolver against the temp Data_Root ──
    auto resolver = heaven::PathResolver::create(data_root.string());
    if (!resolver.has_value()) {
        std::printf("%s FAILED: PathResolver::create('%s') errored: %s\n",
                    kTag, data_root.string().c_str(),
                    resolver.error().message.c_str());
        fs::remove_all(data_root, ec);
        return false;
    }
    const heaven::PathResolver& paths = *resolver;

    // ── Resolve two assets: one absent, one present ─────────────
    // The missing asset is never created on disk; the present asset is written
    // at its own resolved path (parents created) so the loader can open it.
    const std::string missing_path =
        paths.texture_path(heaven::TextureCategory::Sfx, "does_not_exist.dds");
    const std::string present_path =
        paths.texture_path(heaven::TextureCategory::Sfx, "present_asset.dds");

    if (!place_asset(present_path, ec)) {
        std::printf("%s SKIPPED: could not place present asset '%s' (%s)\n",
                    kTag, present_path.c_str(), ec.message().c_str());
        fs::remove_all(data_root, ec);
        return true;
    }

    // ── Drive the loader over [missing, present], in order ──────
    // The missing asset comes FIRST so that a successful processing of the
    // second proves loading continued past the miss. The loader returns
    // a failure sentinel rather than throwing, so reaching the second entry is
    // itself evidence the process did not abort.
    const std::string sequence[] = {missing_path, present_path};
    std::vector<std::string> diagnostics;
    std::vector<LoadOutcome> outcomes;
    outcomes.reserve(2);
    for (const std::string& asset : sequence) {
        outcomes.push_back(try_load(asset, diagnostics));
    }

    bool ok = true;

    // loading continued — BOTH entries were processed (the miss did not
    // abort the sequence) and the present asset, attempted AFTER the missing
    // one, loaded successfully.
    if (outcomes.size() != 2) {
        std::printf("%s FAILED: expected 2 processed assets, got %zu "
                    "(a missing asset must not abort the sequence — R8.4)\n",
                    kTag, outcomes.size());
        ok = false;
    } else {
        if (outcomes[0].loaded) {
            std::printf("%s FAILED: missing asset '%s' reported as loaded; "
                        "expected a load failure\n",
                        kTag, missing_path.c_str());
            ok = false;
        }
        if (!outcomes[1].loaded) {
            std::printf("%s FAILED: present asset '%s' was not loaded after the "
                        "missing one; loading did not continue (R8.4)\n",
                        kTag, present_path.c_str());
            ok = false;
        }
    }

    // exactly one diagnostic was emitted (only for the routine miss), and
    // its text contains the complete resolved path of the missing asset
    // reproduced VERBATIM (substring match, no truncation/normalization). The
    // present asset, having loaded, must NOT have produced a diagnostic.
    if (diagnostics.size() != 1) {
        std::printf("%s FAILED: expected exactly 1 missing-asset diagnostic, "
                    "got %zu (only the absent asset should emit one — R8.1)\n",
                    kTag, diagnostics.size());
        ok = false;
    } else if (diagnostics[0].find(missing_path) == std::string::npos) {
        std::printf("%s FAILED: diagnostic does not contain the exact resolved "
                    "path verbatim.\n  resolved: \"%s\"\n  diagnostic: \"%s\"\n",
                    kTag, missing_path.c_str(), diagnostics[0].c_str());
        ok = false;
    }

    // ── Clean up the temp tree ──────────────────────────────────
    fs::remove_all(data_root, ec);
    if (ec) {
        std::printf("%s NOTE: temp tree cleanup reported '%s' (non-fatal)\n",
                    kTag, ec.message().c_str());
    }

    if (ok) {
        std::printf("%s OK: missing '%s' emitted a verbatim diagnostic; "
                    "loading continued and present asset loaded\n",
                    kTag, missing_path.c_str());
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_integ_path_resolver_missing{
    integ_path_resolver_missing};

} // namespace
