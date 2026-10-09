/*
 * HeavenBench — integration test: used-asset packer renderer parity
 *
 *
 *
 * This is the renderer-parity integration test for the used-asset packer. It
 * builds a tiny but REAL Source_Tree on disk — synthesized binary xml0
 * (.world /.node /.mat /.track) files and an mi08 (.mesh) file, plus stub
 * .dds textures — then runs discover_used_assets(config, paths), which drives
 * the renderer's OWN resolution path (RhiMaterialDatabase::build +
 * load_scene_rhi + per-surface resolve + the.track loaders) through a
 * NullBackend and the AssetUsageRecorder. The discovered Used_Asset_Set is then
 * asserted to equal the exact set the renderer's resolution touches.
 *
 * Because discover_used_assets IS the recorder-captured set produced by running
 * the renderer's resolution on the fixture's Data_Root, asserting it against
 * the hand-derived expected set proves the packer's discovery is identical to
 * what a render run loads  and that it covers, by
 * construction, every edge the requirements call out:
 *
 *   - the world file and a NodeReference-reached node file;
 *   - the explicit bridges.node;
 *   - a mesh file reached from a node;
 *   - a resolved material's.mat PLUS a real cross-file parent.mat in the
 *     inheritance chain;
 *   - an explicitly-bound.dds texture slot AND a convention-based
 *     auto-discovered (_d-by-suffix).dds slot;
 *   - the clip.track + camera-track benchmark track files.
 *
 * The fixture is laid out so the resolution flows EXCLUSIVELY through the binary
 * xml0 extractors (surface→material bindings come from the.node/.world files,
 * never a text map — AGENTS.md /).
 *
 * GATING: the fixture is built under std::filesystem::temp_directory_path(). If
 * that temp directory cannot be created (read-only sandbox, etc.) the test
 * SKIPS gracefully (prints a notice, returns true) — it never dodges the hard
 * fixture construction, only a genuinely unavailable temp dir.
 *
 * Self-registers with the shared heaven_benchmark_tests harness (test_main.cpp)
 * at static-init time via heaven::testing::Register, picked up by the
 * tests glob with no CMake edit.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "asset_packer/asset_packer.h"
#include "engine/material_rhi.h"
#include "engine/null_backend.h"
#include "engine/path_resolver.h"
#include "engine/types.h"
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

// ── Little-endian byte buffer ───────────────────────────────────

struct Bytes {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
};

// ── xml0 tree model + serializer ────────────────────────────────

// An element either carries text (a single data string) OR element children,
// never both — matching the xml0 binary encoding the parser expects (see
// xml0_parser.h: num_data_children > 0 ⇒ text, otherwise num_element_children).

struct X0 {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;
    std::vector<X0> kids;
};

X0 elem(std::string tag,
        std::vector<std::pair<std::string, std::string>> attrs = {},
        std::vector<X0> kids = {}) {
    X0 n;
    n.tag = std::move(tag);
    n.attrs = std::move(attrs);
    n.kids = std::move(kids);
    return n;
}

X0 text_elem(std::string tag, std::string text,
             std::vector<std::pair<std::string, std::string>> attrs = {}) {
    X0 n;
    n.tag = std::move(tag);
    n.attrs = std::move(attrs);
    n.text = std::move(text);
    return n;
}

struct X0Pools {
    std::vector<std::string> tags, attr_names, values;
    std::map<std::string, int> tag_idx, an_idx, val_idx;
};

void collect_pools(const X0& n, X0Pools& p) {
    if (!p.tag_idx.contains(n.tag)) {
        p.tag_idx[n.tag] = static_cast<int>(p.tags.size());
        p.tags.push_back(n.tag);
    }
    for (const auto& [k, v] : n.attrs) {
        if (!p.an_idx.contains(k)) {
            p.an_idx[k] = static_cast<int>(p.attr_names.size());
            p.attr_names.push_back(k);
        }
        if (!p.val_idx.contains(v)) {
            p.val_idx[v] = static_cast<int>(p.values.size());
            p.values.push_back(v);
        }
    }
    for (const auto& c : n.kids) collect_pools(c, p);
}

// Length-prefixed string: uint32 length (incl. null) + bytes + null terminator.
// Shared by the strict header strings and the non-strict element-text strings —
// the on-disk encoding is identical.
void write_lps(Bytes& out, const std::string& s) {
    out.u32(static_cast<uint32_t>(s.size() + 1));
    out.str(s);
    out.u8(0);
}

int idx_width(size_t pool_size) { return pool_size > 255 ? 2 : 1; }

void write_idx(Bytes& out, int idx, int width) {
    if (width == 2) out.u16(static_cast<uint16_t>(idx));
    else out.u8(static_cast<uint8_t>(idx));
}

void write_element(Bytes& out, const X0& n, const X0Pools& p,
                   int wt, int wa, int wv) {
    write_idx(out, p.tag_idx.at(n.tag), wt);
    out.u8(static_cast<uint8_t>(n.attrs.size()));
    out.u8(0); // separator byte (parser reads and ignores)
    for (const auto& [k, v] : n.attrs) {
        write_idx(out, p.an_idx.at(k), wa);
        write_idx(out, p.val_idx.at(v), wv);
    }
    if (!n.text.empty()) {
        // num_data_children = 1: a single element-text data string.
        out.u8(1);
        write_lps(out, n.text);
    } else {
        // num_data_children = 0, then num_element_children, each wrapped in the
        // 0x00 separators the parser skips before/after a child element.
        out.u8(0);
        out.u8(static_cast<uint8_t>(n.kids.size()));
        for (const auto& c : n.kids) {
            out.u8(0);
            write_element(out, c, p, wt, wa, wv);
            out.u8(0);
        }
    }
}

std::vector<uint8_t> serialize_xml0(const X0& root) {
    X0Pools p;
    collect_pools(root, p);

    Bytes out;
    out.str("xml0");
    out.u32(0); // header word (unused by the parser)

    // Tag-name pool: the parser reads strict LPS strings until one fails. The
    // following num_attr_names uint32 reliably fails that strict read (its low
    // byte is a small string-length value < 32, or the count is < 2), which is
    // exactly how the tag list terminates.
    for (const auto& t : p.tags) write_lps(out, t);

    out.u32(static_cast<uint32_t>(p.attr_names.size()));
    for (const auto& a : p.attr_names) write_lps(out, a);

    out.u32(static_cast<uint32_t>(p.values.size()));
    for (const auto& v : p.values) write_lps(out, v);

    const int wt = idx_width(p.tags.size());
    const int wa = idx_width(p.attr_names.size());
    const int wv = idx_width(p.values.size());
    write_element(out, root, p, wt, wa, wv);
    return out.b;
}

// ── mi08 mesh serializer ────────────────────────────────────────

// Minimal valid mi08: magic, bounding volume (10 floats), surface count,
// per-surface header (name + 40-byte BV), then per-surface vertex/uv/index
// data. One triangle (3 verts) per surface is enough for load_mi08 to succeed
// and expose the surface names that drive material resolution.

std::vector<uint8_t> serialize_mi08(const std::vector<std::string>& surface_names) {
    Bytes b;
    b.u8('m'); b.u8('i'); b.u8('0'); b.u8('8');

    // AABB min(3) + max(3) + center(3) + radius(1).
    const float bv[10] = {-1, -1, -1, 1, 1, 1, 0, 0, 0, 2};
    for (float x : bv) b.f32(x);

    b.i32(static_cast<int32_t>(surface_names.size()));

    // Surface headers: name_len (incl. null) + name + null + 40-byte surface BV.
    for (const auto& nm : surface_names) {
        b.i32(static_cast<int32_t>(nm.size() + 1));
        b.str(nm);
        b.u8(0);
        for (int i = 0; i < 40; ++i) b.u8(0);
    }

    // Surface data.
    for (size_t s = 0; s < surface_names.size(); ++s) {
        // 3 vertices: position (3 floats) + compressed normal (3 uint16).
        b.i32(3);
        const float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        for (auto po : pos) {
            b.f32(po[0]); b.f32(po[1]); b.f32(po[2]);
            b.u16(32767); b.u16(32767); b.u16(65535); // ~(0,0,+1)
        }
        // UV set 0 (3) + UV set 1 (0).
        b.i32(3);
        for (int v = 0; v < 3; ++v) { b.f32(0.0f); b.f32(0.0f); }
        b.i32(0);
        // One triangle.
        b.i32(1);
        b.u16(0); b.u16(1); b.u16(2);
    }
    return b.b;
}

// ── Filesystem helpers ──────────────────────────────────────────

bool write_bytes(const fs::path& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

bool write_xml0(const fs::path& path, const X0& root) {
    return write_bytes(path, serialize_xml0(root));
}

bool write_stub_dds(const fs::path& path) {
    // Discovery only stat()s textures and asks NullBackend to "load" them (it
    // returns a sentinel without reading the bytes), so an empty file that
    // exists on disk is sufficient for the texture-slot recording path.
    return write_bytes(path, {});
}

// ── The test ────────────────────────────────────────────────────

bool integ_packer_parity() {
    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[integ_packer_parity] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path root =
        temp_base / ("heaven_packer_parity_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(root, ec);

    // Data_Root is <root>/data, so the Source_Tree root (recorder root) is
    // <root> and every Relative_Asset_Path is prefixed "data/".
    const fs::path data = root / "data";

    if (!fs::create_directories(data, ec) && ec) {
        std::printf("[integ_packer_parity] SKIPPED: cannot create temp tree '%s' (%s)\n",
                    root.string().c_str(), ec.message().c_str());
        return true;
    }

    bool ok = true;

    // ── Build the synthetic Source_Tree ──

    // World: a single NodeReference to cube.node.
    const X0 world = elem("nodes", {}, {
        elem("node", {{"type", "NodeReference"}}, {
            text_elem("reference", "heaven/nodes/cube.node"),
        }),
    });

    // cube.node: one ObjectMesh with two surfaces:
    //   cube_surf → cube_mat   (explicit.mat with a cross-file parent)
    //   auto_surf → auto_mat   (undefined.mat → convention auto-discovery)
    const X0 cube_node = elem("nodes", {}, {
        elem("node", {{"type", "ObjectMesh"}}, {
            text_elem("mesh", "heaven/meshes/environment/cube.mesh"),
            elem("surface", {{"name", "cube_surf"}, {"material", "cube_mat"}}),
            elem("surface", {{"name", "auto_surf"}, {"material", "auto_mat"}}),
        }),
    });

    // bridges.node: explicitly loaded by the renderer in addition to the world.
    const X0 bridges_node = elem("nodes", {}, {
        elem("node", {{"type", "ObjectDummy"}, {"name", "bdummy"}}),
    });

    // props.mat: cube_mat (parent=base_mat) binds an explicit diffuse texture.
    const X0 props_mat = elem("materials", {}, {
        elem("material", {{"name", "cube_mat"}, {"parent", "base_mat"}}, {
            text_elem("texture", "heaven/textures/props/cube_d.dds",
                      {{"name", "diffuse"}}),
        }),
    });

    // base.mat: the parent material in a SEPARATE file, binding a normal map
    // that cube_mat inherits — exercising a real cross-file parent edge.
    const X0 base_mat = elem("materials", {}, {
        elem("material", {{"name", "base_mat"}}, {
            text_elem("texture", "heaven/textures/props/base_n.dds",
                      {{"name", "normal"}, {"wrap", "clamp_x"}}),
        }),
    });

    // clip.track: the master benchmark timeline naming one camera track.
    const X0 clip_track = elem("track", {{"max_time", "1.0"}}, {
        elem("parameter", {{"type", "track"}}, {
            elem("keys_n", {}, {
                text_elem("key", "camera_00.track", {{"time", "0"}}),
            }),
        }),
    });

    // camera_00.track: a minimal but valid camera position track.
    const X0 camera_track = elem("track", {{"max_time", "1.0"}}, {
        elem("parameter", {{"type", "node.position"}}, {
            elem("keys_x", {}, {
                text_elem("key", "0.0", {{"time", "0"}}),
            }),
        }),
    });

    ok = write_xml0(data / "heaven" / "heaven.world", world) && ok;
    ok = write_xml0(data / "heaven" / "nodes" / "cube.node", cube_node) && ok;
    ok = write_xml0(data / "heaven" / "nodes" / "bridges.node", bridges_node) && ok;
    ok = write_xml0(data / "heaven" / "materials" / "props.mat", props_mat) && ok;
    ok = write_xml0(data / "heaven" / "materials" / "base.mat", base_mat) && ok;
    ok = write_xml0(data / "heaven" / "tracks" / "clip.track", clip_track) && ok;
    ok = write_xml0(data / "heaven" / "tracks" / "camera_00.track", camera_track) && ok;

    ok = write_bytes(data / "heaven" / "meshes" / "environment" / "cube.mesh",
                     serialize_mi08({"cube_surf", "auto_surf"})) && ok;
    ok = write_bytes(data / "heaven" / "meshes" / "environment" / "fly_island1.mesh",
                     serialize_mi08({"island1_grass"})) && ok;
    ok = write_bytes(data / "heaven" / "meshes" / "environment" / "fly_island2.mesh",
                     serialize_mi08({"island2_ground"})) && ok;
    ok = write_bytes(data / "heaven" / "meshes" / "environment" / "fly_island3.mesh",
                     serialize_mi08({"island3_ground"})) && ok;

    ok = write_stub_dds(data / "heaven" / "textures" / "props" / "cube_d.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "props" / "base_n.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "environment" / "auto_mat_d.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "environment" / "grass_d.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "environment" / "grass_irregularity_d.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "environment" / "grass_variation_d.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "sfx" / "fire_glow_dr.dds") && ok;
    ok = write_stub_dds(data / "heaven" / "textures" / "sfx" / "spark_d.dds") && ok;

    // The background Music_Track: a render run with music enabled (the default)
    // resolves and loads heaven/heaven.oga, so discovery records it as used. An
    // existing readable file is all the discovery probe requires (it stats the
    // path and opens it for reading — it never decodes here), so an empty stub
    // suffices for the parity assertion.
    ok = write_bytes(data / "heaven" / "heaven.oga", {}) && ok;

    if (!ok) {
        std::printf("[integ_packer_parity] FAILED: could not write the fixture tree\n");
        fs::remove_all(root, ec);
        return false;
    }

    // Sanity: confirm every synthesized xml0 file round-trips through the real
    // parser (a fixture-encoding bug should fail here with a clear message,
    // rather than as an opaque discovery mismatch below).
    for (const char* rel : {"heaven/heaven.world", "heaven/nodes/cube.node",
                            "heaven/nodes/bridges.node", "heaven/materials/props.mat",
                            "heaven/materials/base.mat", "heaven/tracks/clip.track",
                            "heaven/tracks/camera_00.track"}) {
        const std::string full = (data / rel).string();
        if (xml0_parse_tree(full.c_str()).tag.empty()) {
            std::printf("[integ_packer_parity] FAILED: synthesized xml0 '%s' did "
                        "not parse back to a non-empty root\n", rel);
            fs::remove_all(root, ec);
            return false;
        }
    }

    // ── Run the renderer's resolution through the packer's discovery ──

    heaven::AppConfig config;
    config.data_dir = data.string();

    auto paths = heaven::PathResolver::create(config.data_dir);
    if (!paths) {
        std::printf("[integ_packer_parity] FAILED: PathResolver::create rejected "
                    "'%s': %s\n", config.data_dir.c_str(),
                    paths.error().message.c_str());
        fs::remove_all(root, ec);
        return false;
    }

    auto discovery = heaven::discover_used_assets(config, *paths);
    if (!discovery) {
        std::printf("[integ_packer_parity] FAILED: discover_used_assets errored: %s\n",
                    discovery.error().message.c_str());
        fs::remove_all(root, ec);
        return false;
    }

    // ── Expected Used_Asset_Set: exactly what the renderer's resolution loads ──
    const std::set<std::string> expected = {
        "data/heaven/heaven.world",                            // world
        "data/heaven/nodes/bridges.node",                      // explicit bridges.node
        "data/heaven/nodes/cube.node",                         // NodeReference target
        "data/heaven/meshes/environment/cube.mesh",            // mesh from a node
        "data/heaven/meshes/environment/fly_island1.mesh",     // procedural grass roots
        "data/heaven/meshes/environment/fly_island2.mesh",     // procedural grass roots
        "data/heaven/meshes/environment/fly_island3.mesh",     // procedural grass roots
        "data/heaven/materials/props.mat",                     // resolved material
        "data/heaven/materials/base.mat",                      // cross-file parent.mat
        "data/heaven/textures/props/cube_d.dds",               // explicit texture slot
        "data/heaven/textures/props/base_n.dds",               // inherited texture slot
        "data/heaven/textures/environment/auto_mat_d.dds",     // auto-discovered slot
        "data/heaven/textures/environment/grass_d.dds",        // procedural grass atlas
        "data/heaven/textures/environment/grass_irregularity_d.dds", // grass variation
        "data/heaven/textures/environment/grass_variation_d.dds", // flower clutter atlas
        "data/heaven/textures/sfx/fire_glow_dr.dds",           // particles/flares/rain
        "data/heaven/textures/sfx/spark_d.dds",                // rain drops
        "data/heaven/tracks/clip.track",                       // benchmark clip track
        "data/heaven/tracks/camera_00.track",                  // camera track
        "data/heaven/heaven.oga",                              // background music track
    };

    if (discovery->used != expected) {
        std::printf("[integ_packer_parity] FAILED: discovered Used_Asset_Set does "
                    "not match the renderer's resolution.\n  Expected (%zu):\n",
                    expected.size());
        for (const std::string& e : expected) std::printf("    %s\n", e.c_str());
        std::printf("  Got (%zu):\n", discovery->used.size());
        for (const std::string& g : discovery->used) std::printf("    %s\n", g.c_str());
        ok = false;
    }

    // No reference in the fixture is dangling, so the parity run must record
    // zero unresolved references.
    if (!discovery->unresolved.empty()) {
        std::printf("[integ_packer_parity] FAILED: expected no unresolved "
                    "references, got %zu:\n", discovery->unresolved.size());
        for (const heaven::Unresolved& u : discovery->unresolved)
            std::printf("    %s (referenced by %s)\n",
                        u.missing_rel_path.c_str(), u.referring_rel_path.c_str());
        ok = false;
    }

    const auto inherited = discovery->texture_inventory.find(
        "data/heaven/textures/props/base_n.dds");
    if (inherited == discovery->texture_inventory.end() ||
        inherited->second.role != "normal" ||
        inherited->second.wrap != "clamp_x") {
        std::printf("[integ_packer_parity] FAILED: inherited normal/wrap "
                    "metadata was not retained in texture inventory\n");
        ok = false;
    }
    const auto feature = discovery->texture_inventory.find(
        "data/heaven/textures/sfx/fire_glow_dr.dds");
    if (feature == discovery->texture_inventory.end() ||
        feature->second.role != "emissive" ||
        feature->second.wrap != "clamp") {
        std::printf("[integ_packer_parity] FAILED: feature-owned texture "
                    "metadata was not declared explicitly\n");
        ok = false;
    }

    fs::remove_all(root, ec);
    return ok;
}

const heaven::testing::Register reg_integ_packer_parity{integ_packer_parity};

// The emission texture remains part of the renderer/packer asset set even when
// its authored state disables synthetic lighting. Exercise both material file
// formats through the real database, including cross-file inheritance.
bool material_emission_state() {
    const fs::path root = fs::temp_directory_path() /
        ("heaven emission state_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    const fs::path data = root / "heaven";
    const std::string custom = "heaven/textures/props/custom_e.dds";
    const std::string fallback = "core/textures/mesh_base_emission.dds";
    const std::string xml = R"(<materials>
<material name="xml_base"><texture name="diffuse">heaven/textures/props/base_d.dds</texture></material>
<material name="xml_on" parent="xml_base"><state name="emission">1</state><texture name="emission">heaven/textures/props/custom_e.dds</texture></material>
<material name="xml_off" parent="xml_base"><state name="emission">0</state><texture name="emission">heaven/textures/props/custom_e.dds</texture></material>
<material name="xml_unknown" parent="xml_base"><texture name="emission">heaven/textures/props/custom_e.dds</texture></material>
<material name="xml_default" parent="xml_base"><texture name="emission">core/textures/mesh_base_emission.dds</texture></material>
<material name="xml_uniform" parent="xml_base"><state name="emission">1</state><texture name="emission">core/textures/mesh_base_emission.dds</texture></material>
<material name="xml_no_texture" parent="xml_base"><state name="emission">1</state></material>
</materials>)";
    const X0 binary = elem("materials", {}, {
        elem("material", {{"name", "child_off"}, {"parent", "xml_on"}}, {
            text_elem("state", "0", {{"name", "emission"}})}),
        elem("material", {{"name", "child_inherited_off"}, {"parent", "xml_off"}}),
        elem("material", {{"name", "binary_on"}, {"parent", "xml_base"}}, {
            text_elem("state", "1", {{"name", "emission"}}),
            text_elem("texture", custom, {{"name", "emission"}})}),
        elem("material", {{"name", "child_inherited_on"}, {"parent", "binary_on"}}),
        elem("material", {{"name", "binary_uniform"}, {"parent", "xml_base"}}, {
            text_elem("state", "1", {{"name", "emission"}}),
            text_elem("texture", fallback, {{"name", "emission"}})}),
        elem("material", {{"name", "binary_unknown"}, {"parent", "xml_base"}}, {
            text_elem("texture", custom, {{"name", "emission"}})}),
        elem("material", {{"name", "binary_default"}, {"parent", "xml_base"}}, {
            text_elem("texture", fallback, {{"name", "emission"}})}),
    });
    struct Expected {
        std::string name;
        int state;
        bool light;
        std::string texture;
    };
    const std::vector<Expected> cases{
        {"xml_on", 1, true, custom},
        {"xml_off", 0, false, custom},
        {"xml_unknown", -1, true, custom},
        {"xml_default", -1, false, fallback},
        {"xml_uniform", 1, true, fallback},
        {"xml_no_texture", 1, false, ""},
        {"child_off", 0, false, custom},
        {"child_inherited_off", 0, false, custom},
        {"binary_on", 1, true, custom},
        {"child_inherited_on", 1, true, custom},
        {"binary_uniform", 1, true, fallback},
        {"binary_unknown", -1, true, custom},
        {"binary_default", -1, false, fallback},
    };
    X0 object = elem("node", {{"type", "ObjectMesh"}}, {
        text_elem("mesh", "heaven/meshes/emission.mesh")});
    for (const auto& test : cases) {
        object.kids.push_back(elem("surface", {
            {"name", test.name}, {"material", test.name}}));
    }
    if (!write_bytes(root / "core/core/materials/emission.mat", {xml.begin(), xml.end()}) ||
        !write_xml0(data / "heaven/materials/emission.mat", binary) ||
        !write_xml0(data / "heaven/heaven.world", elem("nodes", {}, {object})) ||
        !write_stub_dds(data / "heaven/textures/props/base_d.dds") ||
        !write_stub_dds(data / custom) || !write_stub_dds(root / "core" / fallback)) {
        std::printf("[material_emission_state] FAIL: fixture write failed\n");
        return false;
    }
    std::error_code canonical_error;
    const fs::path canonical_root = fs::weakly_canonical(root, canonical_error);
    if (canonical_error) {
        std::printf("[material_emission_state] FAIL: canonical source root: %s\n",
                    canonical_error.message().c_str());
        return false;
    }
    auto paths = heaven::PathResolver::create(data.string());
    if (!paths) {
        std::printf("[material_emission_state] FAIL: resolver: %s\n",
                    paths.error().message.c_str());
        return false;
    }
    heaven::RhiMaterialDatabase database;
    database.build(*paths);
    heaven::NullBackend backend;
    bool ok = true;
    for (const auto& test : cases) {
        // Production discovery passes a canonical source root. Windows temp
        // paths can use short names that differ from weakly_canonical hits.
        heaven::AssetUsageRecorder recorder(canonical_root.generic_string());
        const auto* definition = database.get_material_for_surface(test.name);
        const auto material = database.resolve(test.name, *paths, backend, &recorder);
        if (!definition || definition->emission_enabled != test.state ||
            material.emission_light_source != test.light ||
            (material.emission != 0) != !test.texture.empty()) {
            std::printf("[material_emission_state] FAIL: %s state/light/binding mismatch\n",
                        test.name.c_str());
            ok = false;
        }
        if (!test.texture.empty()) {
            const std::string key = test.texture.starts_with("core/")
                ? "core/" + test.texture : "heaven/" + test.texture;
            const auto texture = recorder.texture_inventory().find(key);
            if (!recorder.used().contains(key) || texture == recorder.texture_inventory().end() ||
                texture->second.role != "emissive") {
                std::printf("[material_emission_state] FAIL: %s dropped emission asset metadata\n",
                            test.name.c_str());
                std::printf("  expected key: %s; source root: %s\n", key.c_str(),
                            canonical_root.generic_string().c_str());
                for (const auto& [actual_key, hint] : recorder.texture_inventory()) {
                    std::printf("  actual key: %s; role: %s; wrap: %s\n",
                                actual_key.c_str(), hint.role.c_str(), hint.wrap.c_str());
                }
                ok = false;
                continue;
            }

            // Resolve the same file through native, generic and dot-segment
            // spellings without changing its relative key or semantic role.
            const auto original_used = recorder.used();
            const auto original_inventory_size = recorder.texture_inventory().size();
            const std::string original_wrap = texture->second.wrap;
            const fs::path asset = root / fs::path(key);
            for (const auto& spelling : {
                    asset.string(), asset.generic_string(),
                    (root / "heaven" / ".." / fs::path(key)).string()}) {
                recorder.record_texture(spelling, "emissive", original_wrap);
            }
            const auto retained = recorder.texture_inventory().find(key);
            if (recorder.used() != original_used ||
                recorder.texture_inventory().size() != original_inventory_size ||
                retained == recorder.texture_inventory().end() ||
                retained->second.role != "emissive" ||
                retained->second.wrap != original_wrap) {
                std::printf("[material_emission_state] FAIL: %s native/generic/dot "
                            "texture paths did not deduplicate\n", test.name.c_str());
                ok = false;
            }
        }
    }
    if (ok) {
        std::printf("[material_emission_state] %zu inherited emission cases and "
                    "native/generic/dot texture keys verified\n", cases.size());
    }
    return ok;
}

const heaven::testing::Register reg_material_emission_state{material_emission_state};

} // namespace
