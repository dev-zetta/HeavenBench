/*
 * HeavenBench — Property test: tessellation-cache / BC-decoder OOB safety
 *
 * Feature: codebase-hardening
 *   •: Tessellation-cache and BC-decoder out-of-bounds safety
 *
 *
 * The on-disk tessellation cache (engine/cpu_tessellation.cpp) and the BC block
 * decoders it feeds parse untrusted byte sequences: a stale/corrupt cache file
 * carries on-disk element counts, and a displacement DDS carries compressed
 * block data, both of which are hostile until validated against the actual
 * backing-buffer size.  hardened both:
 *
 *   • load_tessellation_cache captures the cache file size and verifies, via
 *     loadval::fits in 64-bit, that the on-disk vertex/index counts fit the
 *     remaining bytes BEFORE any resize/read. A bad count returns false, which
 *     the caller treats as a cache miss → recompute from source geometry
 *
 *   • decompress_bc4 / decompress_bc1_r take a std::size_t src_size and check
 *     fits(block_off, 8, src_size) before each 8-byte block; an out-of-range
 *     block stops decoding, leaves the zero-initialised output unchanged, and
 *     signals OOB so load_heightmap_chain skips that mip
 *
 *
 * Testability boundary: the BC decoders are file-local `static` functions in
 * cpu_tessellation.cpp and are NOT reachable from a separate test TU, and the
 * heightmap path (load_heightmap_chain → BC decoders) is itself static and only
 * reachable via cpu_tessellate_surface, which needs a real DDS displacement map
 * on disk. The reachable PUBLIC surface that exercises the same
 * loadval::fits-based OOB-safety contract is the disk cache:
 * save_tessellation_cache / load_tessellation_cache. This property therefore
 * pins the cache half directly (round-trip + truncated + empty + random + bad
 * on-disk count → reject/recompute, no crash, no over-read) and the BC-decoder
 * bounds are covered indirectly: they share the identical loadval::fits block
 * check, are unit-exercised through load_heightmap_chain in the renderer, and
 * are pinned for OOB safety by  (prop_dds_fuzz.cpp), which fuzzes the
 * DDS source buffers (including BC1/BC3) the decoders read from.
 *
 * load_tessellation_cache derives its on-disk path from a hash of the cache
 * parameters (mesh/surface/height path + scale + subdivisions + transform), so
 * the synthesiser first calls save_tessellation_cache with a freshly-built
 * valid surface to materialise the cache file at exactly the path the loader
 * will read, locates that file, then overwrites it with the hostile byte
 * variant before loading with the identical parameters. Each iteration uses its
 * own temp cache directory so concurrent test runs never collide.
 *
 * The assertion is the OOB-safety contract: control returns from
 * load_tessellation_cache in a defined true/false state for every input (the
 * no-crash / no-over-read guarantee, enforced under AddressSanitizer). A valid
 * round-tripped cache loads back byte-identically; an empty, truncated, or
 * bad-count cache is rejected (returns false → caller recomputes).
 *
 * cpu_tessellation.cpp is GPU-free and host-testable. This file self-registers
 * with the RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point. RapidCheck's default of 100 successful iterations
 * meets the "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "engine/cpu_tessellation.h"
#include "json.hpp"
#include "engine/types.h"

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

// ── Cache parameters bundle ────────────────────────────────────

// The fields that drive load/save_tessellation_cache's filename hash. Holding
// them together guarantees save and load resolve to the same on-disk path.
struct CacheParams {
    std::string mesh_path;
    std::string surface_name;
    std::string height_path;
    float disp_scale = 0.15f;
    float edge_scale = 0.5f;
    int subdivisions = 2;
};

// Build a randomised-but-deterministic parameter set so different iterations
// hash to different filenames (avoiding accidental reuse across iterations).
CacheParams gen_params() {
    CacheParams p;
    const int tag = *rc::gen::inRange(0, 1'000'000);
    p.mesh_path = std::format("mesh_{}.mesh", tag);
    p.surface_name = std::format("surf_{}", *rc::gen::inRange(0, 64));
    p.height_path = std::format("height_{}_h.dds", tag);
    p.disp_scale = *rc::gen::element(0.15f, 0.5f, 1.0f);
    p.subdivisions = *rc::gen::inRange(1, 4);
    return p;
}

// ── Valid surface synthesiser ──────────────────────────────────

// A surface with `nverts` vertices and `ntris` triangles whose indices are all
// strictly in range. save_tessellation_cache serialises vertices + indices, so
// a round-trip through the cache reproduces these exactly.
heaven::Surface build_valid_surface(int nverts, int ntris) {
    heaven::Surface s;
    s.name = "tess_surface";
    s.vertices.reserve(static_cast<std::size_t>(nverts));
    for (int i = 0; i < nverts; ++i) {
        heaven::Vertex v{};
        v.px = static_cast<float>(i);
        v.py = static_cast<float>(i) + 0.5f;
        v.pz = static_cast<float>(i) - 0.5f;
        v.nx = 0.0f; v.ny = 1.0f; v.nz = 0.0f;
        v.u = 0.25f; v.v = 0.75f;
        v.tx = 1.0f; v.ty = 0.0f; v.tz = 0.0f; v.tw = 1.0f;
        s.vertices.push_back(v);
    }
    s.indices.reserve(static_cast<std::size_t>(ntris) * 3);
    for (int t = 0; t < ntris; ++t) {
        const std::uint32_t base = nverts > 0
            ? static_cast<std::uint32_t>((t * 3) % nverts) : 0u;
        s.indices.push_back(base);
        s.indices.push_back(nverts > 1
            ? static_cast<std::uint32_t>((base + 1) % nverts) : 0u);
        s.indices.push_back(nverts > 2
            ? static_cast<std::uint32_t>((base + 2) % nverts) : 0u);
    }
    return s;
}

// Compare two surfaces' geometry. load_tessellation_cache only restores the
// vertex and index arrays (not the name), so we compare those. Vertex is POD,
// so byte-equal vertex arrays mean an exact round-trip.
bool geometry_equal(const heaven::Surface& a, const heaven::Surface& b) {
    if (a.vertices.size() != b.vertices.size()) return false;
    if (a.indices != b.indices) return false;
    if (!a.vertices.empty() &&
        std::memcmp(a.vertices.data(), b.vertices.data(),
                    a.vertices.size() * sizeof(heaven::Vertex)) != 0) {
        return false;
    }
    return true;
}

// ── Temp-dir scaffolding ───────────────────────────────────────

// Each iteration gets its own cache directory so concurrent test runs (shared
// build dir) never collide on the cache file.
struct TempDir {
    std::filesystem::path path;
    bool ok = false;

    TempDir() {
        static std::atomic<std::uint64_t> counter{0};
        static const std::uint64_t salt = std::random_device{}();
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_tess_fuzz_{}_{}", salt, id);
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        ok = !ec;
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec); // best-effort cleanup
    }
};

// Locate the single cache.bin written under a cache directory. save_…_cache
// writes "<cache_dir>/v<N>/tess_<hash>.bin"; with a fresh temp dir there is
// exactly one such file. Returns an empty path if none is found.
std::filesystem::path find_cache_file(const std::filesystem::path& cache_dir) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(cache_dir, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) &&
            it->path().extension() == ".bin") {
            return it->path();
        }
    }
    return {};
}

// Read a whole file into a byte vector.
std::vector<std::uint8_t> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    return bytes;
}

// Overwrite a file with the given bytes.
void write_all(const std::filesystem::path& p,
               const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
}

// Save a valid surface to the cache for `params` (materialising the on-disk
// file) and return the path of the written cache file.
std::filesystem::path save_and_locate(const heaven::Surface& surface,
                                      const CacheParams& params,
                                      const std::filesystem::path& cache_dir) {
    heaven::save_tessellation_cache(surface, params.mesh_path,
                                    params.surface_name, params.height_path,
                                    params.disp_scale, params.edge_scale,
                                    params.subdivisions,
                                    cache_dir.string(), nullptr,
                                    surface.vertices.size(),
                                    surface.indices.size());
    return find_cache_file(cache_dir);
}

// Load via the public entry with `params`/`cache_dir`. Returns the loader's
// bool result; the output surface is left for the caller to inspect.
bool load_cache(heaven::Surface& out, const CacheParams& params,
                const std::filesystem::path& cache_dir) {
    return heaven::load_tessellation_cache(
        out, params.mesh_path, params.surface_name, params.height_path,
        params.disp_scale, params.edge_scale, params.subdivisions,
        cache_dir.string(), nullptr);
}

// ── Properties ─────────────────────────────────────────────────

// A freshly-saved cache round-trips: load returns true and reproduces the
// vertex/index geometry byte-for-byte (the valid-cache path is unchanged —
// parity for valid input).
bool run_p10_roundtrip() {
    return rc::check(
        "Feature: codebase-hardening, Property 10: tessellation-cache OOB "
        "safety — valid round-trip (Validates: Requirements 6.1, 6.2)",
        [] {
            const int nverts = *rc::gen::inRange(1, 64);
            const int ntris = *rc::gen::inRange(1, 64);
            const CacheParams params = gen_params();
            const heaven::Surface surface = build_valid_surface(nverts, ntris);

            TempDir dir;
            RC_ASSERT(dir.ok);
            const std::filesystem::path cache_file =
                save_and_locate(surface, params, dir.path);
            RC_ASSERT(!cache_file.empty()); // save must have written a file
            auto metadata_file = cache_file;
            metadata_file.replace_extension(".json");
            std::ifstream metadata_stream(metadata_file);
            RC_ASSERT(metadata_stream.good());
            const nlohmann::json metadata =
                nlohmann::json::parse(metadata_stream);
            RC_ASSERT(metadata.at("schema_version") == 1);
            RC_ASSERT(metadata.at("cache_version") ==
                      heaven::kTessellationCacheVersion);
            RC_ASSERT(metadata.at("surface") == params.surface_name);
            RC_ASSERT(metadata.at("topology").at("subdivisions") ==
                      params.subdivisions);
            RC_ASSERT(metadata.at("source").at("vertices") ==
                      surface.vertices.size());
            RC_ASSERT(metadata.at("output").at("vertices") ==
                      surface.vertices.size());
            RC_ASSERT(metadata.at("output").at("triangles") ==
                      surface.indices.size() / 3);

            heaven::Surface loaded;
            const bool ok = load_cache(loaded, params, dir.path);
            RC_ASSERT(ok); // a valid round-tripped cache must load
            RC_ASSERT(geometry_equal(surface, loaded));
        });
}

// An empty (zero-byte) cache file is rejected without reading any element
// (header reads fail) — load returns false → caller recomputes.
bool run_p10_empty() {
    return rc::check(
        "Feature: codebase-hardening, Property 10: tessellation-cache OOB "
        "safety — empty cache file (Validates: Requirements 6.1, 6.2, 6.5)",
        [] {
            const CacheParams params = gen_params();
            const heaven::Surface surface = build_valid_surface(8, 8);

            TempDir dir;
            RC_ASSERT(dir.ok);
            const std::filesystem::path cache_file =
                save_and_locate(surface, params, dir.path);
            RC_ASSERT(!cache_file.empty());

            // Overwrite with zero bytes.
            write_all(cache_file, {});

            heaven::Surface loaded;
            const bool ok = load_cache(loaded, params, dir.path);
            RC_ASSERT(!ok); // empty cache → miss
        });
}

// A valid cache truncated to any strictly-shorter prefix is rejected: a cut
// inside the header fails the header reads, and a cut inside the body leaves
// the on-disk counts claiming more bytes than remain, so loadval::fits rejects
// before any over-read — load returns false in every case.
bool run_p10_truncated() {
    return rc::check(
        "Feature: codebase-hardening, Property 10: tessellation-cache OOB "
        "safety — truncated cache file (Validates: Requirements 6.1, 6.2, 6.5)",
        [] {
            const int nverts = *rc::gen::inRange(1, 64);
            const int ntris = *rc::gen::inRange(1, 64);
            const CacheParams params = gen_params();
            const heaven::Surface surface = build_valid_surface(nverts, ntris);

            TempDir dir;
            RC_ASSERT(dir.ok);
            const std::filesystem::path cache_file =
                save_and_locate(surface, params, dir.path);
            RC_ASSERT(!cache_file.empty());

            const std::vector<std::uint8_t> full = read_all(cache_file);
            RC_ASSERT(!full.empty());

            // Cut to a strictly-shorter prefix: [0, full.size()).
            const std::size_t cut =
                *rc::gen::inRange<std::size_t>(0, full.size());
            const std::vector<std::uint8_t> truncated(full.begin(),
                full.begin() + static_cast<std::ptrdiff_t>(cut));
            write_all(cache_file, truncated);

            heaven::Surface loaded;
            const bool ok = load_cache(loaded, params, dir.path);
            RC_ASSERT(!ok); // any truncation → miss, no over-read
        });
}

// A cache whose header is intact but whose on-disk element counts are
// over-large is rejected by the count/fits check BEFORE any resize/read — the
// "bad on-disk count → cache treated as miss → recompute" contract (6.1, 6.2).
// The valid header bytes (magic + version) are reused from a real saved file so
// only the counts are hostile.
bool run_p10_bad_count() {
    return rc::check(
        "Feature: codebase-hardening, Property 10: tessellation-cache OOB "
        "safety — over-large on-disk count rejected (Validates: Requirements "
        "6.1, 6.2, 6.5)",
        [] {
            const CacheParams params = gen_params();
            const heaven::Surface surface = build_valid_surface(8, 8);

            TempDir dir;
            RC_ASSERT(dir.ok);
            const std::filesystem::path cache_file =
                save_and_locate(surface, params, dir.path);
            RC_ASSERT(!cache_file.empty());

            // Reuse the real 8-byte header (magic + version) so the magic/
            // version gate passes and the count check is what does the
            // rejecting; then append hostile counts and a tiny/empty body.
            const std::vector<std::uint8_t> full = read_all(cache_file);
            RC_ASSERT(full.size() >= 8u);

            std::vector<std::uint8_t> bad(full.begin(), full.begin() + 8);
            // Pick over-large vertex/index counts (count * element size far
            // exceeds the body bytes present).
            const std::uint32_t vert_count =
                *rc::gen::inRange<std::uint32_t>(1'000'000u, 4'000'000'000u);
            const std::uint32_t idx_count =
                *rc::gen::inRange<std::uint32_t>(1'000'000u, 4'000'000'000u);
            const auto* vp = reinterpret_cast<const std::uint8_t*>(&vert_count);
            const auto* ip = reinterpret_cast<const std::uint8_t*>(&idx_count);
            bad.insert(bad.end(), vp, vp + 4);
            bad.insert(bad.end(), ip, ip + 4);
            // Optionally a few stray body bytes — still far short of the claim.
            const int extra = *rc::gen::inRange(0, 16);
            for (int i = 0; i < extra; ++i) bad.push_back(0u);
            write_all(cache_file, bad);

            heaven::Surface loaded;
            const bool ok = load_cache(loaded, params, dir.path);
            RC_ASSERT(!ok); // bad count → rejected before any read → recompute
        });
}

// Arbitrary random bytes at the cache path: the loader must return in a defined
// true/false state without crashing or over-reading. (A random buffer almost
// always fails the magic/version gate; if it happens to look valid, the
// counts/fits guard still bounds every read.)
bool run_p10_random_bytes() {
    return rc::check(
        "Feature: codebase-hardening, Property 10: tessellation-cache OOB "
        "safety — arbitrary bytes (Validates: Requirements 6.1, 6.2, 6.5)",
        [] {
            const CacheParams params = gen_params();
            const heaven::Surface surface = build_valid_surface(8, 8);

            TempDir dir;
            RC_ASSERT(dir.ok);
            const std::filesystem::path cache_file =
                save_and_locate(surface, params, dir.path);
            RC_ASSERT(!cache_file.empty());

            const auto bytes = *rc::gen::container<std::vector<std::uint8_t>>(
                rc::gen::arbitrary<std::uint8_t>());
            write_all(cache_file, bytes);

            // The property is that control returns in a defined state without
            // an out-of-bounds access or process termination (ASan-enforced).
            heaven::Surface loaded;
            const bool ok = load_cache(loaded, params, dir.path);
            // Touch the result so the call cannot be optimised away. On a
            // successful (rare) load the restored arrays are internally
            // consistent by construction of resize().
            if (ok) {
                RC_ASSERT(loaded.vertices.size() <= loaded.vertices.capacity());
            }
        });
}

bool run_tess_cache_content_invalidation() {
    TempDir dir;
    if (!dir.ok) return false;

    CacheParams params;
    params.surface_name = "content_fingerprint";
    params.disp_scale = 0.15f;
    params.edge_scale = 0.5f;
    params.subdivisions = 2;
    params.mesh_path = (dir.path / "source.mesh").string();
    params.height_path = (dir.path / "density.dds").string();
    write_all(params.mesh_path, {1, 2, 3, 4});
    write_all(params.height_path, {5, 6, 7, 8});

    const heaven::Surface surface = build_valid_surface(4, 2);
    save_and_locate(surface, params, dir.path / "cache");

    heaven::Surface loaded;
    if (!load_cache(loaded, params, dir.path / "cache")) return false;

    // Paths and topology parameters are unchanged, but either source-content
    // mutation must resolve to a different cache key.
    write_all(params.mesh_path, {1, 2, 3, 9});
    if (load_cache(loaded, params, dir.path / "cache")) return false;
    write_all(params.mesh_path, {1, 2, 3, 4});
    write_all(params.height_path, {5, 6, 7, 9});
    return !load_cache(loaded, params, dir.path / "cache");
}

bool run_tess_bc_fuzz_properties() {
    bool ok = true;
    ok = run_p10_roundtrip() && ok;
    ok = run_p10_empty() && ok;
    ok = run_p10_truncated() && ok;
    ok = run_p10_bad_count() && ok;
    ok = run_p10_random_bytes() && ok;
    ok = run_tess_cache_content_invalidation() && ok;
    return ok;
}

const heaven::testing::Register g_register_tess_bc_fuzz{
    &run_tess_bc_fuzz_properties};

} // namespace
