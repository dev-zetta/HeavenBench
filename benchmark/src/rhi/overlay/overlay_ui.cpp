/*
 * HeavenBench — Overlay UI widget builder (implementation)
 *
 * Pure Dear ImGui immediate-mode rendering driven by a BenchmarkStatus
 * snapshot. See overlay_ui.h for the contract. No engine/Filament types are
 * touched and no benchmark state is mutated; the returned OverlayIntents tell
 * the caller what the user asked for.
 */
#include "rhi/overlay/overlay_ui.h"
#include "engine/render_test_scene_catalog.h"
#include "engine/tessellation_levels.h"

#include "benchmark/score_calculator.h"
#include "rhi/overlay/settings_catalog.h"
#include "rhi/overlay/credits_catalog.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <variant>

namespace heaven {

// ── Public entry point ───────────────────────────────────────────

OverlayIntents OverlayUI::build(const BenchmarkStatus& status,
                                const BenchConfig&     working,
                                const ConfigUiState&   ui_state) {
    OverlayIntents intents;

    // Keep the toolbar and any open settings accessible throughout playback.
    // The menu's first action changes from Benchmark to Cancel while running;
    // opening settings alone does not mutate the benchmark state.
    build_menu_bar(status, intents);
    if (status.state == BenchmarkState::Running) {
        build_running_overlay(status);
    }
    build_config_window(working, ui_state, intents);

    // The Result_Report persists whenever the snapshot still carries a result,
    // independent of the menu bar, until the user dismisses it (Requirement
    // The snapshot reports has_result only in the Complete state.
    if (status.has_result) {
        build_result_report(status, intents);
    }

    draw_driver_notice();
    intents.microprofile = microprofiler_.draw();
    draw_credits();

    return intents;
}

void OverlayUI::set_driver_check(const DriverCheck& check,
                                 std::string_view description) {
    driver_check_ = check;
    driver_description_ = description;
    driver_notice_open_ = check.severity == DriverCheckSeverity::Warning;
}

void OverlayUI::draw_driver_notice() {
    if (!driver_notice_open_) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(std::min(560.0f, viewport->WorkSize.x),
               std::min(360.0f, viewport->WorkSize.y)), ImGuiCond_Appearing);
    if (ImGui::Begin("Graphics driver", &driver_notice_open_,
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", driver_description_.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", driver_check_.summary.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", driver_check_.recommendation.c_str());
        if (!driver_check_.help_url.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("Driver help: %s", driver_check_.help_url.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Copy driver details")) {
            const std::string details = driver_description_ + "\n" +
                driver_check_.summary + "\n" + driver_check_.recommendation +
                "\n" + driver_check_.help_url;
            ImGui::SetClipboardText(details.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) driver_notice_open_ = false;
        ImGui::TextWrapped("Rendering can continue. Reopen this advice in Diagnostics.");
    }
    ImGui::End();
}

// ── Diagnostics sidebar (left edge) ─────────────────────────────

namespace {

std::string grouped_count(uint64_t value) {
    std::string text = std::to_string(value);
    for (std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(text.size()) - 3;
         pos > 0; pos -= 3) {
        text.insert(static_cast<size_t>(pos), 1, ',');
    }
    return text;
}

std::string optional_metric(double value, std::string_view suffix,
                            int precision = 1) {
    if (value < 0.0) return "N/A";
    return std::vformat("{:.{}f} {}", std::make_format_args(
        value, precision, suffix));
}

std::string memory_metric(double used_mb, double total_mb) {
    if (used_mb < 0.0 && total_mb < 0.0) return "N/A";
    const bool use_gib = std::max(used_mb, total_mb) >= 1024.0;
    const double divisor = use_gib ? 1024.0 : 1.0;
    const std::string_view unit = use_gib ? "GiB" : "MiB";
    if (used_mb < 0.0) {
        return std::format("N/A / {:.1f} {}", total_mb / divisor, unit);
    }
    if (total_mb < 0.0) {
        return std::format("{:.1f} {}", used_mb / divisor, unit);
    }
    return std::format("{:.1f} / {:.1f} {}", used_mb / divisor,
                       total_mb / divisor, unit);
}

std::string byte_metric(uint64_t bytes) {
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mib >= 1024.0
        ? std::format("{:.2f} GiB", mib / 1024.0)
        : std::format("{:.1f} MiB", mib);
}

template <typename Rows>
void draw_metric_table(const char* id, const Rows& rows) {
    if (!ImGui::BeginTable(id, 2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 105.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& [label, value] : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", value.c_str());
    }
    ImGui::EndTable();
}

} // namespace

void OverlayUI::draw_debug_sidebar(const DebugSidebarInfo& info,
                                   const FrameGenerationStatus& frame_generation) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    const float expanded_width = std::min(
        330.0f, std::max(180.0f, viewport->WorkSize.x * 0.45f));
    const float collapsed_width = ImGui::GetFrameHeight() +
        ImGui::GetStyle().WindowPadding.x * 2.0f;
    const float sidebar_width =
        debug_sidebar_expanded_ ? expanded_width : collapsed_width;

    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(sidebar_width, viewport->WorkSize.y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Diagnostics Sidebar", nullptr, flags)) {
        if (!debug_sidebar_expanded_) {
            if (ImGui::Button(">##expand_diagnostics", ImVec2(-1.0f, 0.0f))) {
                debug_sidebar_expanded_ = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Expand diagnostics");
            }
        } else {
            ImGui::TextUnformatted("Diagnostics");
            ImGui::SameLine();
            const float button_width = ImGui::GetFrameHeight();
            ImGui::SetCursorPosX(
                std::max(ImGui::GetCursorPosX(),
                         ImGui::GetWindowWidth() - button_width -
                             ImGui::GetStyle().WindowPadding.x));
            if (ImGui::Button("<##collapse_diagnostics",
                              ImVec2(button_width, 0.0f))) {
                debug_sidebar_expanded_ = false;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Collapse diagnostics");
            }

            ImGui::SeparatorText("Performance");
            const double cpu_work_ms = info.camera_ms + info.scene_sync_ms +
                info.feature_ms + info.render_submit_ms;
            const std::array performance_rows{
                std::pair{frame_generation.requested==FrameGenerationMode::Off?"FPS":"Real FPS", std::format("{:.1f}", info.fps)},
                std::pair{"Frame time", std::format("{:.2f} ms", info.frame_time_ms)},
                std::pair{"1% low", std::format("{:.1f} FPS", info.one_percent_low_fps)},
                std::pair{"GPU frame", optional_metric(info.gpu_frame_ms, "ms", 2)},
                std::pair{"CPU work", std::format("{:.2f} ms", cpu_work_ms)},
                std::pair{"Camera", std::format("{:.2f} ms", info.camera_ms)},
                std::pair{"Scene sync", std::format("{:.2f} ms", info.scene_sync_ms)},
                std::pair{"Features", std::format("{:.2f} ms", info.feature_ms)},
                std::pair{"Submit", std::format("{:.2f} ms", info.render_submit_ms)},
            };
            draw_metric_table("##performance", performance_rows);

            ImGui::SeparatorText("Frame generation");
            const auto& fg = frame_generation;
            const auto& counts = fg.counters;
            const std::array generation_rows{
                std::pair{"Requested", std::string(frame_generation_mode_id(fg.requested))},
                std::pair{"Effective", std::string(frame_generation_mode_id(fg.effective))},
                std::pair{"Real rendered", grouped_count(counts.real_frames)},
                std::pair{"Generated", counts.generated_frames_available
                    ? grouped_count(counts.generated_frames) : std::string{"Unavailable"}},
                std::pair{"Real submitted", counts.presentation_submissions_available
                    ? grouped_count(counts.real_presented_frames) : std::string{"Unavailable"}},
                std::pair{"Gen. submitted", counts.presentation_submissions_available
                    ? grouped_count(counts.generated_presented_frames) : std::string{"Unavailable"}},
                std::pair{"Failed submits", counts.presentation_submissions_available
                    ? grouped_count(counts.failed_presentations) : std::string{"Unavailable"}},
                std::pair{"SDK submitted", fg.sdk_presented_frames
                    ? grouped_count(*fg.sdk_presented_frames) : std::string{"Unavailable"}},
                std::pair{"Display confirmed", counts.presentation_feedback_available
                    ? grouped_count(counts.confirmed_displayed_frames) : std::string{"Unavailable"}},
            };
            draw_metric_table("##frame_generation", generation_rows);
            ImGui::TextWrapped("%s", fg.reason.c_str());
            ImGui::TextDisabled("Counters: current frame-generation runtime; reload resets them");
            if (fg.sdk_presented_frames)
                ImGui::TextWrapped("SDK submissions combine real and generated images since provider initialization.");
            if (!counts.presentation_feedback_available)
                ImGui::TextWrapped("Display feedback unavailable; native presentation submissions are not displayed frames.");

            ImGui::SeparatorText("GPU");
            const std::string driver = info.gpu_driver.empty()
                ? "N/A" : info.gpu_driver;
            const std::string driver_info = info.gpu_driver_info.empty()
                ? "N/A" : info.gpu_driver_info;
            const std::array gpu_rows{
                std::pair{"Device", info.gpu_name.empty() ? std::string{"N/A"} : info.gpu_name},
                std::pair{"Driver", driver},
                std::pair{"Driver info", driver_info},
                std::pair{"Driver version", info.gpu_driver_version.empty() ? std::string{"N/A"} : info.gpu_driver_version},
                std::pair{"API", info.graphics_api.empty() ? std::string{"N/A"} : info.graphics_api},
                std::pair{"API version", info.graphics_api_version.empty() ? std::string{"N/A"} : info.graphics_api_version},
                std::pair{"PCI address", info.pci_address.empty() ? std::string{"N/A"} : info.pci_address},
                std::pair{"Utilization", optional_metric(info.gpu_utilization_percent, "%")},
                std::pair{"Memory busy", optional_metric(info.memory_utilization_percent, "%")},
                std::pair{"App heap", memory_metric(info.process_vram_used_mb, info.process_vram_budget_mb)},
                std::pair{"Device VRAM", memory_metric(info.device_vram_used_mb, info.device_vram_total_mb)},
                std::pair{"Temperature", optional_metric(info.gpu_temperature_c, "C")},
                std::pair{"Power", optional_metric(info.gpu_power_w, "W")},
                std::pair{"Core clock", optional_metric(info.gpu_core_clock_mhz, "MHz", 0)},
                std::pair{"Memory clock", optional_metric(info.gpu_memory_clock_mhz, "MHz", 0)},
                std::pair{"Fan", optional_metric(info.gpu_fan_rpm, "RPM", 0)},
            };
            draw_metric_table("##gpu", gpu_rows);
            if (!driver_check_.summary.empty()) {
                if (driver_check_.severity == DriverCheckSeverity::Warning) {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                       "Driver advisory");
                }
                ImGui::TextWrapped("%s", driver_check_.summary.c_str());
                if (ImGui::Button("Driver details")) driver_notice_open_ = true;
            }

            ImGui::SeparatorText("Scene");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                ImGui::TextUnformatted("Mesh counts describe unique stored scene meshes. "
                                       "They exclude repeated instances, generated foliage and "
                                       "additional shadow/depth passes; they are not frame geometry totals.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            const bool tessellated = info.vertices != info.base_vertices ||
                info.triangles != info.base_triangles;
            const std::array scene_rows{
                std::pair{"Mesh vertices", grouped_count(info.vertices)},
                std::pair{"Mesh triangles", grouped_count(info.triangles)},
                std::pair{"Base mesh", tessellated
                    ? grouped_count(info.base_vertices) + " vertices / " +
                        grouped_count(info.base_triangles) + " triangles"
                    : std::string{"Same as mesh"}},
                std::pair{"Meshes", grouped_count(info.mesh_count)},
                std::pair{"Instances", grouped_count(info.instance_count)},
                std::pair{"Textures", grouped_count(info.texture_count)},
                std::pair{"Texture data", byte_metric(info.selected_texture_bytes)},
                std::pair{"Features", std::format("{} / {} enabled",
                    info.enabled_render_features, info.total_render_features)},
            };
            draw_metric_table("##scene", scene_rows);

            ImGui::SeparatorText("Renderer");
            const std::array renderer_rows{
                std::pair{"Resolution", std::format("{}x{}", info.width, info.height)},
                std::pair{"Window", info.fullscreen ? std::string{"Fullscreen"} : std::string{"Windowed"}},
                std::pair{"Preset", info.render_preset.empty() ? std::string{"N/A"} : info.render_preset},
                std::pair{"MSAA", info.msaa_samples > 1
                    ? std::format("{}x", info.msaa_samples) : std::string{"Off"}},
                std::pair{"Textures", info.texture_quality.empty() ? std::string{"N/A"} : info.texture_quality},
            };
            draw_metric_table("##renderer", renderer_rows);

            ImGui::SeparatorText("System");
            const std::array system_rows{
                std::pair{"Platform", info.platform.empty() ? std::string{"N/A"} : info.platform},
                std::pair{"CPU", info.cpu.empty() ? std::string{"N/A"} : info.cpu},
                std::pair{"Version", info.application_version.empty() ? std::string{"N/A"} : info.application_version},
                std::pair{"Build", info.build_architecture.empty() ? std::string{"N/A"} : info.build_architecture},
            };
            draw_metric_table("##system", system_rows);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ── Menu bar (all interactive states) ─────────────────────────────────────────

void OverlayUI::build_menu_bar(const BenchmarkStatus& status,
                               OverlayIntents& intents) {
    // The main controls retain their order: Benchmark/Cancel, Settings, Quit. The six
    // per-section entries (Graphics / Display / Environment / Camera /
    // Geometry / Audio) were collapsed into a single Settings entry now that
    // every setting group lives as a tab inside the one consolidated
    // configuration window — separate menu items for each were redundant with
    // those tabs. Top-level MenuItems act as clickable buttons; each returns
    // true the frame it is activated.
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (status.state == BenchmarkState::Running) {
        if (ImGui::MenuItem("Cancel")) intents.cancel = true;
    } else if (ImGui::MenuItem("Benchmark")) {
        intents.start_benchmark = true;
    }

    // Settings opens the consolidated configuration window on Graphics, where
    // the broad rendering preset and highest-impact controls appear first.
    if (ImGui::MenuItem("Settings")) {
        intents.open_panel = OverlayIntents::Panel::Quality;
    }

    // Quit terminates the application. The caller honours quit before
    // start_benchmark, so activating both in one frame quits without starting a
    // run.
    if (ImGui::MenuItem("Quit")) {
        intents.quit = true;
    }

    // Append the informational dialog after the existing controls so opening
    // Credits never makes cancellation, settings or quitting depend on it.
    if (ImGui::MenuItem("Credits")) {
        credits_open_ = true;
        ImGui::SetWindowFocus("Credits##credits");
    }

    // Keep optional diagnostics after all primary controls. Compact surfaces
    // use a short menu label; REC remains a visible capture indicator there.
    const float spacing = ImGui::GetStyle().ItemSpacing.x * 4.0f;
    const float diagnostics_width = ImGui::CalcTextSize("Diagnostics").x + spacing;
    const float recording_width = microprofiler_.recording()
        ? ImGui::CalcTextSize("Recording").x + spacing : 0.0f;
    const bool compact = ImGui::GetContentRegionAvail().x < diagnostics_width + recording_width;
    const char* diagnostics_label = compact
        ? (microprofiler_.recording() ? "REC" : "More") : "Diagnostics";
    if (ImGui::BeginMenu(diagnostics_label)) {
        if (microprofiler_.recording()) ImGui::TextUnformatted("Recording");
        if (ImGui::MenuItem("Microprofiler", "F6", microprofiler_.is_open()))
            microprofiler_.toggle();
        if (ImGui::MenuItem("Graphics driver advice")) driver_notice_open_ = true;
        ImGui::EndMenu();
    }
    if (!compact && microprofiler_.recording()) {
        if (ImGui::MenuItem("Recording##microprofile_indicator")) microprofiler_.set_open(true);
    }

    ImGui::EndMainMenuBar();
}

void OverlayUI::draw_credits() {
    if (!credits_open_) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    // A bounded nonmodal dialog keeps the toolbar available on small windows
    // and scaled touch displays. Only its text region scrolls; Close stays put.
    const float font = ImGui::GetFontSize();
    const ImVec2 available(std::max(1.0f, viewport->WorkSize.x),
                           std::max(1.0f, viewport->WorkSize.y));
    const ImVec2 size(std::min(available.x, font * 46.0f),
                     std::min(available.y, font * 39.0f));
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + available.x * 0.5f,
               viewport->WorkPos.y + available.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("Credits##credits", &credits_open_, flags)) {
        const float close_height = std::max(44.0f, font * 2.5f);
        const float text_height = std::max(1.0f, ImGui::GetContentRegionAvail().y -
            close_height - ImGui::GetStyle().ItemSpacing.y);
        if (ImGui::BeginChild("##credits_text", ImVec2(0.0f, text_height),
                ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("HeavenBench %s", application_build_info_.version.c_str());
            ImGui::TextUnformatted(credits::kAssetNotice);
            for (const auto& section : credits::kSections) {
                ImGui::Spacing();
                // Wrapped headings also fit narrow/scaled portrait surfaces.
                ImGui::Separator();
                ImGui::TextUnformatted(section.title);
                for (const auto& entry : section.entries) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted(entry.name);
                    ImGui::TextDisabled("%s", entry.role);
                }
            }
            ImGui::Spacing();
            ImGui::TextUnformatted(credits::kOriginalNotice);
            ImGui::Spacing();
            ImGui::TextUnformatted(credits::kLicenseNotice);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        if (ImGui::Button("Close##credits_close",
                ImVec2(std::min(ImGui::GetContentRegionAvail().x, font * 8.0f), close_height))) {
            credits_open_ = false;
        }
    }
    ImGui::End();
}

// ── Options_Panels ───────────────────────────────────────────────

namespace {

struct InputTextStringData {
    std::string* value = nullptr;
};

int input_text_string_resize(ImGuiInputTextCallbackData* data) {
    auto* user = static_cast<InputTextStringData*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize && user && user->value) {
        user->value->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = const_cast<char*>(user->value->c_str());
    }
    return 0;
}

bool input_text_string(const char* label, std::string& value) {
    InputTextStringData user{&value};
    return ImGui::InputText(
        label, const_cast<char*>(value.c_str()), value.capacity() + 1,
        ImGuiInputTextFlags_CallbackResize, input_text_string_resize, &user);
}

// SettingValue → concrete-type coercions. A control seeds itself from
// entry.read(working), which returns the field's natural alternative; these
// helpers pull the value back out defensively (a mismatched alternative falls
// back to a neutral default) so seeding never throws.
[[nodiscard]] bool sv_bool(const SettingValue& v) {
    if (const bool* b = std::get_if<bool>(&v)) return *b;
    if (const int* i = std::get_if<int>(&v)) return *i != 0;
    return false;
}

[[nodiscard]] int sv_int(const SettingValue& v) {
    if (const int* i = std::get_if<int>(&v)) return *i;
    if (const QualityLevel* q = std::get_if<QualityLevel>(&v))
        return static_cast<int>(*q);
    if (const float* f = std::get_if<float>(&v)) return static_cast<int>(*f);
    return 0;
}

[[nodiscard]] float sv_float(const SettingValue& v) {
    if (const float* f = std::get_if<float>(&v)) return *f;
    if (const int* i = std::get_if<int>(&v)) return static_cast<float>(*i);
    return 0.0f;
}

[[nodiscard]] std::string sv_string(const SettingValue& v) {
    if (const std::string* s = std::get_if<std::string>(&v)) return *s;
    return std::string{};
}

// A Reload setting carries a persistent trailing [reload] marker plus a tooltip
// distinguishing it from a Live setting that takes effect immediately
// Live settings render nothing extra.
void draw_reload_marker(const CatalogEntry& entry) {
    if (entry.apply != Apply::Reload) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("[reload]");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "This setting is read once at startup. Use the Apply / Reload "
            "button to rebuild the benchmark so the change takes effect.");
    }
}

// A separate wrapped line leaves the existing control and reload marker width
// unchanged, including on the tablet's scaled UI. Explanations stay available
// on hover without turning every control into a paragraph.
void draw_performance_impact(const PerformanceImpact& impact) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Estimated: %.*s", static_cast<int>(impact.estimate.size()),
                        impact.estimate.data());
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(impact.explanation.data(),
                              impact.explanation.data() + impact.explanation.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Render exactly one control for one catalog entry, seeded
// from `working`. A commit appends exactly one
// ConfigurationEdit. Bounded sliders pass [min,max] so the
// widget cannot return an out-of-range value; numeric input
// paths clamp to the bound before emitting and write the clamped value back
// into the local for display. The builder mutates no engine
// state — it only reads `working` and emits edits.
void render_control(const CatalogEntry& entry,
                    const BenchConfig&  working,
                    OverlayIntents&     intents) {
    // A per-entry unique ImGui label: the human label plus a hidden ##id so two
    // controls that share a label (none today, but cheap insurance) never
    // collide in ImGui's id stack.
    const std::string ui_label =
        std::string(entry.label) + "##" +
        std::to_string(static_cast<int>(entry.id));

    switch (entry.widget) {
        case Widget::Checkbox: {
            bool val = sv_bool(entry.read(working));
            if (ImGui::Checkbox(ui_label.c_str(), &val)) {
                emit_edit(intents, entry.id, SettingValue{val});
            }
            draw_reload_marker(entry);
            break;
        }
        case Widget::SliderFloat: {
            float val = sv_float(entry.read(working));
            // Slider values live in a frame-local because OverlayUI only emits
            // intents. Commit every drag update so the controller advances the
            // Working configuration before the next UI frame seeds this local
            // again. Waiting for deactivation would re-seed from the old value
            // throughout the drag, making the control snap back on release.
            if (ImGui::SliderFloat(ui_label.c_str(), &val, entry.min_value,
                                   entry.max_value)) {
                emit_edit(intents, entry.id, SettingValue{val});
            }
            draw_reload_marker(entry);
            break;
        }
        case Widget::SliderInt: {
            int val = sv_int(entry.read(working));
            if (ImGui::SliderInt(ui_label.c_str(), &val,
                                 static_cast<int>(entry.min_value),
                                 static_cast<int>(entry.max_value))) {
                emit_edit(intents, entry.id, SettingValue{val});
            }
            draw_reload_marker(entry);
            break;
        }
        case Widget::InputFloat: {
            float val = sv_float(entry.read(working));
            // Commit on ANY change the widget reports (typed value AND the
            // +/- step buttons) as well as on deactivation. Detecting only
            // IsItemDeactivatedAfterEdit() misses the step buttons — they
            // commit immediately without the field losing focus — so the
            // increment was previously discarded when the local reseeded.
            const bool changed = ImGui::InputFloat(ui_label.c_str(), &val);
            if (changed || ImGui::IsItemDeactivatedAfterEdit()) {
                // Unbounded entries leave min==max==0 and skip the clamp; a
                // bounded numeric input clamps to the catalog bound and shows
                // the clamped value.
                if (entry.min_value != entry.max_value) {
                    val = std::clamp(val, entry.min_value, entry.max_value);
                }
                emit_edit(intents, entry.id, SettingValue{val});
            }
            draw_reload_marker(entry);
            break;
        }
        case Widget::InputInt: {
            int val = sv_int(entry.read(working));
            // See InputFloat above: commit on the widget's own change signal so
            // the +/- step buttons actually take effect.
            const bool changed = ImGui::InputInt(ui_label.c_str(), &val);
            if (changed || ImGui::IsItemDeactivatedAfterEdit()) {
                if (entry.min_value != entry.max_value) {
                    val = std::clamp(val, static_cast<int>(entry.min_value),
                                     static_cast<int>(entry.max_value));
                }
                emit_edit(intents, entry.id, SettingValue{val});
            }
            draw_reload_marker(entry);
            break;
        }
        case Widget::Combo: {
            const SettingValue cur = entry.read(working);
            if (entry.id == SettingId::RenderTestScene) {
                const auto scenes = render_test_scene_catalog();
                const auto& current = std::get<std::string>(cur);
                const auto selected = std::ranges::find(scenes, current, &RenderTestSceneInfo::id);
                const std::string preview = selected == scenes.end() ? "Heaven (current settings)" : std::string(selected->title);
                if (ImGui::BeginCombo(ui_label.c_str(), preview.c_str())) {
                    if (ImGui::Selectable("Heaven (current settings)", current.empty()))
                        emit_edit(intents, entry.id, SettingValue{std::string{}});
                    for (const auto& scene : scenes) {
                        const std::string title(scene.title);
                        if (ImGui::Selectable(title.c_str(), current == scene.id))
                            emit_edit(intents, entry.id, SettingValue{std::string(scene.id)});
                    }
                    ImGui::EndCombo();
                }
                if (selected != scenes.end()) {
                    ImGui::TextWrapped("%.*s", static_cast<int>(selected->description.size()), selected->description.data());
                }
            } else if (std::holds_alternative<QualityLevel>(cur)) {
                // QualityLevel combo: Low / Medium / High mapped to [0,2]. The
                // selected index is emitted as a QualityLevel so the catalog's
                // apply_to (clamp_quality_level) coerces it directly.
                constexpr std::array kQualityItems = {"Low", "Medium", "High"};
                int idx = std::clamp(static_cast<int>(std::get<QualityLevel>(cur)),
                                     0, 2);
                if (ImGui::Combo(ui_label.c_str(), &idx, kQualityItems.data(),
                                 static_cast<int>(kQualityItems.size()))) {
                    emit_edit(intents, entry.id,
                              SettingValue{static_cast<QualityLevel>(idx)});
                }
            } else if (std::holds_alternative<RenderPreset>(cur)) {
                constexpr std::array kPresets = {
                    RenderPreset::Custom,
                    RenderPreset::BalancedSystem,
                    RenderPreset::VisualUltra,
                    RenderPreset::GpuTorture,
                    RenderPreset::CpuTorture,
                };
                constexpr std::array kPresetItems = {
                    "Custom", "Balanced System", "Visual Ultra",
                    "GPU Torture", "CPU Torture"};
                int idx = std::clamp(
                    static_cast<int>(std::get<RenderPreset>(cur)), 0, 4);
                if (ImGui::Combo(ui_label.c_str(), &idx, kPresetItems.data(),
                                 static_cast<int>(kPresetItems.size()))) {
                    emit_edit(intents, entry.id,
                              SettingValue{kPresets[idx]});
                }
            } else if (std::holds_alternative<MaterialWorkflow>(cur)) {
                constexpr std::array kWorkflows = {
                    MaterialWorkflow::MetallicRoughnessCompat,
                    MaterialWorkflow::SpecularGlossinessExperimental,
                };
                constexpr std::array kWorkflowItems = {
                    "Metallic / roughness (compat)",
                    "Specular / glossiness (experimental)",
                };
                int idx = std::clamp(
                    static_cast<int>(std::get<MaterialWorkflow>(cur)), 0, 1);
                if (ImGui::Combo(ui_label.c_str(), &idx, kWorkflowItems.data(),
                                 static_cast<int>(kWorkflowItems.size()))) {
                    emit_edit(intents, entry.id,
                              SettingValue{kWorkflows[idx]});
                }
            } else if (std::holds_alternative<FrameGenerationMode>(cur)) {
                constexpr std::array kFrameGenerationItems = {"Off", "FSR 3", "DLSS"};
                int idx = std::clamp(static_cast<int>(std::get<FrameGenerationMode>(cur)), 0, 2);
                if (ImGui::Combo(ui_label.c_str(), &idx, kFrameGenerationItems.data(),
                                 static_cast<int>(kFrameGenerationItems.size()))) {
                    emit_edit(intents, entry.id, SettingValue{static_cast<FrameGenerationMode>(idx)});
                }
            } else if (std::holds_alternative<TextureQuality>(cur)) {
                constexpr std::array kTextureItems = {
                    "Original (authored)", "High 2K", "Ultra 4K"};
                int idx = std::clamp(
                    static_cast<int>(std::get<TextureQuality>(cur)), 0, 2);
                if (ImGui::Combo(ui_label.c_str(), &idx, kTextureItems.data(),
                                 static_cast<int>(kTextureItems.size()))) {
                    emit_edit(intents, entry.id,
                              SettingValue{static_cast<TextureQuality>(idx)});
                }
            } else {
                // MsaaSamples combo: the fixed accepted set {1,2,4}. The
                // selected sample count is emitted as an int so apply_msaa_samples
                // validates it with reject-and-retain semantics.
                constexpr std::array kMsaaSamples = {1, 2, 4};
                constexpr std::array kMsaaItems = {"1x", "2x", "4x"};
                const int cur_samples = sv_int(cur);
                int       idx         = 0;
                for (std::size_t i = 0; i < kMsaaSamples.size(); ++i) {
                    if (kMsaaSamples[i] == cur_samples) {
                        idx = static_cast<int>(i);
                    }
                }
                if (ImGui::Combo(ui_label.c_str(), &idx, kMsaaItems.data(),
                                 static_cast<int>(kMsaaItems.size()))) {
                    emit_edit(intents, entry.id,
                              SettingValue{kMsaaSamples[idx]});
                }
            }
            draw_reload_marker(entry);
            break;
        }
        case Widget::InputText: {
            // Seed a fixed local buffer from the working value; commit the
            // edited text on deactivation. The string settings governed here
            // (quality_preset_label) are short report metadata.
            char buf[256] = {};
            const std::string seed = sv_string(entry.read(working));
            std::strncpy(buf, seed.c_str(), sizeof(buf) - 1);
            ImGui::InputText(ui_label.c_str(), buf, sizeof(buf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                emit_edit(intents, entry.id, SettingValue{std::string(buf)});
            }
            draw_reload_marker(entry);
            break;
        }
    }
    if (const auto impact = catalog_performance_impact(entry.id)) {
        draw_performance_impact(*impact);
    }
}

// True when the given panel governs at least one Reload setting, so the
// dirty/Apply-Reload/error affordances belong in it (co-located
// with the affected controls).
[[nodiscard]] bool panel_has_reload(Panel panel) {
    return std::ranges::any_of(settings_catalog(), [panel](const CatalogEntry& e) {
        return e.panel == panel && e.apply == Apply::Reload;
    });
}

void render_tessellation_level(const BenchConfig& working, OverlayIntents& intents) {
    const auto current = detect_tessellation_level(working.tessellation);
    const std::string preview(tessellation_level_label(current));
    if (ImGui::BeginCombo("Tessellation level", preview.c_str())) {
        for (const auto level : kSelectableTessellationLevels) {
            const std::string label(tessellation_level_label(level));
            if (!ImGui::Selectable(label.c_str(), current == level)) continue;
            auto selected = working.tessellation;
            apply_tessellation_level(selected, level);
            if (selected.enabled != working.tessellation.enabled)
                emit_edit(intents, SettingId::TessEnabled, selected.enabled);
            if (selected.scale != working.tessellation.scale)
                emit_edit(intents, SettingId::TessScale, selected.scale);
            if (selected.factor != working.tessellation.factor)
                emit_edit(intents, SettingId::TessFactor, selected.factor);
            if (selected.distance != working.tessellation.distance)
                emit_edit(intents, SettingId::TessDistance, selected.distance);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Named levels set Scale, Factor and Distance together. Manual values "
        "show Custom. Rendering presets supply base settings; select a level "
        "after applying a preset to override them.");
    draw_reload_marker(catalog_entry(SettingId::TessEnabled));
}

// Render every catalog control belonging to `panel`, then — for panels that own
// Reload settings — the co-located dirty/Apply-Reload line and the reload-error
// line. When `skip_resolution` is set the Width and
// Height entries are omitted: the caller renders a dedicated Resolution picker
// in their place (Display tab, when a monitor resolution list is available).
void render_panel_body(Panel                panel,
                       const BenchConfig&   working,
                       const ConfigUiState& ui_state,
                       OverlayIntents&      intents,
                       bool                 skip_resolution,
                       std::string_view     leading_section = {}) {
    std::string_view current_section = leading_section;
    for (const CatalogEntry& e : settings_catalog()) {
        if (e.panel != panel) {
            continue;
        }
        if (skip_resolution &&
            (e.id == SettingId::Width || e.id == SettingId::Height)) {
            continue;
        }
        const std::string_view section = catalog_section(e.id);
        if (section != current_section) {
            ImGui::SeparatorText(std::string(section).c_str());
            current_section = section;
            if (section == "Textures - high VRAM impact") {
                const auto gib = [](uint64_t bytes) {
                    return static_cast<double>(bytes) /
                           (1024.0 * 1024.0 * 1024.0);
                };
                if (!ui_state.texture_set_available) {
                    ImGui::TextWrapped(
                        "This archive contains authored textures only. High "
                        "2K and Ultra 4K will fall back to Original.");
                } else {
                    ImGui::Text(
                        "Archive tiers: Original %.2f GiB | 2K %.2f GiB | "
                        "4K %.2f GiB",
                        gib(ui_state.texture_original_gpu_bytes),
                        gib(ui_state.texture_2k_gpu_bytes),
                        gib(ui_state.texture_4k_gpu_bytes));
                }
            }
        }
#ifdef __ANDROID__
        if (e.id == SettingId::Fullscreen) {
            ImGui::TextUnformatted("Display mode: immersive fullscreen");
            continue;
        }
#endif
        if (e.id == SettingId::TessEnabled) render_tessellation_level(working, intents);
        render_control(e, working, intents);
        if (e.id == SettingId::FrameGeneration) {
            const auto& fg = ui_state.frame_generation;
            ImGui::Text("Effective: %s", std::string(frame_generation_mode_id(fg.effective)).c_str());
            ImGui::TextWrapped("%s", fg.reason.c_str());
            ImGui::TextWrapped("Requesting frame generation during a tour makes the complete tour diagnostic and unscored.");
            ImGui::Text("Real renders: %llu", static_cast<unsigned long long>(fg.counters.real_frames));
            if (fg.counters.generated_frames_available)
                ImGui::Text("Generated frames: %llu", static_cast<unsigned long long>(fg.counters.generated_frames));
            else ImGui::TextDisabled("Generated frame count: unavailable");
            if (fg.counters.presentation_submissions_available) {
                ImGui::Text("Native presentation submissions: %llu",
                    static_cast<unsigned long long>(fg.counters.presented_frames));
                ImGui::Text("Real submitted: %llu | Generated submitted: %llu",
                    static_cast<unsigned long long>(fg.counters.real_presented_frames),
                    static_cast<unsigned long long>(fg.counters.generated_presented_frames));
            } else ImGui::TextDisabled("Separate native submission counts: unavailable");
            if (fg.sdk_presented_frames)
                ImGui::Text("SDK native submissions (real + generated): %llu",
                    static_cast<unsigned long long>(*fg.sdk_presented_frames));
            if (fg.counters.presentation_feedback_available) {
                ImGui::Text("Display confirmations: %llu",
                    static_cast<unsigned long long>(fg.counters.confirmed_displayed_frames));
            } else ImGui::TextDisabled("Displayed frame count: unavailable (no presentation feedback)");
        }
    }

    if (panel == Panel::Quality) {
        ImGui::SeparatorText("Additional rendering features");
        for (const auto& feature : additional_feature_impacts()) {
            ImGui::TextUnformatted(feature.feature.data(),
                                  feature.feature.data() + feature.feature.size());
            draw_performance_impact(feature.impact);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%.*s", static_cast<int>(feature.impact.explanation.size()),
                                feature.impact.explanation.data());
            ImGui::PopTextWrapPos();
        }
    }

    if (!panel_has_reload(panel)) {
        return;
    }

    // Dirty_State affordance: a continuously visible "unsaved changes" line and
    // an Apply / Reload button whenever the Working_Configuration diverges from
    // the Active_Configuration in a Reload setting.
    if (ui_state.dirty) {
        ImGui::Separator();
        ImGui::TextUnformatted("Unsaved changes \xE2\x80\x94 Reload to apply");
        if (ImGui::Button(
                (std::string("Apply / Reload##") +
                 std::to_string(static_cast<int>(panel)))
                    .c_str())) {
            intents.reload = true;
        }
    }

    // A failed reload keeps an error line visible while the prior configuration
    // stays in effect.
    if (ui_state.reload_error) {
        ImGui::TextUnformatted(
            "Reload failed \xE2\x80\x94 the previous configuration is still in "
            "effect.");
    }
}

// The six setting groups, in menu order, each shown as a tab in the single
// consolidated configuration window. Pairs the menu-bar intent enum with the
// catalog Panel the tab's controls are drawn from.
struct TabDef {
    OverlayIntents::Panel intent;
    Panel                 panel;
    const char*           label;
};

constexpr TabDef kConfigTabs[] = {
    {OverlayIntents::Panel::Quality,      Panel::Quality,      "Graphics"},
    {OverlayIntents::Panel::Settings,     Panel::Settings,     "Display"},
    {OverlayIntents::Panel::Environment,  Panel::Environment,  "Environment"},
    {OverlayIntents::Panel::Camera,       Panel::Camera,       "Camera"},
    {OverlayIntents::Panel::Tessellation, Panel::Tessellation, "Geometry"},
    {OverlayIntents::Panel::Sound,        Panel::Sound,        "Audio"},
};

} // namespace

void OverlayUI::build_config_window(const BenchConfig&   working,
                                    const ConfigUiState& ui_state,
                                    OverlayIntents&      intents) {
    // A menu-bar settings control activated this frame opens the window and
    // requests its tab be focused. The pending tab is
    // consumed (reset to None) after rendering so the user can freely switch
    // tabs on subsequent frames.
    if (intents.open_panel != OverlayIntents::Panel::None) {
        config_window_open_ = true;
        pending_tab_        = intents.open_panel;
    }

    if (!config_window_open_) {
        return;
    }

    // A sensible default size + position the first time the window appears, so
    // it no longer opens as a tiny box the user must resize by hand. The window
    // id carries a "##cfg" suffix (hidden from the title) so it does NOT inherit
    // the stale tiny [Window][Settings] entry an older build left in imgui.ini;
    // it gets a fresh identity that honours this default and then remembers any
    // later user resize/move.
#ifdef __ANDROID__
    // Android's DPI-scaled font/style can be much larger than the default
    // 13-pixel font. Reopen at a readable size within the work area, which
    // already excludes the toolbar, instead of restoring a desktop-sized box.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font_scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 available(
        std::max(1.0f, viewport->WorkSize.x - style.WindowPadding.x * 2.0f),
        std::max(1.0f, viewport->WorkSize.y - style.WindowPadding.y * 2.0f));
    ImGui::SetNextWindowSize(
        ImVec2(std::min(520.0f * font_scale, available.x),
               std::min(640.0f * font_scale, available.y)),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + style.WindowPadding.x,
               viewport->WorkPos.y + style.WindowPadding.y),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), available);
#else
    ImGui::SetNextWindowSize(ImVec2(520.0f, 640.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20.0f, 60.0f), ImGuiCond_FirstUseEver);
#endif

    if (ImGui::Begin("Settings##cfg", &config_window_open_)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Performance estimates describe enabled workloads, not measured FPS. "
                            "Impact varies with hardware, resolution, scene and quality.");
        ImGui::PopTextWrapPos();
#ifdef __ANDROID__
        // Reserve the measured right-hand labels and reload markers rather
        // than letting the default input width consume two thirds of the box.
        float label_width = 0.0f;
        for (const CatalogEntry& entry : settings_catalog()) {
            const float marker_width = entry.apply == Apply::Reload
                ? style.ItemSpacing.x + ImGui::CalcTextSize("[reload]").x : 0.0f;
            label_width = std::max(
                label_width, ImGui::CalcTextSize(entry.label).x + marker_width);
        }
        ImGui::PushItemWidth(std::max(
            ImGui::GetFontSize() * 4.0f,
            ImGui::GetContentRegionAvail().x - label_width - style.ItemInnerSpacing.x));
#endif
        // One tab per setting group. The whole editable surface lives in this
        // single window, each group in its own subsection.
        const bool have_resolutions = !resolutions_.empty();
        if (ImGui::BeginTabBar("##settings_tabs", ImGuiTabBarFlags_None)) {
            for (const TabDef& tab : kConfigTabs) {
                ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
                // Force-select the tab the user just opened from the menu bar.
                if (pending_tab_ == tab.intent) {
                    flags |= ImGuiTabItemFlags_SetSelected;
                }
                if (ImGui::BeginTabItem(tab.label, nullptr, flags)) {
                    // The Display tab leads with the Resolution picker (a
                    // predefined dropdown of the monitor's modes + common
                    // windowed sizes) in place of the raw Width/Height fields,
                    // when the Main_Loop has supplied a resolution list.
                    const bool res_combo =
                        tab.panel == Panel::Settings && have_resolutions;
                    if (res_combo) {
                        ImGui::SeparatorText("Display");
                        build_resolution_combo(working, intents);
                        if (const auto impact = catalog_performance_impact(SettingId::Width))
                            draw_performance_impact(*impact);
                    }
                    render_panel_body(tab.panel, working, ui_state, intents,
                                      res_combo,
                                      res_combo ? "Display" : "");
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        // The save/load toolbar and diagnostics apply to the whole
        // configuration, so they sit below the tab bar and stay visible
        // regardless of the active tab. Pure ImGui, raises only intents

        ImGui::Separator();
        build_config_toolbar(ui_state, intents);
#ifdef __ANDROID__
        ImGui::PopItemWidth();
#endif
    }
    ImGui::End();

    // Consume the focus request for this frame.
    pending_tab_ = OverlayIntents::Panel::None;
}

// ── Resolution picker (Display tab) ──────────────────────────────

void OverlayUI::set_available_resolutions(
    std::vector<std::pair<int, int>> resolutions) {
    resolutions_ = std::move(resolutions);
}

void OverlayUI::set_application_build_info(ApplicationBuildInfo info) {
    application_build_info_ = std::move(info);
}

void OverlayUI::build_resolution_combo(const BenchConfig& working,
                                       OverlayIntents&    intents) {
    // A predefined dropdown of the monitor's supported modes plus common
    // windowed sizes (the list is gathered from SDL by the Main_Loop and handed
    // in via set_available_resolutions — OverlayUI stays GPU/SDL-free). Width
    // and Height are both Reload_Settings, so selecting an entry emits an edit
    // for each that diverges from the Working_Configuration; the per-tab
    // dirty/Apply-Reload affordance then offers the reload.
    const int cur_w = working.width;
    const int cur_h = working.height;

    // Preview shows the current Working_Configuration resolution even when it is
    // not one of the predefined entries (e.g. a value typed in via a config
    // file or CLI flag).
    const std::string preview =
        std::to_string(cur_w) + " x " + std::to_string(cur_h);

    if (ImGui::BeginCombo("Resolution", preview.c_str())) {
        for (const auto& [w, h] : resolutions_) {
            const bool selected = (w == cur_w && h == cur_h);
            const std::string item =
                std::to_string(w) + " x " + std::to_string(h);
            if (ImGui::Selectable(item.c_str(), selected)) {
                if (w != cur_w) {
                    emit_edit(intents, SettingId::Width, SettingValue{w});
                }
                if (h != cur_h) {
                    emit_edit(intents, SettingId::Height, SettingValue{h});
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Resolution requires a reload to take effect — mark it like the catalog's
    // Reload settings do.
    ImGui::SameLine();
    ImGui::TextDisabled("[reload]");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Resolution is read once at startup. Use the Apply / Reload button "
            "to rebuild the benchmark so the change takes effect.");
    }
}

// ── Configuration toolbar + diagnostics (Settings panel) ─────────

void OverlayUI::build_config_toolbar(const ConfigUiState& ui_state,
                                     OverlayIntents&      intents) {
    ImGui::SeparatorText("Configuration");

    const std::string seed = ui_state.current_config_name.empty()
        ? std::string("config.json")
        : ui_state.current_config_name;
    if (seed != path_seed_) {
        path_seed_ = seed;
        save_path_ = seed;
        load_path_ = seed;
    }

    // Save: a path InputText + Save button. The
    // builder raises only the save intent and the path; the Main_Loop performs
    // the write through the Config_File module.
    input_text_string("Save path", save_path_);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        intents.save      = true;
        intents.save_path = save_path_;
    }

    // Load: a path InputText + Load button. A non-empty path raises
    // the load intent; an empty path withholds the intent and
    // shows an inline "no file selected" note instead. The
    // note is UI-local — no Config_File diagnostic, no I/O.
    input_text_string("Load path", load_path_);
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (!load_path_.empty()) {
            intents.load      = true;
            intents.load_path = load_path_;
            load_no_file_     = false;
        } else {
            load_no_file_ = true;
        }
    }
    if (load_no_file_) {
        ImGui::TextUnformatted("no file selected");
    }

    // Diagnostics area: render every surfaced Diagnostic_Message in the order
    // the Main_Loop reported them, with a Dismiss button that
    // raises dismiss_diagnostics. When there are no
    // diagnostics and the last operation succeeded, show the success line
    // instead.
    ImGui::SeparatorText("Diagnostics");
    if (!ui_state.diagnostics.empty()) {
        for (const std::string& msg : ui_state.diagnostics) {
            ImGui::TextWrapped("%s", msg.c_str());
        }
        if (ImGui::Button("Dismiss")) {
            intents.dismiss_diagnostics = true;
        }
    } else if (ui_state.success) {
        if (!ui_state.success_detail.empty()) {
            ImGui::Text("Success: %s", ui_state.success_detail.c_str());
        } else {
            ImGui::TextUnformatted("Success");
        }
    } else {
        ImGui::TextDisabled("No diagnostics.");
    }
}

// ── Running overlay  ──────────────────────────────

void OverlayUI::build_running_overlay(const BenchmarkStatus& status) {
    // Scene-cut fade-to-black. The original benchmark snaps the camera at each
    // scene boundary and hides the discontinuity behind a black fade; we
    // reproduce it by painting a fullscreen black quad at the snapshot's fade
    // alpha. Keep it behind the UI so Cancel and Settings remain visible even
    // at the fully black scene boundary.
    if (status.fade_alpha > 0.0f) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImU32 black = ImGui::GetColorU32(
            ImVec4(0.0f, 0.0f, 0.0f, std::clamp(status.fade_alpha, 0.0f, 1.0f)));
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), io.DisplaySize, black);
    }

