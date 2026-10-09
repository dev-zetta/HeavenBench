/*
 * HeavenBench — headless loose/ZIP loading benchmark implementation
 */
#include "engine/loading_benchmark.h"

#include "engine/asset_file_system.h"
#include "engine/material_rhi.h"
#include "engine/null_backend.h"
#include "engine/path_resolver.h"
#include "engine/scene_rhi.h"
#include "engine/zip_archive.h"
#include "engine/tessellation_cache_archive.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>
#include <string>
#include <vector>

namespace heaven {
namespace {

using Clock = std::chrono::steady_clock;

struct Method {
    std::string name;
    std::string data_root;
    uint64_t archive_bytes = 0;
    std::vector<double> open_ms;
    std::vector<double> lookup_ms;
    std::vector<double> search_ms;
    std::vector<double> stream_ms;
    std::vector<double> read_all_ms;
    std::vector<double> scene_ms;
    uint64_t bytes = 0;
    uint64_t hash = 0;
    uint64_t vertices = 0;
    uint64_t triangles = 0;
};

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

uint64_t fnv(std::span<const std::byte> bytes, uint64_t hash) {
    for (const std::byte byte : bytes) {
        hash ^= std::to_integer<uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())) - 1);
    return values[std::min(index, values.size() - 1)];
}

std::string method_path(const Method& method, std::string_view prefix,
                        std::string_view entry) {
    const std::string prefix_slash = std::string(prefix) + "/";
    if (entry.starts_with(prefix_slash)) {
        return method.data_root + "/" +
               std::string(entry.substr(prefix.size() + 1));
    }
    return method.data_root + "/../" + std::string(entry);
}

Result<void> create_variant(
    const std::filesystem::path& target,
    const std::vector<std::pair<std::string, std::filesystem::path>>& files,
    std::string_view prefix, ZipCompression compression) {
    auto writer_result = ZipArchiveWriter::create(target, true);
    if (!writer_result) return std::unexpected(writer_result.error());
    auto writer = std::move(*writer_result);
    nlohmann::json manifest = {
        {"schema_version", 1},
        {"format", "heavenbench-assets"},
        {"data_root_prefix", prefix},
        {"entry_count", files.size()},
        {"benchmark_variant",
         compression == ZipCompression::Store ? "stored" : "deflate-fast"}
    };
    const std::string text = manifest.dump(2) + "\n";
    if (auto result = writer.add_bytes(
            std::string(kAssetArchiveManifest),
            std::as_bytes(std::span(text)),
            ZipCompression::Deflate); !result) {
        return result;
    }
    for (const auto& [entry, source] : files) {
        if (auto result = writer.add_file(entry, source, compression); !result) {
            return result;
        }
    }
    return writer.finish();
}

