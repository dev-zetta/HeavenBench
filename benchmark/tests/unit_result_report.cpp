/*
 * HeavenBench — automatic JSON/HTML result report tests
 */
#include "benchmark/result_report.h"

#include "json.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        static unsigned counter = 0;
        path = fs::temp_directory_path() /
            ("heaven_result_report_" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "_" + std::to_string(counter++));
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec) path.clear();
    }

    ~TempDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

heaven::RunResult sample_result() {
    heaven::RunResult r;
    r.performance.score_version = 3;
    r.performance.scores.overall = 1234.4;
    r.performance.scores.cpu = 2345.5;
    r.performance.scores.gpu = 3456.6;
    r.performance.metrics.average_fps = 74.125;
    r.performance.metrics.path_geomean_fps = 70.25;
    r.performance.metrics.one_percent_low_fps = 51.75;
    r.performance.metrics.effective_fps = 60.5;
    r.performance.metrics.cpu.updates_per_second = 12345.25;
    r.performance.metrics.cpu.checksum = 456.75;
    r.performance.metrics.cpu.scene_meshes = 64;
    r.performance.metrics.cpu.scene_instances = 321;
    r.performance.metrics.cpu.timing = {0.081, 0.095, 0.102, 300};
    r.performance.metrics.gpu_equivalent_fps = 82.5;
    r.performance.metrics.gpu = {12.125, 15.25, 18.5, 240};
    r.cpu_stress.enabled = true;
    r.cpu_stress.workers = 2;
    r.cpu_stress.scene_meshes = 64;
    r.cpu_stress.scene_instances = 321;
    r.average_fps = 74.125;
    r.min_fps = 40.25f;
    r.max_fps = 144.75f;
    r.frames = 123456;
    r.total_time = 1665.125;

    r.system.platform = "TestOS <kernel> & arch";
    r.system.cpu = "CPU \"Model\" (4200MHz) x8";
    r.system.gpu = "GPU <fast> & \"bright\" (8192MB)";
    r.system.platform_raw = {"TestOS", "1.2.3", "test64"};
    r.system.cpu_raw = {"CPU \"Model\"", 4200.5, 8};
    r.system.gpu_raw = {"GPU <fast>", 8192};
    r.system.gpu_raw.driver_name = "Driver <vendor> & partners";
    r.system.gpu_raw.driver_version = "25.1 \"preview\"";
    r.system.gpu_raw.driver_info = "Build <script>alert(1)</script>";
    r.system.gpu_raw.driver_check_summary = "Advisory <check> & details";
    r.system.gpu_raw.driver_check_recommendation = "Use \"vendor\" & distro packages";
    r.settings.render_api = "Vulkan";
    r.settings.display_mode = "1920x1080 4xAA windowed";
    r.settings.quality_preset = "Ultra & <Custom>";
    r.settings.display_raw = {1920, 1080, 4, false};
    r.renderer_features.texture_quality_requested =
        heaven::TextureQuality::Ultra4K;
    r.renderer_features.texture_quality_active =
        heaven::TextureQuality::High2K;
    r.renderer_features.hires_textures = 211;
    r.renderer_features.original_textures = 32;
    r.renderer_features.legacy_color_textures = 17;
    r.renderer_features.texture_fallbacks = 3;
    r.renderer_features.texture_missing_variant_fallbacks = 1;
    r.renderer_features.texture_unsupported_format_fallbacks = 1;
    r.renderer_features.texture_invalid_variant_fallbacks = 1;
    r.renderer_features.selected_texture_bytes = 987654321;
    r.renderer_features.effective_texture_max_dimension = 2048;
    return r;
}

bool check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[unit_result_report] FAILED: %s\n", message);
        return false;
    }
    return true;
}

