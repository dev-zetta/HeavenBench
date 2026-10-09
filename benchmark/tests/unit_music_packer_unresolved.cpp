/*
 * HeavenBench — unit test: packer music-missing-in-source unresolved case
 *
 *
 *
 * The used-asset packer records the background Music_Track (heaven/heaven.oga,
 * == k_music_rel_path) as a used asset whenever music is enabled (the default).
 * When music is enabled but the track is ABSENT from the Source_Tree, discovery
 * must NOT fail outright: it records an Unresolved_Reference for the music path
 * and continues, and the orchestrator then reports the non-success
 * EXIT_UNRESOLVED status.
 *
 * This is a focused unit example built on a synthetic on-disk Source_Tree — the
 * SAME kind of binary xml0 (.world/.node/.mat/.track) + mi08 (.mesh) + stub
 * .dds fixture as integ_packer_parity.cpp, with one deliberate difference: the
 * heaven/heaven.oga music file is NOT written. Every other reference resolves,
 * so the music track is the ONLY unresolved reference, giving a clean
 * assertion. The test:
 *
 *   1. asserts discover_used_assets(config, paths) succeeds (Result has value)
 *      — discovery continues despite the missing music file;
 *   2. asserts discovery->unresolved holds exactly one entry whose
 *      missing_rel_path == k_music_rel_path, referenced by "<music config>";
 *   3. drives run_asset_packer(config, opts) end-to-end against the temp fixture
 *      with a real temp Pack_Target and asserts it returns EXIT_UNRESOLVED, so
 *      the non-success status path  is genuinely exercised.
 *
 * GATING: the fixture is built under std::filesystem::temp_directory_path(). If
 * that temp directory is unavailable the test SKIPS gracefully (prints a notice,
 * returns true), exactly like integ_packer_parity.cpp.
 *
 * Self-registers with the shared heaven_benchmark_tests harness via
 * heaven::testing::Register, picked up by the tests glob (no CMake edit).
 */
#include <atomic>
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
#include "engine/path_resolver.h"
#include "engine/types.h"
#include "dds_loader_cpu.h"
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
// never both — matching the xml0 binary encoding the parser expects.

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

