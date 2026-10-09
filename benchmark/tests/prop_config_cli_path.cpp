// Feature: benchmark-config-file,
//   --config records the path and consumes its value token
//     (Validates:)
//   The last --config occurrence wins
//     (Validates:)
/*
 * HeavenBench — Property tests for the CLI parser's `--config` path
 * capture.
 *
 * Feature: benchmark-config-file
 *   •: --config records the path and consumes its value token
 *
 *   •: The last --config occurrence wins
 *
 *
 * `--config FILE` is an Arity::Value flag whose value token is captured on a
 * side channel (ParseResult::config_path) rather than written into BenchConfig,
 * because it is a startup action, not a renderer Setting. These properties
 * exercise the PUBLIC entry point directly: they build a real argv, call
 * heaven::parse_cli(argc, argv), and assert on ParseResult::config_path and
 * ParseResult::diagnostics.
 *
 * (path capture + value-token consumption):
 *   For any command line "--config PATH" where PATH is a non-empty value token
 *   that is not flag-like (does not begin with "--") — the precondition of
 *    — parse_cli records exactly that PATH as config_path, and
 *   it does NOT report PATH as an unknown option. (PATH is consumed as the
 *   flag's value, so it can never fall through to the unknown-flag path.)
 *
 * (last occurrence wins):
 *   For any command line containing one or more "--config PATH" pairs, the
 *   recorded config_path equals the value token of the LAST pair, and no
 *   earlier value is retained.
 *
 * Generators produce non-empty, path-like tokens that cannot collide with a
 * real flag name: each token starts with an alphanumeric character (so it can
 * never begin with "--") followed by characters drawn from a path-friendly
 * charset. This keeps the Requirement-1.1 precondition (non-empty, value-token)
 * satisfied by construction.
 *
 * parse_cli is pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

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

// Generate a non-empty, path-like value token that cannot be confused with a
// flag: the first character is alphanumeric (so the token never begins with
// "--"), followed by zero or more characters from a path-friendly charset
// (letters, digits, and the usual path punctuation). This satisfies the
// Requirement-1.1 precondition — a non-empty value token — by construction.
rc::Gen<std::string> gen_path_token() {
    static const std::string head_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const std::string tail_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "/._-~";

    return rc::gen::map(
        rc::gen::pair(
            rc::gen::elementOf(head_chars),
            rc::gen::container<std::string>(rc::gen::elementOf(tail_chars))),
        [](const std::pair<char, std::string>& parts) {
            std::string s;
            s.push_back(parts.first);
            s += parts.second;
            return s;
        });
}

// ──: --config records the path and consumes its value token ──
bool run_p1_config_path_capture() {
    return rc::check(
        "Feature: benchmark-config-file, Property 1: --config records the path "
        "and consumes its value token (Validates: Requirements 1.1)",
        [] {
            const std::string path = *gen_path_token();

            // Precondition guard: the generated token is a non-empty value
            // token, not a flag ('s WHEN clause).
            RC_PRE(!path.empty());
            RC_PRE(!(path.size() >= 2 && path[0] == '-' && path[1] == '-'));

            const ParseResult r = run_parse({"--config", path});

            // The exact token is recorded as the Config_File path.
            RC_ASSERT(r.config_path.has_value());
            RC_ASSERT(*r.config_path == path);

            // The token was consumed as --config's value, so it is NEVER
            // reported as an unknown option.
            const std::string unknown_msg =
                std::format("unknown option '{}'", path);
            RC_ASSERT(std::find(r.diagnostics.begin(), r.diagnostics.end(),
                                unknown_msg) == r.diagnostics.end());
        });
}

// ──: the last --config occurrence wins ──────────────
bool run_p2_last_config_wins() {
    return rc::check(
        "Feature: benchmark-config-file, Property 2: the last --config "
        "occurrence wins (Validates: Requirements 1.7)",
        [] {
            // One or more "--config PATH" pairs, each with a non-empty,
            // non-flag value token.
            const std::vector<std::string> paths =
                *rc::gen::nonEmpty(rc::gen::container<std::vector<std::string>>(
                    gen_path_token()));

            std::vector<std::string> tokens;
            tokens.reserve(paths.size() * 2);
            for (const std::string& p : paths) {
                tokens.emplace_back("--config");
                tokens.push_back(p);
            }

            const ParseResult r = run_parse(tokens);

            // The recorded path is the LAST occurrence's value token, with no
            // earlier value retained.
            RC_ASSERT(r.config_path.has_value());
            RC_ASSERT(*r.config_path == paths.back());
        });
}

bool run_config_cli_path_properties() {
    bool ok = true;
    ok = run_p1_config_path_capture() && ok;
    ok = run_p2_last_config_wins() && ok;
    return ok;
}

const heaven::testing::Register g_register_config_cli_path{
    &run_config_cli_path_properties};

} // namespace
