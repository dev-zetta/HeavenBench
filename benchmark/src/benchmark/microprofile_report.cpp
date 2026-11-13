#include "microprofile_report.h"

#include "json.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace heaven::profile {
namespace {
using Json = nlohmann::json;
constexpr std::array kFrameKinds{"Render", "Loading"};
constexpr std::array kOutcomes{"Pending", "Accepted", "Rejected", "Cancelled"};
constexpr std::array kPhases{"Begin", "Step", "End"};
constexpr std::array kStatuses{"Available", "Pending", "Unsupported", "Disjoint", "Error", "Dropped"};
constexpr std::array kSources{"HardwareTimestamp", "FenceEstimate", "Unavailable"};
constexpr std::array kPipelineKinds{"Pipeline", "Statistic", "InternalRepresentation", "Use", "Loss"};
constexpr std::array kStatisticFormats{"Bool32", "Int64", "Uint64", "Float64"};
template<class E, std::size_t N>
const char* enum_name(E value, const std::array<const char*, N>& names) {
    return names.at(static_cast<std::size_t>(value));
}
template<class E, std::size_t N>
E read_enum(const Json& value, const std::array<const char*, N>& names) {
    const auto text = value.get<std::string>();
    for (std::size_t i = 0; i < N; ++i) if (text == names[i]) return static_cast<E>(i);
    throw std::invalid_argument("Unknown microprofile enum: " + text);
}
Json id(std::uint64_t value) { return std::to_string(value); }
std::uint64_t read_id(const Json& value) {
    const auto text = value.get<std::string>();
    std::uint64_t out = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid microprofile uint64 identifier");
    return out;
}
std::uint64_t read_time(const Json& value) {
    if (!value.is_number_unsigned()) throw std::invalid_argument("Invalid microprofile timestamp");
    const auto out = value.get<std::uint64_t>();
    if (out > 9'007'199'254'740'991ull)
        throw std::invalid_argument("Microprofile relative time exceeds JavaScript exact range");
    return out;
}
std::string read_label(const Json& value) {
    auto out = value.get<std::string>();
    if (out.size() > 4096) throw std::invalid_argument("Microprofile label exceeds limit");
    return out;
}
std::int64_t read_signed_id(const Json& value) {
    const auto text = value.get<std::string>();
    std::int64_t out = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid microprofile int64 statistic");
    return out;
}
Json context_json(const Context& c) {
    return {{"attempt_id", id(c.attempt_id)}, {"submission_id", id(c.submission_id)},
        {"filament_frame_id", id(c.filament_frame_id)}, {"renderer_id", id(c.renderer_id)},
        {"view_id", id(c.view_id)}, {"capture_id", id(c.capture_id)}};
}
Context read_context(const Json& value) {
    return {read_id(value.at("attempt_id")), read_id(value.at("submission_id")),
        read_id(value.at("filament_frame_id")), read_id(value.at("renderer_id")),
        read_id(value.at("view_id")), read_id(value.at("capture_id"))};
}
void interval(std::uint64_t begin, std::uint64_t end) {
    if (end < begin) throw std::invalid_argument("Reversed microprofile interval");
}
Json make_json(const Snapshot& s) {
    Json out = {{"format", "heaven-microprofile"}, {"schema", s.schema},
        {"session_id", id(s.session_id)}, {"origin_ns", id(s.origin_ns)},
        {"begin_ns", s.begin_ns}, {"end_ns", s.end_ns}, {"active", s.active},
        {"dropped_events", s.dropped_events}, {"overwritten_events", s.overwritten_events},
        {"truncated_labels", s.truncated_labels}, {"memory_bytes", s.memory_bytes},
        {"stale_epoch_events", s.stale_epoch_events},
        {"dropped_pipeline_events", s.dropped_pipeline_events},
        {"overwritten_pipeline_events", s.overwritten_pipeline_events},
        {"truncated_pipeline_artifacts", s.truncated_pipeline_artifacts},
        {"pipeline_loss_breakdown_available", s.pipeline_loss_breakdown_available},
        {"metadata", s.metadata}, {"clock", {{"unit", "nanoseconds"},
        {"cpu", "steady-clock-relative"}, {"gpu", "frame-relative-uncalibrated"}}}};
    if (s.pipeline_loss_breakdown_available) {
        out["dropped_pipeline_headers"] = s.dropped_pipeline_headers;
        out["dropped_pipeline_statistics"] = s.dropped_pipeline_statistics;
        out["overwritten_pipeline_statistics"] = s.overwritten_pipeline_statistics;
        out["dropped_pipeline_artifacts"] = s.dropped_pipeline_artifacts;
        out["overwritten_pipeline_artifacts"] = s.overwritten_pipeline_artifacts;
    }
    for (const char* key : {"threads", "spans", "frames", "counters", "flows", "gpu_samples", "pipeline_events"})
        out[key] = Json::array();
    for (const auto& t : s.threads) out["threads"].push_back({{"id", id(t.id)}, {"name", t.name}});
    for (const auto& p : s.spans) out["spans"].push_back({{"id", id(p.id)},
        {"parent_id", id(p.parent_id)}, {"thread_id", id(p.thread_id)},
        {"begin_ns", p.begin_ns}, {"end_ns", p.end_ns}, {"name", p.name},
        {"category", p.category}, {"context", context_json(p.context)}, {"incomplete", p.incomplete}});
    for (const auto& f : s.frames) out["frames"].push_back({{"context", context_json(f.context)},
        {"begin_ns", f.begin_ns}, {"end_ns", f.end_ns}, {"kind", enum_name(f.kind, kFrameKinds)},
        {"outcome", enum_name(f.outcome, kOutcomes)}, {"incomplete", f.incomplete}});
    for (const auto& c : s.counters) out["counters"].push_back({{"thread_id", id(c.thread_id)},
        {"timestamp_ns", c.timestamp_ns}, {"name", c.name}, {"category", c.category},
        {"value", c.value}, {"context", context_json(c.context)}});
    for (const auto& f : s.flows) out["flows"].push_back({{"id", id(f.id)},
        {"thread_id", id(f.thread_id)}, {"timestamp_ns", f.timestamp_ns}, {"name", f.name},
        {"phase", enum_name(f.phase, kPhases)}, {"context", context_json(f.context)}});
    for (const auto& g : s.gpu_samples) {
        Json sample{{"context", context_json(g.context)},
        {"pass_id", id(g.pass_id)}, {"parent_pass_id", id(g.parent_pass_id)},
        {"begin_ns", g.begin_ns}, {"end_ns", g.end_ns}, {"name", g.name},
        {"status", enum_name(g.status, kStatuses)}, {"source", enum_name(g.source, kSources)},
        {"capability", g.capability}, {"command_count", id(g.command_count)},
        {"primitive_count", id(g.primitive_count)}, {"visible_renderables", id(g.visible_renderables)},
        {"draw_instance_invocations", id(g.draw_instance_invocations)},
        {"native_draw_commands_available", g.native_draw_commands_available}};
        if (g.native_draw_commands_available) {
            sample["native_draw_commands_full"] = id(g.native_draw_commands_full);
            sample["native_draw_commands_2x1"] = id(g.native_draw_commands_2x1);
            sample["native_draw_commands_2x2"] = id(g.native_draw_commands_2x2);
        }
        Json statistics{{"status", enum_name(g.pipeline_statistics_status, kStatuses)},
            {"reason", g.pipeline_statistics_reason}};
        if (g.pipeline_statistics_status == SampleStatus::Available) {
            statistics["input_assembly_vertices"] = id(g.input_assembly_vertices);
            statistics["input_assembly_primitives"] = id(g.input_assembly_primitives);
            statistics["vertex_shader_invocations"] = id(g.vertex_shader_invocations);
            statistics["clipping_invocations"] = id(g.clipping_invocations);
            statistics["clipping_primitives"] = id(g.clipping_primitives);
            statistics["fragment_shader_invocations"] = id(g.fragment_shader_invocations);
        }
        sample["pipeline_statistics"] = std::move(statistics);
        out["gpu_samples"].push_back(std::move(sample));
    }
    for (const auto& e : s.pipeline_events) {
        Json event{{"context", context_json(e.context)}, {"kind", enum_name(e.kind, kPipelineKinds)},
            {"pipeline_id", id(e.pipeline_id)}, {"program_id", id(e.program_id)}, {"pass_id", id(e.pass_id)},
            {"executable_index", e.executable_index}, {"stages", e.stages}, {"subgroup_size", e.subgroup_size},
            {"program_name", e.program_name}, {"name", e.name}, {"description", e.description},
            {"status", enum_name(e.status, kStatuses)}, {"reason", e.reason},
            {"format", enum_name(e.format, kStatisticFormats)}, {"original_size", id(e.original_size)},
            {"truncated", e.truncated}, {"text", e.text}};
        if (e.kind == PipelineEventKind::Statistic && e.status == SampleStatus::Available) {
            switch (e.format) {
            case PipelineStatisticFormat::Bool32: event["value"] = e.bool_value; break;
            case PipelineStatisticFormat::Int64: event["value"] = std::to_string(e.signed_value); break;
            case PipelineStatisticFormat::Uint64: event["value"] = id(e.unsigned_value); break;
            case PipelineStatisticFormat::Float64:
                if (!std::isfinite(e.float_value)) throw std::invalid_argument("Nonfinite driver statistic");
                event["value"] = e.float_value;
                break;
            }
        }
        if (e.kind == PipelineEventKind::Loss && e.unsigned_value != 0)
            event["loss_count"] = id(e.unsigned_value);
        out["pipeline_events"].push_back(std::move(event));
    }
    // First-occurrence ordering makes label references deterministic for a
    // snapshot. Inline labels remain for schema-1 readers without table support.
    out["strings"] = Json::array();
    std::map<std::string, std::size_t> strings;
    for (const char* key : {"threads", "spans", "counters", "flows", "gpu_samples"}) {
        for (auto& record : out[key]) {
            for (const char* label : {"name", "category", "capability"}) {
                if (!record.contains(label)) continue;
                const auto value = record.at(label).get<std::string>();
                const auto [entry, inserted] = strings.emplace(value, strings.size());
                if (inserted) out["strings"].push_back(value);
                record[std::string(label) + "_id"] = entry->second;
            }
        }
    }
    return out;
}
void write_atomic(const std::filesystem::path& path, const std::string& bytes) {
    static std::atomic<std::uint64_t> serial{0};
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(Recorder::steady_now_ns()) + "-" + std::to_string(serial.fetch_add(1));
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Cannot open microprofile temporary output");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) throw std::runtime_error("Cannot finish microprofile output");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}

std::string snapshot_json(const Snapshot& snapshot) {
    return make_json(snapshot).dump(-1, ' ', false, Json::error_handler_t::replace);
}
std::expected<Snapshot, std::string> parse_snapshot(std::string_view text) {
    try {
        if (text.size() > std::size_t{256} * 1024 * 1024) throw std::invalid_argument("Microprofile JSON too large");
        const auto j = Json::parse(text);
        if (j.at("format") != "heaven-microprofile" || j.at("schema") != 1)
            throw std::invalid_argument("Unsupported microprofile format/schema");
        if (j.at("clock").at("gpu") != "frame-relative-uncalibrated")
            throw std::invalid_argument("Unsupported GPU clock contract");
        Snapshot s;
        s.session_id = read_id(j.at("session_id"));
        s.origin_ns = read_id(j.at("origin_ns"));
        s.begin_ns = read_time(j.at("begin_ns"));
        s.end_ns = read_time(j.at("end_ns"));
        interval(s.begin_ns, s.end_ns);
        s.active = j.at("active").get<bool>();
        s.dropped_events = j.at("dropped_events").get<std::uint64_t>();
        s.overwritten_events = j.at("overwritten_events").get<std::uint64_t>();
        s.truncated_labels = j.at("truncated_labels").get<std::uint64_t>();
        if (j.contains("stale_epoch_events")) s.stale_epoch_events = read_time(j.at("stale_epoch_events"));
        s.dropped_pipeline_events = j.value("dropped_pipeline_events", std::uint64_t{0});
        s.overwritten_pipeline_events = j.value("overwritten_pipeline_events", std::uint64_t{0});
        s.truncated_pipeline_artifacts = j.value("truncated_pipeline_artifacts", std::uint64_t{0});
        s.pipeline_loss_breakdown_available = j.value("pipeline_loss_breakdown_available", false);
        if (s.pipeline_loss_breakdown_available) {
            s.dropped_pipeline_headers = j.at("dropped_pipeline_headers").get<std::uint64_t>();
            s.dropped_pipeline_statistics = j.at("dropped_pipeline_statistics").get<std::uint64_t>();
            s.overwritten_pipeline_statistics = j.at("overwritten_pipeline_statistics").get<std::uint64_t>();
            s.dropped_pipeline_artifacts = j.at("dropped_pipeline_artifacts").get<std::uint64_t>();
            s.overwritten_pipeline_artifacts = j.at("overwritten_pipeline_artifacts").get<std::uint64_t>();
        }
        s.memory_bytes = j.at("memory_bytes").get<std::size_t>();
        s.metadata = j.at("metadata").get<std::map<std::string, std::string>>();
        if (s.metadata.size() > 128) throw std::invalid_argument("Microprofile metadata too large");
        std::size_t total = 0;
        for (const char* key : {"threads", "spans", "frames", "counters", "flows", "gpu_samples"}) {
            if (!j.at(key).is_array()) throw std::invalid_argument("Microprofile records must be arrays");
            total += j.at(key).size();
        }
        if (j.contains("pipeline_events")) {
            if (!j.at("pipeline_events").is_array()) throw std::invalid_argument("Pipeline events must be an array");
            total += j.at("pipeline_events").size();
        }
        if (total > 1'000'000) throw std::invalid_argument("Too many microprofile records");
        const Json* strings = j.contains("strings") ? &j.at("strings") : nullptr;
        if (strings) {
            if (!strings->is_array() || strings->size() > 1'000'000)
                throw std::invalid_argument("Invalid microprofile string table");
            for (const auto& value : *strings) static_cast<void>(read_label(value));
        }
        for (const char* key : {"threads", "spans", "counters", "flows", "gpu_samples"}) {
            for (const auto& record : j.at(key)) {
                for (const char* label : {"name", "category", "capability"}) {
                    const auto index_key = std::string(label) + "_id";
                    if (!record.contains(index_key)) continue;
                    const auto& value = record.at(index_key);
                    if (!strings || !value.is_number_unsigned())
                        throw std::invalid_argument("Invalid microprofile string reference");
                    const auto index = value.get<std::uint64_t>();
                    if (index >= strings->size() || strings->at(static_cast<std::size_t>(index)) != record.at(label))
                        throw std::invalid_argument("Mismatched microprofile string reference");
                }
            }
        }
        for (const auto& t : j.at("threads")) s.threads.push_back({read_id(t.at("id")), read_label(t.at("name"))});
        for (const auto& p : j.at("spans")) {
            Span span{read_id(p.at("id")), read_id(p.at("parent_id")), read_id(p.at("thread_id")),
                read_time(p.at("begin_ns")), read_time(p.at("end_ns")), read_label(p.at("name")),
                read_label(p.at("category")), read_context(p.at("context")), p.at("incomplete").get<bool>()};
            interval(span.begin_ns, span.end_ns);
            s.spans.push_back(std::move(span));
        }
        for (const auto& f : j.at("frames")) {
            Frame frame{read_context(f.at("context")), read_time(f.at("begin_ns")), read_time(f.at("end_ns")),
                read_enum<FrameKind>(f.at("kind"), kFrameKinds), read_enum<FrameOutcome>(f.at("outcome"), kOutcomes),
                f.at("incomplete").get<bool>()};
            interval(frame.begin_ns, frame.end_ns);
            s.frames.push_back(frame);
        }
        for (const auto& c : j.at("counters")) {
            const auto value = c.at("value").get<double>();
            if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite microprofile counter");
            s.counters.push_back({read_id(c.at("thread_id")), read_time(c.at("timestamp_ns")),
                read_label(c.at("name")), read_label(c.at("category")), value, read_context(c.at("context"))});
        }
        for (const auto& f : j.at("flows")) s.flows.push_back({read_id(f.at("id")), read_id(f.at("thread_id")),
            read_time(f.at("timestamp_ns")), read_label(f.at("name")),
            read_enum<FlowPhase>(f.at("phase"), kPhases), read_context(f.at("context"))});
        for (const auto& g : j.at("gpu_samples")) {
            GpuSample sample{read_context(g.at("context")), read_id(g.at("pass_id")), read_id(g.at("parent_pass_id")),
                read_time(g.at("begin_ns")), read_time(g.at("end_ns")), read_label(g.at("name")),
                read_enum<SampleStatus>(g.at("status"), kStatuses),
                read_enum<TimerSource>(g.at("source"), kSources), read_label(g.at("capability")),
                read_id(g.at("command_count")), read_id(g.at("primitive_count")),
                read_id(g.at("visible_renderables")),
                g.contains("draw_instance_invocations") ? read_id(g.at("draw_instance_invocations")) : 0};
            sample.native_draw_commands_available = g.value("native_draw_commands_available", false);
            if (sample.native_draw_commands_available) {
                sample.native_draw_commands_full = read_id(g.at("native_draw_commands_full"));
                sample.native_draw_commands_2x1 = read_id(g.at("native_draw_commands_2x1"));
                sample.native_draw_commands_2x2 = read_id(g.at("native_draw_commands_2x2"));
            }
            if (g.contains("pipeline_statistics")) {
                const auto& statistics = g.at("pipeline_statistics");
                sample.pipeline_statistics_status = read_enum<SampleStatus>(statistics.at("status"), kStatuses);
                sample.pipeline_statistics_reason = read_label(statistics.at("reason"));
                if (sample.pipeline_statistics_status == SampleStatus::Available) {
                    sample.input_assembly_vertices = read_id(statistics.at("input_assembly_vertices"));
                    sample.input_assembly_primitives = read_id(statistics.at("input_assembly_primitives"));
                    sample.vertex_shader_invocations = read_id(statistics.at("vertex_shader_invocations"));
                    sample.clipping_invocations = read_id(statistics.at("clipping_invocations"));
                    sample.clipping_primitives = read_id(statistics.at("clipping_primitives"));
                    sample.fragment_shader_invocations = read_id(statistics.at("fragment_shader_invocations"));
                }
            }
            if (sample.status == SampleStatus::Available) interval(sample.begin_ns, sample.end_ns);
            s.gpu_samples.push_back(std::move(sample));
        }
        if (j.contains("pipeline_events")) for (const auto& value : j.at("pipeline_events")) {
            PipelineEvent event;
            event.context = read_context(value.at("context"));
            event.kind = read_enum<PipelineEventKind>(value.at("kind"), kPipelineKinds);
            event.pipeline_id = read_id(value.at("pipeline_id"));
            event.program_id = read_id(value.at("program_id"));
            event.pass_id = read_id(value.at("pass_id"));
            for (const char* field : {"executable_index", "stages", "subgroup_size"})
                if (!value.at(field).is_number_unsigned() || value.at(field).get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument("Invalid pipeline executable uint32 field");
            event.executable_index = value.at("executable_index").get<std::uint32_t>();
            event.stages = value.at("stages").get<std::uint32_t>();
            event.subgroup_size = value.at("subgroup_size").get<std::uint32_t>();
            event.program_name = read_label(value.at("program_name"));
            event.name = read_label(value.at("name"));
            event.description = read_label(value.at("description"));
            event.reason = read_label(value.at("reason"));
            event.text = read_label(value.at("text"));
            event.status = read_enum<SampleStatus>(value.at("status"), kStatuses);
            event.format = read_enum<PipelineStatisticFormat>(value.at("format"), kStatisticFormats);
            event.original_size = read_id(value.at("original_size"));
            event.truncated = value.at("truncated").get<bool>();
            if (value.contains("loss_count")) {
                if (event.kind != PipelineEventKind::Loss)
                    throw std::invalid_argument("Loss count attached to a non-Loss pipeline event");
                event.unsigned_value = read_id(value.at("loss_count"));
                if (event.unsigned_value == 0) throw std::invalid_argument("Empty pipeline loss delta");
            }
            if (event.kind == PipelineEventKind::Statistic && event.status == SampleStatus::Available) {
                switch (event.format) {
                case PipelineStatisticFormat::Bool32: event.bool_value = value.at("value").get<bool>(); break;
                case PipelineStatisticFormat::Int64: event.signed_value = read_signed_id(value.at("value")); break;
                case PipelineStatisticFormat::Uint64: event.unsigned_value = read_id(value.at("value")); break;
                case PipelineStatisticFormat::Float64:
                    if (!value.at("value").is_number() || value.at("value").is_boolean())
                        throw std::invalid_argument("Invalid floating driver statistic");
                    event.float_value = value.at("value").get<double>();
                    if (!std::isfinite(event.float_value)) throw std::invalid_argument("Nonfinite driver statistic");
                    break;
                }
            }
            s.pipeline_events.push_back(std::move(event));
        }
        return s;
    } catch (const std::exception& e) { return std::unexpected(e.what()); }
}

std::string snapshot_html(const Snapshot& snapshot) {
    auto data = snapshot_json(snapshot);
    // JSON in a script element must not terminate that element, even when an
    // instrumented file/task name contains HTML. Labels are only textContent.
    for (const auto& [character, replacement] : std::array<std::pair<char, const char*>, 3>{
        {{'<', "\\u003c"}, {'>', "\\u003e"}, {'&', "\\u0026"}}}) {
        std::size_t pos = 0;
        while ((pos = data.find(character, pos)) != std::string::npos) {
            data.replace(pos, 1, replacement);
            pos += std::char_traits<char>::length(replacement);
        }
    }
    return R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>HeavenBench microprofiler</title>
<style>body{margin:0;background:#111923;color:#dce5ed;font:14px system-ui}header{position:sticky;top:0;background:#182332;padding:12px;z-index:2}button,input,select{background:#243447;color:inherit;border:1px solid #53677b;padding:6px;margin:3px}canvas{display:block;width:100%;touch-action:none}#cpuBox{max-height:55vh;overflow:auto}h2{font-size:15px;margin:14px}pre{white-space:pre-wrap;padding:14px;background:#172332;max-height:30vh;overflow:auto}.muted{color:#a5b5c5}table{border-collapse:collapse;margin:12px;font:12px ui-monospace,monospace}td,th{text-align:left;padding:5px 12px;border-bottom:1px solid #304051;white-space:nowrap}</style>
<header><b>HeavenBench microprofiler</b> <button id="fit">Fit CPU</button><input id="filter" placeholder="Filter name / category / thread"><span id="summary"></span><div class="muted">CPU: wheel zoom, drag pan, click a span for details. GPU time is relative to its own frame; it is not aligned to the CPU clock.</div></header>
<div id="warning" style="padding:8px;color:#ffd077"></div><div id="cpuBox"><canvas id="cpu"></canvas></div><h2>Complete accepted CPU frame durations (not presentation or GPU timing)</h2><canvas id="frames"></canvas><h2>CPU scope distributions</h2><div class="muted">p50/p95/p99 use linear interpolation (R7). Incomplete spans are excluded. Exclusive costs are unavailable on trace loss or incomplete child intervals.</div><div style="overflow:auto"><table id="cpuStats"></table></div><h2>GPU pass distributions by renderer and view</h2><div class="muted">Identically named passes retain separate renderer/view identities. Per-pass inclusive samples may overlap. Hardware timestamps and fence estimates remain separate; neither is added to CPU cost.</div><div style="overflow:auto"><table id="gpuStats"></table></div><h2>Hardware pipeline counters</h2><div class="muted">Per-pass query observations, independent of timestamp status. Named passes may overlap; counts are not summed into frame totals.</div><div style="overflow:auto"><table id="pipelineStats"></table></div><h2>Compiled pipeline executables</h2><div class="muted">Driver-provided compile-time properties are not per-frame costs. IDs are device-local. Select a row to inspect names, types and bounded IR text; missing metrics are unavailable.</div><div style="overflow:auto"><table id="pipelineExecutables"></table></div><h2>Counter graph <select id="counter"></select></h2><canvas id="graph"></canvas><h2>GPU frame <select id="gpuFrame"></select></h2><canvas id="gpu"></canvas><pre id="details">Select a CPU or GPU span. Missing, overwritten and incomplete events are not measured time.</pre><details><summary>Metadata and flows</summary><pre id="metadata"></pre></details>
<script id="data" type="application/json">)HTML" + data + R"HTML(</script><script>
'use strict';
const data=JSON.parse(document.getElementById('data').textContent), $=id=>document.getElementById(id);
let lo=data.begin_ns,hi=Math.max(data.end_ns,lo+1),hits=[],gpuHits=[],drag=null;
const names=new Map(data.threads.map(t=>[t.id,t.name])), byId=new Map(data.spans.map(s=>[s.thread_id+':'+s.id,s]));
const depth=s=>{let d=0,p=s.parent_id,seen=new Set;while(p!=='0'&&d<64&&!seen.has(p)){seen.add(p);let a=byId.get(s.thread_id+':'+p);if(!a)break;d++;p=a.parent_id;}return d;};
const spans=data.spans.map(s=>({...s,depth:depth(s)}));
function color(s){let h=0;for(const c of (s.category||s.name))h=(h*31+c.charCodeAt(0))>>>0;return `hsl(${h%360} 55% ${s.incomplete?28:44}%)`;}
function setup(canvas,height){const ratio=devicePixelRatio||1,w=Math.max(400,canvas.clientWidth);canvas.width=Math.round(w*ratio);canvas.height=Math.round(height*ratio);canvas.style.height=height+'px';const c=canvas.getContext('2d');c.scale(ratio,ratio);c.font='12px system-ui';return[c,w];}
function cpu(){const filter=$('filter').value.toLowerCase(),visible=spans.filter(s=>(s.name+' '+s.category+' '+(names.get(s.thread_id)||'')).toLowerCase().includes(filter));
let lanes=[],y=28;for(const t of data.threads){const ss=visible.filter(s=>s.thread_id===t.id);if(!ss.length)continue;const n=1+ss.reduce((max,s)=>Math.max(max,s.depth),0);lanes.push({id:t.id,name:t.name,y,n});y+=n*22+28;}
const[c,w]=setup($('cpu'),Math.max(120,y));hits=[];c.fillStyle='#111923';c.fillRect(0,0,w,y);const left=165,scale=(w-left-10)/(hi-lo);
for(let i=0;i<=10;i++){const x=left+i*(w-left-10)/10;c.strokeStyle='#304051';c.beginPath();c.moveTo(x,20);c.lineTo(x,y);c.stroke();c.fillStyle='#a5b5c5';c.fillText(((lo+(hi-lo)*i/10)/1e6).toFixed(2)+' ms',x,14);}
for(const lane of lanes){c.fillStyle='#dce5ed';c.fillText(lane.name.slice(0,23),6,lane.y+14);for(const s of visible){if(s.thread_id!==lane.id||s.end_ns<lo||s.begin_ns>hi)continue;const x=Math.max(left,left+(s.begin_ns-lo)*scale),end=Math.min(w,left+(s.end_ns-lo)*scale),yy=lane.y+s.depth*22;c.fillStyle=color(s);c.fillRect(x,yy,Math.max(1,end-x),19);if(end-x>35){c.save();c.beginPath();c.rect(x,yy,end-x,19);c.clip();c.fillStyle='white';c.fillText(s.name,x+3,yy+14);c.restore();}hits.push({x,y:yy,w:Math.max(1,end-x),h:19,item:s});}}
$('summary').textContent=` ${data.active?'Recording':'Stopped'} · ${data.spans.length} CPU spans · dropped ${data.dropped_events}, overwritten ${data.overwritten_events}, truncated labels ${data.truncated_labels}, stale-epoch imports ${data.stale_epoch_events||0}`;graph();drawFrames();renderStats();}
function detail(item){let note='';if('source' in item){note='GPU timing and hardware counters have independent availability.\n'+(item.native_draw_commands_available?'Native draw commands are classified by bound pipeline rate, not fragment invocations or hardware rate-clamp measurements.\n':'Native draw commands by rate: unavailable.\n');const uses=(data.pipeline_events||[]).filter(e=>e.kind==='Use'&&e.pass_id===item.pass_id&&gpuKey({context:e.context})===gpuKey(item));const ids=new Set(uses.map(e=>e.pipeline_id));item={...item,pipeline_uses:uses,compiled_executables:(data.pipeline_events||[]).filter(e=>e.kind!=='Use'&&ids.has(e.pipeline_id))};}if(item.kind==='InternalRepresentation')note='Optional driver IR text. original_size and truncated disclose incomplete artifacts.\n';$('details').textContent=note+JSON.stringify(item,null,2);}
$('cpu').onwheel=e=>{e.preventDefault();const r=$('cpu').getBoundingClientRect(),t=Math.max(0,Math.min(1,(e.clientX-r.left-165)/(r.width-175))),center=lo+t*(hi-lo),range=Math.max(100,Math.min(Math.max(1,data.end_ns-data.begin_ns)*10,(hi-lo)*Math.exp(e.deltaY*.001)));lo=center-t*range;hi=lo+range;cpu();};
$('cpu').onpointerdown=e=>{drag={x:e.clientX,lo,hi};$('cpu').setPointerCapture(e.pointerId);};
$('cpu').onpointermove=e=>{if(!drag)return;const shift=(e.clientX-drag.x)*(drag.hi-drag.lo)/Math.max(1,$('cpu').clientWidth-175);lo=drag.lo-shift;hi=drag.hi-shift;cpu();};
$('cpu').onpointerup=e=>{if(drag&&Math.abs(e.clientX-drag.x)<4){const r=$('cpu').getBoundingClientRect(),hit=hits.findLast(h=>e.clientX-r.left>=h.x&&e.clientX-r.left<=h.x+h.w&&e.clientY-r.top>=h.y&&e.clientY-r.top<=h.y+h.h);if(hit)detail(hit.item);}drag=null;};
$('fit').onclick=()=>{lo=data.begin_ns;hi=Math.max(lo+1,data.end_ns);cpu();};$('filter').oninput=cpu;
for(const name of new Set(data.counters.map(c=>c.name))){let o=document.createElement('option');o.textContent=name;o.value=name;$('counter').append(o);}
function graph(){const[c,w]=setup($('graph'),110),ss=data.counters.filter(s=>s.name===$('counter').value&&s.timestamp_ns>=lo&&s.timestamp_ns<=hi);c.clearRect(0,0,w,110);if(!ss.length){c.fillStyle='#a5b5c5';c.fillText('No samples in this window',12,25);return;}const min=ss.reduce((min,s)=>Math.min(min,s.value),Infinity),max=ss.reduce((max,s)=>Math.max(max,s.value),-Infinity);c.fillStyle='#a5b5c5';c.fillText(`${min.toPrecision(5)} … ${max.toPrecision(5)}`,12,14);c.strokeStyle='#66cbb7';c.beginPath();ss.forEach((s,i)=>{let x=165+(s.timestamp_ns-lo)*(w-175)/(hi-lo),y=100-(s.value-min)*75/(max-min||1);i?c.lineTo(x,y):c.moveTo(x,y);});c.stroke();}$('counter').onchange=graph;
const gpuKey=g=>[g.context.capture_id,g.context.renderer_id,g.context.filament_frame_id,g.context.submission_id].join(':');
for(const key of new Set(data.gpu_samples.map(gpuKey))){let o=document.createElement('option');o.value=key;o.textContent=key;$('gpuFrame').append(o);}
function gpu(){const ss=data.gpu_samples.filter(g=>gpuKey(g)===$('gpuFrame').value),[c,w]=setup($('gpu'),Math.max(80,ss.length*25+25));gpuHits=[];const end=ss.reduce((max,s)=>s.status==='Available'?Math.max(max,s.end_ns):max,1);ss.forEach((s,i)=>{const y=20+i*25;c.fillStyle='#dce5ed';c.fillText(s.name.slice(0,22),5,y+14);const x=165+s.begin_ns*(w-175)/end,width=Math.max(2,(s.end_ns-s.begin_ns)*(w-175)/end);c.fillStyle=s.source==='FenceEstimate'?'#967c49':'#4779a5';if(s.status==='Available')c.fillRect(x,y,width,19);else c.fillText(s.status+' · '+s.capability,170,y+14);gpuHits.push({x:0,y,w,h:22,item:s});});}
$('gpu').onclick=e=>{const r=$('gpu').getBoundingClientRect(),h=gpuHits.find(h=>e.clientY-r.top>=h.y&&e.clientY-r.top<=h.y+h.h);if(h)detail(h.item);};$('gpuFrame').onchange=gpu;
function distribution(values){if(!values.length)return null;let v=values.slice().sort((a,b)=>a-b),q=f=>{let p=(v.length-1)*f,i=Math.floor(p);return v[i]+(v[Math.min(i+1,v.length-1)]-v[i])*(p-i);};return{count:v.length,sum:v.reduce((s,n)=>s+n,0),p50:q(.5),p95:q(.95),p99:q(.99),max:v[v.length-1]};}
function unionLength(intervals){let total=0,end=-1;for(const [a,b] of intervals.sort((a,b)=>a[0]-b[0])){if(b>end){total+=b-Math.max(a,end);end=b;}}return total;}
function table(id,headers,rows){let target=$(id);target.replaceChildren();let tr=document.createElement('tr');for(const h of headers){let cell=document.createElement('th');cell.textContent=h;tr.append(cell);}target.append(tr);for(const row of rows){let tr=document.createElement('tr');for(const value of row){let cell=document.createElement('td');cell.textContent=String(value);tr.append(cell);}target.append(tr);}}
const ms=n=>(n/1e6).toFixed(4),quant=d=>d?[ms(d.p50),ms(d.p95),ms(d.p99)]:['unavailable','unavailable','unavailable'];
const childMap=new Map;for(const s of data.spans){let key=s.thread_id+':'+s.parent_id;if(!childMap.has(key))childMap.set(key,[]);childMap.get(key).push(s);}
function renderStats(){let groups=new Map,filter=$('filter').value.toLowerCase(),loss=data.dropped_events>0||data.overwritten_events>0;
for(const s of data.spans){if(s.incomplete)continue;let label=(names.get(s.thread_id)||s.thread_id)+' / '+s.category+' / '+s.name;if(!label.toLowerCase().includes(filter))continue;let key=s.thread_id+':'+s.category+':'+s.name;if(!groups.has(key))groups.set(key,{label,inc:[],exc:[],valid:true});let row=groups.get(key),duration=s.end_ns-s.begin_ns;row.inc.push(duration);let ranges=[],valid=!loss;for(const c of childMap.get(s.thread_id+':'+s.id)||[]){if(c.incomplete||c.begin_ns<s.begin_ns||c.end_ns>s.end_ns)valid=false;let a=Math.max(s.begin_ns,c.begin_ns),b=Math.min(s.end_ns,c.end_ns);if(b>a)ranges.push([a,b]);}if(valid)row.exc.push(duration-unionLength(ranges));else row.valid=false;}
let rows=[...groups.values()].map(r=>{let a=distribution(r.inc),b=r.valid?distribution(r.exc):null;return[r.label,a.count,ms(a.sum),...quant(a),b?ms(b.sum):'unavailable',...quant(b)];});table('cpuStats',['Thread / category / scope','Count','Inclusive sum ms','Inc p50 ms','Inc p95 ms','Inc p99 ms','Exclusive sum ms','Exc p50 ms','Exc p95 ms','Exc p99 ms'],rows);
let gg=new Map;for(const s of data.gpu_samples){if(s.status!=='Available')continue;let key=s.source+' / renderer '+s.context.renderer_id+' / view '+s.context.view_id+' / '+s.name;if(!key.toLowerCase().includes(filter))continue;if(!gg.has(key))gg.set(key,[]);gg.get(key).push(s.end_ns-s.begin_ns);}table('gpuStats',['Source / renderer / view / pass','Count','p50 ms','p95 ms','p99 ms','Max ms'],[...gg].map(([name,v])=>{let d=distribution(v);return[name,d.count,...quant(d),ms(d.max)];}));}
function drawFrames(){const[c,w]=setup($('frames'),110),ss=data.frames.filter(f=>f.kind==='Render'&&f.outcome==='Accepted'&&!f.incomplete&&f.begin_ns>=lo&&f.end_ns<=hi);c.clearRect(0,0,w,110);if(!ss.length){c.fillStyle='#a5b5c5';c.fillText('No complete accepted CPU frame intervals in this window',12,25);return;}let max=ss.reduce((m,f)=>Math.max(m,f.end_ns-f.begin_ns),1);c.strokeStyle='#dca561';c.beginPath();ss.forEach((f,i)=>{let x=165+(f.begin_ns-lo)*(w-175)/(hi-lo),y=100-(f.end_ns-f.begin_ns)*80/max;i?c.lineTo(x,y):c.moveTo(x,y);});c.stroke();c.fillStyle='#a5b5c5';c.fillText('Maximum '+ms(max)+' ms CPU attempt duration',12,14);}
$('warning').textContent=data.metadata.settings_mixed==='true'?'Mixed settings: this capture contains configuration changes. It is unsuitable for a matched fixed-settings comparison.':'';
function renderPipelineDiagnostics(){const fields=['input_assembly_vertices','input_assembly_primitives','vertex_shader_invocations','clipping_invocations','clipping_primitives','fragment_shader_invocations'];table('pipelineStats',['Renderer','View','Pass','Timing','Query status','IA vertices','IA primitives','VS invocations','Clip invocations','Clip primitives','FS invocations','Reason'],data.gpu_samples.map(s=>{const q=s.pipeline_statistics||{status:'Unsupported',reason:'Not recorded by this trace version'};return[s.context.renderer_id,s.context.view_id,s.name,s.status,q.status,...fields.map(f=>q.status==='Available'?q[f]:'unavailable'),q.reason];}));const events=(data.pipeline_events||[]).filter(e=>e.kind!=='Use');table('pipelineExecutables',['Pipeline','Program / material variant','Executable','Kind','Driver name','Status','Type','Compile value / artifact'],events.map(e=>[e.pipeline_id,e.program_name,e.executable_index,e.kind,e.name,e.status,e.format,e.kind==='Loss'&&e.loss_count?`lost metadata delta ${e.loss_count}`:e.kind==='Statistic'&&e.status==='Available'?e.value:e.kind==='InternalRepresentation'?`${e.text.length} retained / ${e.original_size} bytes${e.truncated?' (truncated)':''}`:e.reason]));[...$('pipelineExecutables').rows].slice(1).forEach((row,i)=>{row.style.cursor='pointer';row.onclick=()=>detail(events[i]);});}renderPipelineDiagnostics();
$('metadata').textContent=JSON.stringify({clock:data.clock,metadata:data.metadata,flows:data.flows,pipeline_loss:{dropped:data.dropped_pipeline_events||0,overwritten:data.overwritten_pipeline_events||0,truncated_artifacts:data.truncated_pipeline_artifacts||0,breakdown_available:data.pipeline_loss_breakdown_available||false,dropped_headers:data.dropped_pipeline_headers??null,dropped_statistics:data.dropped_pipeline_statistics??null,overwritten_statistics:data.overwritten_pipeline_statistics??null,dropped_artifacts:data.dropped_pipeline_artifacts??null,overwritten_artifacts:data.overwritten_pipeline_artifacts??null}},null,2);window.onresize=()=>{cpu();gpu();};cpu();gpu();
</script></html>)HTML";
}

struct ReportWriter::Impl {
    struct Job {
        Snapshot snapshot;
        std::filesystem::path prefix;
        Recorder* recorder = nullptr;
        Context context;
        std::uint64_t flow_id = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable wake, done;
    std::optional<Job> job;
    ReportStatus status;
    bool stop = false;
    std::thread worker;
    Impl() : worker([this] { run(); }) {}
    void update(ReportState state, float progress, std::string error = {}) {
        std::scoped_lock lock(mutex);
        status.state = state;
        status.progress = progress;
        status.error = std::move(error);
    }
    void run() {
        for (;;) {
            std::optional<Job> current;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stop || job.has_value(); });
                if (!job) return;
                current = std::move(job);
                job.reset();
                status.state = ReportState::Writing;
            }
            std::string error;
            {
                std::optional<ContextScope> inherited;
                std::optional<Scope> worker_scope;
                if (current->recorder) {
                    current->recorder->set_thread_name("Report writer");
                    inherited.emplace(*current->recorder, current->context);
                    worker_scope.emplace(*current->recorder, "Report worker", "Profiler export", current->context);
                    current->recorder->flow(current->flow_id, FlowPhase::Step, "Report export", current->context);
                }
                try {
                auto json_path = current->prefix;
                auto html_path = current->prefix;
                json_path += ".json";
                html_path += ".html";
                if (!current->prefix.parent_path().empty())
                    std::filesystem::create_directories(current->prefix.parent_path());
                std::string json;
                {
                    std::optional<Scope> scope;
                    if (current->recorder) scope.emplace(*current->recorder, "Serialize trace JSON", "Profiler export", current->context);
                    json = snapshot_json(current->snapshot);
                }
                update(ReportState::Writing, 0.25f);
                std::string html;
                {
                    std::optional<Scope> scope;
                    if (current->recorder) scope.emplace(*current->recorder, "Serialize trace HTML", "Profiler export", current->context);
                    html = snapshot_html(current->snapshot);
                }
                update(ReportState::Writing, 0.5f);
                {
                    std::optional<Scope> scope;
                    if (current->recorder) scope.emplace(*current->recorder, "Atomic write JSON", "Profiler export", current->context);
                    write_atomic(json_path, json);
                }
                update(ReportState::Writing, 0.75f);
                {
                    std::optional<Scope> scope;
                    if (current->recorder) scope.emplace(*current->recorder, "Atomic write HTML", "Profiler export", current->context);
                    write_atomic(html_path, html);
                }
                } catch (const std::exception& e) { error = e.what(); }
                if (current->recorder)
                    current->recorder->flow(current->flow_id, FlowPhase::End, "Report export", current->context);
            }
            const bool success = error.empty();
            update(success ? ReportState::Complete : ReportState::Error, success ? 1.0f : 0.0f, std::move(error));
            done.notify_all();
        }
    }
};
ReportWriter::ReportWriter() : impl_(std::make_unique<Impl>()) {}
ReportWriter::~ReportWriter() {
    { std::scoped_lock lock(impl_->mutex); impl_->stop = true; }
    impl_->wake.notify_all();
    impl_->worker.join();
}
bool ReportWriter::submit(Snapshot snapshot, std::filesystem::path prefix) {
    auto* trace = profile::recorder();
    if (trace && !trace->active()) trace = nullptr;
    std::optional<Scope> dispatch;
    if (trace) dispatch.emplace(*trace, "Dispatch trace report", "Profiler export");
    std::scoped_lock lock(impl_->mutex);
    if (impl_->status.state == ReportState::Queued || impl_->status.state == ReportState::Writing) return false;
    if (prefix.empty() || prefix.filename().empty()) {
        impl_->status = {ReportState::Error, 0.0f, {}, "Microprofile output prefix is empty"};
        return false;
    }
    const auto utf8 = prefix.generic_u8string();
    impl_->status = {ReportState::Queued, 0.0f, std::string(utf8.begin(), utf8.end()), {}};
    static std::atomic<std::uint64_t> next_flow{1};
    const auto flow_id = (std::uint64_t{1} << 63) | next_flow.fetch_add(1, std::memory_order_relaxed);
    const auto context = trace ? trace->frame_context() : Context{};
    if (trace) trace->flow(flow_id, FlowPhase::Begin, "Report export", context);
    impl_->job = Impl::Job{std::move(snapshot), std::move(prefix), trace, context, flow_id};
    impl_->wake.notify_one();
    return true;
}
ReportStatus ReportWriter::status() const { std::scoped_lock lock(impl_->mutex); return impl_->status; }
void ReportWriter::wait() {
    std::unique_lock lock(impl_->mutex);
    impl_->done.wait(lock, [&] {
        return impl_->status.state != ReportState::Queued && impl_->status.state != ReportState::Writing;
    });
}
} // namespace heaven::profile
