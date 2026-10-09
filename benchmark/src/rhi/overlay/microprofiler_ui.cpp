#include "rhi/overlay/microprofiler_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heaven {
namespace {
constexpr double kNsPerMs = 1'000'000.0;
constexpr std::size_t kDrawLimit = 12'000;

int resize_text(ImGuiInputTextCallbackData* data) {
    auto* text = static_cast<std::string*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}
void continue_row_for(const char* label) {
    const auto& style = ImGui::GetStyle();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() -
                        style.WindowPadding.x - style.ScrollbarSize;
    const float needed = ImGui::CalcTextSize(label).x + 2.0f * style.FramePadding.x;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + needed <= right) ImGui::SameLine();
}
bool edit_text(const char* label, std::string& value) {
    return ImGui::InputText(label, value.data(), value.capacity() + 1,
                           ImGuiInputTextFlags_CallbackResize, resize_text, &value);
}
double duration(std::uint64_t begin, std::uint64_t end) {
    return end >= begin ? static_cast<double>(end - begin) : 0.0;
}
ImU32 category_color(std::string_view category) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : category) hash = (hash ^ c) * 16777619u;
    return IM_COL32(70u + (hash & 127u), 70u + ((hash >> 8u) & 127u),
                    70u + ((hash >> 16u) & 127u), 255);
}
const char* outcome_name(profile::FrameOutcome value) {
    switch (value) {
        case profile::FrameOutcome::Accepted: return "accepted";
        case profile::FrameOutcome::Rejected: return "rejected";
        case profile::FrameOutcome::Cancelled: return "cancelled";
        case profile::FrameOutcome::Pending: return "pending";
    }
    return "unknown";
}
const char* sample_status(profile::SampleStatus value) {
    switch (value) {
        case profile::SampleStatus::Available: return "available";
        case profile::SampleStatus::Pending: return "pending";
        case profile::SampleStatus::Unsupported: return "unsupported";
        case profile::SampleStatus::Disjoint: return "disjoint";
        case profile::SampleStatus::Error: return "error";
        case profile::SampleStatus::Dropped: return "dropped";
    }
    return "unknown";
}
bool matches(const profile::Span& span, std::string_view search) {
    return search.empty() || span.name.find(search) != std::string::npos ||
           span.category.find(search) != std::string::npos;
}
void context_details(const profile::Context& context) {
    ImGui::Text("Attempt %llu | submission %llu | Filament frame %llu",
        static_cast<unsigned long long>(context.attempt_id),
        static_cast<unsigned long long>(context.submission_id),
        static_cast<unsigned long long>(context.filament_frame_id));
    ImGui::Text("Renderer %llu | view %llu",
        static_cast<unsigned long long>(context.renderer_id),
        static_cast<unsigned long long>(context.view_id));
}
} // namespace

void MicroprofilerUI::set_snapshot(std::shared_ptr<const profile::Snapshot> snapshot,
                                   std::string feedback) {
    live_ = std::move(snapshot);
    feedback_ = std::move(feedback);
}

void MicroprofilerUI::fit(const profile::Snapshot& snapshot) {
    view_begin_ns_ = 0.0;
    view_duration_ns_ = std::max(kNsPerMs, duration(snapshot.begin_ns, snapshot.end_ns));
}
void MicroprofilerUI::clamp_view(const profile::Snapshot& snapshot) {
    const double total = std::max(kNsPerMs, duration(snapshot.begin_ns, snapshot.end_ns));
    view_duration_ns_ = std::clamp(view_duration_ns_, 1'000.0, total);
    view_begin_ns_ = std::clamp(view_begin_ns_, 0.0, total - view_duration_ns_);
}

MicroprofileIntents MicroprofilerUI::draw() {
    MicroprofileIntents intents;
    if (!open_) return intents;
    const auto snapshot = frozen_ ? frozen_ : live_;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float font = ImGui::GetFontSize();
    if (viewport) {
        const ImVec2 size(std::min(viewport->WorkSize.x, 78.0f * font),
                          std::min(viewport->WorkSize.y, 45.0f * font));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(viewport->WorkSize.x, 24.0f * font),
                   std::min(viewport->WorkSize.y, 12.0f * font)), viewport->WorkSize);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_FirstUseEver);
