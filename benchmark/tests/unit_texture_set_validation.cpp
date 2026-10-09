#include "asset_packer/asset_packer.h"
#include "engine/asset_usage_recorder.h"
#include "engine/runtime_paths.h"
#include "engine/sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "json.hpp"

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

struct TempTree {
    fs::path root = fs::temp_directory_path() /
        ("heaven-texture-set-validation-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree() { fs::create_directories(root); }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

bool write_text(const fs::path& path, std::string_view text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

bool write_bytes(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift : {0u, 8u, 16u, 24u})
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> compressed_dds(
        bool normal, std::uint32_t width = 8,
        std::uint32_t height = 8) {
    std::vector<std::uint8_t> out;
    put_u32(out, 0x20534444);
    put_u32(out, 124);
    put_u32(out, 0x000A1007);
    put_u32(out, height);
    put_u32(out, width);
    put_u32(out,
        std::max(1u, (width + 3u) / 4u) *
        std::max(1u, (height + 3u) / 4u) * 16u);
    put_u32(out, 0);
    const std::uint32_t mip_count = static_cast<std::uint32_t>(
        std::bit_width(std::max(width, height)));
    put_u32(out, mip_count);
    for (int i = 0; i < 11; ++i) put_u32(out, 0);
    put_u32(out, 32);
    put_u32(out, 0x4);
    put_u32(out, normal ? 0x32495441 : 0x30315844); // ATI2 / DX10
    for (int i = 0; i < 5; ++i) put_u32(out, 0);
    put_u32(out, 0x00401008);
    for (int i = 0; i < 4; ++i) put_u32(out, 0);
    if (!normal) {
        put_u32(out, 99);
        put_u32(out, 3);
        put_u32(out, 0);
        put_u32(out, 1);
        put_u32(out, 0);
    }
    for (std::uint32_t mip = 0; mip < mip_count; ++mip) {
        const std::uint32_t w = std::max(1u, width >> mip);
        const std::uint32_t h = std::max(1u, height >> mip);
        const std::size_t size =
            static_cast<std::size_t>(std::max(1u, (w + 3u) / 4u)) *
            std::max(1u, (h + 3u) / 4u) * 16u;
        out.insert(out.end(), size, 0x5a);
    }
    return out;
}

std::pair<std::uint32_t, std::uint32_t>
png_dimensions(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::array<std::uint8_t, 24> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), "\x89PNG\r\n\x1a\n", 8) != 0) {
        return {};
    }
    const auto be32 = [&](std::size_t offset) {
        return (std::uint32_t(header[offset]) << 24u) |
               (std::uint32_t(header[offset + 1]) << 16u) |
               (std::uint32_t(header[offset + 2]) << 8u) |
               std::uint32_t(header[offset + 3]);
    };
    return {be32(16), be32(20)};
}

