/* Diagnostic trace flags are transient, strict, and compatible with captures/tours. */
#include "engine/cli_parser.h"

#include <array>
#include <cstdio>
#include <functional>
#include <string_view>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
bool check(bool value, const char* message) {
    if (!value) std::printf("[unit_microprofiler_cli] %s\n", message);
    return value;
}
bool microprofiler_cli() {
    using heaven::parse_cli;
    const auto defaults = parse_cli(std::array<std::string_view, 1>{"heaven"});
    if (!check(!defaults.microprofile.output_base && defaults.microprofile.seconds == 10.0f &&
               !defaults.microprofile.visible && !defaults.microprofile.invalid,
               "default must not record or open UI")) return false;
    for (const auto seconds : {"0.1", "10", "3600"}) {
        const auto parsed = parse_cli(std::array<std::string_view, 6>{
            "heaven", "--microprofile", "trace with spaces/run", "--microprofile-seconds", seconds,
            "--microprofile-visible"});
        if (!check(!parsed.microprofile.invalid && parsed.diagnostics.empty() &&
                   parsed.microprofile.output_base == "trace with spaces/run" &&
                   parsed.microprofile.visible && parsed.seen_keys.empty() &&
                   parsed.config_operations.empty(), "valid trace mutated persisted config")) return false;
    }
    for (const auto seconds : {"0", "0.099", "-1", "3601", "nan", "inf", "1s", "1e50", ""}) {
        const auto parsed = parse_cli(std::array<std::string_view, 5>{
            "heaven", "--microprofile", "trace", "--microprofile-seconds", seconds});
        if (!check(parsed.microprofile.invalid && !parsed.diagnostics.empty(),
                   "invalid duration accepted")) return false;
    }
    for (const auto flag : {"--microprofile", "--microprofile-seconds"}) {
        const auto parsed = parse_cli(std::array<std::string_view, 3>{"heaven", flag, "--benchmark"});
        if (!check(parsed.microprofile.invalid && parsed.config.benchmark_mode,
                   "missing value swallowed next flag")) return false;
    }
    for (const auto flag : {"--microprofile-seconds", "--microprofile-visible"}) {
        const auto parsed = flag == std::string_view("--microprofile-visible") ?
            parse_cli(std::array<std::string_view, 2>{"heaven", flag}) :
            parse_cli(std::array<std::string_view, 3>{"heaven", flag, "10"});
        if (!check(parsed.microprofile.invalid, "orphan trace option accepted")) return false;
    }
    const auto nul = parse_cli(std::array<std::string_view, 3>{
        "heaven", "--microprofile", std::string_view("a\0b", 3)});
    if (!check(nul.microprofile.invalid, "NUL output base accepted")) return false;
    const auto empty = parse_cli(std::array<std::string_view, 3>{"heaven", "--microprofile", ""});
    if (!check(empty.microprofile.invalid, "empty output base accepted")) return false;
    const auto benchmark = parse_cli(std::array<std::string_view, 4>{
        "heaven", "--benchmark", "--microprofile", "tour"});
    const auto capture = parse_cli(std::array<std::string_view, 5>{
        "heaven", "--capture", "image.ppm", "--microprofile", "trace"});
    const auto profile = parse_cli(std::array<std::string_view, 5>{
        "heaven", "--profile", "times.json", "--microprofile", "trace"});
    const auto duplicate = parse_cli(std::array<std::string_view, 9>{
        "heaven", "--microprofile", "first", "--microprofile-seconds", "1", "--microprofile", "last",
        "--microprofile-seconds", "2"});
    return check(!benchmark.microprofile.invalid && !capture.microprofile.invalid &&
                 !capture.capture.invalid && !profile.microprofile.invalid && !profile.capture.invalid &&
                 duplicate.microprofile.output_base == "last" && duplicate.microprofile.seconds == 2.0f &&
                 heaven::generate_help().find("--microprofile-visible") != std::string::npos,
                 "compatible actions, repeated values, or help failed");
}
const heaven::testing::Register registered{microprofiler_cli};
}
