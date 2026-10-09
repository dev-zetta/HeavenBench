#include "benchmark/microprofiler.h"
/*
 * HeavenBench — scene-scoped ZIP tessellation cache implementation
 */
#include "engine/tessellation_cache_archive.h"

#include "engine/asset_file_system.h"
#include "engine/cpu_tessellation.h"
#include "engine/loader_validation.h"
#include "engine/zip_archive.h"
#include "json.hpp"

#include <zip.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>

namespace heaven {
namespace {

constexpr uint32_t kMagic = 0x5a535354; // "TSSZ"
constexpr uint32_t kStaticKind = 1;
constexpr uint32_t kSkinnedKind = 2;
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint64_t kMaxCacheEntryBytes = 4ull * 1024 * 1024 * 1024;

struct ZipArchiveClose {
    void operator()(zip_t* archive) const noexcept {
        if (archive) zip_close(archive);
    }
};

struct ZipFileClose {
    void operator()(zip_file_t* file) const noexcept {
        if (file) zip_fclose(file);
    }
};

using ZipArchiveOwner = std::unique_ptr<zip_t, ZipArchiveClose>;
using ZipFileOwner = std::unique_ptr<zip_file_t, ZipFileClose>;

void hash_bytes(uint64_t& hash, std::span<const std::byte> bytes) {
    for (std::byte byte : bytes) {
        hash ^= std::to_integer<uint8_t>(byte);
        hash *= kFnvPrime;
    }
}

void hash_string(uint64_t& hash, std::string_view text) {
    hash_bytes(hash, std::as_bytes(std::span(text)));
    const std::byte zero{};
    hash_bytes(hash, std::span(&zero, 1));
}

template <typename T>
void hash_scalar(uint64_t& hash, const T& value) {
    hash_bytes(hash, std::as_bytes(std::span(&value, 1)));
}

std::string hex64(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(value));
    return text;
}

std::string portable_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    for (std::string_view marker : {"/heaven/", "/core/"}) {
        if (const auto at = path.find(marker); at != std::string::npos) {
            return path.substr(at + 1);
        }
    }
    return std::filesystem::path(path).lexically_normal().generic_string();
}

template <typename VertexT>
std::vector<std::byte> encode_surface(
    std::span<const VertexT> vertices, std::span<const uint32_t> indices,
    uint32_t kind) {
    constexpr std::size_t kHeaderWords = 6;
    if (vertices.empty() || indices.empty() ||
        vertices.size() > std::numeric_limits<uint32_t>::max() ||
        indices.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }
    const uint64_t vertex_bytes = vertices.size_bytes();
    const uint64_t index_bytes = indices.size_bytes();
    const uint64_t total = kHeaderWords * sizeof(uint32_t) +
                           vertex_bytes + index_bytes;
    if (total > std::min(kMaxCacheEntryBytes,
                         uint64_t{std::numeric_limits<std::size_t>::max()})) {
        return {};
    }
    const std::array<uint32_t, kHeaderWords> header = {
        kMagic, kTessellationArchiveVersion, kind,
        static_cast<uint32_t>(sizeof(VertexT)),
        static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size())
    };
    std::vector<std::byte> result(static_cast<std::size_t>(total));
    std::size_t offset = 0;
    auto append = [&](const void* source, std::size_t size) {
        std::memcpy(result.data() + offset, source, size);
        offset += size;
    };
    append(header.data(), sizeof(header));
    append(vertices.data(), vertices.size_bytes());
    append(indices.data(), indices.size_bytes());
    return result;
}

