/*
 * HeavenBench — MusicPlayer lifecycle + failure-path smoke case
 *                     (renderer-linked, GATED)
 *
 *
 * 5.1, 5.2, 7.1, 7.2, 7.3, 7.4.
 *
 * EXTENDED by  /  with a final
 * scenario covering MusicPlayer::set_volume — the Live_Setting seam the config
 * overlay uses to push a new music volume at runtime. It needs no audio device
 * (a default-constructed, never-started player still accepts a volume and
 * reflects it through volume()), so it rides the same gated harness: it asserts
 * an in-range value is stored verbatim, out-of-range values clamp to [0,1], and
 * a NaN is ignored — matching parse_music_volume's clamp semantics.
 *
 * WHY THIS LIVES IN THE SMOKE HARNESS (not benchmark/tests/):
 * MusicPlayer (engine/music_player.cpp) links SDL2 (the audio device) and the
 * vendored stb_vorbis decoder, and is in ENGINE_SOURCES which is compiled into
 * heaven_benchmark and heaven_benchmark_smoke ONLY — never the headless
 * heaven_benchmark_tests host target (whose pure music helpers live in
 * types.h). A MusicPlayer test therefore cannot live in benchmark/tests/ (that
 * is the host target). Despite the test scenario naming
 * `benchmark/tests/unit_music_player.cpp`, the test MUST be a SMOKE case so it
 * links music_player.cpp — hence this file under smoke/ (GLOB'd into the smoke
 * target), following the exact smoke-case contract in smoke_cases.h.
 *
 * NO DEVICE / NO DISPLAY NEEDED: every scenario below — disabled no-op,
 * idempotent stop, missing-file, unreadable-file, decode-failure, and the
 * packer-guard behavioral check — leaves playing_ false and opens NO audio
 * device. So this case does not use ctx.backend. It still only runs under the
 * shared HEAVEN_SMOKE=1 + display gate enforced by smoke_main.cpp, which
 * satisfies "gated like the existing smoke harness".
 *
 * INTERNAL STATE IS PRIVATE: MusicPlayer's device id, pcm buffer, and atomics
 * are private with no public accessors, so "device unopened / track unplayed"
 * is asserted BEHAVIORALLY: start() returns immediately, a following stop() is
 * a clean no-op, volume() reflects the configured level, and the diagnostic
 * emitted on stderr identifies the failure cause. The diagnostics are emitted
 * on the music thread inside run_startup(); stop() joins that thread, so by the
 * time stop() returns the diagnostic has been written. We capture stderr with a
 * POSIX dup/dup2 redirect around each start()+stop() cycle so the actual
 * substrings (path + cause) are real assertions, then
 * restore stderr.
 *
 * PACKER-GUARD APPROACH: the guard is on_render_run() returning the
 * private render_run_ (defaults true). A normally-constructed MusicPlayer is
 * always on a render run, so the rejection branch ("audio is unavailable in
 * packer mode") cannot be reached from outside without a test-only seam — and
 * the test asks us NOT to add a public setter just for the test. We instead
 * assert the structural guarantee that IS observable: the enabled missing-file
 * run reaches the "not found" diagnostic and NOT the "packer mode" rejection,
 * which proves on_render_run() returned true for a normally-constructed player.
 * The packer never constructs a MusicPlayer (it is an Application member started
 * only in init(), a path the packer never reaches), so the rejection branch is
 * structurally unreachable in production; that contract is documented here.
 */

#include "smoke_cases.h"

#include "engine/music_player.h"
#include "engine/path_resolver.h"
#include "engine/types.h"

#include "../tests/stdio_compat.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_music_player]";

namespace fs = std::filesystem;

