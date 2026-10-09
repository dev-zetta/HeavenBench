/* Exercise real widgets and immutable freeze/save intents without a GPU. */
#include "rhi/overlay/microprofiler_ui.h"
#include "rhi/overlay/overlay_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
bool check(bool value, const char* message) {
    if (!value) std::printf("[unit_microprofiler_ui] %s\n", message);
    return value;
}
heaven::MicroprofileIntents frame(heaven::MicroprofilerUI& ui, const char* activate = nullptr) {
    if (activate) {
        const ImGuiWindow* window = ImGui::FindWindowByName("Microprofiler");
        if (window) ImGui::ActivateItemByID(ImHashStr(activate, 0, window->ID));
    }
    ImGui::NewFrame();
    auto intents = ui.draw();
    ImGui::Render();
    return intents;
}
const ImGuiWindow* child_window(const ImGuiWindow* parent, const char* label) {
    const ImGuiID id = ImHashStr(label, 0, parent->ID);
    for (const ImGuiWindow* candidate : ImGui::GetCurrentContext()->Windows)
        if (candidate->ParentWindow == parent && candidate->ChildId == id) return candidate;
    return nullptr;
}
bool visible_timing_graphs(const ImGuiWindow* window, float minimum_cpu_rows = 8.0f) {
    if (!window) return false;
    const float font = window->FontRefSize;
    const ImGuiWindow* cpu = child_window(window, "##cpu_timeline");
    float graph_top = std::numeric_limits<float>::max();
    float graph_bottom = 0.0f;
    for (const ImDrawVert& vertex : window->DrawList->VtxBuffer) {
        if (vertex.col != IM_COL32(75, 180, 105, 255)) continue;
        graph_top = std::min(graph_top, vertex.pos.y);
        graph_bottom = std::max(graph_bottom, vertex.pos.y);
    }
    if (!cpu || graph_top < window->InnerClipRect.Min.y ||
        graph_bottom > window->InnerClipRect.Max.y || graph_bottom - graph_top < font) return false;
    float tasks_top = std::numeric_limits<float>::max();
    float tasks_bottom = 0.0f;
    for (const ImDrawVert& vertex : cpu->DrawList->VtxBuffer) {
        const auto red = (vertex.col >> IM_COL32_R_SHIFT) & 255u;
        const auto green = (vertex.col >> IM_COL32_G_SHIFT) & 255u;
        const auto blue = (vertex.col >> IM_COL32_B_SHIFT) & 255u;
        // Filled task bars are colored; labels, grid lines and backgrounds are neutral.
        if ((vertex.col & IM_COL32_A_MASK) != IM_COL32_A_MASK ||
            std::min({red, green, blue}) < 60u || std::max({red, green, blue}) > 230u ||
            std::max({red, green, blue}) - std::min({red, green, blue}) < 10u) continue;
        if (!cpu->InnerClipRect.Contains(vertex.pos)) continue;
        tasks_top = std::min(tasks_top, vertex.pos.y);
        tasks_bottom = std::max(tasks_bottom, vertex.pos.y);
    }
    const float visible_height = std::min(cpu->InnerClipRect.Max.y, window->InnerClipRect.Max.y) -
        std::max(cpu->InnerClipRect.Min.y, window->InnerClipRect.Min.y);
    std::printf("[unit_microprofiler_ui] visible frame graph %.1f..%.1f, CPU %.1fpx, nested task bars %.1fpx\n",
        graph_top, graph_bottom, visible_height, tasks_bottom - tasks_top);
    return cpu->Active && cpu->InnerClipRect.Min.y > graph_bottom && visible_height >= minimum_cpu_rows * font &&
        tasks_bottom - tasks_top >= 2.5f * font;
}
bool microprofiler_ui() {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::GetStyle().FontScaleMain = 1.5f;
    io.DeltaTime = 1.0f / 60.0f;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui's API requires unsigned char**.
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    heaven::MicroprofilerUI ui;
    auto snapshot = std::make_shared<heaven::profile::Snapshot>();
    snapshot->session_id = 1;
    snapshot->dropped_events = 3;
    snapshot->overwritten_events = 5;
    snapshot->stale_epoch_events = 67;
    snapshot->begin_ns = 1000;
    snapshot->end_ns = 10'001'000;
    snapshot->threads = {{1, "Main"}, {2, "Worker"}};
    heaven::profile::Span parent;
    parent.id = 1; parent.thread_id = 1; parent.begin_ns = 1000; parent.end_ns = 9'001'000;
    parent.name = "Frame"; parent.category = "CPU";
    parent.context.attempt_id = 10;
    parent.context.capture_id = 1;
    auto child = parent;
    child.id = 2; child.parent_id = 1; child.begin_ns = 1'001'000; child.end_ns = 3'001'000;
    child.name = "Nested task";
    snapshot->spans = {parent, child};
    heaven::profile::Frame captured;
    captured.begin_ns = 1000; captured.end_ns = 10'001'000;
    captured.outcome = heaven::profile::FrameOutcome::Accepted;
    captured.context.attempt_id = 10;
    captured.context.capture_id = 1;
    auto foreign_capture = captured;
    foreign_capture.context.attempt_id = 11;
    foreign_capture.context.capture_id = 2;
    snapshot->frames = {captured, foreign_capture};
    heaven::profile::GpuSample unsupported;
    unsupported.name = "GPU unavailable";
    unsupported.capability = "Native timestamp unavailable";
    unsupported.context = {10, 20, 30, 40, 50, 1};
    auto available = unsupported;
    available.name = "GPU measured";
    available.status = heaven::profile::SampleStatus::Available;
    available.source = heaven::profile::TimerSource::HardwareTimestamp;
    available.end_ns = 1'000'000;
    available.command_count = 7;
    available.primitive_count = 11;
    available.visible_renderables = 13;
    available.draw_instance_invocations = 17;
    auto later = available;
    later.name = "GPU later frame";
    later.context = {11, 21, 31, 41, 51, 1};
    snapshot->gpu_samples = {available, unsupported, later};
    ui.set_snapshot(snapshot);
    bool ok = check(!frame(ui).start && !ui.is_open(), "closed viewer emitted recording intent");
    ui.toggle();
    for (int i = 0; i < 3; ++i) {
        const auto intents = frame(ui);
        ok = check(!intents.start && !intents.stop && !intents.save,
                   "opening viewer must not record") && ok;
    }
    ImGuiWindow* profiler_window = ImGui::FindWindowByName("Microprofiler");
    ok = check(visible_timing_graphs(profiler_window),
               "1280x720 default view did not show the frame graph and nested CPU tasks") && ok;
    const std::string saved_path = "Saved /a/long/output/path/that/must/not/consume/the/initial/timing/viewport/trace";
    ui.set_snapshot(snapshot, saved_path);
    frame(ui);
    ok = check(visible_timing_graphs(profiler_window),
               "successful export feedback pushed timing graphs out of view") && ok;
    // Both the full saved path and the informational stale count remain available
    // on touch without requiring a hover, behind collapsed sections.
    profiler_window->StateStorage.SetInt(ImHashStr("Capture options", 0, profiler_window->ID), 1);
    profiler_window->StateStorage.SetInt(ImHashStr("Help", 0, profiler_window->ID), 1);
    profiler_window->StateStorage.SetInt(ImHashStr("GPU timing samples (separate GPU-relative clock)",
        0, profiler_window->ID), 1);
    io.DisplaySize = ImVec2(1280.0f, 1800.0f);
    ImGui::GetStyle().FontScaleMain = 1.0f;
    ImGui::SetWindowSize("Microprofiler", ImVec2(1200.0f, 1700.0f));
    frame(ui);
    frame(ui, "Previous GPU frame");
    ImGui::NewFrame();
    ImGui::LogToClipboard(10);
    const auto inspected = ui.draw();
    ImGui::LogFinish();
    ImGui::Render();
    const char* detail_text = ImGui::GetClipboardText();
    ok = check(!inspected.start && detail_text &&
               std::strstr(detail_text, "Dropped 3 | overwritten 5") &&
               std::strstr(detail_text, "Ignored old-capture events 67 (not current event loss)") &&
               std::strstr(detail_text, saved_path.c_str()) &&
               std::strstr(detail_text, "GPU frame 30 / renderer 40") &&
               std::strstr(detail_text, "Commands 7 | primitives 11 | visible renderables 13") &&
               std::strstr(detail_text, "Draw-instance invocations 17 (sum over draws, not unique placements)"),
               "viewer lost separate stale-capture count or actual GPU workload details") && ok;
    const std::string export_error = "Export failed: output directory is not writable";
    ui.set_snapshot(snapshot, export_error);
    ImGui::NewFrame();
    ImGui::LogToClipboard(10);
    const auto failed_export = ui.draw();
    ImGui::LogFinish();
    ImGui::Render();
    const char* error_text = ImGui::GetClipboardText();
    ok = check(!failed_export.save && error_text && std::strstr(error_text, export_error.c_str()),
               "export error was hidden or replaced by successful-export feedback") && ok;
    ui.set_snapshot(snapshot);
    frame(ui);
    const ImGuiWindow* gpu_window = child_window(profiler_window, "##gpu_timeline");
    bool bar_seen = false;
    bool bar_inside = true;
    if (gpu_window) {
        for (const ImDrawVert& vertex : gpu_window->DrawList->VtxBuffer) {
            if (vertex.col != IM_COL32(80, 175, 220, 255)) continue;
            bar_seen = true;
            bar_inside &= vertex.pos.x >= gpu_window->InnerRect.Min.x && vertex.pos.x <= gpu_window->InnerRect.Max.x;
        }
    }
    ok = check(gpu_window && bar_seen && bar_inside,
               "GPU frame-relative bars were absent or escaped the child viewport") && ok;
    if (gpu_window) {
        const int ordinal = 1;
        const ImGuiID row_id = ImHashStr("##gpu_pass", 0, ImHashData(&ordinal, sizeof(ordinal), gpu_window->ID));
        ImGui::ActivateItemByID(row_id);
        frame(ui);
        ImGui::NewFrame();
        ImGui::LogToClipboard(10);
        const auto unavailable = ui.draw();
        ImGui::LogFinish();
        ImGui::Render();
        const char* unavailable_text = ImGui::GetClipboardText();
        ok = check(!unavailable.start && unavailable_text &&
                   std::strstr(unavailable_text, "GPU unavailable: unsupported") &&
                   std::strstr(unavailable_text, "Native timestamp unavailable"),
                   "selecting an unavailable GPU row lost status/capability") && ok;
    }
    frame(ui, "Focus linked CPU attempt");
    ImGui::NewFrame();
    ImGui::LogToClipboard(10);
    const auto linked = ui.draw();
    ImGui::LogFinish();
    ImGui::Render();
    const char* linked_text = ImGui::GetClipboardText();
    ok = check(!linked.start && linked_text &&
               std::strstr(linked_text, "CPU view attempt 10 (identity link only; separate clock)"),
               "GPU pass selection failed its retained CPU-attempt identity link") && ok;
    frame(ui, "Next GPU frame");
    frame(ui, "Focus linked CPU attempt");
    ImGui::NewFrame();
    ImGui::LogToClipboard(10);
    const auto foreign = ui.draw();
    ImGui::LogFinish();
    ImGui::Render();
    const char* foreign_text = ImGui::GetClipboardText();
    ok = check(!foreign.start && foreign_text &&
               std::strstr(foreign_text, "GPU frame 31 / renderer 41") &&
               std::strstr(foreign_text, "CPU view attempt 10") &&
               std::strstr(foreign_text, "shown GPU frame is independently selected") &&
               !std::strstr(foreign_text, "CPU view attempt 11"),
               "GPU link joined the same attempt ID from a different capture") && ok;
    auto unlinked = std::make_shared<heaven::profile::Snapshot>(*snapshot);
    unlinked->gpu_samples = {later};
    ui.set_snapshot(unlinked);
    frame(ui);
    ImGui::NewFrame();
    ImGui::LogToClipboard(10);
    const auto unlinked_intent = ui.draw();
    ImGui::LogFinish();
    ImGui::Render();
    const char* unlinked_text = ImGui::GetClipboardText();
    ok = check(!unlinked_intent.start && unlinked_text &&
               std::strstr(unlinked_text, "No linked GPU frame retained for the selected CPU attempt") &&
               std::strstr(unlinked_text, "GPU frame 31 / renderer 41"),
               "unlinked CPU attempt silently displayed an old GPU frame as correlated") && ok;

    // A manual pass selection must survive the periodic publication of a fresh
    // snapshot, even when the user started in the default latest-frame mode.
    auto inspecting = std::make_shared<heaven::profile::Snapshot>(*snapshot);
    auto second_pass = later;
    second_pass.name = "GPU selected measured pass";
    second_pass.begin_ns = 1'000'000;
    second_pass.end_ns = 2'000'000;
    auto unavailable_pass = unsupported;
    unavailable_pass.context = later.context;
    unavailable_pass.name = "GPU selected unavailable pass";
    inspecting->gpu_samples.push_back(second_pass);
    inspecting->gpu_samples.push_back(unavailable_pass);
    ui.set_snapshot(inspecting);
    frame(ui, "Latest GPU frame");
    const auto select_gpu_row = [&](int ordinal) {
        const ImGuiWindow* timeline = child_window(profiler_window, "##gpu_timeline");
        if (!timeline) return false;
        ImGui::ActivateItemByID(ImHashStr("##gpu_pass", 0,
            ImHashData(&ordinal, sizeof(ordinal), timeline->ID)));
        frame(ui);
        return true;
    };
    const auto gpu_details_contain = [&](const char* pass, const char* frame_identity) {
        ImGui::NewFrame();
        ImGui::LogToClipboard(10);
        const auto intents = ui.draw();
        ImGui::LogFinish();
        ImGui::Render();
        const char* details = ImGui::GetClipboardText();
        return !intents.start && !intents.stop && !intents.save && details &&
            std::strstr(details, pass) && std::strstr(details, frame_identity);
    };
    ok = check(select_gpu_row(1) && gpu_details_contain("GPU selected measured pass: available",
                   "GPU frame 31 / renderer 41"),
               "manual measured GPU pass selection did not update details") && ok;
    inspecting = std::make_shared<heaven::profile::Snapshot>(*inspecting);
    ui.set_snapshot(inspecting);
    ok = check(gpu_details_contain("GPU selected measured pass: available", "GPU frame 31 / renderer 41"),
               "fresh equivalent snapshot reset the manually selected measured GPU pass") && ok;
    frame(ui, "Latest GPU frame");
    ok = check(select_gpu_row(2) && gpu_details_contain("GPU selected unavailable pass: unsupported",
                   "GPU frame 31 / renderer 41"),
               "manual unavailable GPU row selection did not update details") && ok;
    inspecting = std::make_shared<heaven::profile::Snapshot>(*inspecting);
    ui.set_snapshot(inspecting);
    ok = check(gpu_details_contain("GPU selected unavailable pass: unsupported", "GPU frame 31 / renderer 41"),
               "fresh equivalent snapshot reset the manually selected unavailable GPU row") && ok;
    auto newest = later;
    newest.name = "GPU newest frame";
    newest.context = {12, 22, 32, 42, 52, 1};
    inspecting = std::make_shared<heaven::profile::Snapshot>(*inspecting);
    inspecting->gpu_samples.push_back(newest);
    ui.set_snapshot(inspecting);
    ok = check(gpu_details_contain("GPU selected unavailable pass: unsupported", "GPU frame 31 / renderer 41"),
               "a newer GPU frame displaced the manually pinned pass") && ok;
    frame(ui, "Latest GPU frame");
    newest.context = {13, 23, 33, 43, 53, 1};
    inspecting = std::make_shared<heaven::profile::Snapshot>(*inspecting);
    inspecting->gpu_samples.push_back(newest);
    ui.set_snapshot(inspecting);
    ok = check(gpu_details_contain("GPU newest frame: available", "GPU frame 33 / renderer 43"),
               "Latest GPU frame did not restore automatic following after manual pass selection") && ok;
    ui.set_snapshot(snapshot);
    const auto record = frame(ui, "Record");
    ok = check(record.start && !record.stop && record.duration_seconds == 10.0f &&
               record.output_base.empty(), "Record intent missing/automatic path default incorrect") && ok;
    auto active = std::make_shared<heaven::profile::Snapshot>(*snapshot);
    active->active = true;
    ui.set_snapshot(active);
    frame(ui);
    const auto freeze = frame(ui, "Freeze view");
    ok = check(ui.frozen() && !freeze.stop, "freezing view must not stop recording") && ok;
    auto newer = std::make_shared<heaven::profile::Snapshot>(*active);
    newer->end_ns += 10'000'000;
    ui.set_snapshot(newer);
    frame(ui);
    const auto save = frame(ui, "Save trace");
    ok = check(save.save && save.snapshot == active && ui.recording(),
               "Save did not retain exactly the frozen immutable view") && ok;
    const auto stop = frame(ui, "Stop");
    ok = check(stop.stop && !stop.start, "Stop intent was not emitted") && ok;
    ui.set_open(false);
    const auto closed = frame(ui);
    ok = check(ui.recording() && !closed.stop && !closed.start,
               "closing viewer must not implicitly stop recorder") && ok;
    ui.set_recording(false);
    if (!check(!ui.recording(), "closed viewer retained stale recording indicator")) ok = false;
    ui.set_recording(true);
    ui.set_open(true);
    frame(ui);
    frame(ui, "Unfreeze view");
    const auto live_save = frame(ui, "Save trace");
    ok = check(!ui.frozen() && live_save.snapshot == newer,
               "unfreeze did not restore live snapshot") && ok;

    heaven::OverlayUI overlay;
    overlay.set_microprofiler_snapshot(newer);
    ImGui::NewFrame();
    ImGui::LogToClipboard();
    const auto intents = overlay.build({}, {}, {});
    ImGui::LogFinish();
    ImGui::Render();
    const char* text = ImGui::GetClipboardText();
    ok = check(!overlay.microprofiler_open() && !intents.microprofile.start &&
               text && std::strstr(text, "Diagnostics") && std::strstr(text, "Recording"),
               "touch toolbar lost Diagnostics or recording indicator while panel closed") && ok;
    overlay.toggle_microprofiler();
    ImGui::NewFrame();
    const auto only = overlay.build_microprofiler();
    ImGui::Render();
    ok = check(overlay.microprofiler_open() && !only.microprofile.start,
               "F6/CLI-only viewer opening unexpectedly recorded") && ok;
    // A touch-sized initial view must show actual graph/task bars, not merely
    // buttons that can be activated while the timing data remains below the fold.
    io.DisplaySize = ImVec2(640.0f, 360.0f);
    ImGui::GetStyle().ScaleAllSizes(1.5f);
    ImGui::GetStyle().FontScaleMain = 1.5f;
    heaven::MicroprofilerUI small;
    small.set_snapshot(newer);
    small.set_open(true);
    profiler_window->StateStorage.Clear();
    ImGui::SetScrollY(profiler_window, 0.0f);
    for (int i = 0; i < 3; ++i) frame(small);
    const ImGuiWindow* window = ImGui::FindWindowByName("Microprofiler");
    if (window) std::printf("[unit_microprofiler_ui] small viewport: pos %.1f %.1f size %.1f %.1f content %.1f inner %.1f\n",
        window->Pos.x, window->Pos.y, window->Size.x, window->Size.y, window->ContentSize.x, window->InnerRect.GetWidth());
    ok = check(window && window->Active && window->Pos.x >= 0.0f && window->Pos.y >= 0.0f &&
               window->Rect().Max.x <= io.DisplaySize.x && window->Rect().Max.y <= io.DisplaySize.y &&
               window->ContentSize.x <= window->InnerRect.GetWidth() + 2.0f,
               "small touch viewport clipped profiler content horizontally") && ok;
    ok = check(visible_timing_graphs(window, 3.0f),
               "small touch viewport did not show the frame graph and nested CPU tasks") && ok;
    const auto small_stop = frame(small, "Stop");
    ok = check(small_stop.stop, "touch-sized panel lost Stop control") && ok;
    const auto small_freeze = frame(small, "Freeze view");
    const auto small_save = frame(small, "Save trace");
    ok = check(!small_freeze.stop && small.frozen() && small_save.save && small_save.snapshot == newer,
               "touch-sized panel lost immutable Freeze/Save controls") && ok;
    heaven::MicroprofilerUI focused;
    focused.set_snapshot(newer);
    focused.set_open(true);
    ImGui::NewFrame();
    overlay.draw_debug_sidebar({}); // Match main's first-frame sidebar-before-widgets order.
    const auto focus_intent = focused.draw();
    ImGui::Render();
    const ImGuiWindow* sidebar = ImGui::FindWindowByName("Diagnostics Sidebar");
    const ImGuiWindow* front = ImGui::FindWindowByName("Microprofiler");
    const auto display_index = [&](const ImGuiWindow* candidate) {
        for (int i = 0; i < context->Windows.Size; ++i)
            if (context->Windows[i] == candidate) return i;
        return -1;
    };
    ok = check(!focus_intent.start && context->NavWindow == front &&
               display_index(front) > display_index(sidebar),
               "programmatic profiler opening stayed behind the newly created sidebar") && ok;
    ImGui::NewFrame();
    ImGui::SetWindowFocus("Diagnostics Sidebar");
    overlay.draw_debug_sidebar({});
    focused.set_open(true); // The Recording indicator also raises an already-open panel.
    const auto unfocused_intent = focused.draw();
    ImGui::Render();
    ok = check(!unfocused_intent.start && context->NavWindow == front,
               "an explicit open request failed to raise the already-open profiler") && ok;
    ImGui::NewFrame();
    ImGui::SetWindowFocus("Diagnostics Sidebar");
    overlay.draw_debug_sidebar({});
    const auto later_focus_intent = focused.draw();
    ImGui::Render();
    ok = check(!later_focus_intent.start && context->NavWindow == sidebar,
               "the profiler continued stealing focus after its one-shot request") && ok;
    focused.set_open(false);
    frame(focused);
    ok = check(context->NavWindow == sidebar, "closing the profiler requested window focus") && ok;
    focused.toggle();
    frame(focused);
    ok = check(context->NavWindow == front, "reopening the profiler did not request focus") && ok;

    io.DisplaySize = ImVec2(1280.0f, 1800.0f);
    ImGui::GetStyle().FontScaleMain = 1.0f;
    ImGui::SetWindowSize("Microprofiler", ImVec2(1200.0f, 1700.0f));
    profiler_window->StateStorage.Clear();
    ImGui::SetScrollY(profiler_window, 0.0f);
    heaven::MicroprofilerUI dense_ui;
    dense_ui.set_open(true);
    for (const bool wide_labels : {false, true}) {
        auto dense = std::make_shared<heaven::profile::Snapshot>(*snapshot);
        dense->session_id = wide_labels ? 91 : 90;
        dense->threads = {{1, "Main"}};
        dense->spans.clear();
        for (std::uint64_t i = 0; i < 12'000; ++i) {
            auto task = parent;
            task.id = i + 1;
            if (wide_labels) task.name = "A long named task with a visible label";
            else {
                task.begin_ns = 1000 + i * 750;
                task.end_ns = task.begin_ns + 500;
            }
            dense->spans.push_back(std::move(task));
        }
        dense_ui.set_snapshot(dense);
        for (int i = 0; i < 3; ++i) frame(dense_ui);
        const ImGuiWindow* dense_cpu = child_window(profiler_window, "##cpu_timeline");
        bool indices_valid = dense_cpu && dense_cpu->DrawList->VtxBuffer.Size < 65'536;
        if (dense_cpu) {
            for (const ImDrawIdx index : dense_cpu->DrawList->IdxBuffer)
                indices_valid &= std::cmp_less(index, dense_cpu->DrawList->VtxBuffer.Size);
            for (const ImDrawCmd& command : dense_cpu->DrawList->CmdBuffer)
                indices_valid &= command.VtxOffset == 0;
            std::printf("[unit_microprofiler_ui] dense %s: %d vertices, %d indices\n",
                wide_labels ? "labels" : "tiny bars", dense_cpu->DrawList->VtxBuffer.Size,
                dense_cpu->DrawList->IdxBuffer.Size);
        }
        ok = check(indices_valid, "dense CPU timeline exceeded its 16-bit draw-list vertex/index budget") && ok;
    }
    ImGui::DestroyContext(context);
    return ok;
}
const heaven::testing::Register registered{microprofiler_ui};
}
