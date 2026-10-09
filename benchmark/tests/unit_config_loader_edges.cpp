/*
 * HeavenBench — unit test: Config_Loader edge cases & exact diagnostics
 *
 * Feature: benchmark-config-file
 * — Validates:,
 *
 *
 * load_config_file() is exercised through its PUBLIC entry point against
 * concrete, hand-written Config_File contents written to unique temp files.
 * Where the property tests (prop_config_*.cpp) prove the universal invariants
 * across random inputs, this file pins the EXACT, human-facing behaviour the
 * property tests deliberately leave loose:
 *
 *   • the well-formed-but-empty / all-unknown object is a parse SUCCESS that
 *     leaves the base config untouched,
 *   • a syntactically invalid document is a parse FAILURE that names the path
 *     and a parse-location indicator and applies no field,
 *   • empty / whitespace-only content is a parse FAILURE naming the path
 *
 *   • the clamp, fixed-set reject, unknown-key, and
 *     type-mismatch  diagnostics read EXACTLY as the loader emits them,
 *   • the returned diagnostics vector preserves the loader's traversal order
 *
 *
 * IMPORTANT — diagnostic order vs. document order: the loader iterates
 * `root.items()` over an nlohmann::json object, whose default storage
 * (std::map) keeps members sorted ALPHABETICALLY by key, NOT in the order they
 * appear in the source text. The insertion-order test below is therefore
 * designed around that alphabetical traversal and asserts the diagnostics
 * appear in the loader's actual emission order. The expected strings are pinned
 * to config_file.cpp's current wording (the source of truth); if the loader's
 * text ever changes, update the expectations here, never the loader.
 *
 * load_config_file is GPU-free and does only file I/O, so this runs on the
 * host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "engine/config_file.h"
#include "engine/render_presets.h"
#include "rhi/overlay/settings_catalog.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::BenchConfig;
using heaven::ConfigLoadResult;

// ── Temp-file scaffolding ──────────────────────────────────────

// Write `content` to a fresh, uniquely-named file in the OS temp dir, return
// its path. A monotonic counter keeps names distinct within a run so cases
// never collide. The caller removes the file via TempConfig's destructor.
struct TempConfig {
    std::filesystem::path path;

    explicit TempConfig(const std::string& content) {
        static std::atomic<unsigned> counter{0};
        const unsigned id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               ("heaven_cfg_edge_" + std::to_string(id) + ".json");
        std::ofstream os(path, std::ios::binary | std::ios::trunc);
        os << content;
        os.flush();
    }

    ~TempConfig() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    std::string str() const { return path.string(); }
};

// Whole-config equality used to prove "no field mutated" on the failure/empty
// paths. BenchConfig has no operator==, so compare its serialized form: two
// configs are field-equal iff write_config_file emits identical JSON for each
// (the writer covers every schema Setting). This sidesteps
// hand-listing every field and stays correct as fields are added.
bool configs_equal(const BenchConfig& a, const BenchConfig& b) {
    TempConfig ta("");
    TempConfig tb("");
    const auto a_diagnostics = heaven::write_config_file(ta.str(), a);
    const auto b_diagnostics = heaven::write_config_file(tb.str(), b);
    if (!a_diagnostics.empty() || !b_diagnostics.empty()) {
        return false;
    }
    std::ifstream fa(ta.str(), std::ios::binary);
    std::ifstream fb(tb.str(), std::ios::binary);
    const std::string sa((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
    const std::string sb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
    return sa == sb;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool unit_config_loader_edges() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_config_loader_edges] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    const BenchConfig base{}; // default-constructed baseline

    // ── 1. Empty object "{}" — parse success, zero settings, base intact ──

    {
        TempConfig f("{}");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("empty object: expected parsed_ok==true");
        if (!r.diagnostics.empty())
            fail("empty object: expected zero diagnostics, got " +
                 std::to_string(r.diagnostics.size()));
        if (!configs_equal(r.config, base))
            fail("empty object: config should equal the base (no settings applied)");
    }

    // ── 2. All-unknown object — parse success, one diag per unknown key ──
    // Keys chosen so the alphabetical traversal order is
    // {"bar", "foo"}: each must appear in its own "unknown key" diagnostic.
    {
        TempConfig f(R"({"foo": 1, "bar": true})");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("all-unknown: expected parsed_ok==true");
        if (r.diagnostics.size() != 2u)
            fail("all-unknown: expected exactly 2 diagnostics, got " +
                 std::to_string(r.diagnostics.size()));
        // One diagnostic naming each unknown key, in alphabetical traversal
        // order (bar before foo).
        const std::string expect_bar =
            "--config '" + f.str() + "': unknown key 'bar' ignored";
        const std::string expect_foo =
            "--config '" + f.str() + "': unknown key 'foo' ignored";
        if (r.diagnostics.size() == 2u) {
            if (r.diagnostics[0] != expect_bar)
                fail("all-unknown: diag[0] != expected 'bar' text; got: " + r.diagnostics[0]);
            if (r.diagnostics[1] != expect_foo)
                fail("all-unknown: diag[1] != expected 'foo' text; got: " + r.diagnostics[1]);
        }
        if (!configs_equal(r.config, base))
            fail("all-unknown: config should equal the base (unknown keys change nothing)");
    }

    // ── 3. Syntactically invalid JSON — parse failure, path + "byte" ──

    {
        TempConfig f("{ not json");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (r.parsed_ok) fail("invalid JSON: expected parsed_ok==false");
        if (r.diagnostics.size() != 1u)
            fail("invalid JSON: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        if (!r.diagnostics.empty()) {
            const std::string& d = r.diagnostics[0];
            if (!contains(d, f.str()))
                fail("invalid JSON: diagnostic should name the path; got: " + d);
            // The parse-location indicator is the byte offset reported by the
            // Config_Library ("...JSON parse error at byte N...").
            if (!contains(d, "byte"))
                fail("invalid JSON: diagnostic should carry a parse-location "
                     "indicator ('byte'); got: " + d);
        }
        if (!configs_equal(r.config, base))
            fail("invalid JSON: config should equal the base (no field applied)");
    }

    // ── 4a. Empty content "" — parse failure naming empty/whitespace ──

    {
        TempConfig f("");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (r.parsed_ok) fail("empty content: expected parsed_ok==false");
        if (r.diagnostics.size() != 1u)
            fail("empty content: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        if (!r.diagnostics.empty()) {
            const std::string& d = r.diagnostics[0];
            if (!contains(d, f.str()))
                fail("empty content: diagnostic should name the path; got: " + d);
            if (!contains(d, "empty/whitespace"))
                fail("empty content: diagnostic should mention empty/whitespace; got: " + d);
        }
        if (!configs_equal(r.config, base))
            fail("empty content: config should equal the base");
    }

    // ── 4b. Whitespace-only content — same failure path ──

    {
        TempConfig f("   \n\t");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (r.parsed_ok) fail("whitespace content: expected parsed_ok==false");
        if (r.diagnostics.size() != 1u)
            fail("whitespace content: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        if (!r.diagnostics.empty()) {
            const std::string& d = r.diagnostics[0];
            if (!contains(d, "empty/whitespace"))
                fail("whitespace content: diagnostic should mention empty/whitespace; got: " + d);
        }
        if (!configs_equal(r.config, base))
            fail("whitespace content: config should equal the base");
    }

    // ── 5a. Clamp ABOVE range: soft_shadow_level 5 → clamped to 2 ──
    // Exact clamp string pinned to config_file.cpp.
    {
        TempConfig f(R"({"quality.soft_shadow_level": 5})");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("clamp-high: expected parsed_ok==true");
        if (r.diagnostics.size() != 1u)
            fail("clamp-high: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        const std::string expect =
            "WARN: quality.soft_shadow_level '5' out of range [0,2]; clamped to level 2";
        if (!r.diagnostics.empty() && r.diagnostics[0] != expect)
            fail("clamp-high: diagnostic text mismatch; got: " + r.diagnostics[0]);
        if (static_cast<int>(r.config.quality.soft_shadow_level) != 2)
            fail("clamp-high: soft_shadow_level should be clamped to 2");
    }

    // ── 5b. Clamp BELOW range: soft_shadow_level -1 → clamped to 0 ──

    {
        TempConfig f(R"({"quality.soft_shadow_level": -1})");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("clamp-low: expected parsed_ok==true");
        if (r.diagnostics.size() != 1u)
            fail("clamp-low: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        const std::string expect =
            "WARN: quality.soft_shadow_level '-1' out of range [0,2]; clamped to level 0";
        if (!r.diagnostics.empty() && r.diagnostics[0] != expect)
            fail("clamp-low: diagnostic text mismatch; got: " + r.diagnostics[0]);
        if (static_cast<int>(r.config.quality.soft_shadow_level) != 0)
            fail("clamp-low: soft_shadow_level should be clamped to 0");
    }

    // ── 6. Fixed-set reject: msaa_samples 3 → retain base (default 2) ──
    // The balanced default is 2x, so the rejected request
    // retains 2 samples.
    {
        TempConfig f(R"({"quality.msaa_samples": 3})");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("msaa-reject: expected parsed_ok==true");
        if (r.diagnostics.size() != 1u)
            fail("msaa-reject: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        const std::string expect =
            "WARN: quality.msaa_samples '3' (=3) is not in {1,2,4}; retaining 2 sample(s)";
        if (!r.diagnostics.empty() && r.diagnostics[0] != expect)
            fail("msaa-reject: diagnostic text mismatch; got: " + r.diagnostics[0]);
        if (r.config.quality.msaa_samples != base.quality.msaa_samples)
            fail("msaa-reject: msaa_samples should retain the base value");
    }

    // ── 7. Type mismatch: string for the int field "width" ──
    // nlohmann names a string value "string"; the Int field
    // expects "integer". Field retains the base value.
    {
        TempConfig f(R"({"width": "notanumber"})");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("type-mismatch: expected parsed_ok==true");
        if (r.diagnostics.size() != 1u)
            fail("type-mismatch: expected exactly 1 diagnostic, got " +
                 std::to_string(r.diagnostics.size()));
        const std::string expect =
            "--config '" + f.str() +
            "': key 'width' expected integer but got string; retaining current value";
        if (!r.diagnostics.empty() && r.diagnostics[0] != expect)
            fail("type-mismatch: diagnostic text mismatch; got: " + r.diagnostics[0]);
        if (r.config.width != base.width)
            fail("type-mismatch: width should retain the base value");
    }

    // ── 8. Insertion-order preservation  ──
    // The loader iterates root.items() over an nlohmann::json object whose
    // members are stored ALPHABETICALLY by key (std::map), so document order in
    // the source text is irrelevant — traversal order is the sorted key order.
    // For the keys below the sorted order is:
    //     "quality.soft_shadow_level"  (q…)
    //     "unknownA"                   (u…A)
    //     "unknownB"                   (u…B)
    // which must yield diagnostics in exactly that order: the clamp first, then
    // the two unknown-key reports. The source text deliberately lists them in a
    // DIFFERENT order to prove the result follows traversal order, not text
    // order.
    {
        TempConfig f(R"({"unknownB": 2, "quality.soft_shadow_level": 9, "unknownA": 1})");
        const ConfigLoadResult r = heaven::load_config_file(f.str(), base);
        if (!r.parsed_ok) fail("order: expected parsed_ok==true");
        if (r.diagnostics.size() != 3u) {
            fail("order: expected exactly 3 diagnostics, got " +
                 std::to_string(r.diagnostics.size()));
        } else {
            const std::string expect0 =
                "WARN: quality.soft_shadow_level '9' out of range [0,2]; clamped to level 2";
            const std::string expect1 =
                "--config '" + f.str() + "': unknown key 'unknownA' ignored";
            const std::string expect2 =
                "--config '" + f.str() + "': unknown key 'unknownB' ignored";
            if (r.diagnostics[0] != expect0)
                fail("order: diag[0] should be the clamp; got: " + r.diagnostics[0]);
            if (r.diagnostics[1] != expect1)
                fail("order: diag[1] should be unknownA; got: " + r.diagnostics[1]);
            if (r.diagnostics[2] != expect2)
                fail("order: diag[2] should be unknownB; got: " + r.diagnostics[2]);
        }
    }

    return ok;
}

bool unit_postprocess_config_controls() {
    constexpr struct Control {
        heaven::SettingId id;
        bool heaven::FeatureToggles::* member;
    } controls[]{
        {heaven::SettingId::FeatureBloom, &heaven::FeatureToggles::bloom},
        {heaven::SettingId::FeatureAmbientOcclusion, &heaven::FeatureToggles::ambient_occlusion},
        {heaven::SettingId::FeatureTaa, &heaven::FeatureToggles::taa},
        {heaven::SettingId::FeatureDepthOfField, &heaven::FeatureToggles::depth_of_field},
    };
    const TempConfig empty("{}");
    const auto defaults = heaven::load_config_file(empty.str(), BenchConfig{});
    if (!defaults.parsed_ok || !defaults.diagnostics.empty()) return false;
    for (const auto& control : controls) {
        if (!(defaults.config.features.*control.member)) return false;
        const auto& entry = heaven::catalog_entry(control.id);
        if (entry.apply != heaven::Apply::Reload ||
            entry.widget != heaven::Widget::Checkbox) return false;
        BenchConfig edited;
        entry.apply_to(edited, false);
        if (std::get<bool>(entry.read(edited)) ||
            edited.features.*control.member) return false;
        entry.apply_to(edited, true);
        if (!std::get<bool>(entry.read(edited)) ||
            !(edited.features.*control.member)) return false;
        for (const auto preset : {heaven::RenderPreset::BalancedSystem,
                                  heaven::RenderPreset::VisualUltra,
                                  heaven::RenderPreset::GpuTorture,
                                  heaven::RenderPreset::CpuTorture}) {
            heaven::apply_render_preset(edited, preset);
            edited.features.*control.member = false;
            if (heaven::detect_render_preset(edited) != heaven::RenderPreset::Custom)
                return false;
            heaven::apply_render_preset(edited, preset);
            if (!(edited.features.*control.member) ||
                heaven::detect_render_preset(edited) != preset) return false;
        }
    }
    const TempConfig roundtrip("");
    for (unsigned mask = 0; mask < 16; ++mask) {
        BenchConfig config;
        config.features.bloom = (mask & 1u) != 0;
        config.features.ambient_occlusion = (mask & 2u) != 0;
        config.features.taa = (mask & 4u) != 0;
        config.features.depth_of_field = (mask & 8u) != 0;
        if (!heaven::write_config_file(roundtrip.str(), config).empty()) return false;
        const auto loaded = heaven::load_config_file(roundtrip.str(), BenchConfig{});
        if (!loaded.parsed_ok || !loaded.diagnostics.empty()) return false;
        for (const auto& control : controls) {
            if (loaded.config.features.*control.member !=
                config.features.*control.member) return false;
        }
    }
    return true;
}

const heaven::testing::Register reg_postprocess_controls{unit_postprocess_config_controls};

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_config_loader_edges{
    &unit_config_loader_edges};

} // namespace
