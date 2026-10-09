#ifndef HEAVENBENCH_SRC_BENCHMARK_MICROPROFILE_REPORT_H_
#define HEAVENBENCH_SRC_BENCHMARK_MICROPROFILE_REPORT_H_

#include "microprofiler.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace heaven::profile {

[[nodiscard]] std::string snapshot_json(const Snapshot& snapshot);
[[nodiscard]] std::expected<Snapshot, std::string> parse_snapshot(std::string_view text);
[[nodiscard]] std::string snapshot_html(const Snapshot& snapshot);

enum class ReportState { Idle, Queued, Writing, Complete, Error };
struct ReportStatus {
    ReportState state = ReportState::Idle;
    float progress = 0.0f;
    std::string prefix;
    std::string error;
};

// One asynchronous job at a time. A submitted snapshot is immutable and owned
// by the worker; the frame/UI threads never perform report file I/O. Each file
// replaces its destination atomically, but the JSON/HTML pair is not a single
// filesystem transaction. Destruction drains the current job before returning.
// When recording, jobs inherit the dispatch context and borrow the installed
// recorder; its owner must drain this writer before destroying that recorder.
class ReportWriter {
public:
    ReportWriter();
    ~ReportWriter();
    ReportWriter(const ReportWriter&) = delete;
    ReportWriter& operator=(const ReportWriter&) = delete;
    [[nodiscard]] bool submit(Snapshot snapshot, std::filesystem::path prefix);
    [[nodiscard]] ReportStatus status() const;
    void wait();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven::profile
#endif
