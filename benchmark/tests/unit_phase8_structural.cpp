/*
 * HeavenBench — unit test:  structural single-registration (source)
 *
 * — Validates:
 *
 * (advanced materials, shadows & color grading) adds its capabilities
 * as exactly one of: a Material_Model_Selector extension to
 * MaterialInstanceCache, a View_Option_Group applied through
 * configure_post_processing / LightingSystem / update_camera, or an
 * IRenderFeature under rhi/features/. The hard architectural contract
 * is:
 *
 *   • The single coordinating IRenderFeature — AdvancedMaterialsFeature — is
 *     registered EXACTLY ONCE in FilamentBackend::init_features.
 *   • FilamentBackend gains NO per-effect data member for any
 *     capability. The capabilities are wired through the existing
 *     MaterialInstanceCache setters (set_advanced_models / set_specular_aa /
 *     set_transparent_shadows / set_colored_penumbra), the
 *     configure_post_processing args, and update_camera reading config_ — NOT
 *     new FilamentBackend members.
 *   • NO per-effect work is inlined into FilamentBackend::render_frame — the
 *     capability application calls live in step methods (update_camera),
 *     subsystems (MaterialInstanceCache), and configure_post_processing.
 *
 * Following the unit_cleanup_structural.cpp precedent, these are SOURCE-LEVEL
 * guarantees (registration cardinality and the presence/absence of identifiers
 * in specific files), so the cleanest deterministic check reads the actual
 * source at runtime and asserts the patterns. The source files are located via
 * the FILAMENT_BACKEND_SRC_PATH / FILAMENT_BACKEND_HDR_PATH /
 * ADVANCED_MATERIALS_FEATURE_HDR_PATH compile-time macros, absolute paths
 * defined for this target only in benchmark/CMakeLists.txt (ctest's CWD is the
 * build dir, so a relative path would be fragile). A missing/moved source FAILS
 * the test with a diagnostic — exactly the kind of drift this test catches.
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

#ifndef FILAMENT_BACKEND_SRC_PATH
#error "FILAMENT_BACKEND_SRC_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
#endif
#ifndef FILAMENT_BACKEND_HDR_PATH
#error "FILAMENT_BACKEND_HDR_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
#endif
#ifndef LIGHTING_SYSTEM_SRC_PATH
#error "LIGHTING_SYSTEM_SRC_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
#endif
#ifndef ADVANCED_MATERIALS_FEATURE_HDR_PATH
#error "ADVANCED_MATERIALS_FEATURE_HDR_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
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
        std::printf("[unit_phase8_structural] FAILED: cannot open source '%s'\n",
                    path);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    if (out.empty()) {
        std::printf("[unit_phase8_structural] FAILED: source '%s' is empty\n",
                    path);
        return false;
    }
    return true;
}

// Extract the body of a function given a unique signature anchor (e.g.
// "FilamentBackend::render_frame("). Brace-matches from the first '{' after the
// anchor to its matching '}'. Returns false if the anchor or a balanced body is
// not found. (Same helper shape as unit_cleanup_structural.cpp.)
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

// Count the number of (possibly overlapping is irrelevant here) non-overlapping
// occurrences of `needle` in `haystack`. Used for the single-registration
// cardinality check (mirrors how smoke_feature_registration counts a name).
int count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

bool unit_phase8_structural() {
    bool ok = true;

    std::string backend_src;
    if (!read_source(FILAMENT_BACKEND_SRC_PATH, backend_src)) {
        return false;
    }
    std::string backend_hdr;
    if (!read_source(FILAMENT_BACKEND_HDR_PATH, backend_hdr)) {
        return false;
    }
    std::string lighting_src;
    if (!read_source(LIGHTING_SYSTEM_SRC_PATH, lighting_src)) {
        return false;
    }
    std::string feature_hdr;
    if (!read_source(ADVANCED_MATERIALS_FEATURE_HDR_PATH, feature_hdr)) {
        return false;
    }

    // ── (1) AdvancedMaterialsFeature registered EXACTLY ONCE  ──

    // The coordinating  IRenderFeature is constructed and pushed into
    // render_features_ by exactly one std::make_unique<AdvancedMaterialsFeature>
    // site in init_features. More than one is a double-registration; zero means
    // it was never wired. (Same cardinality contract the feature-registration
    // smoke case asserts at runtime via debug_count_features.)
    const int reg_count =
        count_occurrences(backend_src, "std::make_unique<AdvancedMaterialsFeature>");
    if (reg_count != 1) {
        std::printf("[unit_phase8_structural] FAILED: filament_backend.cpp "
                    "constructs AdvancedMaterialsFeature %d times "
                    "(expected exactly 1 — the single init_features "
                    "registration, R13.5)\n",
                    reg_count);
        ok = false;
    }

    // The coordinating feature's name() identifies it as "advanced-materials"
    // (this is the name the runtime registration smoke case keys on). name() is
    // defined inline in the feature header, so pin the string there so the
    // registered feature and the asserted name cannot drift apart.
    if (!contains(feature_hdr, "\"advanced-materials\"")) {
        std::printf("[unit_phase8_structural] FAILED: AdvancedMaterialsFeature "
                    "does not report name() \"advanced-materials\" "
                    "(R13.5)\n");
        ok = false;
    }

    // ── (2) FilamentBackend gains NO per-effect data member  ──

    // The capabilities are wired through the MaterialInstanceCache
    // setters, configure_post_processing args, and update_camera reading
    // config_ — NOT new FilamentBackend members. None of the per-effect
    // capability identifiers below may appear ANYWHERE in filament_backend.h
    // (catching a per-effect member, observer pointer, or per-effect access
    // path being added). These roots are absent from the header today; the
    // single backend-owned ColorGrading (color_grading_), the TonemapParams
    // render_frame argument, and the wind-arrows "cloth simulation" comment are
    // deliberately NOT in this list so the check stays robust, not brittle.
    const std::vector<std::string> forbidden_backend_members = {
        "advanced_materials",   // no AdvancedMaterials member/observer pointer
        "clearcoat",            // clear coat  is not a backend member
        "clear_coat",
        "anisotropic",          // anisotropic
        "subsurface",           // subsurface/translucency
        "penumbra",             // colored penumbra  — LightingSystem path
        "specular_aa",          // specular AA  — MaterialInstanceCache
        "tone_mapper",          // tone-mapper selection  — post_process
        "physical_camera",      // physical camera  — update_camera
        "dynamic_resolution",   // dynamic resolution  — post_process
        "dynres",
    };
    for (const std::string& ident : forbidden_backend_members) {
        if (contains(backend_hdr, ident)) {
            std::printf("[unit_phase8_structural] FAILED: filament_backend.h "
                        "references per-effect identifier '%s' — Phase-8 "
                        "capabilities must add NO per-effect FilamentBackend "
                        "member (R13.5 / R16.3)\n",
                        ident.c_str());
            ok = false;
        }
    }

    // Positive confirmation that the wiring routes through the existing
    // infrastructure rather than new members: the MaterialInstanceCache
    // subsystem (mat_cache_) and the owning render-feature vector
    // (render_features_) are present in the header. If these disappear the
    // structural contract has been refactored away and this test should be
    // revisited.
    if (!contains(backend_hdr, "mat_cache_")) {
        std::printf("[unit_phase8_structural] FAILED: filament_backend.h no "
                    "longer holds the MaterialInstanceCache subsystem "
                    "(mat_cache_) the Phase-8 material capabilities route "
                    "through (R16.3)\n");
        ok = false;
    }
    if (!contains(backend_hdr, "render_features_")) {
        std::printf("[unit_phase8_structural] FAILED: filament_backend.h no "
                    "longer holds the owning render_features_ vector that the "
                    "AdvancedMaterialsFeature is registered into (R13.5)\n");
        ok = false;
    }

    // Camera exposure has exactly one live owner. LightingSystem is reapplied
    // after update_camera whenever weather/time-of-day changes, so a second
    // setExposure there would overwrite UI camera settings until adaptation
    // settles.
    if (count_occurrences(backend_src, "setExposure(") != 1 ||
        contains(lighting_src, "setExposure(")) {
        std::printf(
            "[unit_phase8_structural] FAILED: Camera::setExposure must occur "
            "exactly once in filament_backend.cpp and never in "
            "lighting_system.cpp\n");
        ok = false;
    }

    // ── (3) NO per-effect work inlined into render_frame  ──

    // Extract the FilamentBackend::render_frame body and assert none of the
    // capability application calls are inlined into it. Those live in
    // step methods (update_camera → setExposure), subsystems
    // (MaterialInstanceCache → select_material_model / set_* setters), and
    // configure_post_processing (setDynamicResolutionOptions). render_frame must
    // stay a thin sequencer of named steps.
    std::string render_frame_body;
    if (!extract_function_body(backend_src, "FilamentBackend::render_frame(",
                               render_frame_body)) {
        std::printf("[unit_phase8_structural] FAILED: could not locate the "
                    "FilamentBackend::render_frame() body in '%s'\n",
                    FILAMENT_BACKEND_SRC_PATH);
        return false;
    }

    const std::vector<std::string> forbidden_in_render_frame = {
        "setExposure",                 // physical camera → update_camera step
        "select_material_model",       // model selection → MaterialInstanceCache
        "setDynamicResolutionOptions", // dynamic resolution → post_process
        "set_advanced_models",         // selector carry-in → feature::init
        "set_specular_aa",             // specular AA → init_pipelines / cache
        "set_transparent_shadows",     // transparent shadows → init / cache
        "set_colored_penumbra",        // colored penumbra → init / cache
    };
    for (const std::string& call : forbidden_in_render_frame) {
        if (contains(render_frame_body, call)) {
            std::printf("[unit_phase8_structural] FAILED: "
                        "FilamentBackend::render_frame() inlines the Phase-8 "
                        "call '%s' — per-effect work must live in a step "
                        "method / subsystem / configure_post_processing, not "
                        "render_frame (R13.5 / R16.3)\n",
                        call.c_str());
            ok = false;
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_phase8_structural{unit_phase8_structural};

} // namespace
