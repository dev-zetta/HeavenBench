/*
 * HeavenBench — settings presentation layout checks.
 *
 * The catalog is also the UI's presentation source: broad/high-impact controls
 * must precede specialized controls, and each named section must stay
 * contiguous so OverlayUI can emit one heading per group.
 */
#include "rhi/overlay/settings_catalog.h"

#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool run_settings_catalog_layout() {
    using namespace heaven;

    bool ok = true;
    const auto& catalog = settings_catalog();

    const CatalogEntry* first_graphics = nullptr;
    std::vector<std::string_view> graphics_sections;
    std::unordered_map<Panel, std::string> active_section;
    std::unordered_map<Panel, std::unordered_set<std::string>> closed_sections;

    for (const CatalogEntry& entry : catalog) {
        const std::string_view section = catalog_section(entry.id);
        if (section.empty()) {
            std::printf("[unit_settings_catalog_layout] setting %d has no section\n",
                        static_cast<int>(entry.id));
            ok = false;
            continue;
        }

        std::string& active = active_section[entry.panel];
        if (active != section) {
            if (!active.empty()) {
                closed_sections[entry.panel].insert(active);
            }
            if (closed_sections[entry.panel].contains(std::string(section))) {
                std::printf(
                    "[unit_settings_catalog_layout] section '%.*s' is not "
                    "contiguous\n",
                    static_cast<int>(section.size()), section.data());
                ok = false;
            }
            active = section;
            if (entry.panel == Panel::Quality) {
                graphics_sections.push_back(section);
            }
        }

        if (entry.panel == Panel::Quality && first_graphics == nullptr) {
            first_graphics = &entry;
        }
    }

    if (first_graphics == nullptr ||
        first_graphics->id != SettingId::RenderPreset) {
        std::printf(
            "[unit_settings_catalog_layout] Rendering preset is not the first "
            "Graphics setting\n");
        ok = false;
    }

    constexpr std::array<std::string_view, 10> kExpectedGraphicsSections = {
        "Rendering preset",
        "Textures - high VRAM impact",
        "Image quality - high GPU impact",
        "Scene complexity - high CPU / GPU impact",
        "Shadows - high GPU impact",
        "Atmosphere & post-processing",
        "Materials & surface effects",
        "GPU execution",
        "CPU stress benchmark",
        "Advanced / metadata",
    };
    if (graphics_sections.size() != kExpectedGraphicsSections.size()) {
        std::printf(
            "[unit_settings_catalog_layout] Graphics has %zu sections; "
            "expected %zu\n",
            graphics_sections.size(), kExpectedGraphicsSections.size());
        ok = false;
    } else {
        for (std::size_t i = 0; i < graphics_sections.size(); ++i) {
            if (graphics_sections[i] != kExpectedGraphicsSections[i]) {
                std::printf(
                    "[unit_settings_catalog_layout] Graphics section %zu is "
                    "'%.*s'; expected '%.*s'\n",
                    i, static_cast<int>(graphics_sections[i].size()),
                    graphics_sections[i].data(),
                    static_cast<int>(kExpectedGraphicsSections[i].size()),
                    kExpectedGraphicsSections[i].data());
                ok = false;
            }
        }
    }

    return ok;
}

bool run_settings_impact_coverage() {
    using namespace heaven;
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_settings_impact_coverage] %s\n", message);
            ok = false;
        }
    };
    // Every rendering, geometry, display and simulation control needs useful
    // guidance, including future controls added to an existing panel. Audio
    // and a report-only label do not describe rendering workloads.
    for (const auto& entry : settings_catalog()) {
        const auto impact = catalog_performance_impact(entry.id);
        const bool relevant = entry.panel != Panel::Sound &&
            entry.id != SettingId::QualityPresetLabel;
        check(impact.has_value() == relevant, "impact coverage does not match the editable rendering controls");
        if (impact) {
            check(!impact->estimate.empty() && !impact->explanation.empty(),
                  "an estimate is missing its label or conditions");
            check(impact->estimate.find('%') == std::string_view::npos &&
                      impact->estimate.find("FPS") == std::string_view::npos,
                  "a qualitative estimate claims an unmeasured frame-rate prediction");
        }
    }
    for (const auto id : {SettingId::FeatureGpuInstancing, SettingId::FeatureGpuSkinning}) {
        const auto impact = catalog_performance_impact(id);
        check(impact && impact->estimate.find("saves work") != std::string_view::npos,
              "a CPU-saving execution feature is presented only as an added cost");
    }
    for (const auto id : {SettingId::PhysicalCameraEnabled, SettingId::CameraIso,
                          SettingId::HdrPaperWhiteNits, SettingId::HdrPeakNits}) {
        const auto impact = catalog_performance_impact(id);
        check(impact && impact->estimate.find("Minimal") != std::string_view::npos,
              "an inexpensive exposure/brightness parameter is presented as a heavy workload");
    }
    const auto extras = additional_feature_impacts();
    bool grading = false, dynamic = false;
    for (const auto& feature : extras) {
        check(!feature.impact.estimate.empty() && !feature.impact.explanation.empty(),
              "a rendering feature outside the editable catalog has no guidance");
        grading |= feature.feature == "Color grading";
        dynamic |= feature.feature == "Dynamic resolution" &&
            feature.impact.explanation.find("Scored benchmark runs keep a fixed pixel workload") != std::string_view::npos;
    }
    check(grading && dynamic, "CLI-only rendering features or their scoring constraints are missing");
    return ok;
}

const heaven::testing::Register g_register_settings_catalog_layout{
    &run_settings_catalog_layout};
const heaven::testing::Register g_register_settings_impact_coverage{
    &run_settings_impact_coverage};

} // namespace