#ifdef __ANDROID__
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
#endif
    }
    if (focus_requested_) {
        ImGui::SetNextWindowFocus();
        focus_requested_ = false;
    }
    if (!ImGui::Begin("Microprofiler", &open_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return intents;
    }
    ImGui::BeginDisabled(recording());
    if (ImGui::Button("Record")) { intents.start = true; frozen_.reset(); follow_ = true; }
    ImGui::EndDisabled();
    continue_row_for("Stop");
    ImGui::BeginDisabled(!recording());
    if (ImGui::Button("Stop")) intents.stop = true;
    ImGui::EndDisabled();
    continue_row_for(frozen_ ? "Unfreeze view" : "Freeze view");
    ImGui::BeginDisabled(!snapshot);
    if (ImGui::Button(frozen_ ? "Unfreeze view" : "Freeze view")) {
        if (frozen_) frozen_.reset();
        else frozen_ = live_;
        follow_ = false;
    }
    ImGui::EndDisabled();
    continue_row_for("Save trace");
    ImGui::BeginDisabled(!snapshot);
    if (ImGui::Button("Save trace")) { intents.save = true; intents.snapshot = snapshot; }
    ImGui::EndDisabled();
    const bool compact = ImGui::GetWindowHeight() < 25.0f * font;
    if (snapshot && viewed_session_ != snapshot->session_id) {
        viewed_session_ = snapshot->session_id;
        selected_span_ = 0;
        selected_cpu_attempt_ = 0;
        selected_gpu_frame_.reset();
        gpu_follow_latest_ = true;
        fit(*snapshot);
    } else if (snapshot && follow_ && !frozen_) fit(*snapshot);
    if (snapshot) {
        clamp_view(*snapshot);
        refresh_layout(snapshot);
        if (compact) ImGui::TextWrapped("%s | CPU %zu | frames %zu | GPU %zu | loss %llu/%llu/%llu",
            recording() ? "REC" : "Stopped", snapshot->spans.size(), snapshot->frames.size(),
            snapshot->gpu_samples.size(), static_cast<unsigned long long>(snapshot->dropped_events),
            static_cast<unsigned long long>(snapshot->overwritten_events),
            static_cast<unsigned long long>(snapshot->truncated_labels));
        else {
            ImGui::TextWrapped("%s | %zu CPU spans | %zu frames | %zu GPU samples | %.2f MiB",
                recording() ? "Recording (diagnostic)" : "Not recording", snapshot->spans.size(),
                snapshot->frames.size(), snapshot->gpu_samples.size(),
                static_cast<double>(snapshot->memory_bytes) / (1024.0 * 1024.0));
            ImGui::TextWrapped("Dropped %llu | overwritten %llu | truncated labels %llu",
                static_cast<unsigned long long>(snapshot->dropped_events),
                static_cast<unsigned long long>(snapshot->overwritten_events),
                static_cast<unsigned long long>(snapshot->truncated_labels));
        }
    } else ImGui::TextUnformatted(recording() ? "Recording (diagnostic)" : "No trace captured. Use Record.");
    const bool export_feedback = feedback_.starts_with("Saved ") || feedback_.starts_with("Saving ");
    if (!feedback_.empty()) {
        if (export_feedback) {
            if (feedback_.starts_with("Saved ")) ImGui::TextUnformatted("Saved trace");
            else if (feedback_.ends_with('%')) ImGui::TextUnformatted(feedback_.c_str());
            else ImGui::TextUnformatted("Saving trace...");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", feedback_.c_str());
        } else ImGui::TextWrapped("%s", feedback_.c_str()); // Errors must remain visible.
    }
    if (snapshot) {
        draw_frames(*snapshot);
        draw_timeline(*snapshot);
    }
    if (ImGui::CollapsingHeader("Capture options")) {
        ImGui::SetNextItemWidth(std::min(12.0f * font, ImGui::GetContentRegionAvail().x * 0.5f));
        ImGui::InputFloat("History (s)", &duration_seconds_, 1.0f, 10.0f, "%.1f");
        ImGui::TextUnformatted("Output base (empty: automatic)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        edit_text("##output_base", output_base_);
        if (export_feedback) ImGui::TextWrapped("%s", feedback_.c_str());
    }
    if (!std::isfinite(duration_seconds_)) duration_seconds_ = 10.0f;
    duration_seconds_ = std::clamp(duration_seconds_, 0.1f, 3600.0f);
    if (intents.start || intents.save) {
        intents.output_base = output_base_;
        intents.duration_seconds = duration_seconds_;
    }
    if (ImGui::CollapsingHeader("Help")) {
        ImGui::TextWrapped("Opening this panel does not record. Any benchmark tour recorded at any point is unscored.");
        ImGui::TextWrapped("Incomplete intervals and missing events are not measured CPU cost.");
        ImGui::TextWrapped("CPU timeline: wheel zooms, dragging pans, and clicking a task selects it.");
        ImGui::TextWrapped("Compact loss counts are dropped events / overwritten events / truncated labels.");
        if (snapshot) ImGui::TextWrapped("Ignored old-capture events %llu (not current event loss)",
            static_cast<unsigned long long>(snapshot->stale_epoch_events));
    }
    if (!snapshot) {
        ImGui::End();
        return intents;
    }
    if (ImGui::CollapsingHeader("View controls")) {
        ImGui::Checkbox("Follow live history", &follow_);
        continue_row_for("Fit history");
        if (ImGui::Button("Fit history")) { fit(*snapshot); follow_ = false; }
        continue_row_for("Zoom in");
        if (ImGui::Button("Zoom in")) { view_begin_ns_ += view_duration_ns_ * 0.25; view_duration_ns_ *= 0.5; follow_ = false; }
        continue_row_for("Zoom out");
        if (ImGui::Button("Zoom out")) { view_begin_ns_ -= view_duration_ns_ * 0.5; view_duration_ns_ *= 2.0; follow_ = false; }
        clamp_view(*snapshot);
        float pan = static_cast<float>(view_begin_ns_ / kNsPerMs);
        const float max_pan = static_cast<float>(std::max(0.0,
            duration(snapshot->begin_ns, snapshot->end_ns) - view_duration_ns_) / kNsPerMs);
        if (ImGui::SliderFloat("Pan (ms)", &pan, 0.0f, max_pan, "%.3f")) {
            view_begin_ns_ = static_cast<double>(pan) * kNsPerMs;
            follow_ = false;
        }
        ImGui::SetNextItemWidth(std::min(16.0f * font, ImGui::GetContentRegionAvail().x * 0.5f));
        edit_text("Search tasks/category", search_);
        if (ImGui::BeginMenu("Thread filters")) {
            for (const auto& thread : snapshot->threads) {
                bool shown = !hidden_threads_.contains(thread.id);
                const std::string label = thread.name + "##" + std::to_string(thread.id);
                if (ImGui::MenuItem(label.c_str(), nullptr, &shown)) {
                    if (shown) hidden_threads_.erase(thread.id); else hidden_threads_.insert(thread.id);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::SameLine();
        if (ImGui::BeginMenu("Category filters")) {
            std::set<std::string> categories;
            for (const auto& span : snapshot->spans) categories.insert(span.category);
            for (const auto& category : categories) {
                bool shown = !hidden_categories_.contains(category);
                if (ImGui::MenuItem(category.c_str(), nullptr, &shown)) {
                    if (shown) hidden_categories_.erase(category); else hidden_categories_.insert(category);
                }
            }
            ImGui::EndMenu();
        }
    }
    draw_details(*snapshot);
    draw_gpu(*snapshot);
    if (ImGui::CollapsingHeader("Counters and metadata")) {
        for (const auto& [name, value] : snapshot->metadata)
            ImGui::TextWrapped("%s: %s", name.c_str(), value.c_str());
        const std::size_t count = std::min(snapshot->counters.size(), std::size_t{64});
        for (std::size_t i = snapshot->counters.size() - count; i < snapshot->counters.size(); ++i) {
            const auto& counter = snapshot->counters[i];
            ImGui::Text("%s: %.4g (thread %llu)", counter.name.c_str(), counter.value,
                        static_cast<unsigned long long>(counter.thread_id));
        }
    }
    ImGui::End();
    return intents;
}

void MicroprofilerUI::draw_frames(const profile::Snapshot& snapshot) {
    ImGui::SeparatorText("Frame intervals (incomplete = partial)");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float graph_height = std::clamp(ImGui::GetWindowHeight() * 0.1f,
        2.0f * ImGui::GetFontSize(), 5.0f * ImGui::GetFontSize());
    const ImVec2 size(std::max(1.0f, ImGui::GetContentRegionAvail().x), graph_height);
    ImGui::InvisibleButton("##frame_graph", size);
    ImDrawList* draws = ImGui::GetWindowDrawList();
    draws->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 23, 30, 255));
    if (snapshot.frames.empty()) return;
    double max_duration = kNsPerMs;
    for (const auto& frame : snapshot.frames) max_duration = std::max(max_duration, duration(frame.begin_ns, frame.end_ns));
    // Dense histories use peak-preserving pixel columns rather than submitting
    // one primitive per frame. Clicking a column inspects its longest frame.
    const auto columns = std::min(snapshot.frames.size(),
        static_cast<std::size_t>(std::max(1.0f, std::ceil(size.x))));
    const float width = size.x / static_cast<float>(columns);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (std::size_t column = 0; column < columns; ++column) {
        const std::size_t first = column * snapshot.frames.size() / columns;
        const std::size_t last = (column + 1) * snapshot.frames.size() / columns;
        std::size_t peak = first;
        bool rejected = false;
        for (std::size_t i = first; i < last; ++i) {
            if (duration(snapshot.frames[i].begin_ns, snapshot.frames[i].end_ns) >
                duration(snapshot.frames[peak].begin_ns, snapshot.frames[peak].end_ns)) peak = i;
            rejected |= snapshot.frames[i].outcome == profile::FrameOutcome::Rejected;
        }
        const auto& frame = snapshot.frames[peak];
        const float height = static_cast<float>(duration(frame.begin_ns, frame.end_ns) / max_duration) * size.y;
        const float x = origin.x + static_cast<float>(column) * width;
        const auto color = frame.incomplete ? IM_COL32(155, 125, 210, 255) : rejected ? IM_COL32(225, 100, 70, 255) :
            frame.outcome == profile::FrameOutcome::Accepted ? IM_COL32(75, 180, 105, 255) : IM_COL32(160, 150, 75, 255);
        draws->AddRectFilled(ImVec2(x, origin.y + size.y - height), ImVec2(x + width, origin.y + size.y), color);
        if (hovered && mouse.x >= x && mouse.x < x + width) {
            ImGui::SetTooltip("%zu frame(s); peak attempt %llu: %.3f ms observed, %s, %s%s",
                last - first, static_cast<unsigned long long>(frame.context.attempt_id),
                duration(frame.begin_ns, frame.end_ns) / kNsPerMs, outcome_name(frame.outcome),
                frame.kind == profile::FrameKind::Loading ? "loading" : "render",
                frame.incomplete ? " (incomplete, not a full frame time)" : "");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                view_begin_ns_ = frame.begin_ns >= snapshot.begin_ns ? static_cast<double>(frame.begin_ns - snapshot.begin_ns) : 0.0;
                view_duration_ns_ = std::max(1'000.0, duration(frame.begin_ns, frame.end_ns));
                follow_ = false;
                clamp_view(snapshot);
                selected_cpu_attempt_ = frame.context.attempt_id;
                link_gpu_attempt(selected_cpu_attempt_, frame.context.capture_id);
            }
        }
    }
}

void MicroprofilerUI::refresh_layout(const std::shared_ptr<const profile::Snapshot>& snapshot) {
    if (layout_snapshot_ == snapshot) return;
    layout_snapshot_ = snapshot;
    std::unordered_map<std::uint64_t, const profile::Span*> by_id;
    for (const auto& span : snapshot->spans) by_id.emplace(span.id, &span);
    depths_.clear();
    thread_depths_.clear();
    for (const auto& span : snapshot->spans) {
        unsigned depth = 0;
        auto parent = span.parent_id;
        while (parent != 0 && depth < 64) {
            const auto found = by_id.find(parent);
            if (found == by_id.end() || found->second->thread_id != span.thread_id) break;
            ++depth;
            parent = found->second->parent_id;
        }
        depths_[span.id] = depth;
        thread_depths_[span.thread_id] = std::max(thread_depths_[span.thread_id], depth);
    }
    threads_ = snapshot->threads;
    for (const auto& span : snapshot->spans) {
        if (std::ranges::none_of(threads_, [&](const auto& thread) { return thread.id == span.thread_id; }))
            threads_.push_back({span.thread_id, "Thread " + std::to_string(span.thread_id)});
    }
    // GPU intervals share a clock only within one native frame and renderer.
    // Missing native frame IDs stay separated by their available correlation.
    gpu_frames_.clear();
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>, std::size_t> frames;
    for (std::size_t i = 0; i < snapshot->gpu_samples.size(); ++i) {
        const auto& sample = snapshot->gpu_samples[i];
        const auto& context = sample.context;
        const auto key = std::tuple(context.capture_id, context.renderer_id, context.filament_frame_id,
            context.filament_frame_id ? 0 : context.attempt_id,
            context.filament_frame_id ? 0 : context.submission_id);
        auto [entry, inserted] = frames.try_emplace(key, gpu_frames_.size());
        if (inserted) gpu_frames_.push_back({context.renderer_id, context.filament_frame_id,
            context.attempt_id, context.submission_id, context.capture_id, {}, 0, false});
        auto& frame = gpu_frames_[entry->second];
        frame.samples.push_back(i);
        if (sample.status == profile::SampleStatus::Available && sample.end_ns >= sample.begin_ns) {
            frame.available = true;
            frame.end_ns = std::max(frame.end_ns, sample.end_ns);
        }
    }
    std::ranges::sort(gpu_frames_, [](const auto& a, const auto& b) {
        return std::tie(a.attempt_id, a.renderer_id, a.frame_id, a.submission_id) <
               std::tie(b.attempt_id, b.renderer_id, b.frame_id, b.submission_id);
    });
    if (gpu_frames_.empty()) selected_gpu_frame_.reset();
    else if (gpu_follow_latest_ || !selected_gpu_frame_ ||
             std::ranges::none_of(gpu_frames_, [&](const auto& frame) {
                 return frame.same_identity(*selected_gpu_frame_);
             })) {
        const auto last_available = std::find_if(gpu_frames_.rbegin(), gpu_frames_.rend(),
            [](const auto& frame) { return frame.available; });
        selected_gpu_frame_ = last_available != gpu_frames_.rend() ? *last_available : gpu_frames_.back();
        selected_gpu_sample_ = 0;
    }
}

bool MicroprofilerUI::GpuFrameLayout::same_identity(const GpuFrameLayout& other) const {
    return capture_id == other.capture_id && renderer_id == other.renderer_id && frame_id == other.frame_id &&
        (frame_id != 0 || (attempt_id == other.attempt_id && submission_id == other.submission_id));
}

void MicroprofilerUI::link_gpu_attempt(std::uint64_t attempt_id, std::uint64_t capture_id) {
    if (attempt_id == 0) return;
    const auto frame = std::ranges::find_if(gpu_frames_, [&](const auto& candidate) {
        return candidate.attempt_id == attempt_id && candidate.capture_id == capture_id;
    });
    if (frame == gpu_frames_.end()) return;
    selected_gpu_frame_ = *frame;
    selected_gpu_sample_ = 0;
    gpu_follow_latest_ = false;
}

void MicroprofilerUI::focus_cpu_attempt(const profile::Snapshot& snapshot, std::uint64_t attempt_id,
                                       std::uint64_t capture_id) {
    if (attempt_id == 0) return;
    const auto frame = std::ranges::find_if(snapshot.frames, [&](const auto& candidate) {
        return candidate.context.attempt_id == attempt_id && candidate.context.capture_id == capture_id;
    });
    if (frame == snapshot.frames.end()) return;
    selected_cpu_attempt_ = attempt_id;
    view_begin_ns_ = frame->begin_ns >= snapshot.begin_ns ? static_cast<double>(frame->begin_ns - snapshot.begin_ns) : 0.0;
    view_duration_ns_ = std::max(1'000.0, duration(frame->begin_ns, frame->end_ns));
    follow_ = false;
    clamp_view(snapshot);
}

void MicroprofilerUI::draw_timeline(const profile::Snapshot& snapshot) {
    ImGui::SeparatorText("CPU timeline (wheel zoom; drag to pan; click a task)");
    if (selected_cpu_attempt_ != 0) ImGui::TextWrapped("CPU view attempt %llu (identity link only; separate clock)",
        static_cast<unsigned long long>(selected_cpu_attempt_));
    const float line = ImGui::GetFontSize() * 1.5f;
    const float label_width = ImGui::GetFontSize() * 10.0f;
    std::unordered_map<std::uint64_t, float> row;
    float height = line;
    for (const auto& thread : threads_) {
        if (hidden_threads_.contains(thread.id)) continue;
        row[thread.id] = height;
        height += line * static_cast<float>(thread_depths_[thread.id] + 2);
    }
    const float visible_height = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y;
    const float timeline_height = std::clamp(visible_height, 5.0f * ImGui::GetFontSize(),
        14.0f * ImGui::GetFontSize());
    ImGui::BeginChild("##cpu_timeline", ImVec2(0.0f, timeline_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(label_width + 1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("##timeline_canvas", ImVec2(width, height));
    ImDrawList* draws = ImGui::GetWindowDrawList();
    draws->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(20, 23, 30, 255));
    const float canvas_width = width - label_width;
    const auto x_at = [&](double ns) {
        return origin.x + label_width + static_cast<float>((ns - view_begin_ns_) / view_duration_ns_) * canvas_width;
    };
    for (int tick = 0; tick <= 5; ++tick) {
        const double ns = view_begin_ns_ + view_duration_ns_ * static_cast<double>(tick) / 5.0;
        const float x = x_at(ns);
        draws->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(55, 58, 65, 255));
        char label[48];
        std::snprintf(label, sizeof(label), "%.3f ms", ns / kNsPerMs);
        draws->AddText(ImVec2(x + 2.0f, origin.y), IM_COL32(190, 190, 190, 255), label);
    }
    for (const auto& thread : threads_) {
        const auto found = row.find(thread.id);
        if (found == row.end()) continue;
        draws->AddText(ImVec2(origin.x + 2.0f, origin.y + found->second), IM_COL32_WHITE, thread.name.c_str());
    }
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 clip_min = draws->GetClipRectMin();
    const ImVec2 clip_max = draws->GetClipRectMax();
    std::size_t drawn = 0;
    bool dense = false;
    // Filagui uses 16-bit indices without base-vertex support. Bound the whole
    // child draw list, including its existing labels and each task's text/border.
    const bool limited_vertices = sizeof(ImDrawIdx) == 2 &&
        (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) == 0;
    for (const auto& span : snapshot.spans) {
        const auto found = row.find(span.thread_id);
        if (found == row.end() || hidden_categories_.contains(span.category) || !matches(span, search_)) continue;
        const double begin = span.begin_ns >= snapshot.begin_ns ? static_cast<double>(span.begin_ns - snapshot.begin_ns) : 0.0;
        const double end = span.end_ns >= snapshot.begin_ns ? static_cast<double>(span.end_ns - snapshot.begin_ns) : begin;
        if (end < view_begin_ns_ || begin > view_begin_ns_ + view_duration_ns_) continue;
        const float y = origin.y + found->second + static_cast<float>(depths_[span.id]) * line;
        if (y + line < clip_min.y || y > clip_max.y) continue;
        const float x0 = std::clamp(x_at(begin), origin.x + label_width, origin.x + width);
        const float x1 = std::clamp(x_at(end), x0 + std::min(1.0f, origin.x + width - x0), origin.x + width);
        const bool label_visible = x1 - x0 > ImGui::CalcTextSize(span.name.c_str()).x + 4.0f;
        const std::size_t vertices_needed = 128 + (label_visible ? span.name.size() * 4 : 0);
        if (drawn >= kDrawLimit || (limited_vertices &&
            static_cast<std::size_t>(draws->VtxBuffer.Size) + vertices_needed >= 65'536)) {
            dense = true;
            break;
        }
        ++drawn;
        draws->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + line - 2.0f), category_color(span.category),
            x1 - x0 >= 4.0f ? 2.0f : 0.0f);
        if (selected_span_ == span.id) draws->AddRect(ImVec2(x0, y), ImVec2(x1, y + line - 2.0f), IM_COL32_WHITE);
        if (label_visible)
            draws->AddText(ImVec2(x0 + 2.0f, y), IM_COL32(10, 12, 16, 255), span.name.c_str());
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y && mouse.y < y + line) {
            ImGui::SetTooltip("%s [%s]: %.4f ms%s", span.name.c_str(), span.category.c_str(),
                              duration(span.begin_ns, span.end_ns) / kNsPerMs, span.incomplete ? " (incomplete)" : "");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_span_ = span.id;
                selected_cpu_attempt_ = span.context.attempt_id;
                link_gpu_attempt(selected_cpu_attempt_, span.context.capture_id);
            }
        }
    }
    if (hovered && mouse.x >= origin.x + label_width) {
        const auto& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            const double fraction = std::clamp(static_cast<double>((mouse.x - origin.x - label_width) / canvas_width), 0.0, 1.0);
            const double anchor = view_begin_ns_ + fraction * view_duration_ns_;
            view_duration_ns_ *= std::pow(0.75, static_cast<double>(io.MouseWheel));
            view_begin_ns_ = anchor - fraction * view_duration_ns_;
            follow_ = false;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            view_begin_ns_ -= static_cast<double>(io.MouseDelta.x / canvas_width) * view_duration_ns_;
            follow_ = false;
        }
        clamp_view(snapshot);
    }
    ImGui::EndChild();
    if (dense) ImGui::TextWrapped("Dense view: zoom/filter to inspect more tasks; export retains the complete snapshot.");
}