std::vector<uint8_t> serialize_mi08(const std::vector<std::string>& surface_names) {
    Bytes b;
    b.u8('m'); b.u8('i'); b.u8('0'); b.u8('8');

    const float bv[10] = {-1, -1, -1, 1, 1, 1, 0, 0, 0, 2};
    for (float x : bv) b.f32(x);

    b.i32(static_cast<int32_t>(surface_names.size()));

    for (const auto& nm : surface_names) {
        b.i32(static_cast<int32_t>(nm.size() + 1));
        b.str(nm);
        b.u8(0);
        for (int i = 0; i < 40; ++i) b.u8(0);
    }

    for (size_t s = 0; s < surface_names.size(); ++s) {
        b.i32(3);
        const float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        for (auto po : pos) {
            b.f32(po[0]); b.f32(po[1]); b.f32(po[2]);
            b.u16(32767); b.u16(32767); b.u16(65535);
        }
        b.i32(3);
        for (int v = 0; v < 3; ++v) { b.f32(0.0f); b.f32(0.0f); }
        b.i32(0);
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
    return write_bytes(path, {});
}

bool write_flat_bc5_dds(const fs::path& path) {
    heaven::dds_detail::Header header{};
    header.size = sizeof(header);
    header.flags = 0x00081007u;
    header.height = 4;
    header.width = 4;
    header.pitchOrLinearSize = 16;
    header.mipMapCount = 1;
    header.pixelFormat.size = sizeof(heaven::dds_detail::PixelFormat);
    header.pixelFormat.flags = heaven::dds_detail::DDPF_FOURCC;
    header.pixelFormat.fourCC =
        heaven::dds_detail::make_fourcc('A', 'T', 'I', '2');
    header.caps = 0x1000u;
    std::vector<uint8_t> bytes(
        sizeof(uint32_t) + sizeof(header) + 16, 0);
    const uint32_t magic = heaven::dds_detail::DDS_MAGIC;
    std::memcpy(bytes.data(), &magic, sizeof(magic));
    std::memcpy(bytes.data() + sizeof(magic), &header, sizeof(header));
    bytes[sizeof(magic) + sizeof(header)] = 128;
    bytes[sizeof(magic) + sizeof(header) + 1] = 128;
    bytes[sizeof(magic) + sizeof(header) + 8] = 128;
    bytes[sizeof(magic) + sizeof(header) + 9] = 128;
    return write_bytes(path, bytes);
}

// ── The test ────────────────────────────────────────────────────

bool unit_music_packer_unresolved() {
    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[unit_music_packer_unresolved] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path root =
        temp_base / ("heaven_music_unresolved_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(root, ec);

    // Data_Root is <root>/data; the Source_Tree root (recorder root) is <root>.
    const fs::path data = root / "data";

    if (!fs::create_directories(data, ec) && ec) {
        std::printf("[unit_music_packer_unresolved] SKIPPED: cannot create temp tree "
                    "'%s' (%s)\n", root.string().c_str(), ec.message().c_str());
        return true;
    }

    bool ok = true;

    // ── Build a fully-resolvable synthetic Source_Tree (no dangling refs) ──
    // Identical in spirit to integ_packer_parity.cpp so EVERY scene/material/
    // track/texture reference resolves — the ONLY thing missing is the music
    // track, so the music path is the sole Unresolved_Reference.

    const X0 world = elem("nodes", {}, {
        elem("node", {{"type", "NodeReference"}}, {
            text_elem("reference", "heaven/nodes/cube.node"),
        }),
    });

    const X0 cube_node = elem("nodes", {}, {
        elem("node", {{"type", "ObjectMesh"}}, {
            text_elem("mesh", "heaven/meshes/environment/cube.mesh"),
            elem("surface", {{"name", "cube_surf"}, {"material", "cube_mat"}}),
            elem("surface", {{"name", "auto_surf"}, {"material", "auto_mat"}}),
        }),
    });

    const X0 bridges_node = elem("nodes", {}, {
        elem("node", {{"type", "ObjectDummy"}, {"name", "bdummy"}}),
    });

    const X0 props_mat = elem("materials", {}, {
        elem("material", {{"name", "cube_mat"}, {"parent", "base_mat"}}, {
            text_elem("texture", "heaven/textures/props/cube_d.dds",
                      {{"name", "diffuse"}}),
        }),
    });

    const X0 base_mat = elem("materials", {}, {
        elem("material", {{"name", "base_mat"}}, {
            text_elem("texture", "heaven/textures/props/base_n.dds",
                      {{"name", "normal"}}),
        }),
    });

    const X0 clip_track = elem("track", {{"max_time", "1.0"}}, {
        elem("parameter", {{"type", "track"}}, {
            elem("keys_n", {}, {
                text_elem("key", "camera_00.track", {{"time", "0"}}),
            }),
        }),
    });

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
    ok = write_flat_bc5_dds(
        data / "heaven" / "textures" / "ruins" /
        "ruins_ground_n.dds") && ok;

    // DELIBERATELY OMITTED: data/heaven/heaven.oga. With config.music.enabled
    // true (the default) and the track absent, discovery must record it as an
    // Unresolved_Reference and the orchestrator must exit EXIT_UNRESOLVED.

    if (!ok) {
        std::printf("[unit_music_packer_unresolved] FAILED: could not write the fixture tree\n");
        fs::remove_all(root, ec);
        return false;
    }

    // ── Discovery: succeeds, records the music path as unresolved  ──

    heaven::AppConfig config;
    config.data_dir = data.string();
    // Music is enabled by default; make the intent explicit for the reader.
    config.music.enabled = true;

    auto paths = heaven::PathResolver::create(config.data_dir);
    if (!paths) {
        std::printf("[unit_music_packer_unresolved] FAILED: PathResolver::create rejected "
                    "'%s': %s\n", config.data_dir.c_str(), paths.error().message.c_str());
        fs::remove_all(root, ec);
        return false;
    }

    auto discovery = heaven::discover_used_assets(config, *paths);
    if (!discovery) {
        std::printf("[unit_music_packer_unresolved] FAILED: discover_used_assets errored "
                    "instead of continuing past the missing music track: %s\n",
                    discovery.error().message.c_str());
        fs::remove_all(root, ec);
        return false;
    }

    // The music path must NOT appear in the resolved Used_Asset_Set.
    if (discovery->used.contains("data/heaven/heaven.oga")) {
        std::printf("[unit_music_packer_unresolved] FAILED: a missing music track was "
                    "recorded as a resolved used asset\n");
        ok = false;
    }

    // Exactly one unresolved reference, and it is the music track referenced by
    // the synthetic "<music config>" referrer. The rest of the fixture resolves,
    // so the music path is the sole Unresolved_Reference (clean assertion).
    const std::string expected_missing(heaven::k_music_rel_path); // "heaven/heaven.oga"

    int music_unresolved = 0;
    for (const heaven::Unresolved& u : discovery->unresolved) {
        if (u.missing_rel_path == expected_missing) {
            ++music_unresolved;
            if (u.referring_rel_path != "<music config>") {
                std::printf("[unit_music_packer_unresolved] FAILED: music unresolved "
                            "referrer was '%s', expected '<music config>'\n",
                            u.referring_rel_path.c_str());
                ok = false;
            }
        }
    }

    if (music_unresolved != 1) {
        std::printf("[unit_music_packer_unresolved] FAILED: expected exactly one "
                    "unresolved reference for '%s', found %d. Full unresolved list (%zu):\n",
                    expected_missing.c_str(), music_unresolved,
                    discovery->unresolved.size());
        for (const heaven::Unresolved& u : discovery->unresolved)
            std::printf("    %s (referenced by %s)\n",
                        u.missing_rel_path.c_str(), u.referring_rel_path.c_str());
        ok = false;
    }

    // ── Orchestrator: end-to-end run reports the non-success EXIT_UNRESOLVED ──
    // Drive run_asset_packer against the temp fixture so the genuine  exit
    // path is exercised. It writes the resolved set into a fresh Pack_Target and,
    // because discovery recorded an unresolved reference, returns EXIT_UNRESOLVED.
    const fs::path pack_target = root / "pack_out.zip";

    heaven::PackOptions opts;
    opts.target = pack_target.string();
    opts.move = false;
    opts.overwrite = true;

    const int exit_code = heaven::run_asset_packer(config, opts);
    if (exit_code != heaven::EXIT_UNRESOLVED) {
        std::printf("[unit_music_packer_unresolved] FAILED: run_asset_packer returned %d, "
                    "expected EXIT_UNRESOLVED (%d)\n", exit_code, heaven::EXIT_UNRESOLVED);
        ok = false;
    }

    fs::remove_all(root, ec);
    return ok;
}

const heaven::testing::Register reg_unit_music_packer_unresolved{unit_music_packer_unresolved};

} // namespace
