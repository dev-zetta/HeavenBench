#include "dds_loader_cpu.h"
#include "engine/asset_file_system.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/sha256.h"
#include "engine/zip_archive.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift : {0u, 8u, 16u, 24u}) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::vector<std::uint8_t> bc7_dds(std::uint32_t dxgi = 99) {
    std::vector<std::uint8_t> out;
    put_u32(out, 0x20534444); // DDS magic
    put_u32(out, 124);
    put_u32(out, 0x000A1007);
    put_u32(out, 8); // height
    put_u32(out, 8); // width
    put_u32(out, 64);
    put_u32(out, 0);
    put_u32(out, 4); // mip count: 8, 4, 2, 1
    for (int i = 0; i < 11; ++i) put_u32(out, 0);
    put_u32(out, 32);
    put_u32(out, 0x4); // FOURCC
    put_u32(out, 0x30315844); // DX10
    for (int i = 0; i < 5; ++i) put_u32(out, 0);
    put_u32(out, 0x00401008); // texture, complex, mipmap
    for (int i = 0; i < 4; ++i) put_u32(out, 0);
    put_u32(out, dxgi);
    put_u32(out, 3); // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    put_u32(out, 0);
    put_u32(out, 1); // array size
    put_u32(out, 0);
    out.insert(out.end(), 64, 0x11);
    out.insert(out.end(), 16, 0x22);
    out.insert(out.end(), 16, 0x33);
    out.insert(out.end(), 16, 0x44);
    return out;
}

