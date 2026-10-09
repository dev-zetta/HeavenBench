// Feature: in-ui-configuration — shared host-test fakes/fixtures
/*
 * HeavenBench — recording fake RendererEffects + Config_File fixtures for
 * the In-UI Configuration controller tests (tasks 4.4–4.12, 5.x).
 *
 * The configuration controller (rhi/overlay/config_controller.{h,cpp}) drives
 * all renderer side effects through the abstract heaven::RendererEffects
 * interface, and reads/writes Config_Files through the engine/config_file.h
 * module. To property-test the controller in the Filament-free host target this
 * header provides:
 *
 *   • FakeRendererEffects — a RendererEffects implementation that RECORDS every
 *     call (feature toggles, live applies, run cancellations, rebuild requests)
 *     instead of touching a renderer, with a switchable success/fail rebuild()
 *     stub so the transactional-reload properties (9 & 10) can drive both
 *     branches deterministically.
 *
 *   • make_load_result(...) — a convenience builder for a heaven::ConfigLoadResult
 *     fixture, so load-handling tests  can assert against a known
 *     result shape (success vs. parse-failure, with/without diagnostics) without
 *     needing a real file on disk. (Tests that prefer to drive the REAL
 *     load_config_file() via a temp file may still do so; this is a convenience
 *     for the controller's load-result handling logic.)
 *
 * GPU-free: includes only config_controller.h (which transitively pulls
 * config_edit.h, settings_catalog.h, engine/config_file.h, engine/types.h) and
 * the standard library. References no Filament symbol, so it links into the
 * Filament-free heaven_benchmark_tests host target. Header-only and inline so
 * multiple test TUs can include it without ODR conflicts.
 */
#ifndef HEAVENBENCH_TESTS_CONFIG_CONTROLLER_FAKES_H_
#define HEAVENBENCH_TESTS_CONFIG_CONTROLLER_FAKES_H_

#include <expected>
#include <string>
#include <vector>

#include "rhi/overlay/config_controller.h"  // heaven::RendererEffects + controller API

namespace heaven::testing {

// One recorded set_feature_enabled(name, on) call.
struct FeatureToggleCall {
    std::string name;
    bool        on;
};

// One recorded apply_live(id, active) call: the SettingId routed live and a
// snapshot of the Active_Configuration the controller had just updated, so
// tests can assert the controller wrote the new value before calling apply.
struct LiveApplyCall {
    SettingId   id;
    BenchConfig active;
};

// Recording RendererEffects fake. Every virtual records into a public member so
// tests can assert exact call counts, arguments, and ordering. rebuild()'s
// outcome is switchable via `rebuild_succeeds` to drive both the success
// (advance Active→Working) and failure (transactional restore) reload branches.
struct FakeRendererEffects : heaven::RendererEffects {
    // set_feature_enabled history, in call order.
    std::vector<FeatureToggleCall> feature_calls;
    // apply_live history, in call order (7).
    std::vector<LiveApplyCall> live_applies;
    // cancel_active_run() invocation count (10;).
    int cancel_count = 0;
    // rebuild() invocation count.
    int rebuild_count = 0;

    // Switchable rebuild() outcome. When true, rebuild() returns success ({});
    // when false it returns std::unexpected(Error{rebuild_error_message}).
    bool        rebuild_succeeds      = true;
    std::string rebuild_error_message = "rebuild failed";

    void set_feature_enabled(const char* name, bool on) override {
        feature_calls.push_back(
            FeatureToggleCall{name != nullptr ? std::string(name) : std::string(), on});
    }

    void apply_live(SettingId id, const BenchConfig& active) override {
        live_applies.push_back(LiveApplyCall{id, active});
    }

    void cancel_active_run() override { ++cancel_count; }

    Result<void> rebuild() override {
        ++rebuild_count;
        if (rebuild_succeeds) {
            return {};
        }
        return std::unexpected(Error{rebuild_error_message});
    }

    // ── small assertion helpers (optional convenience for the tests) ──

    // Count how many recorded set_feature_enabled calls named `name`.
    [[nodiscard]] int feature_call_count(const std::string& name) const {
        int n = 0;
        for (const FeatureToggleCall& c : feature_calls) {
            if (c.name == name) ++n;
        }
        return n;
    }

    // Count how many recorded apply_live calls carried `id`.
    [[nodiscard]] int live_apply_count(SettingId id) const {
        int n = 0;
        for (const LiveApplyCall& c : live_applies) {
            if (c.id == id) ++n;
        }
        return n;
    }
};

// Build a ConfigLoadResult fixture for load-handling tests. `parsed_ok` selects
// the parse-success vs parse-failure shape, `diags` injects any diagnostics the
// loader would have collected (empty = clean load). The returned config is the
// one a successful load would copy into the Working_Configuration.
[[nodiscard]] inline ConfigLoadResult make_load_result(
    BenchConfig cfg, bool parsed_ok, std::vector<std::string> diags = {}) {
    ConfigLoadResult r;
    r.config      = std::move(cfg);
    r.parsed_ok   = parsed_ok;
    r.diagnostics = std::move(diags);
    return r;
}

} // namespace heaven::testing

#endif // HEAVENBENCH_TESTS_CONFIG_CONTROLLER_FAKES_H_