Result<void> run_iteration(Method& method, const BenchConfig& base,
                           std::string_view prefix,
                           const std::vector<std::string>& entries) {
    auto start = Clock::now();
    auto assets_result = AssetFileSystem::open(method.data_root);
    method.open_ms.push_back(elapsed_ms(start));
    if (!assets_result) return std::unexpected(assets_result.error());
    AssetFileSystem assets = std::move(*assets_result);

    start = Clock::now();
    for (int repeat = 0; repeat < 8; ++repeat) {
        for (const auto& entry : entries) {
            if (!assets.exists(method_path(method, prefix, entry))) {
                return std::unexpected(Error("loading benchmark lookup mismatch: " +
                                             entry));
            }
        }
    }
    method.lookup_ms.push_back(elapsed_ms(start));

    start = Clock::now();
    for (int repeat = 0; repeat < 32; ++repeat) {
        [[maybe_unused]] const auto matches =
            assets.find(method.data_root + "/heaven", ".track");
    }
    method.search_ms.push_back(elapsed_ms(start));

    start = Clock::now();
    uint64_t stream_hash = 1469598103934665603ull;
    uint64_t byte_count = 0;
    std::array<std::byte, std::size_t{256} * 1024> buffer{};
    for (const auto& entry : entries) {
        auto stream = assets.open_read(method_path(method, prefix, entry));
        if (!stream) return std::unexpected(stream.error());
        while (true) {
            auto count = (*stream)->read(buffer);
            if (!count) return std::unexpected(count.error());
            if (*count == 0) break;
            stream_hash = fnv(std::span(buffer).first(*count), stream_hash);
            byte_count += *count;
        }
    }
    method.stream_ms.push_back(elapsed_ms(start));

    start = Clock::now();
    uint64_t read_all_hash = 1469598103934665603ull;
    for (const auto& entry : entries) {
        auto bytes = assets.read_all(method_path(method, prefix, entry));
        if (!bytes) return std::unexpected(bytes.error());
        read_all_hash = fnv(
            std::as_bytes(std::span(*bytes)), read_all_hash);
    }
    method.read_all_ms.push_back(elapsed_ms(start));
    if (stream_hash != read_all_hash) {
        return std::unexpected(Error("stream/read-all hash mismatch"));
    }
    method.bytes = byte_count;
    method.hash = stream_hash;

    BenchConfig config = base;
    config.data_dir = method.data_root;
    config.tessellation.enabled = false;
    auto paths = PathResolver::create(config.data_dir);
    if (!paths) return std::unexpected(paths.error());
    NullBackend backend;
    RhiMaterialDatabase materials;
    start = Clock::now();
    materials.build(*paths, nullptr, std::cref(assets));
    auto scene =
        load_scene_rhi(config, backend, nullptr, std::cref(assets));
    method.scene_ms.push_back(elapsed_ms(start));
    if (!scene) return std::unexpected(scene.error());
    method.vertices = scene->total_vertices;
    method.triangles = scene->total_triangles;
    return {};
}

nlohmann::json summarize(const Method& method) {
    auto metric = [](const std::vector<double>& values) {
        std::vector<double> warm;
        if (values.size() > 1) warm.assign(values.begin() + 1, values.end());
        return nlohmann::json{
            {"first_ms", values.empty() ? 0 : values.front()},
            {"warm_median_ms", percentile(warm, 0.5)},
            {"warm_p95_ms", percentile(warm, 0.95)}
        };
    };
    return {
        {"name", method.name},
        {"data_root", method.data_root},
        {"uncompressed_bytes", method.bytes},
        {"archive_bytes", method.archive_bytes},
        {"compression_ratio", method.bytes == 0 ? 0.0 :
             static_cast<double>(method.archive_bytes) / static_cast<double>(method.bytes)},
        {"content_hash_fnv1a64", [&] {
            char text[17];
            std::snprintf(text, sizeof(text), "%016llx",
                          static_cast<unsigned long long>(method.hash));
            return std::string(text);
        }()},
        {"scene", {{"vertices", method.vertices},
                   {"triangles", method.triangles}}},
        {"open_index", metric(method.open_ms)},
        {"exact_lookup", metric(method.lookup_ms)},
        {"search", metric(method.search_ms)},
        {"stream_all", metric(method.stream_ms)},
        {"read_all", metric(method.read_all_ms)},
        {"material_scene_null_backend", metric(method.scene_ms)}
    };
}