std::uint64_t rgba_mip_bytes(std::uint32_t width, std::uint32_t height) {
    std::uint64_t result = 0;
    for (;;) {
        result += std::uint64_t(width) * height * 4u;
        if (width == 1u && height == 1u) return result;
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
}

std::uint64_t block_mip_bytes(std::uint32_t width, std::uint32_t height,
                              std::uint32_t maximum_edge) {
    std::uint64_t result = 0;
    while (std::max(width, height) > maximum_edge) {
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
    for (;;) {
        result += std::uint64_t(std::max(1u, (width + 3u) / 4u)) *
                  std::max(1u, (height + 3u) / 4u) * 16u;
        if (width == 1u && height == 1u) return result;
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
}

std::string hash_file(const fs::path& path) {
    const auto result = heaven::sha256_file(path);
    return result ? *result : std::string{};
}

bool unit_texture_set_validation() {
    bool ok = true;
    auto fail = [&](const char* message) {
        std::printf("[unit_texture_set_validation] FAILED: %s\n", message);
        ok = false;
    };

    TempTree temp;
    const fs::path source_root = temp.root / "source";
    const fs::path manifest_dir = temp.root / "set";
    const std::string data_prefix = "Data_Root";
    const std::string dds_logical = "Data_Root/heaven/test.dds";
    const auto source_dds = compressed_dds(false, 2, 2);
    if (!write_bytes(source_root / dds_logical, source_dds)) {
        fail("fixture setup");
        return false;
    }

    // Shared inventory aggregation: incompatible bindings become data (never
    // generative), and a repeat use wins so a tiled edge cannot be baked with
    // clamp padding.
    std::error_code canonical_error;
    const fs::path canonical_source_root =
        fs::weakly_canonical(source_root, canonical_error);
    if (canonical_error) {
        fail("canonical source root");
        return false;
    }
    heaven::AssetUsageRecorder recorder(canonical_source_root.generic_string());
    recorder.record_texture(
        (source_root / dds_logical).string(), "diffuse", "clamp");
    recorder.record_texture(
        (source_root / dds_logical).string(), "specular", "repeat");
    const auto aggregate = recorder.texture_inventory().find(dds_logical);
    if (aggregate == recorder.texture_inventory().end() ||
        aggregate->second.role != "data" ||
        aggregate->second.wrap != "repeat") {
        fail("semantic/address-mode aggregation");
    }

    const std::array<std::string, 4> logical_paths{
        dds_logical,
        "Data_Root/sky/moon_disk.png",
        "Data_Root/sky/moon_normal.png",
        "Data_Root/sky/milkyway.png",
    };
    nlohmann::json entries = nlohmann::json::array();
    const fs::path bundled_root =
        heaven::bundled_resource_directory(HEAVEN_BUNDLED_ASSET_DIR);
    for (std::size_t i = 0; i < logical_paths.size(); ++i) {
        const std::string& logical = logical_paths[i];
        const fs::path source = i == 0
            ? source_root / logical
            : bundled_root / "sky" /
                fs::path(logical).filename();
        const std::string file = "variants/v" + std::to_string(i) + ".dds";
        const bool normal = i == 2;
        std::uint32_t source_width = 2;
        std::uint32_t source_height = 2;
        std::string source_format = "BC7";
        std::uint64_t source_gpu_bytes = source_dds.size() - 148u;
        if (i != 0) {
            std::tie(source_width, source_height) = png_dimensions(source);
            source_format = "PNG_RGBA8";
            source_gpu_bytes = rgba_mip_bytes(source_width, source_height);
        }
        const std::uint32_t source_edge =
            std::max(source_width, source_height);
        const std::uint32_t scale = source_edge <= 1024u ? 4u :
                                    source_edge <= 2048u ? 2u : 1u;
        const std::uint32_t output_width = source_width * scale;
        const std::uint32_t output_height = source_height * scale;
        const auto encoded = compressed_dds(
            normal, output_width, output_height);
        if (!write_bytes(manifest_dir / file, encoded)) {
            fail("variant fixture setup");
            return false;
        }
        entries.push_back({
            {"original_path", logical},
            {"source_sha256", hash_file(source)},
            {"role", i == 2 ? "normal" : "color"},
            {"wrap", i == 3 ? "repeat" : "clamp"},
            {"source", {
                {"format", source_format},
                {"width", source_width},
                {"height", source_height},
                {"gpu_bytes", source_gpu_bytes}}},
            {"status", "generated"},
            {"variant", {
                {"file", file},
                {"archive_path", "Data_Root/texture_sets/4k/v" +
                    std::to_string(i) + ".dds"},
                {"sha256", hash_file(manifest_dir / file)},
                {"format", normal ? "BC5" : "BC7_SRGB"},
                {"width", output_width},
                {"height", output_height},
                {"gpu_bytes_2k", block_mip_bytes(
                    output_width, output_height, 2048)},
                {"gpu_bytes_4k", block_mip_bytes(
                    output_width, output_height, 4096)},
            }},
        });
    }

    const std::array<std::string, 9> artifact_files{
        "reports/texture-quality-report.json",
        "reports/texture-quality-report.html",
        "reports/contact-sheet-source.png",
        "reports/contact-sheet-2k.png",
        "reports/contact-sheet-4k.png",
        "provenance/PROVENANCE.md",
        "provenance/licenses/HAT-LICENSE.txt",
        "provenance/licenses/BasicSR-LICENSE.txt",
        "provenance/licenses/Compressonator-LICENSE.txt",
    };
    nlohmann::json artifacts = nlohmann::json::array();
    for (const std::string& file : artifact_files) {
        if (!write_text(manifest_dir / file, "fixture-" + file)) {
            fail("artifact fixture setup");
            return false;
        }
        artifacts.push_back({
            {"file", file},
            {"archive_path", "Data_Root/texture_sets/metadata/" + file},
            {"sha256", hash_file(manifest_dir / file)},
        });
    }
    auto base_document = [&] {
        return nlohmann::json{
            {"format", "heavenbench-texture-set"},
            {"schema_version", 1},
            {"complete", true},
            {"generator", {{"id", "test"}, {"version", "1"}}},
            {"entries", entries},
            {"artifacts", artifacts},
        };
    };
    const std::set<std::string> used{dds_logical};
    auto validate = [&](const char* name, const nlohmann::json& document,
                        bool expected) {
        const fs::path manifest = manifest_dir /
            (std::string(name) + ".json");
        if (!write_text(manifest, document.dump(2) + "\n")) {
            fail("manifest fixture write");
            return;
        }
        const bool accepted = static_cast<bool>(
            heaven::validate_texture_set_manifest(
                manifest, used, source_root, data_prefix));
        if (accepted != expected) fail(name);
    };

    validate("complete manifest", base_document(), true);

    auto missing = base_document();
    missing["entries"].erase(missing["entries"].end() - 1);
    validate("missing required mapping", missing, false);

    auto stale = base_document();
    stale["entries"][0]["source_sha256"] = std::string(64, '0');
    validate("stale source fingerprint", stale, false);

    auto stale_metadata = base_document();
    stale_metadata["entries"][0]["source"]["gpu_bytes"] = 1;
    validate("stale source metadata", stale_metadata, false);

    auto duplicate = base_document();
    duplicate["entries"].push_back(duplicate["entries"][0]);
    validate("duplicate original mapping", duplicate, false);

    auto unsafe = base_document();
    unsafe["entries"][0]["variant"]["file"] = "../escape.dds";
    validate("unsafe variant path", unsafe, false);

    auto malformed = base_document();
    malformed.erase("generator");
    validate("corrupt manifest structure", malformed, false);

    auto missing_license = base_document();
    missing_license["artifacts"].erase(
        missing_license["artifacts"].end() - 1);
    validate("missing required license", missing_license, false);

    const fs::path corrupt_manifest = manifest_dir / "corrupt-output.json";
    const auto corrupt = base_document();
    write_text(corrupt_manifest, corrupt.dump(2) + "\n");
    write_text(manifest_dir / "variants/v0.dds", "tampered");
    if (heaven::validate_texture_set_manifest(
            corrupt_manifest, used, source_root, data_prefix)) {
        fail("corrupt variant payload");
    }

    return ok;
}

const heaven::testing::Register reg_unit_texture_set_validation{
    &unit_texture_set_validation};

} // namespace
