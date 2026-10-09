/*
 * HeavenBench — Property test: scene-loader double-load determinism
 *
 * Feature: codebase-hardening
 *   •: Scene-loader double-load determinism
 *
 *
 * The scene loader (engine/scene_rhi.cpp::load_scene_rhi) assigns floating-
 * island group indices and per-group animation parameters (speed, amplitude)
 * while walking the xml0 node/world tree. Historically those were accumulated
 * in file-scope globals (g_next_float_group / g_group_anim), so a SECOND
 * load_scene_rhi call in the same process inherited the first call's counters
 * and produced different group indices than the first. Task
 * 11.1 reset that state at the top of load_scene_rhi; this property pins the
 * resulting contract: loading the SAME scene twice yields byte-identical
 * floating-group indices and per-group animation parameters on the second load
 * as on the first.
 *
 * ── How the scene is exercised headlessly ──────────────────────────────────
 *
 * A REAL headless scene load IS achievable here: load_scene_rhi only needs an
 * IRhiBackend (satisfied by the GPU-free NullBackend the used-asset packer
 * already drives — engine/null_backend.{h,cpp}) plus an AppConfig pointing at a
 * scene file and a Data_Root. Both are synthesised on disk per case:
 *
 *   - A minimal but real xml0 binary `.node` scene (proper magic + tag/attr/
 *     value pools + recursive element tree, per the format in xml0_parser.h).
 *     It contains one or more `WorldExpression` nodes named `move_*` — the
 *     loader's floating-island groups — each carrying an `<expression>` script
 *     in the exact shape parse_island_anim expects (`) * <speed>f` and
 *     `<amp>f - cos(time)`), and each wrapping an `ObjectMesh` child that
 *     references a real on-disk mesh.
 *
 *   - A minimal but real `mi08` static mesh (magic, header bounding volume,
 *     one surface with 3 vertices + 1 triangle) the ObjectMesh nodes point at,
 *     so each instance loads successfully and a FloatingIsland record is built
 *     for it (a FloatingIsland is only emitted when its mesh loads).
 *
 * The loader is then invoked TWICE on the identical config in the same process,
 * and the two RhiScene::floating_islands vectors are compared field-for-field
 * (group index, instance index, speed, amplitude). Equality across the two
 * loads is the determinism property. The group indices are additionally checked
 * to be the sequential 0..N-1 a single clean load produces, and the parsed
 * speed/amplitude to match what the script authored — so the test is meaningful
 * (it genuinely depends on the per-load reset, not a tautology).
 *
 * load_mi08 / load_scene_rhi / xml0 parsing are all GPU-free and host-testable;
 * NullBackend supplies the only backend dependency. This file self-registers
 * with the RapidCheck harness in test_main.cpp via heaven::testing::Register, so
 * the file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the
 * entry point.
 */
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <system_error>
#include <vector>
#include <memory>
#include <span>

#include <rapidcheck.h>

#include "benchmark/cpu_benchmark.h"
#include "engine/asset_file_system.h"
#include "engine/camera_path.h"
#include "engine/material_rhi.h"
#include "engine/null_backend.h"
#include "engine/scene_rhi.h"
#include "engine/types.h"
#include "engine/zip_archive.h"

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

namespace fs = std::filesystem;

// ── Little-endian POD appenders ────────────────────────────────

// The loaders read with std::fread / memcpy into native-layout PODs, so we
// serialise the host's byte representation directly (the renderer targets
// little-endian x86).

void put_i32(std::vector<std::uint8_t>& b, std::int32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}
void put_f32(std::vector<std::uint8_t>& b, float v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}
void put_u16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

// ── Minimal valid mi08 static mesh ─────────────────────────────