template <typename VertexT>
bool decode_surface(std::span<const std::byte> bytes, uint32_t expected_kind,
                    std::vector<VertexT>& vertices,
                    std::vector<uint32_t>& indices) {
    constexpr std::size_t kHeaderBytes = 6 * sizeof(uint32_t);
    if (bytes.size() < kHeaderBytes) return false;
    std::array<uint32_t, 6> header{};
    std::memcpy(header.data(), bytes.data(), sizeof(header));
    const auto [magic, version, kind, vertex_size, vertex_count, index_count] =
        header;
    if (magic != kMagic || version != kTessellationArchiveVersion ||
        kind != expected_kind || vertex_size != sizeof(VertexT) ||
        vertex_count == 0 || index_count == 0 || index_count % 3 != 0 ||
        vertex_count > loadval::kMaxElemCount ||
        index_count > loadval::kMaxElemCount) {
        return false;
    }
    const uint64_t vertex_bytes = uint64_t{vertex_count} * sizeof(VertexT);
    const uint64_t index_bytes = uint64_t{index_count} * sizeof(uint32_t);
    if (!loadval::fits(kHeaderBytes, vertex_bytes, bytes.size()) ||
        !loadval::fits(kHeaderBytes + vertex_bytes, index_bytes, bytes.size()) ||
        kHeaderBytes + vertex_bytes + index_bytes != bytes.size()) {
        return false;
    }
    std::vector<VertexT> new_vertices(vertex_count);
    std::vector<uint32_t> new_indices(index_count);
    std::memcpy(new_vertices.data(), bytes.data() + kHeaderBytes, vertex_bytes);
    std::memcpy(new_indices.data(),
                bytes.data() + kHeaderBytes + vertex_bytes, index_bytes);
    if (std::any_of(new_indices.begin(), new_indices.end(),
                    [vertex_count](uint32_t value) {
                        return value >= vertex_count;
                    })) {
        return false;
    }
    vertices = std::move(new_vertices);
    indices = std::move(new_indices);
    return true;
}

Result<std::vector<std::byte>> read_zip_entry(zip_t& archive,
                                               std::string_view entry) {
    zip_stat_t stat{};
    zip_stat_init(&stat);
    std::string name(entry);
    if (zip_stat(&archive, name.c_str(), ZIP_FL_ENC_UTF_8, &stat) != 0 ||
        !(stat.valid & ZIP_STAT_SIZE) ||
        stat.size > kMaxCacheEntryBytes ||
        stat.size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(Error("invalid tessellation cache entry: " + name));
    }
    ZipFileOwner file(
        zip_fopen(&archive, name.c_str(), ZIP_FL_ENC_UTF_8));
    if (!file) {
        return std::unexpected(Error("cannot open tessellation cache entry: " +
                                     name));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(stat.size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const zip_int64_t count = zip_fread(
            file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            return std::unexpected(Error("corrupt tessellation cache entry: " +
                                         name));
        }
        offset += static_cast<std::size_t>(count);
    }
    if (zip_fclose(file.release()) != 0) {
        return std::unexpected(Error("CRC failure in tessellation cache entry: " +
                                     name));
    }
    return bytes;
}

} // namespace

// Private scene-lifetime state: open archive index, stable manifest, staged
// misses, fingerprints, and the single publish transaction.
struct TessellationCacheArchive::Impl {
    std::string archive_file;
    std::string legacy_cache_dir;
    AssetFileSystemRef assets;
    ZipArchiveOwner archive;
    nlohmann::json manifest = {
        {"schema_version", 1},
        {"cache_version", kTessellationArchiveVersion},
        {"models", nlohmann::json::object()}
    };
    std::filesystem::path stage_dir;
    std::map<std::string, std::filesystem::path> staged;
    std::unordered_map<std::string, uint64_t> fingerprints;
    bool dirty = false;
    bool finished = false;
    std::optional<Error> staging_error;

    ~Impl() {
        std::error_code ec;
        if (!stage_dir.empty()) std::filesystem::remove_all(stage_dir, ec);
    }

    std::string logical(std::string_view path) const {
        if (path.empty()) return {};
        if (assets) {
            auto result = assets->get().logical_path(path);
            if (result) return *result;
        }
        return portable_path(std::string(path));
    }

    uint64_t fingerprint(const std::string& path) {
        if (path.empty()) return 0;
        const std::string logical_path = logical(path);
        if (auto found = fingerprints.find(logical_path);
            found != fingerprints.end()) {
            return found->second;
        }
        uint64_t hash = kFnvOffset;
        if (assets) {
            auto stream_result = assets->get().open_read(path);
            if (stream_result) {
                std::array<std::byte, std::size_t{64} * 1024> buffer{};
                while (true) {
                    auto count = (*stream_result)->read(buffer);
                    if (!count || *count == 0) break;
                    hash_bytes(hash, std::span(buffer).first(*count));
                }
            }
        } else {
            std::ifstream input(path, std::ios::binary);
            std::array<std::byte, std::size_t{64} * 1024> buffer{};
            while (input) {
                input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
                hash_bytes(hash, std::span(buffer).first(
                    static_cast<std::size_t>(input.gcount())));
            }
        }
        fingerprints.emplace(logical_path, hash);
        return hash;
    }

