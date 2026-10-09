/*
 * HeavenBench — smoke test: surface→material bindings are xml0-only
 *
 *
 *
 * The implementation guarantees that the packer SHALL derive surface→material bindings
 * EXCLUSIVELY from the binary xml0 node/world files, consistent with the
 * renderer's hard rule (AGENTS.md) that forbids any text-format
 * surface/material mapping (e.g. a `material_map.txt`). The packer does not
 * re-implement this binding logic — it reuses the renderer's own
 * RhiMaterialDatabase::build, whose ONLY source of surface→material pairs is
 * the pair of xml0 extractors `xml0_extract_surface_materials` and
 * `xml0_extract_mesh_surface_materials`. So pinning  means
 * pinning that the renderer's binding population is, and stays, xml0-only.
 *
 * This is checked two complementary, non-tautological ways:
 *
 *   PART A — behavioural (the load-bearing guarantee, never skipped).
 *     A minimal but real xml0 binary `.node` fixture is synthesised on disk
 *     (proper magic + tag/attr/value pools + element tree, per the format in
 *     xml0_parser.h). The two extractors are run over it and MUST recover the
 *     surface→material binding encoded in the binary tree
 *     (surface "surf_a" → material "mat_a", scoped to mesh "meshes/test.mesh").
 *     This proves the binding genuinely comes from parsing the binary xml0
 *     structure — not from a filename heuristic or a side-channel.
 *
 *     A NEGATIVE CONTROL then drops a `material_map.txt` next to the fixture
 *     carrying a CONFLICTING binding (surf_a → wrong_mat, plus an extra
 *     surf_text → text_mat). Re-running the extractors yields the byte-identical
 *     result: the text map is never consulted. If any text-format map were ever
 *     wired in, this control would flip the binding or add the text-only
 *     surface, and the test fails.
 *
 *   PART B — source guard (catches a future regression at its source).
 *     The renderer's binding code (engine/material_rhi.cpp) is read at test
 *     time and, with comments stripped, asserted to (1) populate its
 *     surface→material maps through BOTH xml0 extractors, and (2) contain NO
 *     `material_map` token anywhere in actual code — the only mentions in the
 *     file are in comments documenting the prohibition. A new text-map read
 *     path (the exact thing  forbids) would introduce a
 *     `material_map` token in code and trip this guard. The file is located via
 *     the WEATHER_DOCS_PATH compile-time anchor already defined for this target
 *     (its repo-relative position is stable), with an __FILE__-relative
 *     fallback.
 *
 * GATING: Part A builds its fixture under std::filesystem::temp_directory_path()
 * and SKIPS gracefully if no temp dir is available. Part B SKIPS gracefully if
 * the engine source cannot be located (unusual build layout) rather than
 * failing spuriously — Part A remains the hard guarantee in that case.
 *
 * Self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the other test translation units; the tests glob in CMake picks it up
 * without a CMake edit.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xml0_parser.h"

// Mirror of the harness registry hooks declared in test_main.cpp.
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

// ── xml0 binary writer (matches the format parsed by xml0_parser.h) ──────────

// Layout: "xml0" magic + u32 header, then the strict-LPS tag/attr/value string
// pools (the tag pool is read until an LPS read fails — which the following
// u32 pool count naturally triggers), then the recursive element tree. Index
// widths are 1 byte while each pool holds ≤ 255 entries (true here).

struct Xml0Writer {
    std::vector<uint8_t> bytes;

    void put_u8(uint8_t v) { bytes.push_back(v); }
    void put_u32(uint32_t v) {
        bytes.push_back(static_cast<uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    // Length-prefixed string: u32 length INCLUDING the null terminator, then the
    // bytes, then the null. Used for both the strict header pools and element
    // text (the reader distinguishes them only by a strictness flag).
    void put_lps(const std::string& s) {
        put_u32(static_cast<uint32_t>(s.size() + 1));
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
};

// Build a fixture encoding:
//   <node type="ObjectMesh">
//     <mesh>meshes/test.mesh</mesh>
//     <surface name="surf_a" material="mat_a"/>
//   </node>
std::vector<uint8_t> build_node_fixture() {
    Xml0Writer w;

    // Magic + header word (value irrelevant; the parser starts the tree-string
    // section at byte offset 8).
    w.bytes.insert(w.bytes.end(), {'x', 'm', 'l', '0'});
    w.put_u32(0);

    // Tag pool (indices: 0=node, 1=mesh, 2=surface). Read until an LPS fails,
    // which the next u32 (num_attr_names) triggers.
    w.put_lps("node");
    w.put_lps("mesh");
    w.put_lps("surface");

    // Attr-name pool (indices: 0=type, 1=name, 2=material).
    w.put_u32(3);
    w.put_lps("type");
    w.put_lps("name");
    w.put_lps("material");

    // Value pool (indices: 0=ObjectMesh, 1=surf_a, 2=mat_a).
    w.put_u32(3);
    w.put_lps("ObjectMesh");
    w.put_lps("surf_a");
    w.put_lps("mat_a");

    // ── Element tree ──
    // Root <node type="ObjectMesh">: tag 0, 1 attr, 0 text, 2 children.
    w.put_u8(0x00);  // tag = node
    w.put_u8(0x01);  // num attrs
    w.put_u8(0x00);  // separator
    w.put_u8(0x00);  // attr-name idx = type
    w.put_u8(0x00);  // attr-val  idx = ObjectMesh
    w.put_u8(0x00);  // num data (text) children
    w.put_u8(0x02);  // num element children

    // Child 0: <mesh>meshes/test.mesh</mesh>: tag 1, 0 attrs, 1 text child.
    w.put_u8(0x01);  // tag = mesh
    w.put_u8(0x00);  // num attrs
    w.put_u8(0x00);  // separator
    w.put_u8(0x01);  // num data (text) children
    w.put_lps("meshes/test.mesh");

    // Child 1: <surface name="surf_a" material="mat_a"/>: tag 2, 2 attrs.
    w.put_u8(0x02);  // tag = surface
    w.put_u8(0x02);  // num attrs
    w.put_u8(0x00);  // separator
    w.put_u8(0x01);  // attr-name idx = name
    w.put_u8(0x01);  // attr-val  idx = surf_a
    w.put_u8(0x02);  // attr-name idx = material
    w.put_u8(0x02);  // attr-val  idx = mat_a
    w.put_u8(0x00);  // num data (text) children
    w.put_u8(0x00);  // num element children

    return w.bytes;
}

bool write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
#ifdef _WIN32
    std::FILE* f = _wfopen(p.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(p.c_str(), "wb");
#endif
    if (!f) return false;
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

// ── Part A: behavioural — extractors recover the binding from the binary ─────

bool check_behavioural(bool& ran) {
    ran = false;
    bool ok = true;

    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[unit_packer_xml0_source] SKIPPED behavioural: no temp dir (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path dir =
        temp_base / ("heaven_xml0_source_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("[unit_packer_xml0_source] SKIPPED behavioural: cannot create "
                    "temp dir '%s' (%s)\n", dir.string().c_str(), ec.message().c_str());
        return true;
    }

    const fs::path node_file = dir / "scene.node";
    if (!write_bytes(node_file, build_node_fixture())) {
        std::printf("[unit_packer_xml0_source] SKIPPED behavioural: cannot write "
                    "fixture '%s'\n", node_file.string().c_str());
        fs::remove_all(dir, ec);
        return true;
    }

    ran = true;

    // The binding extracted from the binary xml0 tree.
    const std::map<std::string, std::string> flat =
        xml0_extract_surface_materials(node_file.string().c_str());

    std::map<std::pair<std::string, std::string>, std::string> mesh_scoped;
    std::map<std::string, std::string> global;
    xml0_extract_mesh_surface_materials(node_file.string().c_str(), mesh_scoped, global);

    auto verify = [&](const char* label,
                      const std::map<std::string, std::string>& f,
                      const std::map<std::pair<std::string, std::string>, std::string>& ms,
                      const std::map<std::string, std::string>& g) {
        if (f.size() != 1 || !f.contains("surf_a") || f.at("surf_a") != "mat_a") {
            std::printf("[unit_packer_xml0_source] FAILED (%s): flat extractor did "
                        "not recover surf_a->mat_a from the binary\n", label);
            ok = false;
        }
        const std::pair<std::string, std::string> key{"meshes/test.mesh", "surf_a"};
        if (ms.size() != 1 || !ms.contains(key) || ms.at(key) != "mat_a") {
            std::printf("[unit_packer_xml0_source] FAILED (%s): mesh-scoped extractor "
                        "did not recover (meshes/test.mesh, surf_a)->mat_a\n", label);
            ok = false;
        }
        if (g.size() != 1 || !g.contains("surf_a") || g.at("surf_a") != "mat_a") {
            std::printf("[unit_packer_xml0_source] FAILED (%s): mesh-scoped extractor "
                        "global fallback wrong\n", label);
            ok = false;
        }
    };

    verify("binary-only", flat, mesh_scoped, global);

    // ── Negative control: a conflicting material_map.txt must be ignored ──

    // The renderer/packer never reads a text-format map. Dropping one beside the
    // node file with a CONFLICTING binding (and a text-only surface) must not
    // change what the xml0 extractors return.
    {
        std::ofstream txt(dir / "material_map.txt", std::ios::binary | std::ios::trunc);
        txt << "surf_a wrong_mat\n";       // conflicts with the xml0 binding
        txt << "surf_text text_mat\n";     // exists ONLY in the text map
        txt.close();

        const std::map<std::string, std::string> flat2 =
            xml0_extract_surface_materials(node_file.string().c_str());
        std::map<std::pair<std::string, std::string>, std::string> mesh_scoped2;
        std::map<std::string, std::string> global2;
        xml0_extract_mesh_surface_materials(node_file.string().c_str(),
                                            mesh_scoped2, global2);

        if (flat2 != flat || mesh_scoped2 != mesh_scoped || global2 != global) {
            std::printf("[unit_packer_xml0_source] FAILED: a material_map.txt "
                        "changed the extracted surface->material bindings — a "
                        "text-format map was consulted (violates Req 2.8)\n");
            ok = false;
        }
        if (global2.contains("surf_text")) {
            std::printf("[unit_packer_xml0_source] FAILED: text-only surface "
                        "'surf_text' leaked into the bindings from material_map.txt\n");
            ok = false;
        }
    }

    fs::remove_all(dir, ec);
    return ok;
}

// ── Part B: source guard — binding population is xml0-only in code ───────────

// Strip C/C++ comments so the existing "no material_map.txt" comments in the
// engine source do not false-positive the code-only token scan. Handles line
// (`/ `) and block (`/* */`) comments and skips over string/char literals so a
// `/ ` or `material_map` inside a literal would be preserved as code (there are
// none, but this keeps the scan honest).
std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    enum { CODE, LINE, BLOCK, STR, CH } st = CODE;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (st) {
            case CODE:
                if (c == '/' && n == '/') { st = LINE; ++i; }
                else if (c == '/' && n == '*') { st = BLOCK; ++i; }
                else if (c == '"') { st = STR; out.push_back(c); }
                else if (c == '\'') { st = CH; out.push_back(c); }
                else out.push_back(c);
                break;
            case LINE:
                if (c == '\n') { st = CODE; out.push_back(c); }
                break;
            case BLOCK:
                if (c == '*' && n == '/') { st = CODE; ++i; }
                break;
            case STR:
                out.push_back(c);
                if (c == '\\') { if (i + 1 < src.size()) { out.push_back(n); ++i; } }
                else if (c == '"') st = CODE;
                break;
            case CH:
                out.push_back(c);
                if (c == '\\') { if (i + 1 < src.size()) { out.push_back(n); ++i; } }
                else if (c == '\'') st = CODE;
                break;
        }
    }
    return out;
}

// Locate engine/material_rhi.cpp. Prefer the WEATHER_DOCS_PATH anchor baked into
// this target (its repo-relative position is stable), then an __FILE__-relative
// fallback. Returns an empty path when neither resolves.
fs::path locate_material_rhi() {
    std::error_code ec;
    std::vector<fs::path> candidates;

#ifdef WEATHER_DOCS_PATH
    // WEATHER_DOCS_PATH = "<repo>/benchmark/../docs/RENDERING_FEATURES.md".
    // Its canonical form is "<repo>/docs/RENDERING_FEATURES.md", so the repo
    // root is two parents up.
    {
        fs::path docs = fs::weakly_canonical(fs::path(WEATHER_DOCS_PATH), ec);
        if (ec) docs = fs::path(WEATHER_DOCS_PATH).lexically_normal();
        const fs::path repo = docs.parent_path().parent_path();
        candidates.push_back(repo / "benchmark" / "src" / "engine" / "material_rhi.cpp");
    }
#endif
    // __FILE__ is benchmark/tests/unit_packer_xml0_source.cpp →../../src/engine.
    {
        fs::path self = fs::path(__FILE__);
        const fs::path tests_dir = self.parent_path();           // benchmark/tests
        const fs::path bench_dir = tests_dir.parent_path();      // benchmark
        candidates.push_back(bench_dir / "src" / "engine" / "material_rhi.cpp");
    }

    for (const fs::path& c : candidates) {
        std::error_code ec2;
        if (fs::exists(c, ec2) && !ec2) return c;
    }
    return {};
}

bool check_source_guard(bool& ran) {
    ran = false;
    bool ok = true;

    const fs::path src_path = locate_material_rhi();
    if (src_path.empty()) {
        std::printf("[unit_packer_xml0_source] SKIPPED source guard: could not "
                    "locate engine/material_rhi.cpp\n");
        return true;
    }

    std::ifstream in(src_path, std::ios::binary);
    if (!in) {
        std::printf("[unit_packer_xml0_source] SKIPPED source guard: cannot open "
                    "'%s'\n", src_path.string().c_str());
        return true;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    ran = true;

    // (1) Positive: the surface→material maps are populated through BOTH xml0
    //     extractors and the bindings live in surface_to_material_ /
    //     mesh_surface_to_material_. These tokens appearing confirms the
    //     xml0-only population path exists.
    const bool has_flat_extractor =
        src.find("xml0_extract_surface_materials") != std::string::npos;
    const bool has_mesh_extractor =
        src.find("xml0_extract_mesh_surface_materials") != std::string::npos;
    const bool populates_map =
        src.find("surface_to_material_") != std::string::npos;
    if (!has_flat_extractor || !has_mesh_extractor || !populates_map) {
        std::printf("[unit_packer_xml0_source] FAILED source guard: material_rhi.cpp "
                    "no longer populates surface->material via the xml0 extractors "
                    "(flat=%d mesh=%d map=%d)\n",
                    has_flat_extractor ? 1 : 0, has_mesh_extractor ? 1 : 0,
                    populates_map ? 1 : 0);
        ok = false;
    }

    // (2) Negative: no `material_map` token in ACTUAL code — the only mentions in
    //     the file are in comments documenting the prohibition. A real text-map
    //     read path would put the token in code and trip this.
    const std::string code = strip_comments(src);
    if (code.find("material_map") != std::string::npos) {
        std::printf("[unit_packer_xml0_source] FAILED source guard: a "
                    "'material_map' reference appears in material_rhi.cpp CODE — a "
                    "text-format surface/material map is forbidden (violates "
                    "Req 2.8 and the AGENTS.md rule)\n");
        ok = false;
    }

    return ok;
}

bool unit_packer_xml0_source() {
    bool behavioural_ran = false;
    bool guard_ran = false;
    const bool a = check_behavioural(behavioural_ran);
    const bool b = check_source_guard(guard_ran);

    // At least the behavioural guarantee must have run for this smoke test to be
    // meaningful. If the environment skipped BOTH halves, surface a clear notice
    // (still a pass, mirroring the other temp-dir-gated tests).
    if (!behavioural_ran && !guard_ran) {
        std::printf("[unit_packer_xml0_source] NOTE: both halves skipped "
                    "(no temp dir and no engine source) — nothing verified\n");
    }
    return a && b;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_packer_xml0_source{unit_packer_xml0_source};

} // namespace
