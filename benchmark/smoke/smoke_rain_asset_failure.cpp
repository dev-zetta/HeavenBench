/*
 * HeavenBench — rain asset-load-failure smoke case (renderer-linked, GATED)
 *
 * IF a precipitation texture or material fails to
 * load during initialization, THEN the Rain_Feature SHALL remain in a disabled
 * state, record a diagnostic indication identifying the resource that failed,
 * and the Renderer SHALL continue rendering the remaining scene without
 * precipitation.
 *
 * This is the example-based unit test the test strategy calls out
 * ("Asset-load-failure path: inject a missing rain asset, assert
 * available_ == false, enabled_ == false, no renderables added, diagnostic
 * emitted, init returns"). It lives in the HEAVEN_SMOKE-gated, renderer-linked
 * harness rather than the headless RapidCheck target because RainFeature::init
 * builds a real filament::Material from the embedded heaven_rain bytes and loads
 * textures through the live IResourceProvider — both of which need a real
 * Filament engine. The headless heaven_benchmark_tests target links no Filament,
 * so it cannot instantiate RainFeature; this case drives init against the live
 * backend the harness already stood up.
 *
 * Injecting the missing asset: RainFeature's two base materials are EMBEDDED
 * (k_heaven_rain_filamat / k_heaven_splash_filamat) so they always build. The
 * precipitation/splash SPRITE TEXTURES, however, are loaded from disk under
 * `config.data_dir` ("<data_dir>/heaven/textures/sfx/spark_d.dds" and
 * "fire_glow_dr.dds"). Pointing a COPY of the config at a bogus data_dir makes
 * TextureStore::load() return 0 (the DDS file does not exist) →
 * get_filament_texture(0) returns nullptr → init takes its  failure branch
 * for the first missing texture (the rain drop sprite). The backend's own
 * data_dir is untouched, so this case never disturbs the shared backend.
 *
 * Observing the disabled state: enabled() is public on IRenderFeature.
 * `available_` is private, so the case observes it through its documented
 * externally-visible consequences — needs_depth_prepass() stays false (it is
 * `enabled_ && built_`), and the build counters streak_build_count() /
 * splash_build_count() stay 0 (no renderable was built). "No renderables added"
 * is checked against the live scene's renderable count, snapshotted before and
 * after init. The diagnostic is captured by redirecting stdout over the init
 * call and asserting it names the missing resource (the failed.dds path).
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines)
 * fails, tolerating a headless/driver-less environment that passed the gate.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/render_feature.h"
#include "rhi/features/rain_feature.h"
#include "engine/types.h"
#include "engine/path_resolver.h"

#include <filament/Scene.h>

#include "../tests/stdio_compat.h"

#include <cstdio>
#include <functional>
#include <string>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_rain_asset_failure]";

// Capture everything written to stdout while `fn` runs, so the case can assert
// RainFeature::init emitted a diagnostic naming the missing resource.
// We redirect the underlying fd (not just std::printf's FILE*) through a temp
// file, then read it back — RainFeature logs with std::printf, which writes to
// fd 1. Best-effort: on any redirection failure we return an empty string and
// the caller treats the diagnostic check as "couldn't capture" rather than a
// hard failure, so the structural assertions still stand.
std::string capture_stdout(const std::function<void()>& fn) {
    std::fflush(stdout);
    int saved = heaven::test::duplicate_fd(heaven::test::file_descriptor(stdout));
    if (saved < 0) { fn(); return {}; }

    std::FILE* tmp = heaven::test::temporary_file();
    if (!tmp) { fn(); heaven::test::close_fd(saved); return {}; }

    std::fflush(stdout);
    if (heaven::test::duplicate_to_fd(heaven::test::file_descriptor(tmp), heaven::test::file_descriptor(stdout)) < 0) {
        fn();
        std::fflush(stdout);
        heaven::test::duplicate_to_fd(saved, heaven::test::file_descriptor(stdout));
        heaven::test::close_fd(saved);
        std::fclose(tmp);
        return {};
    }

    fn();

    std::fflush(stdout);
    heaven::test::duplicate_to_fd(saved, heaven::test::file_descriptor(stdout));
    heaven::test::close_fd(saved);

    // Read the captured bytes back from the temp file.
    std::rewind(tmp);
    std::string out;
    char buf[4096];
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof(buf), tmp);
        out.append(buf, n);
        if (n != sizeof(buf)) break;
    }
    const bool failed = std::ferror(tmp) != 0;
    std::fclose(tmp);
    if (failed) return {};
    return out;
}

} // namespace

CaseResult run_rain_asset_failure_case(const CaseContext& ctx) {
    // The render-context accessor below is FilamentBackend-specific test
    // instrumentation. The harness always constructs a FilamentBackend (it is
    // the sole IRhiBackend), so this is safe; api() lets us bail gracefully if
    // that ever changes rather than risking a bad cast.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the pipeline up so the shared RenderContext (engine / scene /
    // resources) exists. A failure here means the environment can't drive the
    // backend (no usable GPU) — SKIP so the gated test still passes, mirroring
    // the harness's headless fallbacks. init_pipelines is idempotent, so an
    // earlier case having already run it is fine.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    RenderContext& rc = backend.debug_render_context();
    if (!rc.engine || !rc.scene || !rc.resources) {
        std::printf("%s SKIP: render context not populated\n", kTag);
        return CaseResult::Skip;
    }

    // ── Inject a missing rain asset ─────────────────────────────
    // RainFeature::init resolves its sprite-texture paths through the
    // RenderContext's PathResolver (the single source of truth post-refactor),
    // not through config.data_dir. To make the sprite-texture loads under
    // "<root>/heaven/textures/sfx/" fail while the embedded materials still
    // build, temporarily point rc.paths at a resolver rooted at a nonexistent
    // tree, run init, then restore the backend's real resolver. The feature is
    // enabled in the config (the default), so reaching the disabled state proves
    // the failure path — not the toggle. The backend's own resolver is restored
    // immediately, so this case never disturbs the shared backend.
    AppConfig failing = ctx.config;
    failing.features.rain = true;

    auto bogus_paths = PathResolver::create(
        "/heaven_smoke/nonexistent-data-dir-for-rain-asset-test");
    if (!bogus_paths) {
        std::printf("%s SKIP: could not build the failure-injection resolver "
                    "(%s)\n", kTag, bogus_paths.error().message.c_str());
        return CaseResult::Skip;
    }

    // Resource the diagnostic must name (the first missing texture init hits),
    // resolved through the same resolver the feature will use.
    const std::string expected_resource =
        bogus_paths->texture_path(TextureCategory::Sfx, "spark_d.dds");

    // Snapshot the scene's renderable count so we can prove init added none.
    const size_t renderables_before = rc.scene->getRenderableCount();

    // ── Drive init under the failure injection, capturing the log ─
    // Swap in the bogus resolver only for the duration of init(), then restore
    // the backend's real resolver so subsequent cases see the live tree.
    const PathResolverRef saved_paths = rc.paths;
    rc.paths = std::cref(bogus_paths.value());
    RainFeature rain;
    const std::string log = capture_stdout([&] {
        rain.init(rc, failing);
    });
    rc.paths = saved_paths;

    const size_t renderables_after = rc.scene->getRenderableCount();

    // ── Assertions  ───────────────────────────
    bool ok = true;

    // enabled_ == false: the feature must remain disabled after a failed load,
    // even though config.features.rain was true.
    if (rain.enabled()) {
        std::printf("%s FAIL: RainFeature.enabled() is true after a failed "
                    "asset load (must stay disabled — R1.6)\n", kTag);
        ok = false;
    }

    // available_ == false, observed through its visible consequences:
    //   - needs_depth_prepass() is `enabled_ && built_`, so it must be false;
    //   - no geometry was built, so both build counters stay 0.
    if (rain.needs_depth_prepass()) {
        std::printf("%s FAIL: needs_depth_prepass() is true after a failed "
                    "load (feature is not available/built — R1.6)\n", kTag);
        ok = false;
    }
    if (rain.streak_build_count() != 0 || rain.splash_build_count() != 0) {
        std::printf("%s FAIL: build counters advanced after a failed load "
                    "(streak=%llu, splash=%llu; expected 0/0 — no geometry "
                    "may be built — R1.6)\n", kTag,
                    static_cast<unsigned long long>(rain.streak_build_count()),
                    static_cast<unsigned long long>(rain.splash_build_count()));
        ok = false;
    }

    // No renderables added to the shared scene.
    if (renderables_after != renderables_before) {
        std::printf("%s FAIL: scene renderable count changed across init "
                    "(%zu -> %zu); the failed feature must add no renderables "
                    "(R1.6)\n", kTag, renderables_before, renderables_after);
        ok = false;
    }

    // A diagnostic naming the failed resource was emitted. The capture is
    // best-effort: only assert content when we actually captured output.
    if (!log.empty()) {
        const bool names_resource =
            log.find(expected_resource) != std::string::npos;
        const bool mentions_rain_disabled =
            log.find("rain disabled") != std::string::npos;
        if (!names_resource) {
            std::printf("%s FAIL: diagnostic did not name the failed resource "
                        "'%s'. Captured log:\n%s\n", kTag,
                        expected_resource.c_str(), log.c_str());
            ok = false;
        } else if (!mentions_rain_disabled) {
            std::printf("%s FAIL: diagnostic named the resource but did not "
                        "indicate rain was disabled. Captured log:\n%s\n",
                        kTag, log.c_str());
            ok = false;
        }
    } else {
        std::printf("%s NOTE: could not capture stdout; skipping the diagnostic "
                    "text assertion (structural assertions still enforced)\n",
                    kTag);
    }

    // Cleanly tear down the test feature so it leaks no GPU resources. After a
    // failed init nothing was built, but destroy() is null-safe and also proves
    // init returned normally (we reached this line — no crash/exception).
    rain.destroy(rc);

    if (!ok) return CaseResult::Fail;

    std::printf("%s missing rain asset → feature disabled (enabled=false, "
                "needs_depth_prepass=false, 0 builds), no renderables added "
                "(%zu), diagnostic named '%s', init returned normally\n",
                kTag, renderables_after, expected_resource.c_str());
    return CaseResult::Pass;
}

} // namespace heaven::smoke
