/*
 * HeavenBench — unit test: dead-code / debug-output cleanup (structural)
 *
 * — Validates:
 *
 * removed two pieces of shipping-build cruft and this deterministic
 * test pins them out of the source so they cannot quietly creep back in:
 *
 *   •  — SceneBuilder::build() must not emit stray shipping-path debug
 *     `printf` output. The earlier code printed a HIDDEN-island log, a
 *     TRANSFORM/BBOX dump, and an end-of-build "persistent entities created"
 *     count. We assert the build() function body contains no `printf(` call at
 *     all (covers both `printf` and `std::printf`), and that the four removed
 *     log markers no longer appear anywhere in scene_builder.cpp.
 *   •  — MeshStore::upload() must not retain the dead `MeshData fmd`
 *     accumulator (it was filled via push_back but never consumed). We assert
 *     the `fmd` identifier no longer appears anywhere in mesh_store.cpp.
 *
 * Because these are source-level guarantees (the artifacts are absent text, not
 * runtime behaviour), the cleanest deterministic check is to read the actual
 * post-21.1 source at runtime and assert the removed patterns are gone. The two
 * source files are located via the SCENE_BUILDER_SRC_PATH / MESH_STORE_SRC_PATH
 * compile-time macros, absolute paths defined for this target only in
 * benchmark/CMakeLists.txt (ctest's CWD is the build dir, so a relative path
 * would be fragile). If a file cannot be opened the test FAILS with a
 * diagnostic — a missing or moved source is exactly the kind of drift this test
 * exists to catch.
 *
 * Self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time, like the other test translation units.
 */
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#ifndef SCENE_BUILDER_SRC_PATH
#error "SCENE_BUILDER_SRC_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
#endif
#ifndef MESH_STORE_SRC_PATH
#error "MESH_STORE_SRC_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
#endif

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Read an entire source file into a string. Returns false (with a diagnostic)
// if the file cannot be opened or is empty — a moved/missing source is drift.
bool read_source(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("[unit_cleanup_structural] FAILED: cannot open source '%s'\n",
                    path);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    if (out.empty()) {
        std::printf("[unit_cleanup_structural] FAILED: source '%s' is empty\n",
                    path);
        return false;
    }
    return true;
}

// Extract the body of a function given a unique signature anchor (e.g.
// "SceneBuilder::build("). Brace-matches from the first '{' after the anchor to
// its matching '}'. Returns false if the anchor or a balanced body is not found.
bool extract_function_body(const std::string& src, const std::string& anchor,
                           std::string& body) {
    const std::size_t a = src.find(anchor);
    if (a == std::string::npos) return false;

    const std::size_t open = src.find('{', a);
    if (open == std::string::npos) return false;

    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                body = src.substr(open, i - open + 1);
                return true;
            }
        }
    }
    return false; // unbalanced braces
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool unit_cleanup_structural() {
    bool ok = true;

    // ──  — no stray shipping printf in SceneBuilder::build() ──
    std::string scene_builder_src;
    if (!read_source(SCENE_BUILDER_SRC_PATH, scene_builder_src)) {
        return false;
    }

    std::string build_body;
    if (!extract_function_body(scene_builder_src, "SceneBuilder::build(",
                               build_body)) {
        std::printf("[unit_cleanup_structural] FAILED: could not locate the "
                    "SceneBuilder::build() body in '%s'\n",
                    SCENE_BUILDER_SRC_PATH);
        return false;
    }

    // The only printf flavours the codebase uses are `printf(` and
    // `std::printf(`; both contain the substring "printf(". A clean build()
    // body has none.
    if (contains(build_body, "printf(")) {
        std::printf("[unit_cleanup_structural] FAILED: SceneBuilder::build() "
                    "still contains a printf call (R19.1 — shipping-path debug "
                    "output must be removed)\n");
        ok = false;
    }

    // The four specific markers that the three removed prints carried. None may
    // appear anywhere in scene_builder.cpp.
    const std::vector<std::string> removed_markers = {
        "HIDDEN", "TRANSFORM", "BBOX", "persistent entities created"};
    for (const std::string& marker : removed_markers) {
        if (contains(scene_builder_src, marker)) {
            std::printf("[unit_cleanup_structural] FAILED: removed debug marker "
                        "'%s' still present in scene_builder.cpp (R19.1)\n",
                        marker.c_str());
            ok = false;
        }
    }

    // ──  — no dead MeshData accumulator in MeshStore::upload() ──
    std::string mesh_store_src;
    if (!read_source(MESH_STORE_SRC_PATH, mesh_store_src)) {
        return false;
    }

    // The dead accumulator was named `fmd` (a local MeshData filled via
    // push_back but never consumed). Its identifier must be gone entirely.
    if (contains(mesh_store_src, "fmd")) {
        std::printf("[unit_cleanup_structural] FAILED: dead 'fmd' MeshData "
                    "accumulator still present in mesh_store.cpp (R19.2)\n");
        ok = false;
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_cleanup_structural{unit_cleanup_structural};

} // namespace
