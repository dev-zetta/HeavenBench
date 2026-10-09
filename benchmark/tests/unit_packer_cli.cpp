/*
 * HeavenBench — unit test: used-asset packer CLI flags.
 *
 * Feature: used-asset-packer
 * — Validates:
 *
 * This deterministic unit test pins the packer's command-line surface on the
 * PUBLIC parse driver (heaven::parse_cli) plus the trivial ParseResult →
 * PackOptions mapping the main() hook performs:
 *
 *   1. --pack-assets DIR: the activation token sets pack_requested and
 *      records DIR as pack_target, with no diagnostic emitted.
 *   2. Flag absent: with no packer flag on the command line,
 *      pack_requested stays false and pack_target stays unset, so the binary
 *      proceeds to the renderer.
 *   3. --pack-assets as the final token: pack_requested is true (the
 *      TOKEN appeared) but pack_target stays unset, and NO diagnostic is
 *      emitted — the value is optional, so main() defaults the Pack_Target to
 *      "assets".
 *   3b. --pack-assets followed by another flag (e.g. --pack-assets --data x):
 *      the following dash-token is NOT consumed as the target, so pack_target
 *      stays unset (default "assets" applies) while --data x parses normally.
 *   4. --pack-move / --pack-overwrite map to PackOptions:
 *      • default (no --pack-move): pack_move false ⇒ PackOptions.move false
 *        (Copy_Mode is the default —);
 *      • --pack-move: pack_move true ⇒ PackOptions.move true (Move_Mode —);
 *      • --pack-overwrite: pack_overwrite true ⇒ PackOptions.overwrite true
 *        (write into a non-empty target —).
 *
 * The PackOptions mapping main() performs defaults an unset Pack_Target to
 * "assets" (value_or("assets")), mirrored here in to_pack_options.
 *
 * parse_cli is pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_packer/asset_packer.h"
#include "engine/cli_parser.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::PackOptions;
using heaven::ParseResult;

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch).
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

// The trivial mapping main() performs from a parsed result to PackOptions. An
// unset Pack_Target defaults to "assets/assets.zip".
PackOptions to_pack_options(const ParseResult& r) {
    PackOptions opts;
    opts.target    = r.pack_target.value_or("assets/assets.zip");
    opts.move      = r.pack_move;
    opts.overwrite = r.pack_overwrite;
    opts.texture_set_manifest = r.pack_texture_set;
    return opts;
}

bool unit_packer_cli() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_packer_cli] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    // ── 1. --pack-assets DIR sets pack_requested + target  ──
    {
        const ParseResult r = run_parse({"--pack-assets", "/tmp/packed"});
        if (!r.pack_requested) {
            fail("--pack-assets DIR did not set pack_requested");
        }
        if (!r.pack_target.has_value()) {
            fail("--pack-assets DIR did not record a pack_target");
        } else if (*r.pack_target != "/tmp/packed") {
            fail("--pack-assets DIR recorded the wrong pack_target");
        }
        // A well-formed activation produces no diagnostic.
        if (!r.diagnostics.empty()) {
            fail("--pack-assets DIR emitted unexpected diagnostics");
        }
        // Default transfer mode is Copy (no --pack-move), no overwrite.
        if (r.pack_move) {
            fail("--pack-assets DIR set pack_move without --pack-move");
        }
        if (r.pack_overwrite) {
            fail("--pack-assets DIR set pack_overwrite without --pack-overwrite");
        }
    }

    // ── 2. Flag absent leaves pack_requested false  ──────

    // With no packer flag, the binary proceeds to the renderer: pack_requested
    // is false and no target is recorded. An ordinary flag still parses.
    {
        const ParseResult r = run_parse({"--width", "1280"});
        if (r.pack_requested) {
            fail("absent --pack-assets still set pack_requested");
        }
        if (r.pack_target.has_value()) {
            fail("absent --pack-assets still recorded a pack_target");
        }
        if (r.config.width != 1280) {
            fail("unrelated --width 1280 was not applied");
        }
    }

    // ── 3. --pack-assets as the final token  ─────────────

    // The TOKEN appeared, so pack_requested is true; the value is OPTIONAL, so
    // with no value to consume pack_target stays unset and NO diagnostic is
    // emitted. main() then defaults the Pack_Target to "assets".
    {
        const ParseResult r = run_parse({"--pack-assets"});
        if (!r.pack_requested) {
            fail("trailing --pack-assets did not set pack_requested");
        }
        if (r.pack_target.has_value()) {
            fail("trailing --pack-assets recorded a pack_target (expected none)");
        }
        if (!r.diagnostics.empty()) {
            fail("trailing --pack-assets emitted an unexpected diagnostic "
                 "(value is optional now)");
        }
        // main() defaults the unset target to "assets".
        const PackOptions opts = to_pack_options(r);
        if (opts.target != "assets/assets.zip") {
            fail("trailing --pack-assets did not default PackOptions.target to "
                 "'assets/assets.zip'");
        }
    }
    {
        // Parsing continues past the trailing flag: an earlier valid flag still
        // applies (the missing value does not abort the parse).
        const ParseResult r = run_parse({"--width", "800", "--pack-assets"});
        if (!r.pack_requested) {
            fail("trailing --pack-assets (after a flag) did not set pack_requested");
        }
        if (r.pack_target.has_value()) {
            fail("trailing --pack-assets (after a flag) recorded a pack_target");
        }
        if (!r.diagnostics.empty()) {
            fail("trailing --pack-assets (after a flag) emitted an unexpected "
                 "diagnostic");
        }
        if (r.config.width != 800) {
            fail("trailing --pack-assets aborted parsing: earlier --width 800 "
                 "was not applied");
        }
    }

    // ── 3b. --pack-assets followed by another flag  ──────

    // A following token that begins with '-' is NOT taken as the Pack_Target:
    // pack_target stays unset (default "assets" applies) and the decoy flag
    // (--data x) parses normally so config.data_dir becomes "x".
    {
        const ParseResult r = run_parse({"--pack-assets", "--data", "x"});
        if (!r.pack_requested) {
            fail("--pack-assets --data x did not set pack_requested");
        }
        if (r.pack_target.has_value()) {
            fail("--pack-assets --data x wrongly consumed '--data' as pack_target");
        }
        if (!r.diagnostics.empty()) {
            fail("--pack-assets --data x emitted an unexpected diagnostic");
        }
        if (r.config.data_dir != "x") {
            fail("--pack-assets did not leave '--data x' for the parser "
                 "(config.data_dir != \"x\")");
        }
        const PackOptions opts = to_pack_options(r);
        if (opts.target != "assets/assets.zip") {
            fail("--pack-assets --data x did not default PackOptions.target to "
                 "'assets/assets.zip'");
        }
    }

    // ── 4. --pack-move / --pack-overwrite map to PackOptions ───

    // Default (no transfer-mode flag) is Copy_Mode; --pack-move opts into
    // Move_Mode; --pack-overwrite allows writing into a non-empty target
    // Verified both on ParseResult and through the trivial PackOptions
    // mapping main() applies.
    {
        // Default: Copy_Mode, no overwrite.
        const ParseResult r = run_parse({"--pack-assets", "/tmp/packed"});
        const PackOptions opts = to_pack_options(r);
        if (opts.target != "/tmp/packed") {
            fail("PackOptions.target did not map from pack_target");
        }
        if (opts.move != false) {
            fail("PackOptions.move is not Copy_Mode by default (R4.1)");
        }
        if (opts.overwrite != false) {
            fail("PackOptions.overwrite defaulted true without --pack-overwrite");
        }
    }
    {
        // --pack-move ⇒ Move_Mode.
        const ParseResult r = run_parse({"--pack-assets", "/tmp/packed", "--pack-move"});
        if (!r.pack_move) {
            fail("--pack-move did not set pack_move");
        }
        const PackOptions opts = to_pack_options(r);
        if (opts.move != true) {
            fail("--pack-move did not map to PackOptions.move (R4.2)");
        }
        if (opts.overwrite != false) {
            fail("--pack-move unexpectedly set PackOptions.overwrite");
        }
    }
    {
        // --pack-overwrite ⇒ write into a non-empty target.
        const ParseResult r = run_parse({"--pack-assets", "/tmp/packed", "--pack-overwrite"});
        if (!r.pack_overwrite) {
            fail("--pack-overwrite did not set pack_overwrite");
        }
        const PackOptions opts = to_pack_options(r);
        if (opts.overwrite != true) {
            fail("--pack-overwrite did not map to PackOptions.overwrite (R5.3)");
        }
        if (opts.move != false) {
            fail("--pack-overwrite unexpectedly set PackOptions.move");
        }
    }
    {
        // Both transfer-mode flags together map independently.
        const ParseResult r =
            run_parse({"--pack-assets", "/tmp/packed", "--pack-move", "--pack-overwrite"});
        const PackOptions opts = to_pack_options(r);
        if (!(opts.move && opts.overwrite)) {
            fail("--pack-move + --pack-overwrite did not both map to PackOptions");
        }
        if (opts.target != "/tmp/packed") {
            fail("PackOptions.target lost with both transfer-mode flags present");
        }
    }

    // --pack-texture-set is an explicit value-bearing packer input. It does
    // not activate packing by itself; main consumes it only when
    // --pack-assets is also present.
    {
        const ParseResult r = run_parse({
            "--pack-assets", "/tmp/packed", "--pack-texture-set",
            "/tmp/generated/texture-set.json"});
        if (!r.pack_texture_set ||
            *r.pack_texture_set != "/tmp/generated/texture-set.json") {
            fail("--pack-texture-set did not capture its manifest path");
        }
        const PackOptions opts = to_pack_options(r);
        if (!opts.texture_set_manifest ||
            *opts.texture_set_manifest != "/tmp/generated/texture-set.json") {
            fail("pack texture-set path did not map to PackOptions");
        }
        if (!r.diagnostics.empty()) {
            fail("valid --pack-texture-set emitted an unexpected diagnostic");
        }
    }
    {
        const ParseResult r = run_parse({"--pack-texture-set"});
        if (r.pack_texture_set) {
            fail("trailing --pack-texture-set recorded a manifest path");
        }
        if (r.diagnostics.size() != 1) {
            fail("trailing --pack-texture-set did not emit one diagnostic");
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_packer_cli{&unit_packer_cli};

} // namespace
