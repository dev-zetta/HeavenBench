/*
 * HeavenBench — Property test for placeholder substitution / never-blank
 *                     assembly of System_Info / Settings_Info descriptions.
 *
 * Every assembled description substitutes the placeholder for
 *              exactly the unavailable datums and is never blank.
 *
 *
 *
 * For any PlatformRaw / CpuRaw / GpuRaw / DisplayRaw, render-API name, and
 * quality-preset label — with any subset of datums empty / undetermined /
 * out-of-range — the pure assemblers (exercised here through the gather_*
 * composition helpers, which delegate to the format_* functions) must produce
 * descriptions that:
 *   - are never blank
 *   - contain the per-datum placeholder string iff at least one datum feeding
 *     that group is invalid
 *
 *   - contain the rendered token of every VALID datum
 *
 *
 * The generators draw each text datum either VALID (1..20 lowercase letters /
 * digits — never blank and never able to spell a placeholder, whose words are
 * capitalised) or INVALID (empty / whitespace-only). Numeric datums are drawn
 * spanning their valid range and their undetermined / out-of-range sentinels.
 * Keeping valid text short (<= 20 chars) keeps it well clear of the 256-char
 * truncation boundary (that boundary is 's concern) so every valid
 * datum appears verbatim.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/run_info.h"

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp (external
// linkage; the inline Register constructor is token-identical across test TUs,
// satisfying the ODR).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using namespace heaven;

// Matches clean_text_field's notion of "blank": empty or whitespace-only.
bool is_blank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A VALID text datum: 1..20 characters from lowercase letters and digits only.
// Such a token is never blank and can never contain a per-datum placeholder
// (every placeholder begins with the capitalised word "Unknown"), so its
// presence in an assembled string is an unambiguous signal.
rc::Gen<std::string> gen_valid_text() {
    return rc::gen::map(
        rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::elementOf(std::string(
                "abcdefghijklmnopqrstuvwxyz0123456789")))),
        [](std::string s) {
            if (s.size() > 20) {
                s.resize(20);
            }
            return s;
        });
}

// An INVALID (undetermined) text datum: empty or whitespace-only.
rc::Gen<std::string> gen_blank_text() {
    return rc::gen::element<std::string>(
        std::string(""), std::string(" "), std::string("  "),
        std::string("\t"), std::string("\n"), std::string(" \t\r\n"));
}

// Either a valid or an invalid text datum, with roughly even weighting.
rc::Gen<std::string> gen_text_field() {
    return rc::gen::oneOf(gen_valid_text(), gen_blank_text());
}

// Test double for the host source so gather_system_info can be exercised
// without touching the OS — it simply replays the generated raws.
struct FakeHost final : heaven::IHostInfoSource {
    PlatformRaw platform;
    CpuRaw      cpu;
    [[nodiscard]] PlatformRaw query_platform() override { return platform; }
    [[nodiscard]] CpuRaw      query_cpu() override { return cpu; }
};

bool run_run_info_placeholder_property() {
    // RapidCheck's default is 100 successful cases (maxSuccess == 100), meeting
    // the ">= 100 iterations" requirement; raise via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 16: assembled descriptions substitute the placeholder for "
        "exactly the unavailable datums and are never blank (Validates: "
        "Requirements 9.6, 9.7, 12.2, 12.3, 12.4, 12.5, 12.7, 13.2, 13.4, 13.5)",
        [] {
            // ── Generate the raw inputs ──────────────────────────────────────

            // Platform: three text sub-fields, each independently valid/blank.
            PlatformRaw platform;
            platform.os_name        = *gen_text_field();
            platform.kernel_version = *gen_text_field();
            platform.machine_arch   = *gen_text_field();

            // CPU: a text model plus a clock (< 0 ⇒ undetermined) and a core
            // count (< 1 ⇒ undetermined). Clocks are whole MHz so the rendered
            // token (which rounds) is exact.
            CpuRaw cpu;
            cpu.model_name = *gen_text_field();
            cpu.clock_mhz  = static_cast<double>(
                *rc::gen::inRange<long long>(-100000, 6001));  // includes < 0
            cpu.logical_cores = *rc::gen::inRange<int>(-16, 257);  // includes < 1

            // GPU: the selected device's model plus VRAM (< 0 ⇒ undetermined).
            GpuRaw gpu;
            gpu.model_name = *gen_text_field();
            gpu.vram_mb    = *rc::gen::inRange<long>(-100000, 65537);  // includes < 0

            // Display: width/height span below/at/above positivity; aa_samples
            // spans below 1, == 1, [2, 64], and above 64; window state is always
            // determinable from the flag.
            DisplayRaw disp;
            disp.width      = *rc::gen::inRange<int>(-10, 8000);
            disp.height     = *rc::gen::inRange<int>(-10, 8000);
            disp.aa_samples = *rc::gen::inRange<int>(-4, 128);
            disp.fullscreen = *rc::gen::arbitrary<bool>();

            // Render-API name and quality-preset label: single text fields.
            const std::string api    = *gen_text_field();
            const std::string preset = *gen_text_field();

            // ── Assemble via the gather_* helpers ────────────────────────────
            FakeHost host;
            host.platform = platform;
            host.cpu      = cpu;
            const SystemInfo   sys = gather_system_info(host, gpu);
            const SettingsInfo set = gather_settings_info(api, disp, preset);

            // The gather_* helpers must be pure composition over the format_*
            // functions — every assembled field equals its direct formatter.
            RC_ASSERT(sys.platform == format_platform_description(platform));
            RC_ASSERT(sys.cpu == format_cpu_description(cpu));
            RC_ASSERT(sys.gpu == format_gpu_description(gpu));
            RC_ASSERT(set.render_api == format_render_api(api));
            RC_ASSERT(set.display_mode == format_display_mode(disp));
            RC_ASSERT(set.quality_preset == format_quality_preset(preset));

            // ── No assembled description is ever blank ───────────────────────

            RC_ASSERT(!sys.platform.empty());
            RC_ASSERT(!sys.cpu.empty());
            RC_ASSERT(!sys.gpu.empty());
            RC_ASSERT(!set.render_api.empty());
            RC_ASSERT(!set.display_mode.empty());
            RC_ASSERT(!set.quality_preset.empty());

            // ── Platform: per-datum validity from the formatter's own rules ──
            const bool os_ok     = !is_blank(platform.os_name);
            const bool kernel_ok = !is_blank(platform.kernel_version);
            const bool arch_ok   = !is_blank(platform.machine_arch);
            const bool platform_invalid = !os_ok || !kernel_ok || !arch_ok;
            RC_ASSERT(contains(sys.platform, kUnknownPlatform) == platform_invalid);
            if (os_ok) {
                RC_ASSERT(contains(sys.platform, platform.os_name));
            }
            if (kernel_ok) {
                RC_ASSERT(contains(sys.platform, platform.kernel_version));
            }
            if (arch_ok) {
                RC_ASSERT(contains(sys.platform, platform.machine_arch));
            }

            // ── CPU ──────────────────────────────────────────────────────────
            const bool model_ok = !is_blank(cpu.model_name);
            const bool clock_ok = cpu.clock_mhz >= 0.0;
            const bool cores_ok = cpu.logical_cores >= 1;
            const bool cpu_invalid = !model_ok || !clock_ok || !cores_ok;
            RC_ASSERT(contains(sys.cpu, kUnknownCpu) == cpu_invalid);
            if (model_ok) {
                RC_ASSERT(contains(sys.cpu, cpu.model_name));
            }
            if (clock_ok) {
                RC_ASSERT(contains(
                    sys.cpu,
                    "(" + std::to_string(std::llround(cpu.clock_mhz)) + "MHz)"));
            }
            if (cores_ok) {
                RC_ASSERT(contains(sys.cpu,
                                   "x" + std::to_string(cpu.logical_cores)));
            }

            // ── GPU ──────────────────────────────────────────────────────────
            const bool gpu_model_ok = !is_blank(gpu.model_name);
            const bool vram_ok      = gpu.vram_mb >= 0;
            const bool gpu_invalid  = !gpu_model_ok || !vram_ok;
            RC_ASSERT(contains(sys.gpu, kUnknownGpu) == gpu_invalid);
            if (gpu_model_ok) {
                RC_ASSERT(contains(sys.gpu, gpu.model_name));
            }
            if (vram_ok) {
                RC_ASSERT(contains(sys.gpu,
                                   "(" + std::to_string(gpu.vram_mb) + "MB)"));
            }

            // ── Display ──────────────────────────────────────────────────────
            const bool res_ok = (disp.width >= 1 && disp.height >= 1);
            const bool aa_no  = (disp.aa_samples == 1);
            const bool aa_n   = (disp.aa_samples >= 2 && disp.aa_samples <= 64);
            const bool aa_ok  = aa_no || aa_n;
            const bool display_invalid = !res_ok || !aa_ok;
            RC_ASSERT(contains(set.display_mode, kUnknownDisplay) ==
                      display_invalid);
            if (res_ok) {
                RC_ASSERT(contains(set.display_mode,
                                   std::to_string(disp.width) + "x" +
                                       std::to_string(disp.height)));
            }
            if (aa_no) {
                RC_ASSERT(contains(set.display_mode, "no AA"));
            }
            if (aa_n) {
                RC_ASSERT(contains(set.display_mode,
                                   std::to_string(disp.aa_samples) + "xAA"));
            }
            // Window state is always determinable and always present.
            const std::string window = disp.fullscreen ? "fullscreen" : "windowed";
            RC_ASSERT(contains(set.display_mode, window));

            // ── Render API ───────────────────────────────────────────────────
            const bool api_ok = !is_blank(api);
            RC_ASSERT(contains(set.render_api, kUnknownRenderApi) == !api_ok);
            if (api_ok) {
                RC_ASSERT(contains(set.render_api, api));
            }

            // ── Quality preset ───────────────────────────────────────────────
            const bool preset_ok = !is_blank(preset);
            RC_ASSERT(contains(set.quality_preset, kUnknownPreset) == !preset_ok);
            if (preset_ok) {
                RC_ASSERT(contains(set.quality_preset, preset));
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_run_info_placeholder{
    &run_run_info_placeholder_property};

} // namespace