// Mirrors the on-disk layout load_mi08 expects (see prop_mi08_indices.cpp):
// magic 'mi08', a 10-float header bounding volume, an int32 surface count, then
// per-surface { int32 name length, name bytes + null, 40-byte surface bounding
// volume } headers, then per-surface { int32 num_verts, vertex (3×f32 pos +
// 3×u16 normal) array, int32 num_tc0, uv (2×f32) array, int32 num_tc1 (0),
// int32 num_tris, triangle (3×u16) index array } blocks. One surface, 3 valid
// (in-range) vertices, 1 triangle — just enough to load successfully.
std::vector<std::uint8_t> build_valid_mi08() {
    constexpr std::uint32_t kMi08Magic =
        'm' | ('i' << 8) | ('0' << 16) | ('8' << 24);

    std::vector<std::uint8_t> b;
    put_u32(b, kMi08Magic);
    for (int i = 0; i < 10; ++i) put_f32(b, static_cast<float>(i) * 0.5f);

    put_i32(b, 1); // one surface

    // Surface header: name + 40-byte surface bounding volume.
    const std::string name = "surf_0";
    put_i32(b, static_cast<std::int32_t>(name.size()) + 1);
    b.insert(b.end(), name.begin(), name.end());
    b.push_back('\0');
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);

    // Surface data: 3 vertices.
    const int verts = 3;
    put_i32(b, verts);
    for (int v = 0; v < verts; ++v) {
        put_f32(b, static_cast<float>(v));
        put_f32(b, static_cast<float>(v) + 1.0f);
        put_f32(b, static_cast<float>(v) + 2.0f);
        put_u16(b, 32768);
        put_u16(b, 32768);
        put_u16(b, 65535);
    }
    // UV set 0.
    put_i32(b, verts);
    for (int v = 0; v < verts; ++v) { put_f32(b, 0.25f); put_f32(b, 0.75f); }
    // UV set 1: none.
    put_i32(b, 0);
    // One triangle with in-range indices.
    put_i32(b, 1);
    put_u16(b, 0); put_u16(b, 1); put_u16(b, 2);
    return b;
}

// ── xml0 binary writer (matches the format parsed by xml0_parser.h) ─────────

// Element encoding (per xml0_parser.h):
//   tag_idx(u8), num_attrs(u8), separator(u8),
//   [attr_name_idx(u8), attr_val_idx(u8)]* num_attrs,
//   num_data_children(u8),
//   if ndc > 0: [LPS(text)]* ndc       (text — no element children)
//   if ndc == 0: num_element_children(u8), [0, child, 0]* nec
// Index widths are 1 byte while each pool holds <= 255 entries (true here).

struct Element {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs; // (name, value)
    std::string text;                  // data child (exclusive with children)
    std::vector<Element> children;
};

struct Xml0Builder {
    std::vector<std::string> tags;
    std::vector<std::string> attr_names;
    std::vector<std::string> values;
    std::vector<std::uint8_t> tree;    // element-tree bytes (built first)

    static std::uint8_t intern(std::vector<std::string>& pool, const std::string& s) {
        for (std::size_t i = 0; i < pool.size(); ++i)
            if (pool[i] == s) return static_cast<std::uint8_t>(i);
        pool.push_back(s);
        return static_cast<std::uint8_t>(pool.size() - 1);
    }

    static void put_lps(std::vector<std::uint8_t>& b, const std::string& s) {
        put_u32(b, static_cast<std::uint32_t>(s.size() + 1));
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }

    // Recursively serialise an element into `tree`, interning its strings.
    void write_element(const Element& e) {
        tree.push_back(intern(tags, e.tag));
        tree.push_back(static_cast<std::uint8_t>(e.attrs.size()));
        tree.push_back(0); // separator
        for (const auto& [an, av] : e.attrs) {
            tree.push_back(intern(attr_names, an));
            tree.push_back(intern(values, av));
        }
        if (!e.text.empty()) {
            tree.push_back(1);          // num data children
            put_lps(tree, e.text);
        } else {
            tree.push_back(0);          // num data children
            tree.push_back(static_cast<std::uint8_t>(e.children.size()));
            for (const Element& c : e.children) {
                tree.push_back(0);      // leading separator
                write_element(c);
                tree.push_back(0);      // trailing separator
            }
        }
    }

    // Assemble the full file: magic + header + pools + tree.
    std::vector<std::uint8_t> finish(const Element& root) {
        write_element(root);            // populates the pools

        std::vector<std::uint8_t> out;
        out.insert(out.end(), {'x', 'm', 'l', '0'});
        put_u32(out, 0);                // header word (unused by the parser)

        // Tag pool: read until an LPS fails (the next u32 count triggers that).
        for (const std::string& t : tags) put_lps(out, t);

        put_u32(out, static_cast<std::uint32_t>(attr_names.size()));
        for (const std::string& a : attr_names) put_lps(out, a);

        put_u32(out, static_cast<std::uint32_t>(values.size()));
        for (const std::string& v : values) put_lps(out, v);

        out.insert(out.end(), tree.begin(), tree.end());
        return out;
    }
};

