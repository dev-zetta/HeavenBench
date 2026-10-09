/*
 * Every text field of every assembled description is truncated to
 * at most 256 characters.
 *
 *
 * The pure formatters obtain each text datum (the Platform os/kernel/arch, the
 * CPU and GPU model names, the Render_API name, and the Quality_Preset label)
 * "as a text value truncated to a maximum of 256 characters". This property
 * draws each such field from a mix of empty, short, and over-256-character
 * strings and asserts that the corresponding text component of the assembled
 * output never exceeds `kMaxFieldChars` (256).
 *
 * The descriptions also carry a numeric tail (e.g. "(2295MHz) x32") which adds
 * characters beyond the text datum, so the assertions target the TEXT-derived
 * component: `clean_text_field` directly (the helper every text datum flows
 * through) and the model portion that prefixes the CPU/GPU lines. The numeric
 * groups are bounded by construction and are out of scope for this property.
 *
 * Self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in CMake picks it up
 * without editing the entry point.
 */
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/run_info.h"

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// A text datum drawn from three regimes: empty (exercises the placeholder
// path), short (well under the cap), and over-256 (exercises truncation). The
// over-256 case fills with a non-whitespace character so it survives the
// whitespace-only placeholder substitution and genuinely hits the truncation.
rc::Gen<std::string> gen_text_field() {
    return rc::gen::oneOf(
        rc::gen::just(std::string{}),
        rc::gen::arbitrary<std::string>(),
        rc::gen::map(rc::gen::inRange<std::size_t>(257, 4096),
                     [](std::size_t n) { return std::string(n, 'x'); }));
}

bool run_run_info_truncate_property() {
    // RapidCheck's default is 100 iterations (maxSuccess = 100), meeting the
    // ">= 100 iterations" requirement; it can be raised via RC_PARAMS.
    return rc::check(
        "Property 17: every text field of every assembled description is "
        "truncated to at most 256 characters "
        "(Validates: Requirements 12.2, 12.3, 12.4)",
        [] {
            const std::string os_name    = *gen_text_field();
            const std::string kernel     = *gen_text_field();
            const std::string arch       = *gen_text_field();
            const std::string cpu_model  = *gen_text_field();
            const std::string gpu_model  = *gen_text_field();
            const std::string render_api = *gen_text_field();
            const std::string preset     = *gen_text_field();

            // The helper every text datum flows through must never exceed the
            // cap, regardless of the placeholder it falls back to.
            RC_ASSERT(heaven::clean_text_field(os_name, heaven::kUnknownPlatform).size()
                      <= heaven::kMaxFieldChars);
            RC_ASSERT(heaven::clean_text_field(kernel, heaven::kUnknownPlatform).size()
                      <= heaven::kMaxFieldChars);
            RC_ASSERT(heaven::clean_text_field(arch, heaven::kUnknownPlatform).size()
                      <= heaven::kMaxFieldChars);
            RC_ASSERT(heaven::clean_text_field(cpu_model, heaven::kUnknownCpu).size()
                      <= heaven::kMaxFieldChars);
            RC_ASSERT(heaven::clean_text_field(gpu_model, heaven::kUnknownGpu).size()
                      <= heaven::kMaxFieldChars);

            // Render_API_Description and Quality_Preset_Label ARE a single
            // truncated text field, so the assembled output itself is bounded.
            RC_ASSERT(heaven::format_render_api(render_api).size()
                      <= heaven::kMaxFieldChars);
            RC_ASSERT(heaven::format_quality_preset(preset).size()
                      <= heaven::kMaxFieldChars);

            // The Platform_Description is "<os> <kernel> <arch>"; each text
            // datum is independently capped. Verify the assembled line's three
            // space-separated text components are each within the cap, which is
            // exactly what the helper guarantees above; re-checking through the
            // assembler guards against the assembler bypassing the helper.
            heaven::PlatformRaw platform_raw;
            platform_raw.os_name        = os_name;
            platform_raw.kernel_version = kernel;
            platform_raw.machine_arch   = arch;
            const std::string platform = heaven::format_platform_description(platform_raw);
            // No single text component (delimited by the single-space joins) may
            // exceed the cap. Walk the space-delimited tokens; each derives from
            // one capped text field (placeholders are themselves < cap).
            {
                std::size_t start = 0;
                while (start <= platform.size()) {
                    const std::size_t sp = platform.find(' ', start);
                    const std::size_t end =
                        (sp == std::string::npos) ? platform.size() : sp;
                    RC_ASSERT(end - start <= heaven::kMaxFieldChars);
                    if (sp == std::string::npos) break;
                    start = sp + 1;
                }
            }

            // CPU_Description / GPU_Description are "<model>...": the model is
            // the only unbounded-input text datum and prefixes the line up to
            // the first space, so the model portion must be within the cap.
            heaven::CpuRaw cpu_raw;
            cpu_raw.model_name    = cpu_model;
            cpu_raw.clock_mhz     = 2295.0;
            cpu_raw.logical_cores = 32;
            const std::string cpu = heaven::format_cpu_description(cpu_raw);
            RC_ASSERT(cpu.substr(0, cpu.find(' ')).size() <= heaven::kMaxFieldChars);

            heaven::GpuRaw gpu_raw;
            gpu_raw.model_name = gpu_model;
            gpu_raw.vram_mb    = 256;
            const std::string gpu = heaven::format_gpu_description(gpu_raw);
            RC_ASSERT(gpu.substr(0, gpu.find(' ')).size() <= heaven::kMaxFieldChars);
            RC_ASSERT(gpu == heaven::clean_text_field(
                                 gpu_model, heaven::kUnknownGpu) +
                                 " (256MB)");
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_run_info_truncate{
    &run_run_info_truncate_property};

} // namespace
