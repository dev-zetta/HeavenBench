/*
 * HeavenBench — focused directory/ZIP AssetFileSystem parity tests
 */
#include "engine/asset_file_system.h"
#include "engine/zip_archive.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <span>
#include <thread>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_asset_file_system() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven-asset-fs-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path data = root / "Data_Root";
    std::error_code ec;
    fs::create_directories(data / "heaven", ec);
    fs::create_directories(data / "core", ec);
    if (ec) {
        std::printf("[unit_asset_file_system] FAILED: fixture setup\n");
        return false;
    }
    {
        std::ofstream(data / "heaven/heaven.world", std::ios::binary)
            << "world";
        std::ofstream(data / "heaven/a.track", std::ios::binary)
            << "abcdefghijklmnopqrstuvwxyz";
        std::ofstream(data / "heaven/z.track", std::ios::binary)
            << "last";
    }

    const std::string manifest =
        R"({"schema_version":1,"data_root_prefix":"Data_Root"})";
    const std::string world = "world";
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
    const std::string last = "last";
    const std::string crc_payload =
        "unique-stored-payload-for-crc-validation-47f621";
    std::vector<std::byte> concurrent_payload(std::size_t{128} * 1024);
    for (std::size_t i = 0; i < concurrent_payload.size(); ++i) {
        concurrent_payload[i] = static_cast<std::byte>((i * 37u + i / 251u) % 251u);
    }
    // Result<T> has boolean success semantics; keep the sequence explicit so a
    // failed add cannot be masked by a later successful call.
    auto write_deterministic = [&](const fs::path& path) -> bool {
        auto writer_result = heaven::ZipArchiveWriter::create(path, false);
        if (!writer_result) return false;
        auto writer = std::move(*writer_result);
        if (!writer.add_bytes(
                std::string(heaven::kAssetArchiveManifest),
                std::as_bytes(std::span(manifest)),
                heaven::ZipCompression::Deflate) ||
            !writer.add_bytes(
                "Data_Root/heaven/heaven.world",
                std::as_bytes(std::span(world)),
                heaven::ZipCompression::Store) ||
            !writer.add_bytes(
                "Data_Root/heaven/a.track",
                std::as_bytes(std::span(alphabet)),
                heaven::ZipCompression::DeflateFast) ||
            !writer.add_bytes(
                "Data_Root/heaven/z.track",
                std::as_bytes(std::span(last)),
                heaven::ZipCompression::DeflateFast) ||
            !writer.add_bytes(
                "Data_Root/heaven/crc.bin",
                std::as_bytes(std::span(crc_payload)),
                heaven::ZipCompression::Store) ||
            !writer.add_bytes(
                "Data_Root/heaven/thread-stored.bin", concurrent_payload,
                heaven::ZipCompression::Store) ||
            !writer.add_bytes(
                "Data_Root/heaven/thread-deflated.bin", concurrent_payload,
                heaven::ZipCompression::DeflateFast) ||
            !writer.finish()) {
            return false;
        }
        return true;
    };
    if (!write_deterministic(root / "assets.zip") ||
        !write_deterministic(root / "assets-copy.zip")) {
        std::printf("[unit_asset_file_system] FAILED: deterministic archive write\n");
        fs::remove_all(root, ec);
        return false;
    }
    std::ifstream first(root / "assets.zip", std::ios::binary);
    std::ifstream second(root / "assets-copy.zip", std::ios::binary);
    const std::vector<char> first_bytes{
        std::istreambuf_iterator<char>(first), {}};
    const std::vector<char> second_bytes{
        std::istreambuf_iterator<char>(second), {}};
    if (first_bytes != second_bytes) {
        std::printf("[unit_asset_file_system] FAILED: deterministic bytes differ\n");
        fs::remove_all(root, ec);
        return false;
    }

    // Invalid source-tree paths must be rejected before libzip sees them.
    {
        auto invalid_writer =
            heaven::ZipArchiveWriter::create(root / "invalid.zip", false);
        const std::string value = "x";
        if (!invalid_writer ||
            invalid_writer->add_bytes(
                "../escape", std::as_bytes(std::span(value)),
                heaven::ZipCompression::Store) ||
            invalid_writer->add_bytes(
                "/absolute", std::as_bytes(std::span(value)),
                heaven::ZipCompression::Store) ||
            invalid_writer->add_bytes(
                "back\\slash", std::as_bytes(std::span(value)),
                heaven::ZipCompression::Store)) {
            std::printf("[unit_asset_file_system] FAILED: invalid path accepted\n");
            fs::remove_all(root, ec);
            return false;
        }
    }

    // Aborting an overwrite transaction must preserve the published archive.
    {
        auto aborted =
            heaven::ZipArchiveWriter::create(root / "assets.zip", true);
        bool rejected = false;
        if (aborted) {
            auto added = aborted->add_file(
                "Data_Root/missing.bin", root / "does-not-exist",
                heaven::ZipCompression::Store);
            // libzip may defer opening a file source until zip_close(). Both an
            // eager add failure and a finalize failure are valid rejection.
            rejected = !added || !aborted->finish();
        }
        if (!aborted || !rejected) {
            std::printf("[unit_asset_file_system] FAILED: abort setup\n");
            fs::remove_all(root, ec);
            return false;
        }
    }
    std::ifstream preserved(root / "assets.zip", std::ios::binary);
    const std::vector<char> preserved_bytes{
        std::istreambuf_iterator<char>(preserved), {}};
    if (preserved_bytes != first_bytes) {
        std::printf("[unit_asset_file_system] FAILED: abort replaced archive\n");
        fs::remove_all(root, ec);
        return false;
    }

    auto loose = heaven::AssetFileSystem::open(data.string());
    auto zipped = heaven::AssetFileSystem::open((root / "assets.zip").string());
    bool ok = loose && zipped;
    if (ok) {
        // Audio startup and scene loading open separate streams from the same
        // archive concurrently. Exercise reads, closes, and both native and
        // replay-based seeks with payload bytes that detect mixed offsets.
        std::barrier start(4);
        std::atomic<bool> concurrent_ok{true};
        {
            std::vector<std::jthread> readers;
            readers.reserve(4);
            for (int worker = 0; worker < 4; ++worker) {
                readers.emplace_back([&, worker] {
                    start.arrive_and_wait();
                    const auto path = root / (worker % 2 == 0
                        ? "assets.zip/heaven/thread-stored.bin"
                        : "assets.zip/heaven/thread-deflated.bin");
                    for (int round = 0; round < 16; ++round) {
                        auto stream = zipped->open_read(path.string());
                        if (!stream) {
                            concurrent_ok = false;
                            return;
                        }
                        std::array<std::byte, 1024> buffer{};
                        for (const std::size_t offset : {8192u, 128u, 32768u}) {
                            if (!(*stream)->seek(static_cast<int64_t>(offset),
                                                  heaven::AssetSeekOrigin::Begin)) {
                                concurrent_ok = false;
                                return;
                            }
                            const auto count = (*stream)->read(buffer);
                            if (!count || *count != buffer.size() ||
                                !std::equal(buffer.begin(), buffer.end(),
                                            concurrent_payload.data() + offset)) {
                                concurrent_ok = false;
                                return;
                            }
                        }
                    }
                });
            }
        }
        ok = concurrent_ok.load();
        if (!ok) std::printf("[unit_asset_file_system] concurrent ZIP I/O failed\n");
    }
    if (ok) {
        auto loose_bytes = loose->read_all((data / "heaven/a.track").string());
        auto zip_bytes = zipped->read_all(
            (root / "assets.zip/heaven/a.track").string());
        ok = loose_bytes && zip_bytes && *loose_bytes == *zip_bytes;
    }
    if (ok) {
        auto native_bytes = heaven::read_asset_file(
            (data / "heaven/a.track").string());
        auto mounted_text = heaven::read_asset_text(
            (root / "assets.zip/heaven/a.track").string(),
            std::cref(*zipped));
        ok = native_bytes && mounted_text &&
             std::string(native_bytes->begin(), native_bytes->end()) ==
                 alphabet &&
             *mounted_text == alphabet;
    }
    if (ok) {
        const auto found = zipped->find(
            (root / "assets.zip/heaven").string(), ".track");
        ok = found.size() == 2 && found[0].ends_with("a.track") &&
             found[1].ends_with("z.track");
    }
    if (ok) {
        auto generated = std::make_shared<const heaven::AssetBuffer>(
            heaven::AssetBuffer{'h', 'e', 'i', 'g', 'h', 't'});
        ok = loose->mount_memory(
                 (data / "heaven/generated/height.dds").string(), generated) &&
             zipped->mount_memory(
                 (root / "assets.zip/heaven/generated/height.dds").string(),
                 generated);
        auto loose_generated = loose->read_all(
            (data / "heaven/generated/height.dds").string());
        auto zip_generated = zipped->read_all(
            (root / "assets.zip/heaven/generated/height.dds").string());
        auto loose_shared = loose->read_all_shared(
            (data / "heaven/generated/height.dds").string());
        auto zip_shared = zipped->read_all_shared(
            (root / "assets.zip/heaven/generated/height.dds").string());
        const auto found = zipped->find(
            (root / "assets.zip/heaven/generated").string(), ".dds");
        ok = ok && loose_generated && zip_generated &&
             loose_shared && zip_shared &&
             loose_shared->get() == generated.get() &&
             zip_shared->get() == generated.get() &&
             *loose_generated == *zip_generated && found.size() == 1 &&
             found.front().ends_with("height.dds");
    }
    if (ok) {
        auto stream = zipped->open_read(
            (root / "assets.zip/heaven/a.track").string());
        std::array<std::byte, 4> bytes{};
        ok = stream && (*stream)->seek(20, heaven::AssetSeekOrigin::Begin) &&
             (*stream)->read(bytes) && (*stream)->tell() == 24 &&
             (*stream)->seek(2, heaven::AssetSeekOrigin::Begin) &&
             (*stream)->read(bytes);
        const std::string text(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ok = ok && text == "cdef" &&
             !(*stream)->seek(std::numeric_limits<int64_t>::max(),
                             heaven::AssetSeekOrigin::End) &&
             (*stream)->tell() == 6;
    }
    if (ok) {
        ok = !zipped->exists("/absolute") &&
             !zipped->exists("..\\escape") &&
             !zipped->exists("../../../escape") &&
             !zipped->logical_path("../../../escape");
    }

    // Manifests are per-pack metadata, but payload overlays are ambiguous and
    // must be rejected instead of silently depending on archive order.
    if (ok) {
        const fs::path packs = root / "duplicate-packs";
        fs::create_directories(packs, ec);
        auto base = heaven::ZipArchiveWriter::create(
            packs / "00-base.zip", false);
        auto duplicate = heaven::ZipArchiveWriter::create(
            packs / "10-duplicate.zip", false);
        ok = !ec && base && duplicate &&
             base->add_bytes(std::string(heaven::kAssetArchiveManifest),
                 std::as_bytes(std::span(manifest)),
                 heaven::ZipCompression::Deflate) &&
             base->add_bytes("Data_Root/heaven/heaven.world",
                 std::as_bytes(std::span(world)),
                 heaven::ZipCompression::Store) &&
             base->add_bytes("Data_Root/heaven/a.track",
                 std::as_bytes(std::span(alphabet)),
                 heaven::ZipCompression::Store) &&
             base->finish() &&
             duplicate->add_bytes(std::string(heaven::kAssetArchiveManifest),
                 std::as_bytes(std::span(manifest)),
                 heaven::ZipCompression::Deflate) &&
             duplicate->add_bytes("Data_Root/heaven/a.track",
                 std::as_bytes(std::span(alphabet)),
                 heaven::ZipCompression::Store) &&
             duplicate->finish() &&
             !heaven::AssetFileSystem::open(packs.string());
    }

    // A corrupt stored payload leaves the central directory readable but must
    // fail both full-archive validation and the AssetStream/read_all path.
    if (ok) {
        std::vector<char> corrupt = first_bytes;
        const auto payload_begin = std::search(
            corrupt.begin(), corrupt.end(),
            crc_payload.begin(), crc_payload.end());
        ok = payload_begin != corrupt.end();
        if (ok) {
            *payload_begin ^= 0x01;
            std::ofstream output(
                root / "assets-corrupt.zip",
                std::ios::binary | std::ios::trunc);
            output.write(corrupt.data(),
                         static_cast<std::streamsize>(corrupt.size()));
            output.close();
            auto corrupt_fs = heaven::AssetFileSystem::open(
                (root / "assets-corrupt.zip").string());
            ok = output && corrupt_fs &&
                 !heaven::validate_zip_archive(
                     root / "assets-corrupt.zip", true) &&
                 !corrupt_fs->read_all(
                     (root / "assets-corrupt.zip/heaven/crc.bin").string());
        }
    }

    // A syntactically valid ZIP with an unsupported manifest is not a data
    // root, even when it contains the declared world path.
    if (ok) {
        auto bad = heaven::ZipArchiveWriter::create(
            root / "bad-manifest.zip", false);
        const std::string bad_manifest =
            R"({"schema_version":99,"data_root_prefix":"Data_Root"})";
        ok = bad &&
             bad->add_bytes(
                 std::string(heaven::kAssetArchiveManifest),
                 std::as_bytes(std::span(bad_manifest)),
                 heaven::ZipCompression::Deflate) &&
             bad->add_bytes(
                 "Data_Root/heaven/heaven.world",
                 std::as_bytes(std::span(world)),
                 heaven::ZipCompression::Store) &&
             bad->finish() &&
             !heaven::AssetFileSystem::open(
                 (root / "bad-manifest.zip").string());
    }

    fs::remove_all(root, ec);
    if (!ok) std::printf("[unit_asset_file_system] FAILED\n");
    return ok;
}

const heaven::testing::Register registered(unit_asset_file_system);

} // namespace