// Build a `move_*` floating-island node carrying an island-animation script
// (parse_island_anim shape) and a single ObjectMesh child referencing `mesh_rel`.
Element make_island(int index, float speed, float amplitude,
                    const std::string& mesh_rel) {
    Element mesh_node;
    mesh_node.tag = "node";
    mesh_node.attrs = {{"type", "ObjectMesh"},
                       {"name", std::format("island_mesh_{}", index)}};
    {
        Element mesh;
        mesh.tag = "mesh";
        mesh.text = mesh_rel;
        mesh_node.children.push_back(std::move(mesh));
    }

    // Script shaped exactly as parse_island_anim scans for: a ") * <speed>f"
    // for the speed and a "<amp>f - cos(time)" for the amplitude.
    Element expr;
    expr.tag = "expression";
    expr.text = std::format(
        "v = (engine.game.getTime() - 0.000000) * {:.6f}f; "
        "z = {:.6f}f - cos(time) * {:.6f}f;",
        speed, amplitude, amplitude);

    Element move;
    move.tag = "node";
    move.attrs = {{"type", "WorldExpression"},
                  {"name", std::format("move_island_{}", index)}};
    move.children.push_back(std::move(expr));
    move.children.push_back(std::move(mesh_node));
    return move;
}

bool write_bytes(const fs::path& p, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), f);
    const int closed = std::fclose(f);
    return written == data.size() && closed == 0;
}

