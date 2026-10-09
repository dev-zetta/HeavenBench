/*
 * HeavenBench — System_Info / Settings_Info formatter (pure)
 *
 * Assembles the System_Info (Platform / CPU / GPU) and Settings_Info
 * (Render API / Display mode / Quality preset) description strings shown in the
 * Result_Report  and gathered when a run completes
 * All formatting, truncation, range-checking, and
 * per-datum Unknown_Value_Placeholder substitution is PURE: it depends only on
 * the standard library, with no Filament, SDL, OS, or ImGui dependency, so it
 * compiles into both heaven_benchmark and the host-runnable test target.
 *
 * The raw data is supplied through small injectable sources (IHostInfoSource
 * for the host OS; the backend's GPU / render-API query) so the impure querying
 * stays out of the pure formatter (AGENTS.md).
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_RUN_INFO_H_
#define HEAVENBENCH_SRC_BENCHMARK_RUN_INFO_H_

#include <cstddef>
#include <string>

namespace heaven {

// ── Per-datum placeholder text (Unknown_Value_Placeholder) ──────────────────
// Each datum has its own human-readable placeholder so the Result_Report shows
// a clear value (e.g. "Unknown GPU") rather than a blank or an error

inline constexpr const char* kUnknownPlatform  = "Unknown Platform";
inline constexpr const char* kUnknownCpu       = "Unknown CPU";
inline constexpr const char* kUnknownGpu       = "Unknown GPU";
inline constexpr const char* kUnknownRenderApi = "Unknown API";
inline constexpr const char* kUnknownDisplay   = "Unknown Display Mode";
inline constexpr const char* kUnknownPreset    = "Unknown Preset";

// Maximum length of any single gathered text datum
// Longer values are truncated to this many characters.
inline constexpr size_t kMaxFieldChars = 256;

// ── Raw inputs ──────────────────────────────────────────────────────────────
// Filled by the injectable sources. Any field may be empty / undetermined /
// out of range, which the formatter maps to the per-datum placeholder.

// Host operating system, from uname(2).
struct PlatformRaw {
    std::string os_name;
    std::string kernel_version;
    std::string machine_arch;
};

// Host processor, from the OS.
struct CpuRaw {
    std::string model_name;
    double      clock_mhz     = -1.0;  // < 0 ⇒ undetermined
    int         logical_cores = 0;     // < 1 ⇒ undetermined
};

// Graphics hardware, from the active backend.
struct GpuRaw {
    std::string model_name;
    long        vram_mb = -1;          // < 0 ⇒ undetermined
    std::string driver_name;
    std::string driver_version;
    std::string driver_info;
    std::string driver_check_summary;
    std::string driver_check_recommendation;
};

// Output mode, from AppConfig.
struct DisplayRaw {
    int  width      = 0;               // < 1 ⇒ undetermined
    int  height     = 0;               // < 1 ⇒ undetermined
    int  aa_samples = 0;               // must be in [1, 64]; 1 ⇒ "no AA"
    bool fullscreen = false;
};

// ── Assembled descriptions ──────────────────────────────────────────────────
// The strings the Result_Report renders and logs.
struct SystemInfo {
    std::string platform;   // Platform_Description
    std::string cpu;        // CPU_Description
    std::string gpu;        // GPU_Description
    PlatformRaw platform_raw;
    CpuRaw      cpu_raw;
    GpuRaw      gpu_raw;
};
struct SettingsInfo {
    std::string render_api;     // Render_API_Description
    std::string display_mode;   // Display_Mode_Description
    std::string quality_preset; // Quality_Preset_Label
    DisplayRaw  display_raw;
};

// ── Pure field helper ───────────────────────────────────────────────────────
// Truncate value to at most kMaxFieldChars; an empty (or whitespace-only) field
// yields the supplied placeholder. Never returns a blank string.

[[nodiscard]] std::string clean_text_field(std::string value, const char* placeholder);

// ── Pure description assemblers ──────────────────────────────────────────────
// Each substitutes the per-datum placeholder for an undetermined / empty /
// out-of-range sub-field and NEVER produces a blank string or throws

//   Platform: "<os> <kernel> <arch>"             e.g. "Linux 7.0.0-22-generic x86_64"
//   CPU:      "<model> (<mhz>MHz) x<cores>"       e.g. "AMD Ryzen 9 7945HX3D (2295MHz) x32"
//   GPU:      "<model> (<vram>MB)"                 e.g. "Unknown GPU (256MB)"
//   Display:  "<w>x<h> <aa> <windowed|fullscreen>" e.g. "1600x900 8xAA windowed"
//             where <aa> is "no AA" when aa_samples == 1, "<n>xAA" for [2, 64],
//             and the placeholder token for an aa_samples outside [1, 64];
//             an undetermined resolution (width or height < 1) substitutes the
//             placeholder token for the "<w>x<h>" group.
[[nodiscard]] std::string format_platform_description(const PlatformRaw& raw);
[[nodiscard]] std::string format_cpu_description(const CpuRaw& raw);
[[nodiscard]] std::string format_gpu_description(const GpuRaw& raw);
[[nodiscard]] std::string format_display_mode(const DisplayRaw& raw);
[[nodiscard]] std::string format_render_api(const std::string& api_name);   // empty ⇒ placeholder
[[nodiscard]] std::string format_quality_preset(const std::string& label);  // empty ⇒ placeholder

// ── Injectable host source (the only impure seam) ────────────────────────────
// The concrete LinuxHostInfoSource reads uname(2), sysconf(_SC_NPROCESSORS_ONLN),
// and /proc/cpuinfo; test doubles return canned PlatformRaw/CpuRaw. The GPU /
// render-API raw comes from the backend through IRhiBackend. Keeping these
// behind an interface mirrors how the existing logic classes avoid Filament/SDL
// dependencies.
struct IHostInfoSource {
    virtual ~IHostInfoSource() = default;
    [[nodiscard]] virtual PlatformRaw query_platform() = 0;
    [[nodiscard]] virtual CpuRaw      query_cpu() = 0;
};

// Concrete Linux host source. Declared here; its impure query bodies are
// implemented separately (uname(2) / sysconf / proc/cpuinfo) and live in
// run_info.cpp. The pure formatters above do not depend on it.
class LinuxHostInfoSource final : public IHostInfoSource {
public:
    [[nodiscard]] PlatformRaw query_platform() override;
    [[nodiscard]] CpuRaw      query_cpu() override;
};

// ── Pure composition helpers ─────────────────────────────────────────────────
// Build SystemInfo from a host source (platform + CPU) and the backend's GPU
// raw. Pure composition over the pure formatters.
[[nodiscard]] SystemInfo gather_system_info(IHostInfoSource& host, const GpuRaw& gpu);

// Build SettingsInfo from the backend's render-API name and the AppConfig
// display / preset values. Pure composition.
[[nodiscard]] SettingsInfo gather_settings_info(const std::string& render_api_name,
                                                const DisplayRaw&  display,
                                                const std::string& quality_preset_label);

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_RUN_INFO_H_
