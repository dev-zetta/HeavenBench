/*
 * HeavenBench — requested bloom strength, settings and day/night compatibility
 */
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "rhi/bloom_strength.h"
#include "rhi/bloom_dirt.h"
#include "rhi/overlay/settings_catalog.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool require(bool condition, const char* message) {
    if (!condition) std::printf("[unit_bloom_strength] FAILED: %s\n", message);
    return condition;
}

struct TempConfig {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("heaven_bloom_strength_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ~TempConfig() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    void write(const char* json) const { std::ofstream(path) << json; }
};

bool test_bloom_strength() {
    using namespace heaven;
    bool ok = true;
    // This is an integration contract: the shader multiplies bloom by this
    // map. Black/clean texels must preserve bloom; white dirt adds only 20%.
    const std::array<std::uint8_t, 9> dirt_rgb{0, 0, 0, 128, 128, 128, 255, 255, 255};
    const auto dirt_gain = make_bloom_dirt_gain_rgba(dirt_rgb, 3);
    ok &= require(dirt_gain && dirt_gain->size() == 12 &&
        (*dirt_gain)[0] == 1.0f && (*dirt_gain)[3] == 1.0f &&
        (*dirt_gain)[8] == 1.2f && (*dirt_gain)[11] == 1.0f,
        "lens dirt removed clean bloom or exceeded bounded gain");
    ok &= require(dirt_gain && std::fabs((*dirt_gain)[4] - 1.0431721f) < 1e-6f,
        "lens dirt was encoded as sRGB gain or converted twice");
    for (unsigned value = 0; value < 256; ++value) {
        const float gain = bloom_dirt_gain_from_srgb_byte(static_cast<uint8_t>(value));
        ok &= require(std::isfinite(gain) && gain >= 1.0f && gain <= 1.2f,
                      "lens-dirt gain left its finite bound");
    }
    const std::array<std::uint8_t, 4> dirt_rgba{0, 128, 255, 0};
    const auto alpha_ignored = make_bloom_dirt_gain_rgba(dirt_rgba, 4);
    ok &= require(alpha_ignored && (*alpha_ignored)[3] == 1.0f &&
        (*alpha_ignored)[0] == 1.0f && (*alpha_ignored)[2] == 1.2f,
        "source alpha attenuated RGB bloom");
    ok &= require(!make_bloom_dirt_gain_rgba({}, 3) &&
        !make_bloom_dirt_gain_rgba(dirt_rgba, 3) &&
        !make_bloom_dirt_gain_rgba(dirt_rgb, 2), "malformed dirt layout was accepted");
    const float reference = render_tuning::lighting.bloom_reference_strength;
    ok &= require(AppConfig{}.bloom_strength == 0.3f, "default strength changed");
    for (float lux : std::array{0.0f, 1000.0f, 6000.0f, 55000.0f, 110000.0f}) {
        const float old = render_tuning::lighting.bloom_strength.x +
            std::clamp(lux / render_tuning::lighting.reference_sun_lux, 0.0f, 1.0f) *
            render_tuning::lighting.bloom_strength.y;
        ok &= require(std::bit_cast<uint32_t>(old) == std::bit_cast<uint32_t>(
            bloom_strength_for_time_of_day(reference, lux)), "default curve lost bit parity");
        ok &= require(bloom_strength_for_time_of_day(0.0f, lux) == 0.0f,
                      "zero request still emits bloom");
        ok &= require(bloom_strength_for_time_of_day(0.6f, lux) == old * 2.0f,
                      "request is not respected at another time of day");
    }
    ok &= require(clamp_bloom_strength(-1.0f).value == 0.0f &&
        clamp_bloom_strength(2.0f).value == 1.0f, "strength bounds failed");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
        ok &= require(clamp_bloom_strength(invalid).value == reference &&
                      clamp_bloom_strength(invalid).clamped, "nonfinite request is not safe");
    }

    const std::array<std::string_view, 3> args{"heaven", "--bloom-strength", "0.9"};
    auto parsed = parse_cli(args);
    ok &= require(parsed.config.bloom_strength == 0.9f &&
        parsed.seen_keys.contains("bloom_strength") && parsed.diagnostics.empty(),
        "CLI strength or persistent-key tracking failed");
    const std::array<std::string_view, 3> bad{"heaven", "--bloom-strength", "nan"};
    auto rejected = parse_cli(bad);
    ok &= require(rejected.config.bloom_strength == reference &&
        !rejected.diagnostics.empty(), "malformed CLI strength was accepted");
    const std::array<std::string_view, 3> high{"heaven", "--bloom-strength", "2"};
    ok &= require(parse_cli(high).config.bloom_strength == 1.0f, "CLI range clamp failed");

    TempConfig file;
    file.write("{\"bloom_strength\": 0.6}");
    auto loaded = load_config_file(file.path.string(), BenchConfig{});
    ok &= require(loaded.parsed_ok && loaded.config.bloom_strength == 0.6f &&
        loaded.diagnostics.empty(), "config strength load failed");
    apply_cli_overlay(loaded.config, parsed);
    ok &= require(loaded.config.bloom_strength == 0.9f, "CLI did not override file strength");
    ok &= require(write_config_file(file.path.string(), loaded.config).empty(),
                  "strength serialization failed");
    ok &= require(load_config_file(file.path.string(), BenchConfig{}).config.bloom_strength == 0.9f,
                  "strength did not round trip");
    file.write("{\"bloom_strength\": -1}");
    loaded = load_config_file(file.path.string(), BenchConfig{});
    ok &= require(loaded.parsed_ok && loaded.config.bloom_strength == 0.0f &&
        !loaded.diagnostics.empty(), "config range clamp failed");

    const auto& entry = catalog_entry(SettingId::BloomStrength);
    BenchConfig config;
    ok &= require(entry.apply == Apply::Live && entry.widget == Widget::SliderFloat &&
        entry.min_value == 0.0f && entry.max_value == 1.0f, "strength UI contract failed");
    entry.apply_to(config, 0.9f);
    ok &= require(config.bloom_strength == 0.9f &&
        std::get<float>(entry.read(config)) == 0.9f, "UI strength does not reach active config");
    entry.apply_to(config, 2.0f);
    ok &= require(config.bloom_strength == 1.0f, "UI range clamp failed");
    return ok;
}

const heaven::testing::Register registered{test_bloom_strength};

} // namespace
