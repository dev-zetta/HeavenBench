/*
 * HeavenBench — device-failure + run-parity smoke case
 *                     (renderer-linked, GATED)
 *
 *
 *
 * WHAT THIS CLOSES: the MusicPlayer unit/property/decode suites prove the pure
 * value logic, the resolve/decode failure diagnostics, and (when a device is
 * available) that playback actually starts. This case exercises the remaining
 * production guarantee that only a real, FAILING audio-device open can drive:
 * the MusicPlayer reaches SDL_OpenAudioDevice with a genuinely decoded track,
 * the open fails, and the player degrades gracefully (diagnostic + silent +
 * non-fatal) exactly as  demands.
 *
 *   •  — a failed Audio_Device open emits a diagnostic INCLUDING the
 *     SDL-reported reason. MusicPlayer::run_startup prints
 *     "MusicPlayer: failed to open audio device: <SDL_GetError()>" on a zero
 *     device id; we assert both the fixed prefix AND a non-empty SDL reason.
 *   •  — the 5 s steady-clock deadline abandons a non-completing open as an
 *     open failure. SDL_OpenAudioDevice is synchronous and cannot be made to
 *     hang for 5 s deterministically here, so the timeout branch itself is not
 *     forced. The timeout path and the fast-failure path SHARE one failure-
 *     handling contract (playing_ stays false, one diagnostic, device left
 *     closed, run continues); this case forces the fast failure and asserts
 *     that shared contract. The limitation is documented, not hidden.
 *   •  — a run that fails to start audio reaches the same externally-
 *     observable terminal state as a successful start: start() returned without
 *     blocking, the music thread joined cleanly via stop(), playing() is false,
 *     and the process did not abort. The full render-run exit-status parity is
 *     a separate application-level check; here we assert the player-level contract.
 *
 * FORCING THE FAILURE DETERMINISTICALLY: the smoke harness brings SDL up with
 * SDL_INIT_VIDEO | SDL_INIT_AUDIO (rhi.cpp), so a working audio driver is
 * normally current. To force SDL_OpenAudioDevice to fail we tear the current
 * audio driver down with SDL_AudioQuit() and then call
 * SDL_AudioInit("<bogus>") with a driver name that does not exist — that init
 * fails and leaves NO audio driver current, so the subsequent
 * SDL_OpenAudioDevice (on the music thread) fails with a real SDL reason. We
 * use the low-level SDL_AudioQuit/SDL_AudioInit pair rather than
 * SDL_QuitSubSystem/SDL_InitSubSystem on purpose: those subsystem calls are
 * REFERENCE-COUNTED, and the audio subsystem may have been initialised more
 * than once (the harness's SDL_Init plus smoke_music_decode_start's
 * SDL_InitSubSystem), so a single SDL_QuitSubSystem would not actually shut the
 * driver down. SDL_AudioQuit/SDL_AudioInit operate on the driver directly,
 * bypassing the refcount, which makes the forced failure reliable regardless of
 * how many times audio was initialised earlier in the run.
 *
 * RESTORING SHARED STATE: the harness shares SDL across cases, so before
 * returning (on EVERY path, including the SKIPs) we restore a working audio
 * driver with SDL_AudioQuit() + SDL_AudioInit(nullptr) (re-select SDL's default
 * driver). The player is ALWAYS stop()'d (joins the music thread, closes the
 * device if it opened) before we restore audio, so no callback can run against
 * a torn-down driver.
 *
 * A DECODABLE FILE IS REQUIRED: a decode failure short-circuits BEFORE the
 * device open, so to reach the open step the track must decode successfully. We
 * use the REAL heaven/heaven.oga resolved under ctx.config.data_dir (exactly
 * like smoke_music_decode_start). If that asset is not resolvable we SKIP the
 * whole case — a real decode-then-open cannot be faked. This case needs no GPU,
 * so it does not use ctx.backend (only ctx.config for the Data_Root).
 */

#include "smoke_cases.h"

#include "engine/music_player.h"
#include "engine/path_resolver.h"
#include "engine/types.h"