bool unit_result_report() {
    bool ok = true;
    heaven::BenchConfig config;
    config.data_dir = "assets/<heaven>&";
    config.texture_quality = heaven::TextureQuality::Ultra4K;
    config.features.color_grading = true;
    config.features.physical_camera = true;
    config.features.dynamic_resolution = true;
    config.tone_mapper = heaven::ToneMapperSelection::AgX;
    config.grading.mixer_red = {0.75f, 0.2f, 0.05f};
    config.physical_camera.aperture = 2.8f;
    config.quality.specular_aa_level = heaven::QualityLevel::High;
    config.quality.dynres_min_scale = 0.625f;

    const auto timestamp =
        std::chrono::system_clock::time_point{} +
        std::chrono::milliseconds(123);
    const heaven::BenchmarkReportDocument document =
        heaven::build_benchmark_report(
            sample_result(), config, {"1.0.0-test", "zen4-test<&"}, timestamp);
    const Json root = Json::parse(document.json);

    ok = check(root.at("schema_version") == 2,
               "JSON schema version is not 2") && ok;
    ok = check(root.at("run").at("id") == document.report_id,
               "JSON run ID does not match document ID") && ok;
    ok = check(root.at("run").at("timestamp_utc") ==
                   "1970-01-01T00:00:00.123Z",
               "UTC timestamp is not stable or millisecond-precise") && ok;
    ok = check(root.at("application").at("version") == "1.0.0-test",
               "application version is missing") && ok;
    ok = check(root.at("application").at("build_architecture") ==
                   "zen4-test<&",
               "build architecture is missing") && ok;
    ok = check(root.at("results").at("average_fps") == 74.125,
               "average FPS lost numeric precision") && ok;
    ok = check(root.at("cpu").at("timing_ms").at("samples") == 300,
               "CPU timing sample count is missing") && ok;
    ok = check(root.at("cpu").at("scene_meshes") == 64 &&
                   root.at("cpu").at("scene_instances") == 321 &&
                   root.at("cpu_stress").at("scene_meshes") == 64 &&
                   root.at("cpu_stress").at("scene_instances") == 321,
               "CPU workload counts must describe one loaded world") && ok;
    ok = check(root.at("gpu").at("timing_ms").at("p99") == 18.5,
               "GPU p99 timing is missing") && ok;
    ok = check(root.at("system").at("platform").at("os_name") == "TestOS",
               "structured platform metadata is missing") && ok;
    ok = check(root.at("system").at("cpu").at("clock_mhz") == 4200.5,
               "structured CPU metadata is missing") && ok;
    ok = check(root.at("system").at("gpu").at("vram_mb") == 8192,
               "structured GPU metadata is missing") && ok;
    const Json& gpu_driver = root.at("system").at("gpu");
    ok = check(gpu_driver.at("driver_name") == "Driver <vendor> & partners" &&
                   gpu_driver.at("driver_version") == "25.1 \"preview\"" &&
                   gpu_driver.at("driver_info") == "Build <script>alert(1)</script>" &&
                   gpu_driver.at("driver_check_summary") == "Advisory <check> & details" &&
                   gpu_driver.at("driver_check_recommendation") == "Use \"vendor\" & distro packages",
               "driver metadata must survive JSON round-trip without text changes") && ok;
    for (const char* const escaped : {
             "Driver &lt;vendor&gt; &amp; partners", "25.1 &quot;preview&quot;",
             "Build &lt;script&gt;alert(1)&lt;/script&gt;", "Advisory &lt;check&gt; &amp; details",
             "Use &quot;vendor&quot; &amp; distro packages"}) {
        ok = check(document.html.find(escaped) != std::string::npos,
                   "HTML driver metadata is missing or unescaped") && ok;
    }
    ok = check(document.html.find("<script>alert(1)</script>") == std::string::npos,
               "driver text must not create executable HTML") && ok;

    heaven::RunResult without_driver = sample_result();
    without_driver.system.gpu_raw = {"GPU <fast>", 8192};
    const auto no_driver_document = heaven::build_benchmark_report(
        without_driver, config, {"1.0.0-test", "zen4-test<&"}, timestamp);
    const Json no_driver = Json::parse(no_driver_document.json);
    for (const char* const key : {"driver_name", "driver_version", "driver_info",
                            "driver_check_summary", "driver_check_recommendation"}) {
        ok = check(no_driver.at("system").at("gpu").at(key)
                       .get_ref<const std::string&>().empty(),
                   "unavailable driver metadata must remain empty") && ok;
    }
    ok = check(no_driver_document.html.find("Driver recommendation") == std::string::npos,
               "unavailable driver advisory must not produce an empty HTML row") && ok;
    ok = check(root.at("configuration").at("features")
                       .at("dynamic_resolution") == true,
               "advanced feature configuration is missing") && ok;
    ok = check(root.at("configuration").at("tone_mapper") == "AgX",
               "tone-mapper enum is not serialized by stable name") && ok;
    ok = check(root.at("configuration").at("texture_quality") == "4k",
               "requested texture quality is missing from configuration") && ok;
    ok = check(root.at("configuration").at("grading").at("mixer_red").size() == 3,
               "GLM configuration vector is not a JSON array") && ok;
    ok = check(root.at("configuration").at("quality")
                       .at("specular_aa_level") == "High",
               "quality enum is not serialized by stable name") && ok;
    const Json& textures = root.at("renderer_features").at("textures");
    ok = check(textures.at("quality_requested") == "4k" &&
                   textures.at("quality_active") == "2k" &&
                   textures.at("hires") == 211 &&
                   textures.at("original") == 32 &&
                   textures.at("legacy_color") == 17 &&
                   textures.at("fallbacks") == 3 &&
                   textures.at("missing_variant_fallbacks") == 1 &&
                   textures.at("unsupported_format_fallbacks") == 1 &&
                   textures.at("invalid_variant_fallbacks") == 1 &&
                   textures.at("selected_mip_bytes") == 987654321 &&
                   textures.at("effective_max_dimension") == 2048,
               "effective texture-tier counters are incomplete") && ok;
    ok = check(document.html.find("legacy gamma-2 color 17") != std::string::npos,
               "HTML omits the distinct legacy color count") && ok;
    ok = check(root.find("frame_intervals") == root.end(),
               "raw frame samples unexpectedly appear in the report") && ok;
    ok = check(document.html.find("GPU &lt;fast&gt; &amp; &quot;bright&quot;") !=
                   std::string::npos,
               "HTML metadata is not escaped") && ok;
    ok = check(document.html.find("Full effective configuration") !=
                   std::string::npos,
               "HTML configuration section is missing") && ok;
    ok = check(document.html.find(
                   "HeavenBench 1.0.0-test [zen4-test&lt;&amp;]") !=
                   std::string::npos,
               "HTML application identity is missing or unescaped") && ok;
    ok = check(document.html.find("Build architecture") != std::string::npos,
               "HTML build architecture row is missing") && ok;
    ok = check(document.html.find("64 meshes / 321 instances") != std::string::npos &&
                   document.html.find("Scene per worker") != std::string::npos,
               "HTML CPU workload counts are missing") && ok;

    TempDirectory temp;
    if (temp.path.empty()) {
        std::printf("[unit_result_report] SKIPPED filesystem checks: "
                    "temporary directory unavailable\n");
        return ok;
    }
    const fs::path results = temp.path / "results";
    const heaven::BenchmarkReportWriteResult first =
        heaven::write_benchmark_report(
            sample_result(), config, results,
            {"1.0.0-test", "zen4-test"}, timestamp);
    const heaven::BenchmarkReportWriteResult second =
        heaven::write_benchmark_report(
            sample_result(), config, results,
            {"1.0.0-test", "zen4-test"}, timestamp);

    ok = check(first.success() && second.success(),
               "matched report pairs were not written") && ok;
    ok = check(first.json_path != second.json_path &&
                   second.json_path.stem().string().ends_with("_1"),
               "same-timestamp report did not receive a collision suffix") && ok;
    ok = check(first.json_path.stem() == first.html_path.stem() &&
                   second.json_path.stem() == second.html_path.stem(),
               "JSON and HTML filenames are not paired") && ok;
    if (first.success() && second.success()) {
        const Json first_json = Json::parse(read_file(first.json_path));
        const Json second_json = Json::parse(read_file(second.json_path));
        ok = check(first_json.at("run").at("id") ==
                       first.json_path.stem().string() &&
                   second_json.at("run").at("id") ==
                       second.json_path.stem().string(),
                   "collision-adjusted run IDs do not match filenames") && ok;
    }

    bool temp_file_found = false;
    for (const fs::directory_entry& entry : fs::directory_iterator(results)) {
        if (entry.path().extension() == ".tmp") temp_file_found = true;
    }
    ok = check(!temp_file_found,
               "successful publish left a temporary file behind") && ok;

    const fs::path not_a_directory = temp.path / "blocked";
    {
        std::ofstream file(not_a_directory);
        file << "regular file";
    }
    const heaven::BenchmarkReportWriteResult failed =
        heaven::write_benchmark_report(
            sample_result(), config, not_a_directory,
            {"1.0.0-test", "zen4-test"}, timestamp);
    ok = check(!failed.success() && !failed.diagnostics.empty(),
               "directory-creation failure was not returned as a diagnostic") && ok;

    if (ok) {
        std::printf("[unit_result_report] PASSED\n");
    }
    return ok;
}

const heaven::testing::Register reg_unit_result_report{unit_result_report};

} // namespace