void MicroprofilerUI::draw_details(const profile::Snapshot& snapshot) {
    ImGui::SeparatorText("Selected task");
    const auto selected = std::ranges::find(snapshot.spans, selected_span_, &profile::Span::id);
    if (selected == snapshot.spans.end()) {
        ImGui::TextUnformatted("Select a task in the CPU timeline.");
        return;
    }
    ImGui::TextWrapped("%s [%s]", selected->name.c_str(), selected->category.c_str());
    ImGui::TextWrapped("Observed interval %.6f ms | thread %llu | parent %llu%s",
        duration(selected->begin_ns, selected->end_ns) / kNsPerMs,
        static_cast<unsigned long long>(selected->thread_id),
        static_cast<unsigned long long>(selected->parent_id), selected->incomplete ? " | incomplete" : "");
    context_details(selected->context);
}

void MicroprofilerUI::draw_gpu(const profile::Snapshot& snapshot) {
    if (!ImGui::CollapsingHeader("GPU timing samples (separate GPU-relative clock)")) return;
    ImGui::TextWrapped("Select a native frame/renderer, then a GPU bar or unavailable row. This axis is not calibrated to CPU time; intervals are never added to CPU costs.");
    if (gpu_frames_.empty() || !selected_gpu_frame_) {
        ImGui::TextUnformatted("No GPU timing samples available.");
        return;
    }
    auto selected = std::ranges::find_if(gpu_frames_, [&](const auto& frame) {
        return frame.same_identity(*selected_gpu_frame_);
    });
    if (selected == gpu_frames_.end()) return;
    std::size_t frame_index = static_cast<std::size_t>(selected - gpu_frames_.begin());
    const auto select_frame = [&](std::size_t index) {
        frame_index = index;
        selected_gpu_frame_ = gpu_frames_[index];
        selected_gpu_sample_ = 0;
        gpu_follow_latest_ = false;
    };
    const auto label_for = [](const GpuFrameLayout& frame) {
        return "Frame " + std::to_string(frame.frame_id) + " / renderer " + std::to_string(frame.renderer_id) +
            " / attempt " + std::to_string(frame.attempt_id);
    };
    const std::string preview = label_for(*selected);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##gpu_frame_renderer", preview.c_str())) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(gpu_frames_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::size_t index = static_cast<std::size_t>(i);
                ImGui::PushID(i);
                if (ImGui::Selectable(label_for(gpu_frames_[index]).c_str(), index == frame_index)) select_frame(index);
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(frame_index == 0);
    if (ImGui::Button("Previous GPU frame")) select_frame(frame_index - 1);
    ImGui::EndDisabled();
    continue_row_for("Next GPU frame");
    ImGui::BeginDisabled(frame_index + 1 >= gpu_frames_.size());
    if (ImGui::Button("Next GPU frame")) select_frame(frame_index + 1);
    ImGui::EndDisabled();
    continue_row_for("Latest GPU frame");
    if (ImGui::Button("Latest GPU frame")) {
        const auto latest = std::find_if(gpu_frames_.rbegin(), gpu_frames_.rend(),
            [](const auto& frame) { return frame.available; });
        select_frame(latest != gpu_frames_.rend()
            ? static_cast<std::size_t>(gpu_frames_.rend() - latest - 1) : gpu_frames_.size() - 1);
        gpu_follow_latest_ = true;
    }
    const auto& frame = gpu_frames_[frame_index];
    if (selected_cpu_attempt_ != 0 &&
        (frame.attempt_id != selected_cpu_attempt_ || frame.capture_id != snapshot.session_id)) {
        const bool linked_retained = std::ranges::any_of(gpu_frames_, [&](const auto& candidate) {
            return candidate.attempt_id == selected_cpu_attempt_ && candidate.capture_id == snapshot.session_id;
        });
        ImGui::TextWrapped("%s", linked_retained
            ? "The shown GPU frame is independently selected and does not match the selected CPU attempt."
            : "No linked GPU frame retained for the selected CPU attempt; the shown GPU frame is independently selected.");
    }
    const bool cpu_available = frame.attempt_id != 0 && std::ranges::any_of(snapshot.frames, [&](const auto& cpu) {
        return cpu.context.attempt_id == frame.attempt_id && cpu.context.capture_id == frame.capture_id;
    });
    ImGui::BeginDisabled(!cpu_available);
    if (ImGui::Button("Focus linked CPU attempt")) focus_cpu_attempt(snapshot, frame.attempt_id, frame.capture_id);
    ImGui::EndDisabled();
    if (!cpu_available) ImGui::TextDisabled("No retained CPU frame for this attempt.");
    const bool known_clock = frame.frame_id != 0 && frame.renderer_id != 0;
    if (!known_clock) ImGui::TextWrapped("Native frame/renderer identity unavailable; no shared GPU axis is inferred.");
    else ImGui::TextWrapped("GPU frame %llu / renderer %llu: 0 to %.4f ms (GPU-relative only)",
        static_cast<unsigned long long>(frame.frame_id), static_cast<unsigned long long>(frame.renderer_id),
        static_cast<double>(frame.end_ns) / kNsPerMs);
    const auto first = frame.samples.size() > 256 ? frame.samples.size() - 256 : 0;
    selected_gpu_sample_ = std::clamp(selected_gpu_sample_, first, frame.samples.size() - 1);
    if (first != 0) ImGui::TextDisabled("Showing the latest 256 passes in this frame; export retains all samples.");
    const float line = ImGui::GetFontSize() * 1.6f;
    const double extent = std::max(kNsPerMs, static_cast<double>(frame.end_ns));
    ImGui::BeginChild("##gpu_timeline", ImVec2(0.0f, 11.0f * ImGui::GetFontSize()), ImGuiChildFlags_Borders);
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float label_width = std::min(width * 0.4f, 16.0f * ImGui::GetFontSize());
    const float bar_width = std::max(1.0f, width - label_width);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(frame.samples.size() - first), line + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto ordinal = first + static_cast<std::size_t>(row);
            const auto& sample = snapshot.gpu_samples[frame.samples[ordinal]];
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::PushID(static_cast<int>(ordinal));
            if (ImGui::InvisibleButton("##gpu_pass", ImVec2(width, line), ImGuiButtonFlags_EnableNav)) {
                selected_gpu_sample_ = ordinal;
                gpu_follow_latest_ = false;
            }
            ImGui::PopID();
            ImDrawList* draws = ImGui::GetWindowDrawList();
            draws->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + line - 2.0f), IM_COL32(20, 23, 30, 255));
            const bool measured = known_clock && sample.status == profile::SampleStatus::Available &&
                sample.end_ns >= sample.begin_ns;
            if (measured) {
                const float x0 = origin.x + label_width + static_cast<float>(static_cast<double>(sample.begin_ns) / extent) * bar_width;
                const float x1 = std::min(origin.x + width, std::max(x0 + 1.0f,
                    origin.x + label_width + static_cast<float>(static_cast<double>(sample.end_ns) / extent) * bar_width));
                draws->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + line - 2.0f),
                    sample.source == profile::TimerSource::HardwareTimestamp ? IM_COL32(80, 175, 220, 255) : IM_COL32(180, 145, 70, 255));
            } else {
                draws->AddText(ImVec2(origin.x + label_width, origin.y), IM_COL32(220, 170, 90, 255),
                    sample_status(sample.status));
            }
            draws->PushClipRect(origin, ImVec2(origin.x + label_width - 2.0f, origin.y + line), true);
            draws->AddText(origin, IM_COL32_WHITE, sample.name.c_str());
            draws->PopClipRect();
            if (selected_gpu_sample_ == ordinal) draws->AddRect(origin,
                ImVec2(origin.x + width, origin.y + line - 2.0f), IM_COL32_WHITE);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s%s", sample.name.c_str(), sample_status(sample.status),
                measured ? " (click to inspect GPU interval)" : " (no measured bar)");
        }
    }
    ImGui::EndChild();
    const auto& sample = snapshot.gpu_samples[frame.samples[selected_gpu_sample_]];
    ImGui::SeparatorText("Selected GPU pass");
    ImGui::TextWrapped("%s: %s", sample.name.c_str(), sample_status(sample.status));
    if (sample.status == profile::SampleStatus::Available) ImGui::TextWrapped("GPU-relative interval %.4f to %.4f ms | %s",
        static_cast<double>(sample.begin_ns) / kNsPerMs, static_cast<double>(sample.end_ns) / kNsPerMs,
        sample.source == profile::TimerSource::HardwareTimestamp ? "hardware timestamp" : "estimate");
    context_details(sample.context);
    ImGui::TextWrapped("%s", sample.capability.c_str());
    ImGui::TextWrapped("Commands %llu | primitives %llu | visible renderables %llu",
        static_cast<unsigned long long>(sample.command_count),
        static_cast<unsigned long long>(sample.primitive_count),
        static_cast<unsigned long long>(sample.visible_renderables));
    ImGui::TextWrapped("Draw-instance invocations %llu (sum over draws, not unique placements)",
        static_cast<unsigned long long>(sample.draw_instance_invocations));
}
} // namespace heaven