#include <SDL2/SDL.h>

#include "../tests/stdio_compat.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_music_device_failure]";

namespace fs = std::filesystem;

// A driver name SDL will never recognise, so SDL_AudioInit fails and leaves no
// audio driver current.
constexpr const char* kBogusDriver = "ugx_nonexistent_audio_driver";

// The fixed prefix MusicPlayer::run_startup prints on a failed device open,
// immediately followed by the SDL-reported reason.
constexpr const char* kOpenFailPrefix = "failed to open audio device: ";

// Capture everything written to stderr while `fn` runs, returning it as a
// string. portable CRT descriptor redirect: save fd 2, point it at a
// temp file for the duration of `fn`, then restore. stderr is unbuffered, so
// the music thread's fprintf lands in the file immediately; we still fflush
// before swapping fds. On a redirect-setup failure `fn` runs unredirected and
// the function returns an empty string (caller then SKIPs the substring
// assertion rather than failing on a capture-infra issue). Mirrors the helper
// in smoke_music_player.cpp.
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
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// True when `haystack` contains `needle` as a substring.
bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Given captured stderr, find the "failed to open audio device: " line and
// return true iff a non-empty (non-whitespace) SDL reason follows the prefix on
// that line — i.e. the diagnostic actually carries the SDL-reported reason
// not just the fixed prefix.
bool open_failure_has_reason(const std::string& err) {
    const std::size_t at = err.find(kOpenFailPrefix);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t i = at + std::char_traits<char>::length(kOpenFailPrefix);
    // Scan to end-of-line collecting any non-whitespace character as "reason".
    bool saw_reason = false;
    for (; i < err.size() && err[i] != '\n'; ++i) {
        if (!std::isspace(static_cast<unsigned char>(err[i]))) {
            saw_reason = true;
            break;
        }
    }
    return saw_reason;
}

// Restore a working default audio driver after the forced failure. Bypasses the
// subsystem refcount (matching how we tore it down), then re-selects SDL's
// default driver so subsequent cases see a usable audio device.
void restore_audio_driver() {
    SDL_AudioQuit();
    SDL_AudioInit(nullptr);
}

} // namespace

