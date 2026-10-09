/*
 * HeavenBench — unit test: Config_Writer error reporting
 *
 * Feature: benchmark-config-file
 * — Validates:
 *
 * write_config_file() promises that on an open/write failure it returns a
 * SINGLE diagnostic naming the path and the failure, and performs NO OTHER side
 * effect. This test drives that failure path through
 * the PUBLIC entry point using an unwritable target: a file inside a directory
 * that does not exist. The parent directory's absence makes the underlying
 * ofstream open fail deterministically regardless of the user/permission bits
 * the suite runs under (so it holds for root and non-root alike), which the
 * permission-bit approach ("/", a read-only dir) cannot guarantee portably.
 *
 * The assertions pin the behaviour the current source of truth
 * (config_file.cpp) actually emits:
 *   • EXACTLY one diagnostic is returned,
 *   • that diagnostic NAMES the target path and identifies the write/open
 *     failure (it carries the "--write-config" prefix and the path; the trailing
 *     strerror text is platform-dependent and is NOT asserted), and
 *   • there is no other side effect: the target file does not exist after the
 *     call, and the non-existent parent directory was not created (Requirement
 *
 *
 * write_config_file is GPU-free and does only file I/O, so this runs on the
 * host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "engine/config_file.h"

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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool unit_config_writer_error() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_config_writer_error] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    // Build an unwritable target: a file beneath a directory that does not
    // exist. Open-for-writing fails because the parent path component is
    // missing, independent of permission bits. A monotonic counter keeps the
    // chosen parent name distinct within a run so a stray earlier directory
    // cannot accidentally satisfy the open.
    static std::atomic<unsigned> counter{0};
    const unsigned id = counter.fetch_add(1);
    const std::filesystem::path missing_dir =
        std::filesystem::temp_directory_path() /
        ("heaven_no_such_dir_" + std::to_string(id));
    const std::filesystem::path target = missing_dir / "out.json";

    // Pre-condition: neither the parent directory nor the target may exist, or
    // the test would not be exercising the failure path. (Defensive: remove any
    // stray remnants from a previous aborted run.)
    {
        std::error_code ec;
        std::filesystem::remove_all(missing_dir, ec);
    }
    if (std::filesystem::exists(missing_dir)) {
        fail("precondition: the parent directory should not exist before the call");
    }

    const std::vector<std::string> diags =
        heaven::write_config_file(target.string(), BenchConfig{});

    // Exactly one diagnostic.
    if (diags.size() != 1u) {
        fail("expected exactly 1 diagnostic, got " + std::to_string(diags.size()));
    }

    // The diagnostic names the path and identifies the write/open failure. The
    // strerror suffix is platform-dependent, so assert the stable prefix and
    // the path rather than the full text.
    if (!diags.empty()) {
        const std::string& d = diags[0];
        if (!contains(d, "--write-config")) {
            fail("diagnostic should carry the '--write-config' prefix; got: " + d);
        }
        if (!contains(d, target.string())) {
            fail("diagnostic should name the target path; got: " + d);
        }
    }

    // No other side effect: the target file was not created,
    // and the non-existent parent directory was not created either.
    if (std::filesystem::exists(target)) {
        fail("no-side-effect: the target file must not exist after a failed write");
    }
    if (std::filesystem::exists(missing_dir)) {
        fail("no-side-effect: the missing parent directory must not have been created");
    }

    // Clean up nothing should have been created; remove defensively in case a
    // regression created it, so a failing run does not leave artifacts behind.
    {
        std::error_code ec;
        std::filesystem::remove_all(missing_dir, ec);
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_config_writer_error{
    &unit_config_writer_error};

} // namespace