    // A fixed, non-interactive window pinned to the top-right, clear of the
    // left diagnostics sidebar and below the toolbar. NoInputs keeps it
    // from stealing mouse/keyboard from the scene; the values are rebuilt every
    // frame from the live snapshot so no displayed statistic is more than one
    // frame old.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 20.0f,
               viewport->WorkPos.y + 20.0f),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("##benchmark_running", nullptr, flags)) {
        // The "Benchmarking..." status label.
        ImGui::TextUnformatted(status.diagnostic ? "Diagnostic tour - unscored" : "Benchmarking...");

        // Scene position as (Scene_Index + 1) / Scene_Count.
        ImGui::Text("Scene %zu / %zu",
                    static_cast<size_t>(status.scene_index + 1),
                    status.scene_count);

        ImGui::Separator();

        // The cumulative stats block. Before the first valid
        // frame the snapshot already reports zeros.
        ImGui::Text("Average FPS: %.1f", round_fps(status.average_fps));
        ImGui::Text("Time: %.1f s", status.elapsed);
        ImGui::Text("Frames: %llu",
                    static_cast<unsigned long long>(status.frames));
        ImGui::Text("Min FPS: %.1f",
                    round_fps(static_cast<double>(status.min_fps)));
        ImGui::Text("Max FPS: %.1f",
                    round_fps(static_cast<double>(status.max_fps)));

        ImGui::Separator();

        // Cancel hint. ESC itself is handled in the main loop.
        ImGui::TextUnformatted("Cancel in toolbar or press ESC");
    }
    ImGui::End();
}