bool unit_cpu_benchmark_asset_parity() {
    const fs::path root = fs::temp_directory_path() /
        std::format("heaven_cpu_asset_parity_{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code error; fs::remove_all(dir, error); }
    } cleanup{root};
    const fs::path data = root / "Data_Root";
    fs::create_directories(data / "heaven/meshes");
    const std::string mesh_path = "heaven/meshes/test.mesh";
    const auto scene_bytes = [&](int count) {
        Element world;
        world.tag = "world";
        for (int index = 0; index < count; ++index) {
            world.children.push_back(make_island(index, 1.0f, 2.0f, mesh_path));
        }
        return Xml0Builder{}.finish(world);
    };
    const auto scene = scene_bytes(2);
    const auto empty_scene = scene_bytes(0);
    const auto mesh = build_valid_mi08();
    const std::vector<std::uint8_t> malformed{'n', 'o', 't', 'x', 'm', 'l'};
    if (!write_bytes(data / mesh_path, mesh) ||
        !write_bytes(data / "heaven/heaven.world", scene) ||
        !write_bytes(data / "empty.node", empty_scene) ||
        !write_bytes(data / "malformed.node", malformed)) return false;
    fs::create_directory(data / "unreadable.node");

    const fs::path archive_path = root / "assets.zip";
    auto writer = heaven::ZipArchiveWriter::create(archive_path, false);
    const std::string manifest =
        R"({"schema_version":1,"data_root_prefix":"Data_Root"})";
    if (!writer ||
        !writer->add_bytes(std::string(heaven::kAssetArchiveManifest),
                           std::as_bytes(std::span(manifest)), heaven::ZipCompression::Store) ||
        !writer->add_bytes("Data_Root/heaven/heaven.world",
                           std::as_bytes(std::span(scene)), heaven::ZipCompression::Deflate) ||
        !writer->add_bytes("Data_Root/" + mesh_path,
                           std::as_bytes(std::span(mesh)), heaven::ZipCompression::Deflate) ||
        !writer->add_bytes("Data_Root/empty.node",
                           std::as_bytes(std::span(empty_scene)), heaven::ZipCompression::Deflate) ||
        !writer->add_bytes("Data_Root/malformed.node",
                           std::as_bytes(std::span(malformed)), heaven::ZipCompression::Store) ||
        !writer->finish()) return false;
    auto mounted = heaven::AssetFileSystem::open(archive_path.string());
    if (!mounted) return false;

    heaven::AppConfig config;
    config.data_dir = data.string();
    config.tessellation.enabled = false;
    config.cpu_stress.enabled = true;
    config.cpu_stress.workers = 2;
    config.cpu_stress.duration_seconds = 0.01f;
    heaven::RhiMaterialDatabase materials;
    const heaven::CameraPath camera_path;
    const std::string cache = (root / "cache").string();
    const auto check_loaded = [&](heaven::AssetFileSystemRef assets,
                                  std::size_t instances) {
        const auto normal = heaven::run_cpu_benchmark(
            config, materials, cache, camera_path, 0.01, assets);
        const auto stress = heaven::run_cpu_stress_benchmark(
            config, materials, cache, camera_path, assets);
        if (!normal || !stress) {
            std::fprintf(stderr, "[CPU asset parity] normal: %s; stress: %s\n",
                         normal ? "loaded" : normal.error().message.c_str(),
                         stress ? "loaded" : stress.error().message.c_str());
            return false;
        }
        return normal->scene_meshes == 1 && normal->scene_instances == instances &&
               normal->timing.samples > 0 && normal->updates_per_second > 0.0 &&
               std::isfinite(normal->checksum) &&
               stress->enabled && stress->workers == 2 &&
               stress->scene_meshes == 1 && stress->scene_instances == instances &&
               stress->worker_updates_per_second.size() == 2 &&
               stress->checksums.size() == 2 &&
               std::all_of(stress->worker_updates_per_second.begin(),
                           stress->worker_updates_per_second.end(),
                           [](double rate) { return std::isfinite(rate) && rate > 0.0; }) &&
               std::all_of(stress->checksums.begin(), stress->checksums.end(),
                           [](double checksum) { return std::isfinite(checksum); });
    };
    if (!check_loaded(std::nullopt, 2)) return false;
    config.data_dir = archive_path.string();
    if (!check_loaded(std::cref(*mounted), 2) || !check_loaded(std::nullopt, 2)) return false;

    // A supplied filesystem can contain generated memory assets that reopening
    // the ZIP would lose. The explicit mount must remain authoritative.
    const auto overridden_scene = std::make_shared<const heaven::AssetBuffer>(scene_bytes(3));
    if (!mounted->mount_memory("heaven/heaven.world", overridden_scene) ||
        !check_loaded(std::cref(*mounted), 3) || !check_loaded(std::nullopt, 2)) return false;

    // An empty/unreadable scene must not produce a plausible empty-world score.
    const auto rejects = [&](heaven::AssetFileSystemRef assets) {
        return !heaven::run_cpu_benchmark(config, materials, cache, camera_path, 0.01, assets) &&
               !heaven::run_cpu_stress_benchmark(config, materials, cache, camera_path, assets);
    };
    for (const bool zipped : {false, true}) {
        config.data_dir = zipped ? archive_path.string() : data.string();
        const heaven::AssetFileSystemRef assets = zipped
            ? heaven::AssetFileSystemRef(std::cref(*mounted)) : std::nullopt;
        for (const char* name : {"missing.node", "empty.node", "malformed.node", "unreadable.node"}) {
            config.node_file = config.data_dir + "/" + name;
            if (!rejects(assets)) return false;
        }
    }
    config.node_file.clear();
    config.data_dir = (root / "absent.zip").string();
    if (!rejects(std::nullopt)) return false;
    const fs::path broken_archive = root / "broken.zip";
    if (!write_bytes(broken_archive, malformed)) return false;
    config.data_dir = broken_archive.string();
    return rejects(std::nullopt);
}

// A single authored island spec, carried so the test can both build the scene
// and assert the loaded values match what was authored.
struct IslandSpec {
    float speed;
    float amplitude;
};

