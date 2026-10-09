/* GPU-free diagnostic trace viewer. Only the application executes its intents. */
#ifndef HEAVENBENCH_SRC_RHI_OVERLAY_MICROPROFILER_UI_H_
#define HEAVENBENCH_SRC_RHI_OVERLAY_MICROPROFILER_UI_H_

#include "benchmark/microprofiler.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace heaven {

struct MicroprofileIntents {
    bool start = false;
    bool stop = false;
    bool save = false;
    std::string output_base;
    float duration_seconds = 10.0f; // Interactive rolling history; recording stops only on Stop.
    std::shared_ptr<const profile::Snapshot> snapshot; // Exact frozen/live view requested by Save.
};

class MicroprofilerUI {
public:
    void set_snapshot(std::shared_ptr<const profile::Snapshot> snapshot,
                      std::string feedback = {});
    void set_open(bool open) { open_ = open; focus_requested_ = open; }
    void toggle() { set_open(!open_); }
    [[nodiscard]] bool is_open() const { return open_; }
    // Cheap live status stays accurate when the closed viewer stops snapshotting.
    void set_recording(bool active) { recording_ = active; }
    [[nodiscard]] bool recording() const { return recording_.value_or(live_ && live_->active); }
    [[nodiscard]] bool frozen() const { return static_cast<bool>(frozen_); }
    [[nodiscard]] MicroprofileIntents draw();

private:
    void draw_frames(const profile::Snapshot& snapshot);
    void draw_timeline(const profile::Snapshot& snapshot);
    void draw_details(const profile::Snapshot& snapshot);
    void draw_gpu(const profile::Snapshot& snapshot);
    void fit(const profile::Snapshot& snapshot);
    void refresh_layout(const std::shared_ptr<const profile::Snapshot>& snapshot);
    void clamp_view(const profile::Snapshot& snapshot);
    void link_gpu_attempt(std::uint64_t attempt_id, std::uint64_t capture_id);
    void focus_cpu_attempt(const profile::Snapshot& snapshot, std::uint64_t attempt_id,
                           std::uint64_t capture_id);

    struct GpuFrameLayout {
        std::uint64_t renderer_id = 0, frame_id = 0, attempt_id = 0, submission_id = 0, capture_id = 0;
        std::vector<std::size_t> samples;
        std::uint64_t end_ns = 0;
        bool available = false;
        bool same_identity(const GpuFrameLayout& other) const;
    };

    bool open_ = false;
    bool follow_ = true;
    std::optional<bool> recording_;
    std::shared_ptr<const profile::Snapshot> live_;
    std::shared_ptr<const profile::Snapshot> frozen_;
    // Reuse hierarchy analysis between the application's throttled snapshots.
    // Retaining the owner also prevents pointer reuse from aliasing this cache.
    std::shared_ptr<const profile::Snapshot> layout_snapshot_;
    std::unordered_map<std::uint64_t, unsigned> depths_;
    std::unordered_map<std::uint64_t, unsigned> thread_depths_;
    std::vector<profile::ThreadInfo> threads_;
    std::string feedback_;
    std::string output_base_;
    std::string search_;
    float duration_seconds_ = 10.0f;
    double view_begin_ns_ = 0.0;
    double view_duration_ns_ = 1'000'000.0;
    std::uint64_t viewed_session_ = 0;
    std::uint64_t selected_span_ = 0;
    std::uint64_t selected_cpu_attempt_ = 0;
    std::vector<GpuFrameLayout> gpu_frames_;
    std::optional<GpuFrameLayout> selected_gpu_frame_;
    std::size_t selected_gpu_sample_ = 0;
    bool gpu_follow_latest_ = true;
    std::unordered_set<std::uint64_t> hidden_threads_;
    std::unordered_set<std::string> hidden_categories_;
    bool focus_requested_ = false;
};

} // namespace heaven
#endif