// ── Result report  ────────────────────────────────

void OverlayUI::build_result_report(const BenchmarkStatus& status,
                                     OverlayIntents&        intents) {
    // The Result_Report stays visible on top of the scene until dismissed
    // It is a normal interactive window so the dismiss
    // button can be clicked.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Benchmark Result", nullptr, flags)) {
        const HardwareScoreResult& perf = status.result.performance;
        ImGui::TextUnformatted(
            format_application_title(application_build_info_).c_str());
        ImGui::Text("Score version: %d", perf.score_version);
        ImGui::SeparatorText("Scores");
        if (status.result.diagnostic) {
            ImGui::TextUnformatted("Diagnostic run - unscored");
            ImGui::TextWrapped("%s. Frame statistics describe accepted real renders.",
                status.result.diagnostic_reason.empty() ? "Microprofiler recording"
                    : status.result.diagnostic_reason.c_str());
        } else {
            ImGui::Text("Overall: %lld", round_score(perf.scores.overall));
            ImGui::Text("CPU: %lld", round_score(perf.scores.cpu));
            ImGui::Text("GPU: %lld", round_score(perf.scores.gpu));
        }

        ImGui::SeparatorText("Overall");
        ImGui::Text("Average FPS: %.1f",
                    round_fps(perf.metrics.average_fps));
        ImGui::Text("Path geomean: %.1f FPS",
                    round_fps(perf.metrics.path_geomean_fps));
        ImGui::Text("1%% low: %.1f FPS",
                    round_fps(perf.metrics.one_percent_low_fps));
        ImGui::Text("Effective FPS: %.1f",
                    round_fps(perf.metrics.effective_fps));

        ImGui::SeparatorText("CPU");
        ImGui::Text("Update rate: %.1f/s",
                    perf.metrics.cpu.updates_per_second);
        ImGui::Text("Real-time speed: %.1fx",
                    perf.metrics.cpu.updates_per_second /
                        kCpuSimulationRate);
        ImGui::Text("Average / p95 / p99: %.3f / %.3f / %.3f ms",
                    perf.metrics.cpu.timing.average_ms,
                    perf.metrics.cpu.timing.p95_ms,
                    perf.metrics.cpu.timing.p99_ms);

        ImGui::SeparatorText("GPU");
        ImGui::Text("Equivalent FPS: %.1f",
                    round_fps(perf.metrics.gpu_equivalent_fps));
        ImGui::Text("Average / p95 / p99: %.3f / %.3f / %.3f ms",
                    perf.metrics.gpu.average_ms,
                    perf.metrics.gpu.p95_ms,
                    perf.metrics.gpu.p99_ms);

        // The labelled System block lists Platform / CPU / GPU
        // The overlay performs no gathering, formatting, or
        // placeholder logic: the snapshot strings are rendered straight, already
        // carrying any Unknown_Value_Placeholder substituted at gather time.
        ImGui::SeparatorText("System");
        ImGui::TextUnformatted(status.result.system.platform.c_str());
        ImGui::TextUnformatted(status.result.system.cpu.c_str());
        ImGui::TextUnformatted(status.result.system.gpu.c_str());

        // The labelled Settings block lists Render API / Display mode / Quality
        // preset, likewise read straight from the snapshot.
        ImGui::SeparatorText("Settings");
        ImGui::TextUnformatted(status.result.settings.render_api.c_str());
        ImGui::TextUnformatted(status.result.settings.display_mode.c_str());
        ImGui::TextUnformatted(status.result.settings.quality_preset.c_str());

        ImGui::Separator();

        // Dismiss removes the report within one frame; the
        // caller maps this to BenchmarkSystem::dismiss_result().
        if (ImGui::Button("Dismiss")) {
            intents.dismiss_result = true;
        }
    }
    ImGui::End();
}

} // namespace heaven
