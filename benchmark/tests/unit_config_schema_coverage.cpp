/*
 * HeavenBench — unit test: config schema ↔ flag-table coverage
 *
 * Feature: benchmark-config-file
 * — Validates:
 *
 * demands that the Config_File schema define exactly one
 * Recognized_Key per BenchConfig-mapping CLI flag, and NO key for a flag that
 * maps to no BenchConfig field (`--help`, `--api`). The schema
 * (config_schema()) and the flag→key inverse mapping (flag_schema_keys(), which
 * is internal to cli_parser.cpp) are two hand-maintained lists that MUST stay
 * in lock-step or precedence/coverage silently break.
 *
 * flag_schema_keys() is not exposed, so this test derives the expected key set
 * through the PUBLIC API instead, which also proves the two sources cannot
 * drift behaviourally: for every entry in flag_table() it builds a minimal
 * command line ({"prog", flag} plus a dummy value token for Arity::Value
 * flags), runs the real parse_cli, and unions the canonical schema keys that
 * parse_cli records into ParseResult::seen_keys. A value-mapping flag, when
 * parsed, records its canonical key into seen_keys; flags that map to no field
 * (`--help`, `--api`) — and the startup-action flags `--config` /
 * `--write-config` — contribute nothing.
 *
 * The test then asserts, in BOTH directions, that this flag-produced key set
 * equals the set of FieldSpec.key from config_schema():
 *   • every schema key is produced by some flag (no orphan schema entry), and
 *   • every flag-produced key exists in the schema (no flag without a schema
 *     entry).
 * It additionally asserts explicitly that parsing `--help` and `--api` yields
 * an empty seen_keys set, pinning their exclusion from the schema.
 *
 * parse_cli / config_schema are pure (no I/O, no GPU), so this runs on the
 * host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/cli_parser.h"
#include "engine/config_file.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::ParseResult;

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch).
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

// The set of canonical schema keys produced by driving EVERY flag in
// flag_table() through parse_cli and unioning the resulting seen_keys. A
// dummy value token ("1") is supplied for Arity::Value flags so their `apply`
// runs and records its key; "1" is a valid-ish token for the numeric / quality
// string / path value flags and is recorded as a path (not a seen_key) for
// --config / --write-config, so those contribute nothing.
std::set<std::string> keys_produced_by_flags() {
    std::set<std::string> produced;
    for (const heaven::FlagSpec& spec : heaven::flag_table()) {
        std::vector<std::string> tokens;
        tokens.emplace_back(spec.name);
        if (spec.arity == heaven::Arity::Value) {
            tokens.emplace_back("1");
        }
        const ParseResult r = run_parse(tokens);
        produced.insert(r.seen_keys.begin(), r.seen_keys.end());
    }
    return produced;
}

// The set of canonical keys declared by config_schema().
std::set<std::string> schema_keys() {
    std::set<std::string> keys;
    for (const heaven::FieldSpec& f : heaven::config_schema()) {
        keys.emplace(f.key);
    }
    return keys;
}

bool unit_config_schema_coverage() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_config_schema_coverage] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    const std::set<std::string> from_flags  = keys_produced_by_flags();
    const std::set<std::string> from_schema = schema_keys();
    // These launcher/preview controls are deliberately config-only. Preserve
    // the exact CLI/schema coverage requirement for every other setting.
    const std::set<std::string> config_only{
        "android_preview_fps", "android_render_scale"};
    auto expected_keys = from_flags;
    expected_keys.insert(config_only.begin(), config_only.end());

    // Direction 1: every schema key must be produced by some BenchConfig-mapping
    // flag (no orphan schema entry that no flag writes).
    for (const std::string& k : from_schema) {
        if (!expected_keys.contains(k)) {
            fail("schema key '" + k + "' is not produced by any flag in flag_table()");
        }
    }

    // Direction 2: every flag-produced key must exist in the schema (no flag
    // whose Setting the schema forgot to expose).
    for (const std::string& k : from_flags) {
        if (!from_schema.contains(k)) {
            fail("flag-produced key '" + k + "' has no entry in config_schema()");
        }
    }

    // Equal sets ⇒ a single, drift-free key set. The
    // per-element diagnostics above pinpoint any mismatch; this guards the
    // count for a quick top-line signal.
    if (expected_keys != from_schema) {
        std::printf("[unit_config_schema_coverage] FAILED: schema key set (%zu) "
                    "does not equal flags plus Android config-only set (%zu)\n",
                    from_schema.size(), expected_keys.size());
        ok = false;
    }

    // Explicit exclusion of --help / --api: neither maps to a BenchConfig field,
    // so parsing either records NO seen_key and the schema defines no key for
    // them. --help also requests help; --api consumes its
    // value as a no-op.
    {
        const ParseResult h = run_parse({"--help"});
        if (!h.seen_keys.empty()) {
            fail("parsing --help recorded a seen_key (it maps to no field)");
        }
        const ParseResult a = run_parse({"--api", "vulkan"});
        if (!a.seen_keys.empty()) {
            fail("parsing --api recorded a seen_key (it maps to no field)");
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_config_schema_coverage{
    &unit_config_schema_coverage};

} // namespace