Result<nlohmann::json> benchmark_tessellation_caches(
    const BenchConfig& base, const std::filesystem::path& output_dir) {
    namespace fs = std::filesystem;
    const fs::path legacy =
        fs::path(base.data_dir) / "cache/tessellation/v33";
    const fs::path archive =
        output_dir /
        ("tessellation-v" +
         std::to_string(kTessellationArchiveVersion) + ".zip");

    auto assets = AssetFileSystem::open(base.data_dir);
    auto paths = PathResolver::create(base.data_dir);
    if (!assets || !paths) {
        return std::unexpected(
            assets ? paths.error() : assets.error());
    }
    BenchConfig config = base;
    config.tessellation.cache_file = archive.string();
    NullBackend backend;
    RhiMaterialDatabase materials;
    materials.build(*paths, nullptr, std::cref(*assets));
    const auto build_start = Clock::now();
    auto scene = load_scene_rhi(
        config, backend, materials, archive.string(), nullptr,
        std::cref(*assets));
    if (!scene) return std::unexpected(scene.error());
    const double build_ms = elapsed_ms(build_start);

    std::vector<fs::path> legacy_files;
    uint64_t legacy_bytes = 0;
    std::error_code ec;
    if (fs::is_directory(legacy, ec)) {
        for (const auto& item : fs::directory_iterator(legacy, ec)) {
            if (!item.is_regular_file()) continue;
            if (item.path().extension() != ".bin" &&
                item.path().extension() != ".json") {
                continue;
            }
            legacy_files.push_back(item.path());
            legacy_bytes += item.file_size();
        }
    }
    std::sort(legacy_files.begin(), legacy_files.end());

    std::vector<double> legacy_read_ms;
    std::vector<double> archive_read_ms;
    std::array<std::byte, std::size_t{256} * 1024> buffer{};
    std::optional<uint64_t> legacy_hash;
    for (int pass = 0; pass < 4; ++pass) {
        auto start = Clock::now();
        uint64_t hash = 1469598103934665603ull;
        for (const auto& file : legacy_files) {
            std::ifstream input(file, std::ios::binary);
            while (input) {
                input.read(reinterpret_cast<char*>(buffer.data()),
                           buffer.size());
                hash = fnv(std::span(buffer).first(
                    static_cast<std::size_t>(input.gcount())), hash);
            }
        }
        if (legacy_hash && *legacy_hash != hash) {
            return std::unexpected(
                Error("legacy tessellation cache hash changed between reads"));
        }
        legacy_hash = hash;
        legacy_read_ms.push_back(elapsed_ms(start));

        start = Clock::now();
        if (auto valid = validate_zip_archive(archive, true); !valid) {
            return std::unexpected(valid.error());
        }
        archive_read_ms.push_back(elapsed_ms(start));
    }
    auto metric = [](const std::vector<double>& values) {
        const std::vector<double> warm(values.begin() + 1, values.end());
        return nlohmann::json{
            {"first_ms", values.front()},
            {"warm_median_ms", percentile(warm, 0.5)},
            {"warm_p95_ms", percentile(warm, 0.95)}
        };
    };
    return nlohmann::json{
        {"build_complete_archive_ms", build_ms},
        {"scene", {{"vertices", scene->total_vertices},
                   {"triangles", scene->total_triangles}}},
        {"legacy_loose", {
            {"path", legacy.string()},
            {"files", legacy_files.size()},
            {"bytes", legacy_bytes},
            {"read_all", metric(legacy_read_ms)}
        }},
        {"zip_archive", {
            {"path", archive.string()},
            {"bytes", fs::file_size(archive)},
            {"read_all_crc", metric(archive_read_ms)}
        }}
    };
}

} // namespace

