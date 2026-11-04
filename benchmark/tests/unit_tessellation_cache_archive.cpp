/*
 * HeavenBench — static/skinned ZIP tessellation-cache round trips
 */
#include "engine/asset_file_system.h"
#include "engine/cpu_tessellation.h"
#include "engine/tessellation_cache_archive.h"
#include "engine/zip_archive.h"
#include "json.hpp"

#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
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

template <typename T>
void append_pod(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// Real mi08 source headers let compatibility import prove whether a legacy
// surface name denotes exactly one original file ordinal.
std::string source_mesh_bytes(std::span<const std::string> names,
                              float offset = 0) {
    std::string bytes = "mi08";
    const std::array<float, 10> bounds{};
    append_pod(bytes, bounds);
    append_pod(bytes, static_cast<int32_t>(names.size()));
    for (const auto& name : names) {
        append_pod(bytes, static_cast<int32_t>(name.size() + 1));
        bytes.append(name);
        bytes.push_back('\0');
        append_pod(bytes, bounds);
    }
    for (std::size_t index = 0; index < names.size(); ++index) {
        append_pod(bytes, int32_t{3});
        const float x = offset + 100.0f * static_cast<float>(index);
        for (const auto& position : std::array<std::array<float, 3>, 3>{
                 {{x, 0, 0}, {x + 1, 0, 0}, {x, 1, 0}}}) {
            append_pod(bytes, position);
            append_pod(bytes, std::array<uint16_t, 3>{32767, 32767, 65535});
        }
        append_pod(bytes, int32_t{3});
        append_pod(bytes, std::array<float, 6>{0, 0, 1, 0, 0, 1});
        append_pod(bytes, int32_t{0});
        append_pod(bytes, int32_t{1});
        append_pod(bytes, std::array<uint16_t, 3>{0, 1, 2});
    }
    return bytes;
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
    const std::array<std::string, 1> source_names{"surface"};
    std::ofstream(mesh_path, std::ios::binary) << source_mesh_bytes(source_names);
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
    const auto mesh_v2 = source_mesh_bytes(source_names, 2.0f);
    std::ofstream(mesh_path, std::ios::binary | std::ios::trunc) << mesh_v2;
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
                    {"mesh_fnv1a64", fnv_hex(mesh_v2)},
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

struct CacheTestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("heaven-cache-regression-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    CacheTestDirectory() { std::filesystem::create_directories(path); }
    ~CacheTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

bool cache_check(bool condition, std::string_view message) {
    if (!condition) std::printf("[tessellation_cache] %.*s\n",
        static_cast<int>(message.size()), message.data());
    return condition;
}

template <typename SurfaceT>
SurfaceT cache_triangle(float x) {
    SurfaceT surface;
    surface.name = "duplicate";
    surface.vertices.resize(3);
    surface.indices = {0, 1, 2};
    surface.vertices[0].px = x;
    surface.vertices[1].px = x + 1;
    surface.vertices[2].px = x;
    surface.vertices[2].py = 1;
    if constexpr (std::is_same_v<SurfaceT, heaven::SkinnedSurface>) {
        for (auto& vertex : surface.vertices) {
            vertex.bone_indices[0] = x == 0 ? 0 : 1;
            vertex.bone_weights[0] = 1;
        }
    }
    return surface;
}

template <typename SurfaceT>
bool same_surface(const SurfaceT& actual, const SurfaceT& expected) {
    return actual.name == expected.name && actual.indices == expected.indices &&
        actual.vertices.size() == expected.vertices.size() &&
        std::memcmp(actual.vertices.data(), expected.vertices.data(),
                    expected.vertices.size() * sizeof(expected.vertices.front())) == 0;
}

template <typename SurfaceT>
bool cache_load(heaven::TessellationCacheArchive& cache, SurfaceT& surface,
                const std::filesystem::path& mesh,
                const heaven::TessellationCacheParameters& parameters) {
    if constexpr (std::is_same_v<SurfaceT, heaven::Surface>) {
        return cache.load(surface, mesh.string(), surface.name, parameters);
    } else {
        return cache.load_skinned(surface, mesh.string(), surface.name, parameters);
    }
}

template <typename SurfaceT>
void cache_stage(heaven::TessellationCacheArchive& cache, const SurfaceT& surface,
                 const std::filesystem::path& mesh,
                 const heaven::TessellationCacheParameters& parameters) {
    if constexpr (std::is_same_v<SurfaceT, heaven::Surface>) {
        cache.stage(surface, mesh.string(), surface.name, parameters);
    } else {
        cache.stage_skinned(surface, mesh.string(), surface.name, parameters);
    }
}

template <typename SurfaceT>
bool cache_duplicate_identity(const std::filesystem::path& root,
                               const std::filesystem::path& mesh) {
    const auto archive = root / "cache.zip";
    const auto first = cache_triangle<SurfaceT>(0);
    const auto second = cache_triangle<SurfaceT>(100);
    const heaven::TessellationCacheParameters first_parameters{
        .subdivisions = 2, .source_surface_index = 0};
    const heaven::TessellationCacheParameters second_parameters{
        .subdivisions = 2, .source_surface_index = 1};
    auto cold = heaven::TessellationCacheArchive::open(archive.string());
    if (!cold) return false;
    auto destination = second;
    bool ok = cache_check(!cache_load(*cold, destination, mesh, second_parameters) &&
        same_surface(destination, second), "cold miss must preserve its destination");
    cache_stage(*cold, first, mesh, first_parameters);
    ok = cache_check(!cache_load(*cold, destination, mesh, second_parameters) &&
        same_surface(destination, second),
        "a staged same-name sibling must not become a hit") && ok;
    cache_stage(*cold, second, mesh, second_parameters);
    destination = first;
    ok = cache_check(cache_load(*cold, destination, mesh, first_parameters) &&
        same_surface(destination, first), "first staged surface must retain its bytes") && ok;
    destination = second;
    ok = cache_check(cache_load(*cold, destination, mesh, second_parameters) &&
        same_surface(destination, second), "second staged surface must retain its bytes") && ok;
    if (!cold->finish()) return false;
    auto warm = heaven::TessellationCacheArchive::open(archive.string());
    if (!warm) return false;
    destination = second;
    ok = cache_check(cache_load(*warm, destination, mesh, first_parameters) &&
        same_surface(destination, first), "first warm surface must retain its bytes") && ok;
    destination = first;
    ok = cache_check(cache_load(*warm, destination, mesh, second_parameters) &&
        same_surface(destination, second), "second warm surface must retain its bytes") && ok;
    auto absent_parameters = first_parameters;
    absent_parameters.source_surface_index = 2;
    destination = first;
    ok = cache_check(!cache_load(*warm, destination, mesh, absent_parameters) &&
        same_surface(destination, first), "unknown source ordinal must miss without mutation") && ok;
    return ok;
}

bool unit_tessellation_cache_duplicate_identity() {
    const CacheTestDirectory root;
    const auto mesh = root.path / "model.mesh";
    const std::array<std::string, 2> names{"duplicate", "duplicate"};
    std::ofstream(mesh, std::ios::binary) << source_mesh_bytes(names);
    const bool static_ok = cache_duplicate_identity<heaven::Surface>(root.path / "static", mesh);
    const bool skinned_ok = cache_duplicate_identity<heaven::SkinnedSurface>(root.path / "skinned", mesh);
    return static_ok && skinned_ok;
}

struct TestZipClose {
    void operator()(zip_t* archive) const noexcept { if (archive) zip_close(archive); }
};
struct TestZipFileClose {
    void operator()(zip_file_t* file) const noexcept { if (file) zip_fclose(file); }
};

std::vector<std::byte> read_test_zip(zip_t* archive, const std::string& name) {
    zip_stat_t stat{};
    if (zip_stat(archive, name.c_str(), 0, &stat) != 0 || stat.size > zip_uint64_t{1024} * 1024) return {};
    std::unique_ptr<zip_file_t, TestZipFileClose> file(zip_fopen(archive, name.c_str(), 0));
    if (!file) return {};
    std::vector<std::byte> bytes(static_cast<std::size_t>(stat.size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = zip_fread(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) return {};
        offset += static_cast<std::size_t>(count);
    }
    return bytes;
}

// Patch only the selected entry's CRC metadata after a valid archive has been
// written. The payload remains independently decodable, with unchanged sizes
// and valid geometry, so a structural decoder cannot detect this corruption.
bool replace_test_crc(const std::filesystem::path& path,
                       const std::string& entry_name, uint32_t expected_crc) {
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(input), {});
    if (bytes.size() < 22) return false;
    const auto fits = [&](std::size_t offset, std::size_t count) {
        return offset <= bytes.size() && count <= bytes.size() - offset;
    };
    const auto u16 = [&](std::size_t offset) {
        return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8);
    };
    const auto u32 = [&](std::size_t offset) {
        return u16(offset) | (u16(offset + 2) << 16);
    };
    const auto put32 = [&](std::size_t offset, uint32_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte)
            bytes[offset + byte] = static_cast<unsigned char>(value >> (8 * byte));
    };
    const std::size_t end = bytes.size() - 22;
    if (u32(end) != 0x06054b50u || u16(end + 20) != 0) return false;
    std::size_t central = u32(end + 16);
    bool patched = false;
    while (fits(central, 46) && u32(central) == 0x02014b50u) {
        const auto name_size = u16(central + 28);
        const auto extra_size = u16(central + 30);
        const auto comment_size = u16(central + 32);
        if (!fits(central + 46, name_size + extra_size + comment_size)) return false;
        const std::string_view name(reinterpret_cast<const char*>(bytes.data() + central + 46), name_size);
        if (name == entry_name) {
            const std::size_t local = u32(central + 42);
            if (!fits(local, 30) || u32(local) != 0x04034b50u ||
                u32(central + 16) == expected_crc) return false;
            put32(central + 16, expected_crc);
            put32(local + 14, expected_crc);
            if ((u16(local + 6) & 8u) != 0) {
                std::size_t descriptor = local + 30 + u16(local + 26) +
                    u16(local + 28) + u32(central + 20);
                if (!fits(descriptor, 4)) return false;
                if (u32(descriptor) == 0x08074b50u) descriptor += 4;
                if (!fits(descriptor, 4)) return false;
                put32(descriptor, expected_crc);
            }
            patched = true;
        }
        central += 46 + name_size + extra_size + comment_size;
    }
    if (!patched) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

template <typename SurfaceT>
bool cache_crc_rejection(const std::filesystem::path& root,
                          const std::filesystem::path& mesh) {
    const auto good_path = root / "good.zip";
    const auto expected = cache_triangle<SurfaceT>(0);
    const heaven::TessellationCacheParameters parameters{.subdivisions = 2};
    {
        auto cache = heaven::TessellationCacheArchive::open(good_path.string());
        if (!cache) return false;
        cache_stage(*cache, expected, mesh, parameters);
        if (!cache->finish()) return false;
    }
    int error = 0;
    std::unique_ptr<zip_t, TestZipClose> archive(zip_open(good_path.string().c_str(), ZIP_RDONLY, &error));
    if (!archive) return false;
    const auto manifest = read_test_zip(archive.get(), "manifest.json");
    if (manifest.empty()) return false;
    const auto document = nlohmann::json::parse(
        reinterpret_cast<const char*>(manifest.data()),
        reinterpret_cast<const char*>(manifest.data() + manifest.size()));
    if (document.at("models").size() != 1) return false;
    const std::string model_name = "models/" + document.at("models").begin().key() + ".bin";
    auto model = read_test_zip(archive.get(), model_name);
    if (model.size() < 6 * sizeof(uint32_t) + sizeof(float)) return false;
    const auto original_crc = static_cast<uint32_t>(crc32(0,
        reinterpret_cast<const Bytef*>(model.data()), static_cast<uInt>(model.size())));
    const float corrupted_x = 50;
    std::memcpy(model.data() + 6 * sizeof(uint32_t), &corrupted_x, sizeof(corrupted_x));
    bool ok = true;
    for (const auto compression : {heaven::ZipCompression::Store, heaven::ZipCompression::Deflate}) {
        const auto path = root / (compression == heaven::ZipCompression::Store ? "stored.zip" : "deflated.zip");
        auto writer = heaven::ZipArchiveWriter::create(path, false);
        if (!writer || !writer->add_bytes("manifest.json", manifest, heaven::ZipCompression::Store) ||
            !writer->add_bytes(model_name, model, compression) || !writer->finish()) return false;
        {
            auto valid = heaven::TessellationCacheArchive::open(path.string());
            auto destination = expected;
            auto corrupted = expected;
            corrupted.vertices[0].px = corrupted_x;
            ok = cache_check(valid && cache_load(*valid, destination, mesh, parameters) &&
                same_surface(destination, corrupted), "shape-valid changed payload must load when its CRC is correct") && ok;
        }
        if (!replace_test_crc(path, model_name, original_crc) || heaven::validate_zip_archive(path, true)) return false;
        auto corrupt = heaven::TessellationCacheArchive::open(path.string());
        auto destination = cache_triangle<SurfaceT>(200);
        const auto untouched = destination;
        ok = cache_check(corrupt && !cache_load(*corrupt, destination, mesh, parameters) &&
            same_surface(destination, untouched), "CRC-invalid cache must miss without mutating its destination") && ok;
    }
    return ok;
}

bool unit_tessellation_cache_crc_rejection() {
    const CacheTestDirectory root;
    const auto mesh = root.path / "model.mesh";
    std::ofstream(mesh, std::ios::binary) << "stable-source";
    const bool static_ok = cache_crc_rejection<heaven::Surface>(root.path / "static", mesh);
    const bool skinned_ok = cache_crc_rejection<heaven::SkinnedSurface>(root.path / "skinned", mesh);
    return static_ok && skinned_ok;
}

bool unit_tessellation_cache_legacy_source_identity() {
    const CacheTestDirectory root;
    const std::array<std::string, 2> unique_names{"unused", "surface"};
    const std::array<std::string, 2> duplicate_names{"duplicate", "duplicate"};
    struct ImportCase {
        std::string source;
        std::string surface;
        bool expected_hit;
    };
    const std::array<ImportCase, 3> cases{{
        {source_mesh_bytes(unique_names), "surface", true},
        {source_mesh_bytes(duplicate_names), "duplicate", false},
        {"mi08", "surface", false},
    }};
    bool ok = true;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        const auto directory = root.path / std::to_string(index);
        const auto legacy = directory / ("v" + std::to_string(heaven::kTessellationCacheVersion));
        std::filesystem::create_directories(legacy);
        const auto mesh = directory / "model.mesh";
        std::ofstream(mesh, std::ios::binary) << item.source;
        auto expected = cache_triangle<heaven::Surface>(10);
        expected.name = item.surface;
        const auto binary = legacy / "entry.bin";
        const auto metadata_path = legacy / "entry.json";
        {
            const std::array<uint32_t, 4> header{
                0x54455353u, heaven::kTessellationCacheVersion, 3, 3};
            std::ofstream output(binary, std::ios::binary);
            output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
            output.write(reinterpret_cast<const char*>(expected.vertices.data()),
                static_cast<std::streamsize>(expected.vertices.size() * sizeof(heaven::Vertex)));
            output.write(reinterpret_cast<const char*>(expected.indices.data()),
                static_cast<std::streamsize>(expected.indices.size() * sizeof(uint32_t)));
        }
        const nlohmann::json metadata{
            {"schema_version", 1}, {"cache_version", heaven::kTessellationCacheVersion},
            {"mesh_source", mesh.string()}, {"density_source", ""}, {"surface", item.surface},
            {"fingerprints", {{"mesh_fnv1a64", fnv_hex(item.source)}}},
            {"topology", {{"density_factor", 1.0f}, {"edge_scale", 1.0f},
                {"subdivisions", 2}, {"base_transform", {1.0f, 1.0f, 0.0f, 0.0f}}}}};
        std::ofstream(metadata_path) << metadata.dump();
        const auto archive_path = directory / "cache.zip";
        auto cache = heaven::TessellationCacheArchive::open(
            archive_path.string(), std::nullopt, directory.string());
        if (!cache) return false;
        const heaven::TessellationCacheParameters parameters{
            .subdivisions = 2, .source_surface_index = 1};
        auto destination = expected;
        destination.vertices[0].px = 200;
        const auto untouched = destination;
        const bool hit = cache_load(*cache, destination, mesh, parameters);
        ok = cache_check(hit == item.expected_hit &&
            same_surface(destination, item.expected_hit ? expected : untouched),
            "legacy import must derive a unique ordinal and reject ambiguous or invalid headers") && ok;
        auto wrong_ordinal = parameters;
        wrong_ordinal.source_surface_index = 0;
        destination = untouched;
        ok = cache_check(!cache_load(*cache, destination, mesh, wrong_ordinal) &&
            same_surface(destination, untouched), "legacy import must not alias ordinal zero") && ok;
        if (!cache->finish()) return false;
        ok = cache_check(std::filesystem::exists(binary) && std::filesystem::exists(metadata_path) &&
            std::filesystem::exists(archive_path) == item.expected_hit,
            "legacy files must remain intact and rejected entries must not publish") && ok;
        if (item.expected_hit) {
            auto warm = heaven::TessellationCacheArchive::open(archive_path.string());
            destination = untouched;
            ok = cache_check(warm && cache_load(*warm, destination, mesh, parameters) &&
                same_surface(destination, expected), "safe legacy import must survive warm reopen") && ok;
            int error = 0;
            std::unique_ptr<zip_t, TestZipClose> archive(zip_open(archive_path.string().c_str(), ZIP_RDONLY, &error));
            const auto manifest = archive ? read_test_zip(archive.get(), "manifest.json") : std::vector<std::byte>{};
            if (manifest.empty()) return false;
            const auto document = nlohmann::json::parse(
                reinterpret_cast<const char*>(manifest.data()),
                reinterpret_cast<const char*>(manifest.data() + manifest.size()));
            ok = cache_check(document.at("models").size() == 1 &&
                document.at("models").begin().value().at("source_surface_index") == 1,
                "import metadata must retain the derived source ordinal") && ok;
        }
    }
    return ok;
}

bool unit_tessellation_cache_previous_zip_identity() {
    const CacheTestDirectory root;
    const auto mesh = root.path / "model.mesh";
    std::ofstream(mesh, std::ios::binary) << "stable-source";
    const auto current_path = root.path / "current.zip";
    const auto expected = cache_triangle<heaven::Surface>(0);
    const heaven::TessellationCacheParameters parameters{.subdivisions = 2};
    {
        auto current = heaven::TessellationCacheArchive::open(current_path.string());
        if (!current) return false;
        cache_stage(*current, expected, mesh, parameters);
        if (!current->finish()) return false;
    }
    int error = 0;
    std::unique_ptr<zip_t, TestZipClose> archive(zip_open(current_path.string().c_str(), ZIP_RDONLY, &error));
    if (!archive) return false;
    const auto manifest = read_test_zip(archive.get(), "manifest.json");
    if (manifest.empty()) return false;
    auto document = nlohmann::json::parse(
        reinterpret_cast<const char*>(manifest.data()),
        reinterpret_cast<const char*>(manifest.data() + manifest.size()));
    const auto& models = document.at("models");
    if (models.size() != 1) return false;
    const std::string model_name = "models/" + models.begin().key() + ".bin";
    auto model = read_test_zip(archive.get(), model_name);
    if (model.size() < 6 * sizeof(uint32_t)) return false;
    constexpr uint32_t previous_version = 36;
    document["cache_version"] = previous_version;
    document["models"].begin().value().erase("source_surface_index");
    std::memcpy(model.data() + sizeof(uint32_t), &previous_version, sizeof(previous_version));
    const auto previous_path = root.path / "previous.zip";
    auto writer = heaven::ZipArchiveWriter::create(previous_path, false);
    const auto previous_manifest = document.dump();
    if (!writer || !writer->add_bytes("manifest.json", std::as_bytes(std::span(previous_manifest)),
            heaven::ZipCompression::Deflate) ||
        !writer->add_bytes(model_name, model, heaven::ZipCompression::Deflate) || !writer->finish()) return false;
    auto previous = heaven::TessellationCacheArchive::open(previous_path.string());
    auto destination = cache_triangle<heaven::Surface>(200);
    const auto untouched = destination;
    return cache_check(heaven::kTessellationArchiveVersion > previous_version && previous &&
        !cache_load(*previous, destination, mesh, parameters) && same_surface(destination, untouched),
        "old ZIP identities must miss rather than re-key ambiguous surfaces");
}

const heaven::testing::Register registered(
    unit_tessellation_cache_archive);
const heaven::testing::Register registered_persistence(
    unit_tessellation_cache_persistence);
const heaven::testing::Register registered_duplicate_identity(
    unit_tessellation_cache_duplicate_identity);
const heaven::testing::Register registered_crc_rejection(
    unit_tessellation_cache_crc_rejection);
const heaven::testing::Register registered_legacy_source_identity(
    unit_tessellation_cache_legacy_source_identity);
const heaven::testing::Register registered_previous_zip_identity(
    unit_tessellation_cache_previous_zip_identity);

} // namespace
