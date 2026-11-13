#include "benchmark/microprofiler.h"
#include "benchmark/microprofile_report.h"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
using namespace heaven::profile;
bool require(bool condition, const char* message) {
    if (!condition) std::printf("[unit_microprofiler] FAILED: %s\n", message);
    return condition;
}
bool disabled_and_nested() {
    Recorder recorder({10'000'000'000ull, std::size_t{256} * 1024u, 4});
    set_recorder(&recorder);
    { Scope ignored("Disabled"); }
    if (!require(recorder.snapshot().spans.empty() && recorder.snapshot().memory_bytes == 0,
                 "disabled profiler allocated record storage or recorded a span")) return false;
    recorder.begin_capture();
    recorder.set_thread_name("Main");
    Context context{1, 2, 3, 4, 5, recorder.session_id()};
    recorder.set_frame_context(context);
    recorder.frame_begin(context);
    {
        Scope parent("Parent", "CPU");
        { Scope child("Child", "CPU"); recorder.counter("work", 3); }
        { ContextScope inherited(Context{8, 9, 10, 4, 5, recorder.session_id()});
          Scope child("Context child", "Loading"); }
        if (!require(recorder.frame_context() == context, "ContextScope did not restore inherited context")) return false;
    }
    recorder.flow(42, FlowPhase::Begin, "load");
    recorder.flow(42, FlowPhase::End, "load");
    recorder.frame_end(context, FrameOutcome::Accepted);
    recorder.set_active(false);
    const auto snapshot = recorder.snapshot();
    recorder.set_active(false);
    if (!require(recorder.snapshot().end_ns == snapshot.end_ns, "repeated stop extended the recorded CPU window")) return false;
    set_recorder(nullptr);
    if (!require(snapshot.spans.size() == 3 && snapshot.frames.size() == 1 &&
                 snapshot.frames[0].outcome == FrameOutcome::Accepted && snapshot.counters.size() == 1 &&
                 snapshot.flows.size() == 2, "missing nested/frame/counter/flow records")) return false;
    const auto parent = std::find_if(snapshot.spans.begin(), snapshot.spans.end(), [](const auto& p) { return p.name == "Parent"; });
    return require(std::ranges::all_of(snapshot.spans, [&](const auto& span) {
        return !span.incomplete && span.end_ns >= span.begin_ns &&
            (span.name == "Parent" || span.parent_id == parent->id);
    }), "invalid nesting or interval");
}
bool delayed_gpu_and_epochs() {
    Recorder recorder({10'000'000'000ull, std::size_t{256} * 1024u, 2});
    recorder.begin_capture();
    Context old{1, 1, 17, 5, 6, recorder.session_id()};
    recorder.set_frame_context(old);
    recorder.gpu_sample(old, 10, 0, "Color", 0, 0, SampleStatus::Pending, TimerSource::HardwareTimestamp);
    recorder.set_active(false);
    recorder.gpu_sample(old, 10, 0, "Color", 20, 50, SampleStatus::Available, TimerSource::HardwareTimestamp);
    recorder.gpu_sample(old, 11, 0, "Shadow", 0, 0, SampleStatus::Disjoint, TimerSource::HardwareTimestamp);
    const auto frozen = recorder.snapshot();
    if (!require(frozen.gpu_samples.size() == 2 && frozen.gpu_samples[0].status == SampleStatus::Available &&
                 frozen.gpu_samples[0].begin_ns == 20 && frozen.gpu_samples[0].end_ns == 50,
                 "late GPU status did not replace pending status after stop")) return false;
    recorder.begin_capture();
    recorder.gpu_sample(old, 12, 0, "Old capture", 0, 50, SampleStatus::Available, TimerSource::HardwareTimestamp);
    const auto now = Recorder::steady_now_ns();
    recorder.external_span("Old CPU", "CPU", now, now, old);
    recorder.counter("Old counter", 1, "Counter", old);
    const Context current{1, 1, 17, 5, 6, recorder.session_id()};
    recorder.gpu_sample(current, 13, 0, "Invalid", 50, 20, SampleStatus::Available, TimerSource::HardwareTimestamp);
    recorder.set_active(false);
    const auto next = recorder.snapshot();
    return require(next.session_id != frozen.session_id && frozen.gpu_samples.size() == 2 &&
        next.gpu_samples.size() == 1 && next.gpu_samples[0].status == SampleStatus::Error && next.stale_epoch_events == 3 &&
        next.dropped_events == 0 && next.spans.empty() && next.counters.empty(),
        "capture epoch failed to reject an old delayed sample or snapshot changed");
}
bool overflow_and_concurrency() {
    Recorder small({10'000'000'000ull, std::size_t{64} * 1024u, 2});
    small.begin_capture();
    for (int i = 0; i < 2000; ++i) small.counter("Overflow", i);
    small.counter(std::string(300, 'x'), 1);
    small.counter("Invalid", std::numeric_limits<double>::infinity());
    small.set_active(false);
    const auto lost = small.snapshot();
    if (!require(lost.memory_bytes <= std::size_t{64} * 1024u && lost.overwritten_events > 0 &&
                 lost.truncated_labels > 0 && lost.dropped_events > 0, "bounded overflow/loss counters failed")) return false;
    Recorder concurrent({10'000'000'000ull, std::size_t{8} * 1024u * 1024u, 8});
    concurrent.begin_capture();
    std::atomic<int> remaining{4};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int i = 0; i < 4; ++i) workers.emplace_back([&, i] {
        concurrent.set_thread_name("Worker " + std::to_string(i));
        Context context{static_cast<std::uint64_t>(i + 1), 0, 0, 0, 0, concurrent.session_id()};
        concurrent.set_frame_context(context);
        for (int j = 0; j < 100; ++j) {
            Scope outer(concurrent, "Outer");
            { Scope inner(concurrent, "Inner"); concurrent.counter("Iteration", j); }
        }
        --remaining;
    });
    std::size_t snapshots = 0;
    do { static_cast<void>(concurrent.snapshot()); ++snapshots; std::this_thread::yield(); } while (remaining.load() != 0);
    for (auto& worker : workers) worker.join();
    concurrent.set_active(false);
    const auto snapshot = concurrent.snapshot();
    if (!require(!snapshot.spans.empty() && snapshot.threads.size() >= 4 && snapshots > 0,
                 "concurrent producers/snapshot consumer lost every event")) return false;
    for (const auto& span : snapshot.spans)
        if (!require(span.end_ns >= span.begin_ns && span.context.attempt_id >= 1 && span.context.attempt_id <= 4,
                     "torn concurrent record or mixed thread context")) return false;
    Recorder history({1, std::size_t{64} * 1024u, 2});
    history.begin_capture(1);
    history.counter("Expired", 1);
    // Timestamped external input deterministically tests retention without a
    // sleep or imposing scheduler assumptions on the producer concurrency test.
    history.external_span("Old", "CPU", 1, 2);
    history.set_active(false);
    const auto aged = history.snapshot();
    return require(aged.spans.empty(), "expired external CPU span remained in bounded history");
}
bool roundtrip_and_writer() {
    Recorder recorder({10'000'000'000ull, std::size_t{256} * 1024u, 2});
    recorder.begin_capture();
    recorder.set_metadata("api", "fixture");
    recorder.set_metadata("effective_settings", std::string(16000, 'a'));
    recorder.set_thread_name("Main");
    { Scope span(recorder, "</script><img onerror=bad>", "CPU"); }
    Context context{1, 2, 3, 4, 5, recorder.session_id()};
    recorder.frame_begin(context);
    recorder.frame_end(context, FrameOutcome::Accepted);
    recorder.counter("Counter", 3.5);
    recorder.flow(5, FlowPhase::Begin, "Flow");
    recorder.gpu_sample(context, 6, 0, "Color", 0, 50, SampleStatus::Available,
        TimerSource::HardwareTimestamp, "Hardware", std::numeric_limits<std::uint64_t>::max(), 20, 3, 7);
    auto foreign = context;
    ++foreign.capture_id;
    recorder.counter("Foreign capture", 1, "Counter", foreign);
    recorder.set_active(false);
    const auto snapshot = recorder.snapshot();
    const auto json = snapshot_json(snapshot);
    const auto parsed = parse_snapshot(json);
    if (!require(parsed.has_value() && parsed->session_id == snapshot.session_id &&
        parsed->spans.size() == snapshot.spans.size() && parsed->spans[0].name == snapshot.spans[0].name &&
        parsed->frames.size() == 1 && !parsed->frames[0].incomplete && parsed->counters.size() == 1 &&
        parsed->flows.size() == 1 && parsed->gpu_samples.size() == 1 &&
        parsed->gpu_samples[0].command_count == std::numeric_limits<std::uint64_t>::max() &&
        parsed->gpu_samples[0].draw_instance_invocations == 7 && parsed->stale_epoch_events == 1,
        "versioned JSON roundtrip lost data")) return false;
    const auto tabled = nlohmann::json::parse(json);
    if (!require(tabled.at("strings").at(tabled.at("spans")[0].at("name_id").get<std::size_t>()) == snapshot.spans[0].name &&
        tabled.at("strings").at(tabled.at("gpu_samples")[0].at("name_id").get<std::size_t>()) == "Color" &&
        snapshot_json(snapshot) == json, "string table references are wrong or unstable")) return false;
    auto legacy = tabled;
    legacy.erase("strings");
    legacy.erase("stale_epoch_events");
    legacy["gpu_samples"][0].erase("draw_instance_invocations");
    for (const char* key : {"threads", "spans", "counters", "flows", "gpu_samples"})
        for (auto& record : legacy[key])
            for (const char* label : {"name_id", "category_id", "capability_id"}) record.erase(label);
    if (!require(parse_snapshot(legacy.dump()).has_value(), "legacy inline schema-1 labels rejected")) return false;
    for (int fault = 0; fault < 3; ++fault) {
        auto broken = tabled;
        if (fault == 0) broken["spans"][0]["name_id"] = broken.at("strings").size();
        if (fault == 1) broken["spans"][0]["name_id"] = -1;
        if (fault == 2) broken["strings"][broken["spans"][0]["name_id"].get<std::size_t>()] = "wrong";
        if (!require(!parse_snapshot(broken.dump()), "corrupt string-table reference accepted")) return false;
    }
    const auto html = snapshot_html(snapshot);
    if (!require(html.find("\\u003c/script\\u003e") != std::string::npos &&
        html.find("onpointermove") != std::string::npos && html.find("https://") == std::string::npos,
        "standalone viewer missing interaction or unsafe embedded JSON")) return false;
    if (!require(!parse_snapshot("{}") && !parse_snapshot("{bad"), "corrupt JSON accepted")) return false;
    auto invalid = json;
    const auto schema = invalid.find("\"schema\":1");
    invalid.replace(schema, 10, "\"schema\":2");
    if (!require(!parse_snapshot(invalid), "unsupported schema accepted")) return false;
    const auto directory = std::filesystem::temp_directory_path() /
        ("heaven-microprofile-" + std::to_string(Recorder::steady_now_ns()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
    ReportWriter writer;
    if (!require(!writer.submit(snapshot, {}) && writer.status().state == ReportState::Error,
        "writer accepted an empty output prefix")) return false;
    if (!require(writer.submit(snapshot, directory / "trace"), "writer rejected idle job")) return false;
    writer.wait();
    if (!require(writer.status().state == ReportState::Complete && writer.status().progress == 1 &&
        std::filesystem::is_regular_file(directory / "trace.json") && std::filesystem::is_regular_file(directory / "trace.html"),
        "worker did not atomically finish reports")) return false;
    std::ifstream stream(directory / "trace.json");
    const std::string readback((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (!require(readback == json && parse_snapshot(readback).has_value(), "report output differs from snapshot")) return false;
    std::ofstream blocker(directory / "blocker"); blocker << "file"; blocker.close();
    if (!require(writer.submit(snapshot, directory / "blocker" / "trace"), "writer did not accept error test")) return false;
    writer.wait();
    return require(writer.status().state == ReportState::Error && !writer.status().error.empty(),
                   "writer filesystem failure was hidden");
}
bool native_draw_command_observations() {
    Recorder recorder({10'000'000'000ull, std::size_t{256} * 1024u, 2});
    recorder.begin_capture();
    const Context context{1, 2, 3, 4, 5, recorder.session_id()};
    GpuSample native;
    native.context = context;
    native.pass_id = 1;
    native.name = "Native color";
    native.status = SampleStatus::Pending;
    native.native_draw_commands_full = std::numeric_limits<std::uint64_t>::max();
    native.native_draw_commands_2x1 = 7;
    native.native_draw_commands_2x2 = 11;
    native.native_draw_commands_available = true;
    recorder.gpu_sample(native);
    recorder.set_active(false);
    // Completed draw bookkeeping remains usable even if delayed timing fails.
    native.status = SampleStatus::Disjoint;
    recorder.gpu_sample(native);
    recorder.gpu_sample(context, 2, 0, "Observed zero", 0, 0, SampleStatus::Unsupported,
        TimerSource::Unavailable, {}, 0, 0, 0, 0, 0, 0, 0, true);
    recorder.gpu_sample(context, 3, 0, "Legacy caller", 0, 0, SampleStatus::Unsupported,
        TimerSource::Unavailable);
    const auto snapshot = recorder.snapshot();
    if (!require(snapshot.gpu_samples.size() == 3 && snapshot.gpu_samples[0].status == SampleStatus::Disjoint &&
        snapshot.gpu_samples[0].native_draw_commands_available &&
        snapshot.gpu_samples[0].native_draw_commands_full == std::numeric_limits<std::uint64_t>::max() &&
        snapshot.gpu_samples[0].native_draw_commands_2x1 == 7 && snapshot.gpu_samples[0].native_draw_commands_2x2 == 11 &&
        snapshot.gpu_samples[1].native_draw_commands_available && snapshot.gpu_samples[1].native_draw_commands_full == 0 &&
        !snapshot.gpu_samples[2].native_draw_commands_available && snapshot.memory_bytes <= std::size_t{256} * 1024u,
        "native command observations lost rate counts, measured-zero availability, or delayed timing independence")) return false;
    const auto json = nlohmann::json::parse(snapshot_json(snapshot));
    const auto parsed = parse_snapshot(json.dump());
    if (!require(parsed.has_value() && parsed->gpu_samples[0].native_draw_commands_available &&
        parsed->gpu_samples[0].native_draw_commands_full == std::numeric_limits<std::uint64_t>::max() &&
        parsed->gpu_samples[0].native_draw_commands_2x1 == 7 && parsed->gpu_samples[0].native_draw_commands_2x2 == 11 &&
        json["gpu_samples"][0]["native_draw_commands_full"] == "18446744073709551615" &&
        json["gpu_samples"][1]["native_draw_commands_full"] == "0" &&
        json["gpu_samples"][2]["native_draw_commands_available"] == false &&
        !json["gpu_samples"][2].contains("native_draw_commands_full"),
        "native command JSON lost decimal uint64 precision or invented an unavailable zero")) return false;
    auto legacy = json;
    auto& legacy_sample = legacy["gpu_samples"][0];
    for (const char* key : {"native_draw_commands_available", "native_draw_commands_full",
                           "native_draw_commands_2x1", "native_draw_commands_2x2"}) legacy_sample.erase(key);
    const auto old = parse_snapshot(legacy.dump());
    if (!require(old.has_value() && !old->gpu_samples[0].native_draw_commands_available &&
        !nlohmann::json::parse(snapshot_json(*old))["gpu_samples"][0].contains("native_draw_commands_full"),
        "legacy JSON missing native counters became measured zeros")) return false;
    for (int fault = 0; fault < 4; ++fault) {
        auto broken = json;
        auto& sample = broken["gpu_samples"][0];
        if (fault == 0) sample.erase("native_draw_commands_2x1");
        if (fault == 1) sample["native_draw_commands_available"] = "true";
        if (fault == 2) sample["native_draw_commands_full"] = "18446744073709551616";
        if (fault == 3) sample["native_draw_commands_full"] = 1;
        if (!require(!parse_snapshot(broken.dump()), "malformed available native command counts accepted")) return false;
    }
    return require(snapshot_html(snapshot).find("not fragment invocations or hardware rate-clamp measurements") != std::string::npos &&
        snapshot_html(*old).find("Native draw commands by rate: unavailable") != std::string::npos,
        "portable viewer omitted native counter semantics or unavailable feedback");
}
bool pipeline_diagnostics_observations() {
    Recorder recorder({10'000'000'000ull, std::size_t{1024} * 1024u, 2});
    recorder.begin_capture();
    const Context context{1, 2, 3, 4, 5, recorder.session_id()};
    GpuSample sample;
    sample.context = context;
    sample.pass_id = 17;
    sample.name = "Color";
    sample.status = SampleStatus::Disjoint;
    sample.pipeline_statistics_status = SampleStatus::Pending;
    sample.pipeline_statistics_reason = "Query pending";
    recorder.gpu_sample(sample);
    const auto pending = recorder.snapshot();
    recorder.set_active(false);
    sample.pipeline_statistics_status = SampleStatus::Available;
    sample.pipeline_statistics_reason = "Hardware pipeline query";
    sample.input_assembly_vertices = 0;
    sample.input_assembly_primitives = 12;
    sample.vertex_shader_invocations = 31;
    sample.clipping_invocations = 12;
    sample.clipping_primitives = 11;
    sample.fragment_shader_invocations = std::numeric_limits<std::uint64_t>::max();
    recorder.gpu_sample(sample);
    PipelineEventView event;
    event.context = context;
    event.pipeline_id = 77;
    event.program_id = 88;
    event.pass_id = 17;
    event.program_name = "heaven_lit [variant 23]";
    event.name = "fragment executable";
    event.status = SampleStatus::Available;
    event.stages = 16;
    event.subgroup_size = 16;
    recorder.pipeline_event(event);
    event.kind = PipelineEventKind::Statistic;
    event.name = "Driver arbitrary uint64";
    event.format = PipelineStatisticFormat::Uint64;
    event.unsigned_value = std::numeric_limits<std::uint64_t>::max();
    recorder.pipeline_event(event);
    event.name = "Driver arbitrary int64";
    event.format = PipelineStatisticFormat::Int64;
    event.signed_value = std::numeric_limits<std::int64_t>::min();
    recorder.pipeline_event(event);
    event.name = "Driver arbitrary bool32";
    event.format = PipelineStatisticFormat::Bool32;
    event.bool_value = true;
    recorder.pipeline_event(event);
    event.name = "Driver arbitrary float64";
    event.format = PipelineStatisticFormat::Float64;
    event.float_value = 2.5;
    recorder.pipeline_event(event);
    event.kind = PipelineEventKind::InternalRepresentation;
    event.name = "Driver IR";
    std::string borrowed_text(6000, 'x');
    event.text = borrowed_text;
    event.original_size = borrowed_text.size();
    recorder.pipeline_event(event);
    borrowed_text.assign(6000, 'z');
    event.kind = PipelineEventKind::Use;
    event.text = {};
    recorder.pipeline_event(event);
    const auto snapshot = recorder.snapshot();
    if (!require(pending.gpu_samples[0].pipeline_statistics_status == SampleStatus::Pending &&
        snapshot.gpu_samples[0].status == SampleStatus::Disjoint &&
        snapshot.gpu_samples[0].pipeline_statistics_status == SampleStatus::Available &&
        snapshot.gpu_samples[0].input_assembly_vertices == 0 &&
        snapshot.gpu_samples[0].fragment_shader_invocations == std::numeric_limits<std::uint64_t>::max() &&
        snapshot.pipeline_events.size() == 7 && snapshot.pipeline_events[5].text == std::string(4096, 'x') &&
        snapshot.pipeline_events[5].truncated && snapshot.pipeline_events[5].original_size == 6000 &&
        snapshot.truncated_pipeline_artifacts == 1 && snapshot.memory_bytes <= std::size_t{1024} * 1024u,
        "pipeline counters/compile records lost independence, ownership, immutable snapshot or bounded IR")) return false;
    const auto json = nlohmann::json::parse(snapshot_json(snapshot));
    const auto parsed = parse_snapshot(json.dump());
    if (!require(parsed.has_value() && parsed->pipeline_events.size() == 7 &&
        parsed->pipeline_events[1].unsigned_value == std::numeric_limits<std::uint64_t>::max() &&
        parsed->pipeline_events[2].signed_value == std::numeric_limits<std::int64_t>::min() &&
        parsed->pipeline_events[3].bool_value && parsed->pipeline_events[4].float_value == 2.5 &&
        json["gpu_samples"][0]["pipeline_statistics"]["fragment_shader_invocations"] == "18446744073709551615" &&
        json["pipeline_events"][2]["value"] == "-9223372036854775808",
        "pipeline JSON roundtrip lost typed statistics or uint64 precision")) return false;
    for (int fault = 0; fault < 6; ++fault) {
        auto broken = json;
        if (fault == 0) broken["gpu_samples"][0]["pipeline_statistics"].erase("clipping_primitives");
        if (fault == 1) broken["gpu_samples"][0]["pipeline_statistics"]["status"] = "Maybe";
        if (fault == 2) broken["pipeline_events"][1]["value"] = "18446744073709551616";
        if (fault == 3) broken["pipeline_events"][2]["value"] = "-9223372036854775809";
        if (fault == 4) broken["pipeline_events"][4]["value"] = "2.5";
        if (fault == 5) broken["pipeline_events"][5]["text"] = std::string(4097, 'x');
        if (!require(!parse_snapshot(broken.dump()), "malformed GPU diagnostics were accepted")) return false;
    }
    auto legacy = json;
    legacy["gpu_samples"][0].erase("pipeline_statistics");
    legacy.erase("pipeline_events");
    const auto old = parse_snapshot(legacy.dump());
    if (!require(old && old->gpu_samples[0].pipeline_statistics_status == SampleStatus::Unsupported &&
        !nlohmann::json::parse(snapshot_json(*old))["gpu_samples"][0]["pipeline_statistics"].contains("fragment_shader_invocations"),
        "legacy missing hardware counter became a measured zero")) return false;
    const auto html = snapshot_html(snapshot);
    const auto begin = html.find(R"(<script id="data" type="application/json">)");
    const auto start = html.find('>', begin) + 1;
    const auto end = html.find("</script>", start);
    if (!require(nlohmann::json::parse(html.substr(start, end - start)) == json &&
        html.find("Compiled pipeline executables") != std::string::npos &&
        html.find("GPU pass distributions by renderer and view") != std::string::npos &&
        html.find("['Renderer','View','Pass'") != std::string::npos,
        "portable HTML diagnostics differ from JSON or have no executable viewer")) return false;
    recorder.begin_capture();
    recorder.pipeline_event(event);
    if (!require(recorder.snapshot().stale_epoch_events == 1 && recorder.snapshot().pipeline_events.empty(),
        "previous capture pipeline import contaminated the new epoch")) return false;
    Recorder losses({10'000'000'000ull, std::size_t{256} * 1024u, 2});
    losses.begin_capture();
    PipelineEventView loss;
    loss.context.capture_id = losses.session_id();
    loss.kind = PipelineEventKind::Loss;
    loss.status = SampleStatus::Dropped;
    loss.reason = "driver metadata cache exhausted";
    loss.unsigned_value = 3;
    losses.pipeline_event(loss);
    loss.unsigned_value = 4;
    losses.pipeline_event(loss);
    const auto loss_snapshot = losses.snapshot();
    const auto loss_json = nlohmann::json::parse(snapshot_json(loss_snapshot));
    const auto loss_parsed = parse_snapshot(loss_json.dump());
    if (!require(loss_snapshot.pipeline_events.size() == 2 && loss_parsed &&
        loss_parsed->pipeline_events[0].unsigned_value == 3 && loss_parsed->pipeline_events[1].unsigned_value == 4 &&
        loss_json["pipeline_events"][0]["loss_count"] == "3",
        "driver metadata loss deltas were deduplicated or omitted by export")) return false;
    auto bad_loss = loss_json;
    bad_loss["pipeline_events"][0]["loss_count"] = "0";
    if (!require(!parse_snapshot(bad_loss.dump()), "empty driver loss delta was accepted as measured loss")) return false;
    Recorder utf8({10'000'000'000ull, std::size_t{256} * 1024u, 2});
    utf8.begin_capture();
    event.context.capture_id = utf8.session_id();
    event.kind = PipelineEventKind::InternalRepresentation;
    const std::string boundary = std::string(4095, 'x') + "\xc3\xa9";
    event.text = boundary;
    event.original_size = boundary.size();
    utf8.pipeline_event(event);
    const auto utf8_snapshot = utf8.snapshot();
    if (!require(utf8_snapshot.pipeline_events.size() == 1 &&
        utf8_snapshot.pipeline_events[0].text.size() == 4095 && utf8_snapshot.pipeline_events[0].truncated &&
        parse_snapshot(snapshot_json(utf8_snapshot)).has_value(),
        "IR truncation split a UTF-8 sequence or produced an unreadable report")) return false;
    Recorder small({10'000'000'000ull, std::size_t{64} * 1024u, 2});
    small.begin_capture();
    event.context.capture_id = small.session_id();
    event.kind = PipelineEventKind::Pipeline;
    event.text = {};
    for (std::uint64_t i = 1; i <= 200; ++i) { event.pipeline_id = i; small.pipeline_event(event); }
    const auto overflow = small.snapshot();
    return require(overflow.dropped_pipeline_headers > 0 && overflow.dropped_pipeline_events > 0 && overflow.dropped_events == 0 &&
        overflow.memory_bytes <= std::size_t{64} * 1024u,
        "protected header overflow was unbounded or mislabeled CPU event loss");
}
bool compact_pipeline_catalog_and_ir_lifetime() {
    constexpr std::size_t budget = std::size_t{64} * 1024u * 1024u;
    Recorder recorder({10'000'000'000ull, budget, 2});
    recorder.begin_capture();
    PipelineEventView event;
    event.context = {1, 1, 1, 1, 1, recorder.session_id()};
    event.status = SampleStatus::Available;
    event.description = "A complete arbitrary driver description shared by many compiled variants";
    event.reason = "Driver compiler property, independent of runtime timings";
    const std::string long_program(1000, 'p');
    const std::string long_name(255, 'n');
    event.program_name = long_program;
    for (std::uint64_t pipeline = 1; pipeline <= 612; ++pipeline) {
        event.pipeline_id = pipeline;
        event.program_id = pipeline % 93 + 1;
        for (std::uint32_t executable = 0; executable < 2; ++executable) {
            event.executable_index = executable;
            event.kind = PipelineEventKind::Pipeline;
            event.name = long_name;
            recorder.pipeline_event(event);
            for (std::uint64_t statistic = 0; statistic < 13; ++statistic) {
                const std::string name = "Driver statistic " + std::to_string(statistic);
                event.name = name;
                event.kind = PipelineEventKind::Statistic;
                event.unsigned_value = pipeline * 100 + statistic;
                recorder.pipeline_event(event);
            }
        }
        event.kind = PipelineEventKind::Use;
        event.pass_id = pipeline;
        recorder.pipeline_event(event);
    }
    event.kind = PipelineEventKind::InternalRepresentation;
    event.name = "First retained driver IR";
    event.text = "original IR text";
    event.original_size = event.text.size();
    recorder.pipeline_event(event);
    const auto frozen = recorder.snapshot();
    const auto first_artifact = std::ranges::find(frozen.pipeline_events, PipelineEventKind::InternalRepresentation, &PipelineEvent::kind);
    if (!require(first_artifact != frozen.pipeline_events.end() && first_artifact->text == "original IR text",
        "initial compact IR reference did not resolve to its own immutable text")) return false;
    const std::string replacement(4096, 'z');
    event.text = replacement;
    event.original_size = replacement.size();
    for (std::uint64_t i = 0; i < 2048; ++i) {
        const std::string name = "Later IR " + std::to_string(i);
        event.name = name;
        recorder.pipeline_event(event);
    }
    recorder.set_active(false);
    const auto snapshot = recorder.snapshot();
    const auto headers = std::ranges::count(snapshot.pipeline_events, PipelineEventKind::Pipeline, &PipelineEvent::kind);
    const auto statistics = std::ranges::count(snapshot.pipeline_events, PipelineEventKind::Statistic, &PipelineEvent::kind);
    const auto old_artifact = std::ranges::find(snapshot.pipeline_events, "First retained driver IR", &PipelineEvent::name);
    if (!require(headers == 1224 && statistics == 15912 && snapshot.memory_bytes <= budget &&
        snapshot.dropped_pipeline_headers == 0 && snapshot.dropped_pipeline_statistics == 0 &&
        snapshot.overwritten_pipeline_statistics == 0 && snapshot.overwritten_pipeline_events == 0 &&
        snapshot.overwritten_pipeline_artifacts > 0 && old_artifact != snapshot.pipeline_events.end() &&
        old_artifact->status == SampleStatus::Dropped && old_artifact->text.empty() &&
        old_artifact->reason.find("overwritten") != std::string::npos && first_artifact->text == "original IR text",
        "real-scene-sized compact catalog lost headers/statistics, exceeded budget or aliased overwritten IR")) return false;
    const auto header = std::ranges::find(snapshot.pipeline_events, PipelineEventKind::Pipeline, &PipelineEvent::kind);
    if (!require(header != snapshot.pipeline_events.end() && header->program_name == long_program && header->name == long_name,
        "interned driver/material-variant labels were truncated or aliased")) return false;
    const auto parsed = parse_snapshot(snapshot_json(snapshot));
    auto legacy_json = nlohmann::json::parse(snapshot_json(snapshot));
    for (const char* field : {"pipeline_loss_breakdown_available", "dropped_pipeline_headers", "dropped_pipeline_statistics",
                             "overwritten_pipeline_statistics", "dropped_pipeline_artifacts", "overwritten_pipeline_artifacts"})
        legacy_json.erase(field);
    const auto legacy = parse_snapshot(legacy_json.dump());
    if (!require(legacy && !legacy->pipeline_loss_breakdown_available &&
        !nlohmann::json::parse(snapshot_json(*legacy)).contains("dropped_pipeline_headers"),
        "legacy absent header/statistics/IR loss became a measured zero")) return false;
    if (!require(parsed && parsed->pipeline_loss_breakdown_available && parsed->overwritten_pipeline_artifacts == snapshot.overwritten_pipeline_artifacts &&
        parsed->dropped_pipeline_headers == 0 && parsed->pipeline_events.size() == snapshot.pipeline_events.size(),
        "compact catalog or independent loss fields did not roundtrip")) return false;
    recorder.begin_capture();
    event.context.capture_id = recorder.session_id();
    event.kind = PipelineEventKind::Pipeline;
    event.text = {};
    event.name = "New epoch";
    recorder.pipeline_event(event);
    const auto next = recorder.snapshot();
    if (!require(next.pipeline_events.size() == 1 && next.pipeline_events[0].name == "New epoch" &&
        next.overwritten_pipeline_artifacts == 0 && first_artifact->text == "original IR text",
        "compact string/IR reset leaked a previous capture or mutated its snapshot")) return false;
    Recorder pressure({10'000'000'000ull, std::size_t{64} * 1024u, 2});
    pressure.begin_capture();
    event.context.capture_id = pressure.session_id();
    event.kind = PipelineEventKind::Pipeline;
    event.program_name = "Protected material variant";
    event.description = "Protected executable header";
    event.name = "Header";
    for (std::uint64_t id = 1; id <= 2; ++id) { event.pipeline_id = id; pressure.pipeline_event(event); }
    event.kind = PipelineEventKind::Statistic;
    event.name = "Pressure statistic";
    for (std::uint64_t i = 0; i < 1000; ++i) { event.unsigned_value = i; pressure.pipeline_event(event); }
    const auto pressured = pressure.snapshot();
    if (!require(std::ranges::count(pressured.pipeline_events, PipelineEventKind::Pipeline, &PipelineEvent::kind) == 2 &&
        pressured.dropped_pipeline_headers == 0 && pressured.overwritten_pipeline_statistics > 0 &&
        pressured.overwritten_pipeline_events > 0 && pressured.dropped_events == 0,
        "statistics pressure overwrote protected headers or misclassified compiler loss as CPU loss")) return false;
    for (const auto memory : {std::size_t{16} * 1024u * 1024u, std::size_t{64} * 1024u * 1024u}) {
        Recorder bounded({10'000'000'000ull, memory, 64});
        bounded.begin_capture();
        if (!require(bounded.snapshot().memory_bytes <= memory, "desktop/mobile compact catalog allocation exceeded total cap")) return false;
    }
    return true;
}
bool metadata_budget() {
    Recorder recorder({10'000'000'000ull, std::size_t{1024} * 1024u, 2});
    for (int i = 0; i < 40; ++i) recorder.set_metadata("key" + std::to_string(i), "value");
    recorder.set_metadata("large", std::string(32768, 'x'));
    bool refused = false;
    try { recorder.set_metadata("over budget", std::string(32768, 'y')); }
    catch (const std::invalid_argument&) { refused = true; }
    const auto snapshot = recorder.snapshot();
    return require(refused && snapshot.metadata.size() == 41 && !snapshot.metadata.contains("over budget"),
                   "metadata key capacity or total reserved byte limit failed");
}
bool accepted_submission_contexts() {
    Recorder recorder({10'000'000'000ull, std::size_t{1024} * 1024u, 2});
    recorder.begin_capture();
    const auto context = [&](std::uint64_t attempt, std::uint64_t submission) {
        return Context{attempt, submission, attempt + 100, 3, 4, recorder.session_id()};
    };
    const auto cpu = [&](Context c) {
        const auto now = Recorder::steady_now_ns();
        recorder.external_span("Attempt work", "CPU", now, now, c);
        recorder.counter("Attempt counter", 1, "Counter", c);
        recorder.flow(c.attempt_id, FlowPhase::Step, "Attempt flow", c);
    };
    const auto frame = [&](Context c, FrameOutcome outcome) {
        recorder.frame_begin(c);
        recorder.frame_end(c, outcome);
    };
    const auto gpu = [&](Context c, SampleStatus status = SampleStatus::Available) {
        recorder.gpu_sample(c, c.attempt_id, 0, "Actual frame", 0, 1, status,
            TimerSource::HardwareTimestamp);
    };
    // A later accepted attempt can reuse the rejected candidate. Neither its
    // ID nor even contradictory GPU evidence can relabel the rejected attempt.
    cpu(context(1, 42)); frame(context(1, 0), FrameOutcome::Rejected); gpu(context(1, 42));
    cpu(context(2, 42)); frame(context(2, 42), FrameOutcome::Accepted);
    cpu(context(3, 43)); // No retained outcome or GPU proof.
    cpu(context(4, 44)); gpu(context(4, 44)); // Frame records may age out first.
    cpu(context(5, 45)); gpu(context(5, 45), SampleStatus::Dropped);
    cpu(context(6, 46)); recorder.frame_begin(context(6, 0)); gpu(context(6, 46));
    cpu(context(7, 47)); frame(context(7, 0), FrameOutcome::Cancelled); gpu(context(7, 47));
    cpu(context(8, 48)); gpu(context(8, 48), SampleStatus::Unsupported);
    cpu(context(9, 49)); gpu(context(9, 49));
    auto conflicting = context(9, 50); ++conflicting.filament_frame_id; gpu(conflicting);
    cpu(context(10, 51)); frame(context(10, 52), FrameOutcome::Accepted);
    cpu(context(11, 53)); auto absent_frame = context(11, 53); absent_frame.filament_frame_id = 0; gpu(absent_frame);
    cpu(context(12, 0)); frame(context(12, 54), FrameOutcome::Accepted);
    recorder.set_active(false);
    const auto snapshot = recorder.snapshot();
    constexpr std::array<std::uint64_t, 12> expected{0, 42, 0, 44, 0, 0, 0, 48, 0, 0, 0, 0};
    const auto valid = [&](const auto& records) {
        return records.size() == expected.size() && std::ranges::all_of(records, [&](const auto& record) {
            const auto attempt = record.context.attempt_id;
            return attempt >= 1 && attempt <= expected.size() &&
                record.context.submission_id == expected[attempt - 1];
        });
    };
    return require(valid(snapshot.spans) && valid(snapshot.counters) && valid(snapshot.flows) &&
        snapshot.gpu_samples.size() == 9 && snapshot.gpu_samples[0].context.submission_id == 42,
        "provisional CPU submission escaped acceptance proof or GPU source identity changed");
}
bool writer_instrumentation() {
    Recorder recorder({10'000'000'000ull, std::size_t{1024} * 1024u, 4});
    recorder.begin_capture();
    recorder.set_thread_name("Dispatch thread");
    const Context context{17, 23, 42, 3, 4, recorder.session_id()};
    recorder.set_frame_context(context);
    recorder.frame_begin(context);
    recorder.frame_end(context, FrameOutcome::Accepted);
    set_recorder(&recorder);
    struct ResetGlobal { ~ResetGlobal() { set_recorder(nullptr); } } reset;
    const auto directory = std::filesystem::temp_directory_path() /
        ("heaven-report-instrumentation-" + std::to_string(Recorder::steady_now_ns()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
    ReportWriter writer;
    const auto submitted = recorder.snapshot();
    if (!require(writer.submit(submitted, directory / "trace"), "instrumented export rejected")) return false;
    writer.wait();
    recorder.set_active(false);
    const auto observed = recorder.snapshot();
    const auto worker = std::find_if(observed.threads.begin(), observed.threads.end(), [](const auto& t) { return t.name == "Report writer"; });
    if (!require(writer.status().state == ReportState::Complete && worker != observed.threads.end() &&
        observed.spans.size() == 6 && observed.flows.size() == 3, "writer scopes, flow or thread name missing")) return false;
    for (const auto& span : observed.spans)
        if (!require(span.context == context && !span.incomplete && span.end_ns >= span.begin_ns,
                     "writer scope lost inherited context or completion")) return false;
    const auto root = std::find_if(observed.spans.begin(), observed.spans.end(), [](const auto& s) { return s.name == "Report worker"; });
    for (const auto& span : observed.spans)
        if (span.name != "Report worker" && span.name != "Dispatch trace report")
            if (!require(root != observed.spans.end() && span.parent_id == root->id && span.thread_id == worker->id,
                         "writer stage is not a child of worker task")) return false;
    for (std::size_t i = 0; i < observed.flows.size(); ++i)
        if (!require(observed.flows[i].context == context && observed.flows[i].id == observed.flows[0].id &&
                     observed.flows[i].phase == static_cast<FlowPhase>(i), "writer async flow is incomplete")) return false;
    std::ifstream stream(directory / "trace.json");
    const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto output = parse_snapshot(bytes);
    return require(output.has_value() && output->spans.empty() && output->flows.empty(),
                   "writer mutated the submitted snapshot with its future export work");
}
bool unit_microprofiler() {
    return disabled_and_nested() && delayed_gpu_and_epochs() && overflow_and_concurrency() && roundtrip_and_writer() && native_draw_command_observations() && pipeline_diagnostics_observations() && compact_pipeline_catalog_and_ir_lifetime() && metadata_budget() && accepted_submission_contexts() && writer_instrumentation();
}
const heaven::testing::Register kRegister(unit_microprofiler);
}