struct TempTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("heaven-texture-quality-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree() { std::filesystem::create_directories(root); }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

bool write_bytes(const std::filesystem::path& path,
                 std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

bool unit_texture_quality() {
    bool ok = true;
    auto fail = [&](const char* message) {
        std::printf("[unit_texture_quality] FAILED: %s\n", message);
        ok = false;
    };
    TempTree temp;

    const std::string abc = "abc";
    heaven::Sha256 digest;
    digest.update(std::as_bytes(std::span(abc)));
    if (digest.finish() !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        fail("SHA-256 cache/source fingerprint helper");
    }

    // Stable value parsing and CLI precedence.
    if (heaven::texture_quality_id(heaven::TextureQuality::Original) != "original" ||
        heaven::texture_quality_id(heaven::TextureQuality::High2K) != "2k" ||
        heaven::texture_quality_id(heaven::TextureQuality::Ultra4K) != "4k" ||
        heaven::parse_texture_quality("bogus")) {
        fail("stable texture-quality identifiers");
    }
    {
        const std::vector<std::string_view> args{
            "heaven_benchmark", "--texture-quality", "2k"};
        const auto parsed = heaven::parse_cli(args);
        heaven::BenchConfig config;
        heaven::apply_cli_overlay(config, parsed);
        if (config.texture_quality != heaven::TextureQuality::High2K) {
            fail("CLI texture-quality parse");
        }
    }
    {
        heaven::BenchConfig config;
        config.texture_quality = heaven::TextureQuality::Ultra4K;
        const auto path = temp.root / "config.json";
        if (!heaven::write_config_file(path.string(), config).empty()) {
            fail("texture-quality config write");
        } else {
            const auto loaded = heaven::load_config_file(path.string(), {});
            if (!loaded.parsed_ok ||
                loaded.config.texture_quality != heaven::TextureQuality::Ultra4K) {
                fail("texture-quality config round trip");
            }
        }
    }

    // DX10 BC7 parsing and selective starting-mip streaming.
    const auto dds_path = temp.root / "bc7.dds";
    const auto bytes = bc7_dds();
    if (!write_bytes(dds_path, bytes)) {
        fail("BC7 fixture write");
    } else {
        const auto full = heaven::load_dds_cpu(dds_path.string());
        const auto two_k = heaven::load_dds_cpu(dds_path.string(), std::nullopt, 1);
        if (!full || full.format != heaven::DDSFormatCPU::BC7 ||
            full.width != 8 || full.height != 8 || full.mip_count != 4 ||
            full.mip_data.size() != 4 || full.mip_data[0].size() != 64) {
            fail("BC7 DX10 parse");
        }
        if (!two_k || two_k.format != heaven::DDSFormatCPU::BC7 ||
            two_k.width != 4 || two_k.height != 4 || two_k.mip_count != 3 ||
            two_k.mip_data.size() != 3 || two_k.mip_data[0].size() != 16 ||
            two_k.mip_data[0].bytes()[0] != 0x22) {
            fail("starting mip selection");
        }
        const auto invalid_path = temp.root / "invalid-bc7.dds";
        const auto invalid = bc7_dds(97);
        write_bytes(invalid_path, invalid);
        if (heaven::load_dds_cpu(invalid_path.string())) {
            fail("unsupported DXGI format rejection");
        }
    }

    // Schema-v1 remains original-only; schema-v2 exposes validated mappings
    // and tier estimates.
    const auto make_archive = [&](const std::filesystem::path& path,
                                  std::string manifest) {
        auto writer_result = heaven::ZipArchiveWriter::create(path, false);
        if (!writer_result) return false;
        auto writer = std::move(*writer_result);
        const std::string world = "world";
        const auto texture = bc7_dds();
        return writer.add_bytes(
                   std::string(heaven::kAssetArchiveManifest),
                   std::as_bytes(std::span(manifest)),
                   heaven::ZipCompression::Deflate) &&
               writer.add_bytes("Data_Root/heaven/heaven.world",
                   std::as_bytes(std::span(world)), heaven::ZipCompression::Store) &&
               writer.add_bytes("Data_Root/heaven/x.dds",
                   std::as_bytes(std::span(texture)), heaven::ZipCompression::Store) &&
               writer.add_bytes("Data_Root/texture_sets/4k/x.dds",
                   std::as_bytes(std::span(texture)), heaven::ZipCompression::Store) &&
               writer.finish();
    };
    const std::string v1 =
        R"({"schema_version":1,"data_root_prefix":"Data_Root"})";
    if (!make_archive(temp.root / "v1.zip", v1)) {
        fail("schema-v1 fixture");
    } else {
        const auto assets = heaven::AssetFileSystem::open((temp.root / "v1.zip").string());
        if (!assets || assets->texture_set_summary().available ||
            assets->texture_variant("heaven/x.dds")) {
            fail("schema-v1 original-only compatibility");
        }
    }
    const std::string hash(64, 'a');
    const std::string v2 =
        std::string(R"({"schema_version":2,"data_root_prefix":"Data_Root","texture_sets":{"estimated_gpu_bytes":{"original":112,"2k":48,"4k":112},"textures":{"Data_Root/heaven/x.dds":{"source_sha256":")") +
        hash + R"(","variant":{"path":"Data_Root/texture_sets/4k/x.dds","sha256":")" +
        hash + R"(","format":"BC7_SRGB","width":8,"height":8,"gpu_bytes_2k":48,"gpu_bytes_4k":112}}}}})";
    if (!make_archive(temp.root / "v2.zip", v2)) {
        fail("schema-v2 fixture");
    } else {
        const auto assets = heaven::AssetFileSystem::open((temp.root / "v2.zip").string());
        const auto variant = assets
            ? assets->texture_variant("heaven/x.dds") : std::nullopt;
        if (!assets || assets->manifest_schema_version() != 2 ||
            !assets->texture_set_summary().available ||
            assets->texture_set_summary().high_2k_gpu_bytes != 48 ||
            !variant || variant->width != 8 ||
            variant->variant_path != "Data_Root/texture_sets/4k/x.dds") {
            fail("schema-v2 texture-set index");
        }
    }

    // A ZIP directory is one logical mount: originals can live in a schema-v1
    // base pack while schema-v2 metadata and variants live in an extension
    // pack. Neither archive duplicates the other's payload entries.
    const auto packs = temp.root / "packs";
    std::filesystem::create_directories(packs);
    const auto texture = bc7_dds();
    auto base_result = heaven::ZipArchiveWriter::create(
        packs / "assets.zip", false);
    auto hires_result = heaven::ZipArchiveWriter::create(
        packs / "assets-hires.zip", false);
    bool split_written = base_result && hires_result;
    if (split_written) {
        auto base = std::move(*base_result);
        auto hires = std::move(*hires_result);
        const std::string world = "world";
        split_written =
            base.add_bytes(std::string(heaven::kAssetArchiveManifest),
                std::as_bytes(std::span(v1)), heaven::ZipCompression::Deflate) &&
            base.add_bytes("Data_Root/heaven/heaven.world",
                std::as_bytes(std::span(world)), heaven::ZipCompression::Store) &&
            base.add_bytes("Data_Root/heaven/x.dds",
                std::as_bytes(std::span(texture)), heaven::ZipCompression::Store) &&
            base.finish() &&
            hires.add_bytes(std::string(heaven::kAssetArchiveManifest),
                std::as_bytes(std::span(v2)), heaven::ZipCompression::Deflate) &&
            hires.add_bytes("Data_Root/texture_sets/4k/x.dds",
                std::as_bytes(std::span(texture)), heaven::ZipCompression::Store) &&
            hires.finish();
    }
    if (!split_written) {
        fail("split texture-pack fixture");
    } else {
        const auto assets = heaven::AssetFileSystem::open(packs.string());
        const auto variant = assets
            ? assets->texture_variant("Data_Root/heaven/x.dds")
            : std::nullopt;
        const auto original = assets
            ? assets->read_all("Data_Root/heaven/x.dds")
            : heaven::Result<heaven::AssetBuffer>{
                  std::unexpected(heaven::Error{"mount failed"})};
        const auto generated = assets
            ? assets->read_all("Data_Root/texture_sets/4k/x.dds")
            : heaven::Result<heaven::AssetBuffer>{
                  std::unexpected(heaven::Error{"mount failed"})};
        if (!assets ||
            assets->kind() != heaven::AssetFileSystem::Kind::ZipDirectory ||
            assets->manifest_schema_version() != 2 || !variant ||
            !original || !generated || *original != *generated) {
            fail("split base/hi-res ZIP directory mount");
        }
    }
    return ok;
}

const heaven::testing::Register reg_unit_texture_quality{&unit_texture_quality};

} // namespace
