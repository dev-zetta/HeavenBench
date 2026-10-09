/*
 * HeavenBench — loose/ZIP procedural grass source parity test
 *
 * Builds one minimal grass-bearing mi08 mesh, loads it through both asset
 * backends, and verifies that deterministic root generation produces the same
 * non-empty result. This guards the AssetFileSystem context at the boundary
 * where procedural grass reopens scene meshes.
 */
#include "engine/asset_file_system.h"
#include "engine/grass_cpu.h"
#include "engine/zip_archive.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

struct Bytes {
    std::vector<std::byte> data;
    void u8(uint8_t value) { data.push_back(static_cast<std::byte>(value)); }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value));
        u8(static_cast<uint8_t>(value >> 8));
    }
    void u32(uint32_t value) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(value >> (8 * i)));
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void f32(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void text(std::string_view value) {
        for (const char c : value) u8(static_cast<uint8_t>(c));
    }
};

std::vector<std::byte> grass_mesh() {
    Bytes bytes;
    bytes.text("mi08");
    const float bounds[10] = {-1, -1, -1, 1, 1, 1, 0, 0, 0, 2};
    for (const float value : bounds) bytes.f32(value);
    bytes.i32(1);
    bytes.i32(6);
    bytes.text("grass");
    bytes.u8(0);
    for (int i = 0; i < 40; ++i) bytes.u8(0);
    bytes.i32(3);
    const float positions[3][3] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    for (const auto& position : positions) {
        for (const float value : position) bytes.f32(value);
        bytes.u16(32767); bytes.u16(32767); bytes.u16(65535);
    }
    bytes.i32(3);
    for (int i = 0; i < 3; ++i) { bytes.f32(0); bytes.f32(0); }
    bytes.i32(0);
    bytes.i32(1);
    bytes.u16(0); bytes.u16(1); bytes.u16(2);
    return bytes.data;
}

bool unit_grass_zip_roots() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven-grass-zip-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path loose_root = root / "heaven";
    const fs::path relative = "heaven/meshes/environment/grass.mesh";
    const std::vector mesh = grass_mesh();
    std::error_code ec;
    fs::create_directories((loose_root / relative).parent_path(), ec);
    std::ofstream loose(loose_root / relative, std::ios::binary);
    loose.write(reinterpret_cast<const char*>(mesh.data()),
                static_cast<std::streamsize>(mesh.size()));
    loose.close();

    auto writer = heaven::ZipArchiveWriter::create(root / "assets.zip", false);
    const std::string manifest =
        R"({"schema_version":1,"data_root_prefix":"heaven"})";
    const std::string world = "world";
    bool ok = !ec && loose && writer &&
        writer->add_bytes(std::string(heaven::kAssetArchiveManifest),
                          std::as_bytes(std::span(manifest)),
                          heaven::ZipCompression::Deflate) &&
        writer->add_bytes("heaven/heaven/heaven.world",
                          std::as_bytes(std::span(world)),
                          heaven::ZipCompression::Deflate) &&
        writer->add_bytes("heaven/heaven/meshes/environment/grass.mesh", mesh,
                          heaven::ZipCompression::DeflateFast) &&
        writer->finish();

    auto assets = heaven::AssetFileSystem::open((root / "assets.zip").string());
    if (ok && assets) {
        const std::vector loose_roots = heaven::generate_grass_roots(
            (loose_root / relative).string(), 32, "grass");
        const std::vector zip_roots = heaven::generate_grass_roots(
            (root / "assets.zip/heaven/meshes/environment/grass.mesh").string(),
            32, "grass", std::cref(*assets));
        const std::vector alternate_roots = heaven::generate_grass_roots(
            (root / "assets.zip/heaven/meshes/environment/grass.mesh").string(),
            32, "grass", std::cref(*assets), 0xF10A3u);
        ok = loose_roots.size() == 32 && zip_roots.size() == 32 &&
             alternate_roots.size() == 32;
        bool alternate_seed_changed_position = false;
        for (std::size_t i = 0; ok && i < loose_roots.size(); ++i) {
            ok = loose_roots[i].position == zip_roots[i].position;
            alternate_seed_changed_position =
                alternate_seed_changed_position ||
                alternate_roots[i].position != zip_roots[i].position;
        }
        ok = ok && alternate_seed_changed_position;
    } else {
        ok = false;
    }

    fs::remove_all(root, ec);
    if (!ok) std::printf("[unit_grass_zip_roots] FAILED\n");
    return ok;
}

const heaven::testing::Register registered(unit_grass_zip_roots);

} // namespace