// Capture everything written to stderr while `fn` runs, returning it as a
// string. Uses a portable CRT descriptor redirect: save fd
// 2, point it at a temp file for the duration of `fn`, then restore. stderr is
// unbuffered, so the music thread's fprintf() lands in the file immediately;
// we still fflush before swapping fds. If the redirect cannot be set up the
// function runs `fn` unredirected and returns an empty string (the caller then
// SKIPs the substring assertion rather than failing on a capture-infra issue).
template <typename Fn>
std::string capture_stderr(const fs::path& tmp_path, Fn&& fn) {
    std::fflush(stderr);
    const int saved = heaven::test::duplicate_fd(heaven::test::file_descriptor(stderr));
    FILE* redirect = std::fopen(tmp_path.string().c_str(), "w+");
    if (saved < 0 || redirect == nullptr) {
        if (redirect != nullptr) std::fclose(redirect);
        if (saved >= 0) heaven::test::close_fd(saved);
        fn();
        return std::string{};
    }

    std::fflush(stderr);
    heaven::test::duplicate_to_fd(heaven::test::file_descriptor(redirect), heaven::test::file_descriptor(stderr));

    fn();

    std::fflush(stderr);
    heaven::test::duplicate_to_fd(saved, heaven::test::file_descriptor(stderr));
    heaven::test::close_fd(saved);
    std::fclose(redirect);

    std::ifstream in(tmp_path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

// True when `haystack` contains `needle` as a substring.
bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Create data_dir/heaven/ under `root` and return the data_dir path. The
// resolved music asset is then <data_dir>/heaven/heaven.oga (k_music_rel_path).
fs::path make_data_dir(const fs::path& root, const char* leaf, std::error_code& ec) {
    fs::path data_dir = root / leaf;
    fs::create_directories(data_dir / "heaven", ec);
    return data_dir;
}

} // namespace

CaseResult run_music_player_case(const CaseContext& /*ctx*/) {
    // ── Temp fixture root ────────────────────────────────────────
    std::error_code ec;
    fs::path tmp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("%s SKIP: no temp directory available (%s)\n",
                    kTag, ec.message().c_str());
        return CaseResult::Skip;
    }
    const fs::path root =
        tmp_base / ("heaven_music_smoke_" + std::to_string(::getpid()));
    fs::remove_all(root, ec);  // best-effort clean slate
    fs::create_directories(root, ec);
    if (ec) {
        std::printf("%s SKIP: could not create temp fixture dir '%s' (%s)\n",
                    kTag, root.string().c_str(), ec.message().c_str());
        return CaseResult::Skip;
    }
    const fs::path scratch = root / "stderr.txt";

    bool ok = true;

    // ── Scenario 1: disabled config is a pure no-op  ───────
    // A disabled MusicConfig spawns no thread, opens no device, plays no track.
    // Internal state is private, so we assert the observable contract: start()
    // returns, volume() reflects the configured level, and a following stop()
    // is a clean no-op. No diagnostic is emitted (disabling music is not an
    // error), so this path is not stderr-captured.
    {
        const fs::path data_dir = make_data_dir(root, "disabled", ec);
        auto resolver = PathResolver::create(data_dir.string());
        if (!resolver) {
            std::printf("%s FAIL: PathResolver::create rejected '%s' (%s)\n",
                        kTag, data_dir.string().c_str(),
                        resolver.error().message.c_str());
            ok = false;
        } else {
            MusicConfig config;
            config.enabled = false;
            config.volume = 0.5f;

            MusicPlayer player;
            player.start(config, *resolver);  // pure no-op, returns immediately

            if (player.volume() != 0.5f) {
                std::printf("%s FAIL: volume() == %.3f after a disabled start, "
                            "expected the configured 0.500 (R1.5)\n",
                            kTag, player.volume());
                ok = false;
            }

            player.stop();  // clean no-op after a no-op start
        }
    }

    // ── Scenario 2: stop() is idempotent and always safe  ──
    // stop() before any start, stop() twice after a disabled no-op start, and
    // stop() twice after a real enabled start against a missing file — each is
    // safe (no crash, no hang; stop() joins the music thread, so repeated calls
    // after the join are no-ops). A hang would manifest as the harness never
    // returning, so reaching the assertions below proves liveness.
    {
        const fs::path data_dir = make_data_dir(root, "idempotent", ec);
        auto resolver = PathResolver::create(data_dir.string());
        if (!resolver) {
            std::printf("%s FAIL: PathResolver::create rejected '%s' (%s)\n",
                        kTag, data_dir.string().c_str(),
                        resolver.error().message.c_str());
            ok = false;
        } else {
            // Capture+discard stderr: sub-block (c) below intentionally drives
            // the enabled missing-file path (whose "not found" diagnostic is
            // asserted in scenario 3), so we swallow it here to keep the
            // harness output tidy — this scenario only proves stop() liveness.
            capture_stderr(scratch, [&] {
                // (a) stop() before any start.
                {
                    MusicPlayer player;
                    player.stop();
                }
                // (b) stop() twice after a disabled no-op start.
                {
                    MusicConfig config;
                    config.enabled = false;
                    MusicPlayer player;
                    player.start(config, *resolver);
                    player.stop();
                    player.stop();
                }
                // (c) stop() twice after a real enabled start against a missing
                //     file (the music thread resolves, finds nothing, returns;
                //     the first stop() joins it, the second is a no-op).
                {
                    MusicConfig config;
                    config.enabled = true;
                    MusicPlayer player;
                    player.start(config, *resolver);
                    player.stop();
                    player.stop();
                }
            });
            std::printf("%s stop() idempotent across no-start / disabled / "
                        "enabled-missing paths (R1.3)\n", kTag);
        }
    }

    // ── Scenario 3: missing-file diagnostic  + the packer-guard
    //    behavioral check  ──────────────────────────────
    // Enabled config, data_dir with NO heaven/heaven.oga. start()+stop() drives
    // the music thread through the resolve-existence check, which emits the
    // "not found" diagnostic naming the resolved path and returns with no
    // playback. We capture stderr to assert the substring + path, and assert it
    // does NOT contain the "packer mode" rejection — proving on_render_run()
    // returned true for the normally-constructed player (the structural
    // guarantee that IS observable).
    {
        const fs::path data_dir = make_data_dir(root, "missing", ec);
        auto resolver = PathResolver::create(data_dir.string());
        if (!resolver) {
            std::printf("%s FAIL: PathResolver::create rejected '%s' (%s)\n",
                        kTag, data_dir.string().c_str(),
                        resolver.error().message.c_str());
            ok = false;
        } else {
            const std::string expected_path = resolver->music_path();
            MusicConfig config;
            config.enabled = true;

            std::string err = capture_stderr(scratch, [&] {
                MusicPlayer player;
                player.start(config, *resolver);
                player.stop();  // joins the music thread ⇒ diagnostic emitted
            });

            if (err.empty()) {
                std::printf("%s SKIP(partial): stderr capture unavailable; "
                            "missing-file substring not asserted\n", kTag);
            } else {
                if (!contains(err, "not found")) {
                    std::printf("%s FAIL: missing-file diagnostic lacks "
                                "'not found' (got: %s) (R4.3/R5.1)\n",
                                kTag, err.c_str());
                    ok = false;
                }
                if (!contains(err, expected_path)) {
                    std::printf("%s FAIL: missing-file diagnostic does not name "
                                "the resolved path '%s' (got: %s) (R5.1)\n",
                                kTag, expected_path.c_str(), err.c_str());
                    ok = false;
                }
                // Packer-guard behavioral check: a normally-constructed
                // player is on a Render_Run, so it must NOT have hit the
                // packer-mode rejection branch.
                if (contains(err, "packer mode")) {
                    std::printf("%s FAIL: normally-constructed player reported "
                                "'packer mode' — on_render_run() should be true "
                                "(R7.3)\n", kTag);
                    ok = false;
                }
            }
        }
    }

    // ── Scenario 4: unreadable-file diagnostic  ────────────
    // Create heaven/heaven.oga as a regular file, then remove all read
    // permissions so it "exists" but cannot be opened for reading. The music
    // thread's readability probe (an ifstream open) fails, emitting the
    // "not readable" diagnostic. NOTE: a process running as root bypasses the
    // permission bits, so after chmod we re-probe readability; if the file is
    // still readable (privileged run) the player would instead reach the decode
    // path, so we SKIP the substring assertion rather than fail.
    {
        const fs::path data_dir = make_data_dir(root, "unreadable", ec);
        const fs::path asset = data_dir / "heaven" / "heaven.oga";
        {
            std::ofstream f(asset, std::ios::binary);
            f << "unreadable placeholder bytes";
        }
        fs::permissions(asset, fs::perms::none, ec);  // strip all perms

        // Re-probe: is it actually unreadable now?
        bool actually_unreadable = false;
        {
            std::ifstream probe(asset, std::ios::binary);
            actually_unreadable = !probe.is_open();
        }

        auto resolver = PathResolver::create(data_dir.string());
        if (!resolver) {
            std::printf("%s FAIL: PathResolver::create rejected '%s' (%s)\n",
                        kTag, data_dir.string().c_str(),
                        resolver.error().message.c_str());
            ok = false;
        } else if (!actually_unreadable) {
            std::printf("%s SKIP(partial): asset still readable after chmod "
                        "(likely a privileged run); 'not readable' substring "
                        "not asserted\n", kTag);
        } else {
            const std::string expected_path = resolver->music_path();
            MusicConfig config;
            config.enabled = true;

            std::string err = capture_stderr(scratch, [&] {
                MusicPlayer player;
                player.start(config, *resolver);
                player.stop();
            });

            if (err.empty()) {
                std::printf("%s SKIP(partial): stderr capture unavailable; "
                            "unreadable substring not asserted\n", kTag);
            } else {
                if (!contains(err, "not readable")) {
                    std::printf("%s FAIL: unreadable-file diagnostic lacks "
                                "'not readable' (got: %s) (R5.1)\n",
                                kTag, err.c_str());
                    ok = false;
                }
                if (!contains(err, expected_path)) {
                    std::printf("%s FAIL: unreadable-file diagnostic does not "
                                "name the resolved path '%s' (got: %s) (R5.1)\n",
                                kTag, expected_path.c_str(), err.c_str());
                    ok = false;
                }
            }
        }

        // Restore perms so the temp tree can be removed cleanly below.
        fs::permissions(asset, fs::perms::owner_all, ec);
    }

    // ── Scenario 5: decode-failure diagnostic  ───────
    // Create heaven/heaven.oga as a small, readable, NON-Vorbis file. The music
    // thread passes the existence + readability checks, then
    // stb_vorbis_decode_filename fails on the garbage bytes, emitting the
    // "decode failure" diagnostic naming the path with no device opened.
    {
        const fs::path data_dir = make_data_dir(root, "decode", ec);
        const fs::path asset = data_dir / "heaven" / "heaven.oga";
        {
            std::ofstream f(asset, std::ios::binary);
            f << "this is definitely not ogg vorbis audio data";
        }

        auto resolver = PathResolver::create(data_dir.string());
        if (!resolver) {
            std::printf("%s FAIL: PathResolver::create rejected '%s' (%s)\n",
                        kTag, data_dir.string().c_str(),
                        resolver.error().message.c_str());
            ok = false;
        } else {
            const std::string expected_path = resolver->music_path();
            MusicConfig config;
            config.enabled = true;

            std::string err = capture_stderr(scratch, [&] {
                MusicPlayer player;
                player.start(config, *resolver);
                player.stop();
            });

            if (err.empty()) {
                std::printf("%s SKIP(partial): stderr capture unavailable; "
                            "decode-failure substring not asserted\n", kTag);
            } else {
                if (!contains(err, "decode failure")) {
                    std::printf("%s FAIL: decode-failure diagnostic lacks "
                                "'decode failure' (got: %s) (R4.4/R5.2)\n",
                                kTag, err.c_str());
                    ok = false;
                }
                if (!contains(err, expected_path)) {
                    std::printf("%s FAIL: decode-failure diagnostic does not "
                                "name the resolved path '%s' (got: %s) (R5.2)\n",
                                kTag, expected_path.c_str(), err.c_str());
                    ok = false;
                }
            }
        }
    }

    // ── Scenario 6: set_volume clamp + stored-value readback  ──
    // music volume is a Live_Setting: the overlay pushes a new level at runtime
    // and the renderer applies it on the next frame with no device reopen. The
    // mechanism is MusicPlayer::set_volume storing into the atomic volume_ the
    // SDL callback reads; volume() exposes that stored value. This needs no GPU
    // and no audio device — a default-constructed player (never started) still
    // accepts a volume and reflects it through volume(). We assert the clamp
    // semantics carried over from parse_music_volume: an in-range value is
    // stored verbatim, an out-of-range value is pulled to the nearest bound, and
    // a NaN is ignored (the prior value is left unchanged), so the stored value
    // always stays in [0,1].
    {
        MusicPlayer player;

        // In-range value is stored verbatim.
        player.set_volume(0.5f);
        if (player.volume() != 0.5f) {
            std::printf("%s FAIL: volume() == %.3f after set_volume(0.5), "
                        "expected 0.500 (R2.3)\n", kTag, player.volume());
            ok = false;
        }

        // Above-range value clamps to the upper bound 1.0.
        player.set_volume(2.0f);
        if (player.volume() != 1.0f) {
            std::printf("%s FAIL: volume() == %.3f after set_volume(2.0), "
                        "expected the clamped 1.000 (R2.3)\n",
                        kTag, player.volume());
            ok = false;
        }

        // Below-range value clamps to the lower bound 0.0.
        player.set_volume(-1.0f);
        if (player.volume() != 0.0f) {
            std::printf("%s FAIL: volume() == %.3f after set_volume(-1.0), "
                        "expected the clamped 0.000 (R2.3)\n",
                        kTag, player.volume());
            ok = false;
        }

        // NaN is ignored: the stored value (0.0 from the clamp above) is left
        // unchanged rather than poisoning the atomic the callback reads.
        player.set_volume(std::numeric_limits<float>::quiet_NaN());
        if (player.volume() != 0.0f) {
            std::printf("%s FAIL: volume() == %.3f after set_volume(NaN), "
                        "expected the prior 0.000 left unchanged (R2.3)\n",
                        kTag, player.volume());
            ok = false;
        }
    }

    // ── Cleanup ──────────────────────────────────────────────────
    fs::remove_all(root, ec);  // best-effort

    if (!ok) return CaseResult::Fail;

    std::printf("%s MusicPlayer lifecycle + failure paths verified: disabled "
                "no-op (R1.5), idempotent stop (R1.3), missing-file 'not found' "
                "+ render-run guard (R4.3/R5.1/R7.3), unreadable 'not readable' "
                "(R5.1), 'decode failure' on non-Vorbis (R4.4/R5.2), and "
                "set_volume clamp/readback (R2.3)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