    std::string key(const std::string& mesh_path,
                    const std::string& surface_name,
                    const TessellationCacheParameters& parameters,
                    uint32_t kind) {
        uint64_t hash = kFnvOffset;
        hash_scalar(hash, kTessellationArchiveVersion);
        hash_scalar(hash, kind);
        hash_string(hash, logical(mesh_path));
        hash_scalar(hash, fingerprint(mesh_path));
        hash_string(hash, surface_name);
        hash_string(hash, logical(parameters.density_path));
        hash_scalar(hash, fingerprint(parameters.density_path));
        hash_string(hash, logical(parameters.height_path));
        hash_scalar(hash, fingerprint(parameters.height_path));
        hash_string(hash, logical(parameters.source_normal_path));
        hash_scalar(hash, fingerprint(parameters.source_normal_path));
        hash_string(hash, parameters.generator_revision);
        hash_scalar(hash, std::bit_cast<uint32_t>(parameters.density_factor));
        hash_scalar(hash, std::bit_cast<uint32_t>(parameters.edge_scale));
        hash_scalar(hash, parameters.subdivisions);
        for (float component : parameters.base_transform) {
            hash_scalar(hash, std::bit_cast<uint32_t>(component));
        }
        hash_scalar(hash, std::bit_cast<uint32_t>(parameters.boundary_fade));
        hash_scalar(hash, parameters.surface_policy_version);
        return hex64(hash);
    }

    Result<std::vector<std::byte>> read_model(const std::string& key) {
        if (auto found = staged.find(key); found != staged.end()) {
            std::ifstream input(found->second, std::ios::binary | std::ios::ate);
            if (!input) return std::unexpected(Error("cannot read staged cache entry"));
            const std::streamoff size = input.tellg();
            if (size < 0 ||
                std::cmp_greater(size, kMaxCacheEntryBytes)) {
                return std::unexpected(Error("invalid staged cache entry"));
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(bytes.data()), size);
            if (!input) return std::unexpected(Error("short staged cache entry"));
            return bytes;
        }
        if (!archive) return std::unexpected(Error("cache archive is not open"));
        return read_zip_entry(*archive, "models/" + key + ".bin");
    }

    void stage_model(const std::string& key, std::span<const std::byte> bytes,
                     const nlohmann::json& metadata) {
        if (bytes.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(stage_dir, ec);
        if (ec) {
            if (!staging_error) staging_error = Error(
                "cannot create tessellation cache staging directory '" +
                stage_dir.string() + "': " + ec.message());
            return;
        }
        const auto path = stage_dir / (key + ".bin");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            std::filesystem::remove(path, ec);
            if (!staging_error) staging_error = Error(
                "cannot stage tessellation cache entry: " + path.string());
            return;
        }
        staged[key] = path;
        manifest["models"][key] = metadata;
        dirty = true;
    }
};