bool run_p15_double_load_determinism() {
    // Resolve a temp base once; SKIP gracefully (pass) if none is available,
    // mirroring the other temp-dir-gated tests in this suite.
    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[prop_scene_double_load] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    return rc::check(
        "Feature: codebase-hardening, Property 15: scene-loader double-load "
        "determinism (Validates: Requirements 9.1, 9.2, 9.3)",
        [&] {
            // Author 1..4 floating islands with distinct, parseable speeds and
            // amplitudes. Keep values in a sane positive range so the formatted
            // script round-trips through strtof identically on both loads.
            const int num_islands = *rc::gen::inRange(1, 5);
            std::vector<IslandSpec> specs;
            specs.reserve(static_cast<std::size_t>(num_islands));
            for (int i = 0; i < num_islands; ++i) {
                const float speed =
                    *rc::gen::map(rc::gen::inRange(1, 400),
                                  [](int x) { return static_cast<float>(x) / 100.0f; });
                const float amp =
                    *rc::gen::map(rc::gen::inRange(1, 800),
                                  [](int x) { return static_cast<float>(x) / 100.0f; });
                specs.push_back({speed, amp});
            }

            // ── Per-case isolated Data_Root on disk ──
            static std::atomic<std::uint64_t> counter{0};
            const fs::path root =
                temp_base /
                std::format("heaven_scene_double_load_{}", counter.fetch_add(1));
            std::error_code lec;
            fs::remove_all(root, lec);
            fs::create_directories(root / "heaven" / "meshes", lec);
            RC_ASSERT(!lec); // a writable temp tree is required for a valid case

            // RAII cleanup so a failing RC_ASSERT still removes the tree.
            struct Cleanup {
                fs::path dir;
                ~Cleanup() { std::error_code e; fs::remove_all(dir, e); }
            } cleanup{root};

            // The mesh every island references; one shared file is enough (the
            // loader de-dupes by path, one instance per ObjectMesh node).
            const std::string mesh_rel = "heaven/meshes/test.mesh";
            RC_ASSERT(write_bytes(root / "heaven" / "meshes" / "test.mesh",
                                  build_valid_mi08()));

            // Assemble the scene tree: a root wrapper whose children are the
            // move_* island nodes (extract() iterates root.children for `node`).
            Xml0Builder builder;
            Element scene_root;
            scene_root.tag = "world";
            for (int i = 0; i < num_islands; ++i) {
                scene_root.children.push_back(
                    make_island(i, specs[i].speed, specs[i].amplitude, mesh_rel));
            }
            const fs::path node_file = root / "scene.node";
            RC_ASSERT(write_bytes(node_file, builder.finish(scene_root)));

            // ── Drive two independent loads of the identical config ──
            heaven::AppConfig config;
            config.data_dir  = root.string();
            config.node_file = node_file.string();

            heaven::NullBackend backend;

            const heaven::Result<heaven::RhiScene> r1 =
                heaven::load_scene_rhi(config, backend);
            const heaven::Result<heaven::RhiScene> r2 =
                heaven::load_scene_rhi(config, backend);

            RC_ASSERT(r1.has_value());
            RC_ASSERT(r2.has_value());

            const std::vector<heaven::FloatingIsland>& a = r1->floating_islands;
            const std::vector<heaven::FloatingIsland>& b = r2->floating_islands;

            // One FloatingIsland per authored island (its mesh loads).
            RC_ASSERT(a.size() == static_cast<std::size_t>(num_islands));

            // ── Core determinism property: the second load equals the first ──
            RC_ASSERT(a.size() == b.size());
            for (std::size_t i = 0; i < a.size(); ++i) {
                RC_ASSERT(a[i].group          == b[i].group);
                RC_ASSERT(a[i].instance_index == b[i].instance_index);
                RC_ASSERT(a[i].speed          == b[i].speed);
                RC_ASSERT(a[i].amplitude      == b[i].amplitude);
            }

            // ── Meaningfulness: a clean load assigns the sequential group
            //    indices 0..N-1 and the parsed per-group anim parameters match
            //    what the script authored. Without the per-load state reset the
            //    second load's group indices would be offset by the first
            //    load's count, tripping the equality check above.
            for (std::size_t i = 0; i < a.size(); ++i) {
                RC_ASSERT(a[i].group == static_cast<int>(i));
                RC_ASSERT(a[i].speed == specs[i].speed);
                RC_ASSERT(a[i].amplitude == specs[i].amplitude);
            }
        });
}

const heaven::testing::Register g_register_scene_double_load{
    &run_p15_double_load_determinism};
const heaven::testing::Register g_register_cpu_asset_parity{
    &unit_cpu_benchmark_asset_parity};

} // namespace