Result<void> run_loading_benchmark(
    const BenchConfig& config, const std::filesystem::path& output_dir) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(config.data_dir)) {
        return std::unexpected(Error(
            "--benchmark-loading requires a loose directory Data_Root so it "
            "can produce equivalent stored and deflate-fast variants"));
    }
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) return std::unexpected(Error("cannot create loading benchmark output"));

    const fs::path data = fs::weakly_canonical(config.data_dir, ec);
    if (ec) return std::unexpected(Error("cannot resolve loading benchmark Data_Root"));
    const fs::path source_root = data.parent_path();
    const std::string prefix = data.filename().generic_string();
    std::vector<std::pair<std::string, fs::path>> files;
    for (const fs::path& root : {source_root / "core", data}) {
        if (!fs::exists(root)) continue;
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            if (!item.is_regular_file()) continue;
            const std::string entry =
                fs::relative(item.path(), source_root).generic_string();
            if (entry.find("/cache/") != std::string::npos) continue;
            files.emplace_back(entry, item.path());
        }
    }
    std::sort(files.begin(), files.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });
    if (files.empty()) return std::unexpected(Error("no benchmark assets found"));

    const fs::path stored_zip = output_dir / "assets-stored.zip";
    const fs::path deflate_zip = output_dir / "assets-deflate-fast.zip";
    if (auto result = create_variant(
            stored_zip, files, prefix, ZipCompression::Store); !result) {
        return result;
    }
    if (auto result = create_variant(
            deflate_zip, files, prefix, ZipCompression::DeflateFast); !result) {
        return result;
    }

    std::vector<std::string> entries;
    entries.reserve(files.size());
    for (const auto& [entry, unused] : files) entries.push_back(entry);
    std::vector<Method> methods = {
        {"loose", data.string()},
        {"zip-stored", stored_zip.string(),
         fs::file_size(stored_zip)},
        {"zip-deflate-fast", deflate_zip.string(),
         fs::file_size(deflate_zip)}
    };

    // Alternate method order on every pass so cache warming does not always
    // favour the same representation. "First" is the first observed pass;
    // warm numbers make no claim of a controlled OS-cold cache.
    for (int pass = 0; pass < 4; ++pass) {
        for (std::size_t offset = 0; offset < methods.size(); ++offset) {
            const std::size_t index =
                pass % 2 == 0 ? offset : methods.size() - 1 - offset;
            if (auto result = run_iteration(
                    methods[index], config, prefix, entries); !result) {
                return result;
            }
        }
    }
    for (std::size_t index = 1; index < methods.size(); ++index) {
        if (methods[index].hash != methods[0].hash ||
            methods[index].vertices != methods[0].vertices ||
            methods[index].triangles != methods[0].triangles) {
            return std::unexpected(Error(
                "loading benchmark parity verification failed"));
        }
    }

    nlohmann::json report = {
        {"schema_version", 1},
        {"note", "First-pass is observational; no controlled OS-cold cache is claimed."},
        {"iterations", 4},
        {"methods", nlohmann::json::array()}
    };
    for (const auto& method : methods) {
        report["methods"].push_back(summarize(method));
    }
    auto tessellation = benchmark_tessellation_caches(config, output_dir);
    if (!tessellation) return std::unexpected(tessellation.error());
    report["tessellation_cache"] = std::move(*tessellation);
    const fs::path json_path = output_dir / "loading-benchmark.json";
    std::ofstream(json_path) << report.dump(2) << '\n';

    std::ofstream html(output_dir / "loading-benchmark.html");
    html << "<!doctype html><meta charset=\"utf-8\"><title>HeavenBench loading benchmark</title>"
            "<style>body{font:14px system-ui;margin:2rem}table{border-collapse:collapse}"
            "th,td{border:1px solid #bbb;padding:.45rem;text-align:right}"
            "th:first-child,td:first-child{text-align:left}</style>"
            "<h1>HeavenBench loading benchmark</h1>"
            "<p>Alternating-order runs; first-pass is observational and is not "
            "presented as a controlled OS-cold-cache measurement.</p><table>"
            "<tr><th>Method</th><th>Archive bytes</th><th>Open first ms</th>"
            "<th>Open warm median ms</th><th>Stream warm median ms</th>"
            "<th>Scene warm median ms</th></tr>";
    for (const auto& method : methods) {
        const auto value = summarize(method);
        html << "<tr><td>" << method.name << "</td><td>"
             << method.archive_bytes << "</td><td>"
             << value["open_index"]["first_ms"] << "</td><td>"
             << value["open_index"]["warm_median_ms"] << "</td><td>"
             << value["stream_all"]["warm_median_ms"] << "</td><td>"
             << value["material_scene_null_backend"]["warm_median_ms"]
             << "</td></tr>";
    }
    const auto& tess = report["tessellation_cache"];
    html << "</table><h2>Tessellation cache</h2><table>"
            "<tr><th>Method</th><th>Bytes</th><th>First ms</th>"
            "<th>Warm median ms</th><th>Warm p95 ms</th></tr>"
         << "<tr><td>legacy loose files</td><td>"
         << tess["legacy_loose"]["bytes"] << "</td><td>"
         << tess["legacy_loose"]["read_all"]["first_ms"] << "</td><td>"
         << tess["legacy_loose"]["read_all"]["warm_median_ms"] << "</td><td>"
         << tess["legacy_loose"]["read_all"]["warm_p95_ms"] << "</td></tr>"
         << "<tr><td>ZIP archive with CRC validation</td><td>"
         << tess["zip_archive"]["bytes"] << "</td><td>"
         << tess["zip_archive"]["read_all_crc"]["first_ms"] << "</td><td>"
         << tess["zip_archive"]["read_all_crc"]["warm_median_ms"]
         << "</td><td>"
         << tess["zip_archive"]["read_all_crc"]["warm_p95_ms"]
         << "</td></tr></table>";
    return {};
}

} // namespace heaven