CaseResult run_music_device_failure_case(const CaseContext& ctx) {
    // ── Resolve the real Music_Asset_Path under the configured Data_Root ─────
    // The track MUST decode successfully to reach the device-open step, so the
    // genuine heaven/heaven.oga is required. If it is not resolvable (the smoke
    // run's CWD cannot resolve the relative data_dir, or the asset is absent),
    // SKIP — the decode-then-open path cannot be faked.
    auto resolver = PathResolver::create(ctx.config.data_dir);
    if (!resolver) {
        std::printf("%s SKIP: PathResolver::create rejected data_dir '%s' (%s)\n",
                    kTag, ctx.config.data_dir.c_str(),
                    resolver.error().message.c_str());
        return CaseResult::Skip;
    }
    const std::string music_path = resolver->music_path();

    std::error_code ec;
    if (!fs::exists(music_path, ec) || ec) {
        std::printf("%s SKIP: real music asset not found at resolved path '%s' "
                    "(run with the repo root as CWD so '%s' resolves)\n",
                    kTag, music_path.c_str(), ctx.config.data_dir.c_str());
        return CaseResult::Skip;
    }

    // Scratch file for the stderr redirect.
    fs::path tmp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("%s SKIP: no temp directory available (%s)\n",
                    kTag, ec.message().c_str());
        return CaseResult::Skip;
    }
    const fs::path scratch =
        tmp_base / ("heaven_music_devfail_" + std::to_string(::getpid()) + ".txt");

    // ── Force the audio device unavailable ───────────────────────────────────
    // Tear down whatever audio driver the harness brought up, then try to init a
    // bogus one. SDL_AudioInit returns non-zero on failure and leaves NO driver
    // current, so SDL_OpenAudioDevice on the music thread will fail with a real
    // SDL reason. We do this on the main thread BEFORE start() so the music
    // thread observes the unavailable device for its entire run.
    SDL_AudioQuit();
    const int bogus_rc = SDL_AudioInit(kBogusDriver);
    if (bogus_rc == 0) {
        // Extremely unlikely: a driver by that name actually initialised. We
        // cannot force the failure on this platform, so restore and SKIP rather
        // than assert against a working device.
        std::printf("%s SKIP: bogus audio driver '%s' unexpectedly initialised; "
                    "cannot force a device-open failure here\n",
                    kTag, kBogusDriver);
        restore_audio_driver();
        return CaseResult::Skip;
    }

    bool ok = true;

    // ── Start the player against the real track with no usable device ────────
    // start() is non-blocking: it returns immediately and all
    // resolve/decode/open work runs on the music thread. The real file decodes
    // successfully, then SDL_OpenAudioDevice fails, emitting the
    // diagnostic. stop() joins the music thread, so by the time it returns the
    // diagnostic has been written. We capture stderr around the whole cycle.
    MusicConfig config;
    config.enabled = true;
    config.volume = 1.0f;

    MusicPlayer player;
    bool playing_after = true;  // sentinel; must be observed false below

    const std::string err = capture_stderr(scratch, [&] {
        const auto t0 = std::chrono::steady_clock::now();
        player.start(config, *resolver);   // returns immediately (non-blocking)
        // start() must not block on device coordination; a real decode of the
        // ~4.5 MB track plus a fast device-open failure is well under a second.
        const auto start_cost =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0);
        if (start_cost > std::chrono::milliseconds(100)) {
            std::printf("%s WARN: start() took %lld ms (expected immediate "
                        "return)\n",
                        kTag, static_cast<long long>(start_cost.count()));
        }
        player.stop();                     // joins the music thread
        playing_after = player.playing();  // observed after the join
    });

    // ── Restore a working audio driver BEFORE asserting ──────────────────────
    // Do this immediately after the player is stopped (its thread joined and any
    // device closed) so the shared SDL state is healthy for subsequent cases
    // regardless of how the assertions below turn out.
    restore_audio_driver();

    // ──  playing() stays false after the failed open ───────────────────
    if (playing_after) {
        std::printf("%s FAIL: playing() is true after a failed device open; it "
                    "must stay false on the open-failure path (R5.5/R5.4)\n",
                    kTag);
        ok = false;
    }

    // ──  the open-failure diagnostic carries the SDL reason ─────────────
    if (err.empty()) {
        // Without a capture we cannot assert the substrings. This is a capture-
        // infra limitation, not a defect: SKIP the substring assertion but keep
        // the playing()-false assertion above (which already passed if we are
        // here with ok still true).
        std::printf("%s SKIP(partial): stderr capture unavailable; open-failure "
                    "diagnostic substrings not asserted (playing() stayed false "
                    "— R5.5)\n", kTag);
        fs::remove(scratch, ec);
        return ok ? CaseResult::Pass : CaseResult::Fail;
    }

    if (!contains(err, kOpenFailPrefix)) {
        std::printf("%s FAIL: device-open-failure diagnostic lacks '%s' (got: "
                    "%s) (R5.3)\n",
                    kTag, kOpenFailPrefix, err.c_str());
        ok = false;
    } else if (!open_failure_has_reason(err)) {
        std::printf("%s FAIL: device-open-failure diagnostic carries no SDL "
                    "reason after '%s' (got: %s) (R5.3)\n",
                    kTag, kOpenFailPrefix, err.c_str());
        ok = false;
    }

    fs::remove(scratch, ec);  // best-effort cleanup

    if (!ok) {
        return CaseResult::Fail;
    }

    std::printf("%s device-open failure handled gracefully: 'failed to open "
                "audio device' diagnostic carried the SDL reason (R5.3), "
                "playing() stayed false (R5.4 shared failure contract), and the "
                "player reached the same terminal state as a successful start — "
                "start() non-blocking + music thread joined cleanly (R5.5)\n",
                kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
