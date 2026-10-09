/*
 * HeavenBench — static/skinned ZIP tessellation-cache round trips
 */
#include "engine/asset_file_system.h"
#include "engine/cpu_tessellation.h"
#include "engine/tessellation_cache_archive.h"
#include "engine/zip_archive.h"
#include "json.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

std::string fnv_hex(std::string_view value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(hash));
    return text;
}

bool unit_tessellation_cache_archive() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven-tess-cache-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path data = root / "Data_Root";
    fs::create_directories(data / "heaven");
    const fs::path mesh_path = data / "heaven/model.mesh";
    const fs::path density_path = data / "heaven/density.dds";
    std::ofstream(mesh_path, std::ios::binary) << "mesh-v1";
    std::ofstream(density_path, std::ios::binary) << "density-v1";

    const fs::path archive_path = root / "cache" /
        ("tessellation-v" + std::to_string(heaven::kTessellationArchiveVersion) +
         ".zip");
    auto assets = heaven::AssetFileSystem::open(data.string());
    auto cache = heaven::TessellationCacheArchive::open(
        archive_path.string(),
        assets ? heaven::AssetFileSystemRef{std::cref(*assets)}
               : std::nullopt);
    if (!assets || !cache) return false;

    heaven::Surface source;
    source.name = "surface";
    source.vertices.resize(3);
    source.indices = {0, 1, 2};
    source.vertices[1].px = 1;
    source.vertices[2].py = 1;
    const heaven::TessellationCacheParameters static_parameters{
        .density_path = density_path.string(),
        .density_factor = 2.0f,
        .edge_scale = 0.5f,
        .subdivisions = 2,
    };
    cache->stage(source, mesh_path.string(), source.name, static_parameters);

    heaven::SkinnedSurface skinned;
    skinned.name = "skinned";
    skinned.vertices.resize(3);
    skinned.indices = {0, 1, 2};
    skinned.vertices[1].px = 2;
    skinned.vertices[2].py = 2;
    const heaven::TessellationCacheParameters skinned_parameters{
        .density_path = density_path.string(),
        .density_factor = 3.0f,
        .edge_scale = 0.25f,
        .subdivisions = 3,
    };
    cache->stage_skinned(skinned, mesh_path.string(), skinned.name,
                         skinned_parameters);
    if (!cache->finish()) return false;

    auto reopened = heaven::TessellationCacheArchive::open(
        archive_path.string(),
        std::cref(*assets));
    heaven::Surface loaded;
    heaven::SkinnedSurface loaded_skinned;
    bool ok = reopened &&
        reopened->load(loaded, mesh_path.string(), source.name,
                       static_parameters) &&
        reopened->load_skinned(
            loaded_skinned, mesh_path.string(), skinned.name,
            skinned_parameters) &&
        loaded.indices == source.indices &&
        loaded.vertices.size() == source.vertices.size() &&
        loaded_skinned.indices == skinned.indices &&
        loaded_skinned.vertices.size() == skinned.vertices.size();

    // Topology/configuration values are part of the stable key.
    heaven::Surface topology_miss;
    auto topology_parameters = static_parameters;
    topology_parameters.subdivisions = 3;
    ok = ok && !reopened->load(
        topology_miss, mesh_path.string(), source.name,
        topology_parameters);
    heaven::Surface density_factor_miss;
    auto density_parameters = static_parameters;
    density_parameters.density_factor = 2.5f;
    ok = ok && !reopened->load(
        density_factor_miss, mesh_path.string(), source.name,
        density_parameters);

    auto boundary_parameters = static_parameters;
    boundary_parameters.boundary_fade = 0.30f;
    ok = ok && !reopened->load(
        topology_miss, mesh_path.string(), source.name,
        boundary_parameters);
    auto policy_parameters = static_parameters;
    policy_parameters.surface_policy_version = 2;
    ok = ok && !reopened->load(
        topology_miss, mesh_path.string(), source.name,
        policy_parameters);

    // A content change must invalidate the stable key.
    std::ofstream(mesh_path, std::ios::binary | std::ios::trunc) << "mesh-v2";
    auto changed_assets = heaven::AssetFileSystem::open(data.string());
    auto changed = heaven::TessellationCacheArchive::open(
        archive_path.string(),
        std::cref(*changed_assets));
    heaven::Surface miss;
    ok = ok && changed && !changed->load(
        miss, mesh_path.string(), source.name, static_parameters);

    // A topology semantic bump must also stop compatibility import from
    // resurrecting an old loose payload, even if its metadata or directory is
    // relabelled. Current loose payloads still migrate without deleting them.
    struct ImportCase {
        uint32_t directory_version;
        uint32_t metadata_version;
        uint32_t payload_version;
        bool expect_import;
    };
    const uint32_t current_version = heaven::kTessellationCacheVersion;
    const std::array<ImportCase, 4> import_cases{{
        {33u, 33u, 33u, false},
        {current_version, 33u, current_version, false},
        {current_version, current_version, 33u, false},
        {current_version, current_version, current_version, true},
    }};
    for (std::size_t index = 0; index < import_cases.size(); ++index) {
        const auto& import_case = import_cases[index];
        const fs::path loose_root = root / ("legacy-" + std::to_string(index));
        const fs::path legacy_dir = loose_root /
            ("v" + std::to_string(import_case.directory_version));
        fs::create_directories(legacy_dir);
        const fs::path legacy_bin = legacy_dir / "entry.bin";
        const fs::path legacy_json = legacy_dir / "entry.json";
        {
            const std::array<uint32_t, 4> header = {
                0x54455353u, import_case.payload_version,
                static_cast<uint32_t>(source.vertices.size()),
                static_cast<uint32_t>(source.indices.size())};
            std::ofstream output(legacy_bin, std::ios::binary);
            output.write(reinterpret_cast<const char*>(header.data()),
                         sizeof(header));
            output.write(
                reinterpret_cast<const char*>(source.vertices.data()),
                static_cast<std::streamsize>(source.vertices.size() *
                                             sizeof(heaven::Vertex)));
            output.write(
                reinterpret_cast<const char*>(source.indices.data()),
                static_cast<std::streamsize>(source.indices.size() *
                                             sizeof(uint32_t)));
        }
        {
            nlohmann::json metadata = {
                {"schema_version", 1},
                {"cache_version", import_case.metadata_version},
                {"mesh_source", mesh_path.string()},
                {"density_source", ""},
                {"surface", source.name},
                {"fingerprints", {
                    {"mesh_fnv1a64", fnv_hex("mesh-v2")},
                    {"density_fnv1a64", fnv_hex("")}}},
                {"topology", {
                    {"density_factor", 4.0f},
                    {"edge_scale", 0.75f},
                    {"subdivisions", 1},
                    {"base_transform", {1.0f, 1.0f, 0.0f, 0.0f}}}}
            };
            std::ofstream(legacy_json) << metadata.dump(2);
        }
        const fs::path imported_zip = root / "cache" /
            ("imported-" + std::to_string(index) + ".zip");
        auto imported = heaven::TessellationCacheArchive::open(
            imported_zip.string(), std::cref(*changed_assets),
            loose_root.string());
        if (!imported) { ok = false; continue; }
        heaven::Surface imported_surface;
        const bool imported_ok = imported->load(
            imported_surface, mesh_path.string(), source.name,
            heaven::TessellationCacheParameters{
                .density_factor = 4.0f,
                .edge_scale = 0.75f,
                .subdivisions = 1});
        ok = ok && imported_ok == import_case.expect_import &&
             fs::exists(legacy_bin) && fs::exists(legacy_json);
        if (import_case.expect_import) {
            ok = ok && imported_surface.indices == source.indices &&
                 imported_surface.vertices.size() == source.vertices.size() &&
                 imported->finish() &&
                 heaven::validate_zip_archive(imported_zip, true);
        } else {
            ok = ok && imported->finish() && !fs::exists(imported_zip);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (!ok) std::printf("[unit_tessellation_cache_archive] FAILED\n");
    return ok;
}

bool unit_tessellation_cache_persistence() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven-tess-persistence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const fs::path mesh = root / "model.mesh";
    std::ofstream(mesh, std::ios::binary) << "stable-source";
    const fs::path archive = root / "cache.zip";
    const heaven::TessellationCacheParameters parameters{.subdivisions = 2};
    auto surface_for = [](int index) {
        heaven::Surface surface;
        surface.name = "surface-" + std::to_string(index);
        surface.vertices.resize(3);
        surface.vertices[1].px = static_cast<float>(index + 1);
        surface.vertices[2].py = 1.0f;
        surface.indices = {0, 1, 2};
        return surface;
    };
    auto bytes = [](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input), {});
    };
    auto cold = heaven::TessellationCacheArchive::open(archive.string());
    if (!cold) return false;
    for (int index = 0; index < 500; ++index) {
        const auto surface = surface_for(index);
        cold->stage(surface, mesh.string(), surface.name, parameters);
    }
    if (!cold->finish()) return false;
    const auto published = bytes(archive);
    bool ok = !published.empty();
    for (int run = 0; run < 2; ++run) {
        auto warm = heaven::TessellationCacheArchive::open(archive.string());
        if (!warm) return false;
        for (int index = 0; index < 500; ++index) {
            const auto expected = surface_for(index);
            heaven::Surface loaded;
            ok = warm->load(loaded, mesh.string(), expected.name, parameters) &&
                loaded.indices == expected.indices &&
                loaded.vertices.size() == expected.vertices.size() &&
                std::memcmp(loaded.vertices.data(), expected.vertices.data(),
                            expected.vertices.size() * sizeof(heaven::Vertex)) == 0 && ok;
        }
        ok = warm->finish().has_value() && bytes(archive) == published && ok;
    }

    // Publishing one new topology variant must retain all earlier entries.
    auto updated = heaven::TessellationCacheArchive::open(archive.string());
    if (!updated) return false;
    auto variant = parameters;
    variant.subdivisions = 3;
    const auto expected = surface_for(0);
    updated->stage(expected, mesh.string(), expected.name, variant);
    if (!updated->finish()) return false;
    auto combined = heaven::TessellationCacheArchive::open(archive.string());
    if (!combined) return false;
    heaven::Surface loaded;
    ok = combined->load(loaded, mesh.string(), expected.name, variant) && ok;
    for (int index = 0; index < 500; ++index) {
        const auto source = surface_for(index);
        ok = combined->load(loaded, mesh.string(), source.name, parameters) && ok;
    }

    // A blocked staging parent is a publication error, not a successful no-op.
    const fs::path blocker = root / "not-a-directory";
    std::ofstream(blocker) << "block";
    auto blocked = heaven::TessellationCacheArchive::open(
        (blocker / "cache.zip").string());
    if (!blocked) return false;
    blocked->stage(expected, mesh.string(), expected.name, parameters);
    const auto failure = blocked->finish();
    ok = !failure && failure.error().message.find("not-a-directory") !=
        std::string::npos && !blocked->finish() && ok;

    // Relative filenames are explicitly supported by RuntimePaths and the ZIP
    // writer. No parent directory needs to be created for this spelling.
    struct RestoreDirectory {
        fs::path original = fs::current_path();
        ~RestoreDirectory() {
            std::error_code error;
            fs::current_path(original, error);
        }
    };
    {
        const RestoreDirectory restore;
        fs::current_path(root);
        auto relative = heaven::TessellationCacheArchive::open("relative-cache.zip");
        if (!relative) return false;
        relative->stage(expected, mesh.string(), expected.name, parameters);
        ok = relative->finish().has_value() && fs::exists("relative-cache.zip") && ok;
    }
    std::error_code error;
    fs::remove_all(root, error);
    if (!ok) std::printf("[unit_tessellation_cache_persistence] FAILED\n");
    return ok;
}

const heaven::testing::Register registered(
    unit_tessellation_cache_archive);
const heaven::testing::Register registered_persistence(
    unit_tessellation_cache_persistence);

} // namespace