TessellationCacheArchive::TessellationCacheArchive() = default;
TessellationCacheArchive::TessellationCacheArchive(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
TessellationCacheArchive::~TessellationCacheArchive() = default;
TessellationCacheArchive::TessellationCacheArchive(
    TessellationCacheArchive&&) noexcept = default;
TessellationCacheArchive& TessellationCacheArchive::operator=(
    TessellationCacheArchive&&) noexcept = default;

Result<TessellationCacheArchive> TessellationCacheArchive::open(
    std::string archive_file, AssetFileSystemRef assets,
    std::string legacy_cache_dir) {
    profile::Scope profile_scope("Open tessellation archive", "Cache");
    auto impl = std::make_unique<Impl>();
    impl->archive_file = std::move(archive_file);
    impl->legacy_cache_dir = std::move(legacy_cache_dir);
    impl->assets = assets;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    impl->stage_dir = std::filesystem::path(impl->archive_file).parent_path() /
                      (".tessellation-stage-" + std::to_string(stamp));

    if (std::filesystem::exists(impl->archive_file)) {
        int error = 0;
        impl->archive.reset(
            zip_open(impl->archive_file.c_str(), ZIP_RDONLY, &error));
        if (!impl->archive) {
            return std::unexpected(Error("cannot open tessellation cache archive: " +
                                         impl->archive_file));
        }
        auto manifest = read_zip_entry(*impl->archive, "manifest.json");
        if (manifest) {
            try {
                auto parsed = nlohmann::json::parse(
                    reinterpret_cast<const char*>(manifest->data()),
                    reinterpret_cast<const char*>(manifest->data() +
                                                   manifest->size()));
                if (parsed.value("schema_version", 0) == 1 &&
                    parsed.value("cache_version", 0u) ==
                        kTessellationArchiveVersion &&
                    parsed.contains("models") && parsed["models"].is_object()) {
                    impl->manifest = std::move(parsed);
                }
            } catch (...) {
                // A malformed manifest invalidates all hits but does not block
                // recomputation and a new atomic archive publication.
                impl->manifest["models"] = nlohmann::json::object();
            }
        }
    }

    // Import only loose payloads generated by the current topology semantics.
    // Re-keying an older version would resurrect geometry invalidated by a
    // midpoint correction. Valid source files are deliberately left untouched.
    const std::string loose_version =
        "v" + std::to_string(kTessellationCacheVersion);
    const std::filesystem::path legacy =
        std::filesystem::path(impl->legacy_cache_dir) / loose_version;
    std::error_code legacy_ec;
    if (std::filesystem::is_directory(legacy, legacy_ec)) {
        for (const auto& item :
             std::filesystem::directory_iterator(legacy, legacy_ec)) {
            if (legacy_ec || item.path().extension() != ".json") continue;
            try {
                nlohmann::json metadata;
                std::ifstream metadata_input(item.path());
                metadata_input >> metadata;
                if (metadata.value("schema_version", 0) != 1 ||
                    metadata.value("cache_version", 0u) !=
                        kTessellationCacheVersion) {
                    continue;
                }
                const std::string mesh = metadata.value("mesh_source", "");
                const std::string surface_name = metadata.value("surface", "");
                const std::string density =
                    metadata.value("density_source", "");
                if (mesh.empty() || surface_name.empty()) continue;

                const auto& topology = metadata["topology"];
                const float density_factor =
                    topology.value("density_factor", 0.0f);
                const float edge_scale = topology.value("edge_scale", 0.0f);
                const int subdivisions = topology.value("subdivisions", 0);
                std::array<float, 4> transform{1, 1, 0, 0};
                if (topology.contains("base_transform") &&
                    topology["base_transform"].is_array() &&
                    topology["base_transform"].size() == 4) {
                    for (std::size_t index = 0; index < 4; ++index) {
                        transform[index] =
                            topology["base_transform"][index].get<float>();
                    }
                }
                const auto& fingerprints = metadata["fingerprints"];
                if (fingerprints.value("mesh_fnv1a64", "") !=
                    hex64(impl->fingerprint(mesh))) {
                    continue;
                }
                if (!density.empty() &&
                    fingerprints.value("density_fnv1a64", "") !=
                        hex64(impl->fingerprint(density))) {
                    continue;
                }

                std::filesystem::path binary = item.path();
                binary.replace_extension(".bin");
                std::ifstream input(binary, std::ios::binary | std::ios::ate);
                const std::streamoff size =
                    input ? static_cast<std::streamoff>(input.tellg())
                          : std::streamoff{-1};
                if (size < 16 ||
                    std::cmp_greater(size, kMaxCacheEntryBytes)) {
                    continue;
                }
                std::vector<std::byte> bytes(static_cast<std::size_t>(size));
                input.seekg(0);
                input.read(reinterpret_cast<char*>(bytes.data()), size);
                if (!input) continue;
                std::array<uint32_t, 4> header{};
                std::memcpy(header.data(), bytes.data(), sizeof(header));
                if (header[0] != 0x54455353 ||
                    header[1] != kTessellationCacheVersion ||
                    header[2] == 0 || header[3] == 0 ||
                    header[3] % 3 != 0 ||
                    header[2] > loadval::kMaxElemCount ||
                    header[3] > loadval::kMaxElemCount) {
                    continue;
                }
                const uint64_t vertex_bytes =
                    uint64_t{header[2]} * sizeof(Vertex);
                const uint64_t index_bytes =
                    uint64_t{header[3]} * sizeof(uint32_t);
                if (16 + vertex_bytes + index_bytes != bytes.size()) continue;
                Surface imported;
                imported.name = surface_name;
                imported.vertices.resize(header[2]);
                imported.indices.resize(header[3]);
                std::memcpy(imported.vertices.data(), bytes.data() + 16,
                            vertex_bytes);
                std::memcpy(imported.indices.data(),
                            bytes.data() + 16 + vertex_bytes, index_bytes);
                if (std::any_of(
                        imported.indices.begin(), imported.indices.end(),
                        [&](uint32_t value) {
                            return value >= imported.vertices.size();
                        })) {
                    continue;
                }
                const TessellationCacheParameters parameters{
                    .density_path = density,
                    .density_factor = density_factor,
                    .edge_scale = edge_scale,
                    .subdivisions = subdivisions,
                    .base_transform = transform,
                };
                const std::string key = impl->key(
                    mesh, surface_name, parameters, kStaticKind);
                if (impl->manifest["models"].contains(key)) continue;
                auto encoded = encode_surface<Vertex>(
                    imported.vertices, imported.indices, kStaticKind);
                nlohmann::json imported_metadata = {
                    {"kind", "static"},
                    {"mesh", impl->logical(mesh)},
                    {"surface", surface_name},
                    {"density", impl->logical(density)},
                    {"density_factor", density_factor},
                    {"edge_scale", edge_scale},
                    {"subdivisions", subdivisions},
                    {"base_transform", transform},
                    {"imported_from", loose_version}
                };
                impl->stage_model(key, encoded, imported_metadata);
            } catch (...) {
                // Corrupt legacy metadata is an ordinary miss.
                continue;
            }
        }
    }
    return TessellationCacheArchive(std::move(impl));
}

namespace {
template <typename SurfaceT, typename VertexT>
bool load_typed(TessellationCacheArchive::Impl& impl, SurfaceT& surface,
                const std::string& mesh_path,
                const std::string& surface_name,
                const TessellationCacheParameters& parameters,
                uint32_t kind) {
    const std::string key = impl.key(
        mesh_path, surface_name, parameters, kind);
    if (!impl.manifest["models"].contains(key)) return false;
    auto bytes = impl.read_model(key);
    if (!bytes) return false;
    return decode_surface<VertexT>(*bytes, kind, surface.vertices,
                                   surface.indices);
}

template <typename SurfaceT, typename VertexT>
void stage_typed(TessellationCacheArchive::Impl& impl,
                 const SurfaceT& surface, const std::string& mesh_path,
                 const std::string& surface_name,
                 const TessellationCacheParameters& parameters,
                 uint32_t kind,
                 uint64_t source_vertex_count,
                 uint64_t source_index_count) {
    const std::string key = impl.key(
        mesh_path, surface_name, parameters, kind);
    auto bytes = encode_surface<VertexT>(surface.vertices, surface.indices, kind);
    nlohmann::json metadata = {
        {"kind", kind == kStaticKind ? "static" : "skinned"},
        {"mesh", impl.logical(mesh_path)},
        {"surface", surface_name},
        {"density", impl.logical(parameters.density_path)},
        {"height", impl.logical(parameters.height_path)},
        {"source_normal", impl.logical(parameters.source_normal_path)},
        {"generator_revision", parameters.generator_revision},
        {"mesh_fingerprint", hex64(impl.fingerprint(mesh_path))},
        {"density_fingerprint", hex64(impl.fingerprint(parameters.density_path))},
        {"height_fingerprint", hex64(impl.fingerprint(parameters.height_path))},
        {"source_normal_fingerprint",
         hex64(impl.fingerprint(parameters.source_normal_path))},
        {"density_factor", parameters.density_factor},
        {"edge_scale", parameters.edge_scale},
        {"subdivisions", parameters.subdivisions},
        {"base_transform", parameters.base_transform},
        {"boundary_fade", parameters.boundary_fade},
        {"surface_policy_version", parameters.surface_policy_version},
        {"source_vertices", source_vertex_count},
        {"source_indices", source_index_count},
        {"output_vertices", surface.vertices.size()},
        {"output_indices", surface.indices.size()},
        {"payload_bytes", bytes.size()}
    };
    impl.stage_model(key, bytes, metadata);
}
} // namespace

bool TessellationCacheArchive::load(
    Surface& surface, const std::string& mesh_path,
    const std::string& surface_name,
    const TessellationCacheParameters& parameters) {
    return impl_ && load_typed<Surface, Vertex>(
        *impl_, surface, mesh_path, surface_name, parameters, kStaticKind);
}

void TessellationCacheArchive::stage(
    const Surface& surface, const std::string& mesh_path,
    const std::string& surface_name,
    const TessellationCacheParameters& parameters,
    uint64_t source_vertex_count,
    uint64_t source_index_count) {
    if (impl_) stage_typed<Surface, Vertex>(
        *impl_, surface, mesh_path, surface_name, parameters, kStaticKind,
        source_vertex_count, source_index_count);
}

bool TessellationCacheArchive::load_skinned(
    SkinnedSurface& surface, const std::string& mesh_path,
    const std::string& surface_name,
    const TessellationCacheParameters& parameters) {
    return impl_ && load_typed<SkinnedSurface, SkinnedVertex>(
        *impl_, surface, mesh_path, surface_name, parameters, kSkinnedKind);
}

void TessellationCacheArchive::stage_skinned(
    const SkinnedSurface& surface, const std::string& mesh_path,
    const std::string& surface_name,
    const TessellationCacheParameters& parameters,
    uint64_t source_vertex_count,
    uint64_t source_index_count) {
    if (impl_) stage_typed<SkinnedSurface, SkinnedVertex>(
        *impl_, surface, mesh_path, surface_name, parameters, kSkinnedKind,
        source_vertex_count, source_index_count);
}

Result<void> TessellationCacheArchive::finish() {
    profile::Scope profile_scope("Commit tessellation archive", "Cache");
    if (!impl_ || impl_->finished) return {};
    if (impl_->staging_error) return std::unexpected(*impl_->staging_error);
    if (!impl_->dirty) {
        impl_->finished = true;
        return {};
    }

    // Preserve valid entries from the previous archive when updating only the
    // misses reached by this scene/configuration.
    if (impl_->archive) {
        for (auto item = impl_->manifest["models"].begin();
             item != impl_->manifest["models"].end(); ++item) {
            const std::string& key = item.key();
            if (impl_->staged.contains(key)) continue;
            auto bytes = read_zip_entry(*impl_->archive,
                                        "models/" + key + ".bin");
            if (!bytes) continue;
            impl_->stage_model(key, *bytes, item.value());
        }
        if (impl_->staging_error) return std::unexpected(*impl_->staging_error);
        impl_->archive.reset();
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(impl_->archive_file).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) return std::unexpected(Error("cannot create tessellation cache directory '" +
                                       parent.string() + "': " + ec.message()));

    auto writer_result = ZipArchiveWriter::create(impl_->archive_file, true);
    if (!writer_result) return std::unexpected(writer_result.error());
    ZipArchiveWriter writer = std::move(*writer_result);

    impl_->manifest["model_count"] = impl_->staged.size();
    const std::string manifest_text = impl_->manifest.dump(2) + "\n";
    auto add_manifest = writer.add_bytes(
        "manifest.json", std::as_bytes(std::span(manifest_text)),
        ZipCompression::Deflate);
    if (!add_manifest) return add_manifest;
    for (const auto& [key, path] : impl_->staged) {
        auto added = writer.add_file("models/" + key + ".bin", path,
                                     ZipCompression::DeflateFast);
        if (!added) return added;
    }
    auto finished = writer.finish();
    if (!finished) return finished;
    auto validated = validate_zip_archive(impl_->archive_file, true);
    if (validated) impl_->finished = true;
    return validated;
}

const std::string& TessellationCacheArchive::archive_file() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->archive_file : empty;
}

std::string default_tessellation_cache_archive(
    const std::string& data_root, bool data_root_is_zip) {
    const std::filesystem::path base = data_root_is_zip
        ? std::filesystem::path(data_root).parent_path()
        : std::filesystem::path(data_root);
    return (base / "cache" /
            ("tessellation-v" +
             std::to_string(kTessellationArchiveVersion) + ".zip")).string();
}

} // namespace heaven
