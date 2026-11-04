/*
 * HeavenBench — bounded binary-reader buffering and stream parity regressions
 */
#include "engine/asset_file_system.h"
#include "engine/loader_validation.h"
#include "engine/mesh.h"
#include "engine/zip_archive.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
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
using heaven::loadval::AssetBinaryReader;
using Bytes = std::vector<std::byte>;

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[unit_asset_binary_reader] FAILED: %s\n", message);
    return condition;
}

Bytes payload(std::size_t count) {
    Bytes bytes(count);
    for (std::size_t i = 0; i < count; ++i)
        bytes[i] = static_cast<std::byte>((i * 37u + i / 251u) % 251u);
    return bytes;
}

// This stateful fake counts actual backend reads independently of the reader
// implementation and can return short reads, truncation or explicit failures.
class CountedStream final : public heaven::AssetStream {
public:
    explicit CountedStream(Bytes bytes) : bytes_(std::move(bytes)) {}
    uint64_t size() const noexcept override { return bytes_.size(); }
    uint64_t tell() const noexcept override { return position; }
    heaven::Result<std::size_t> read(std::span<std::byte> destination) override {
        ++reads;
        largest_read = std::max(largest_read, destination.size());
        if (reads == fail_read_call) {
            if (write_on_failed_read && !destination.empty() && position < bytes_.size()) {
                destination.front() = bytes_[position++];
            }
            return std::unexpected(heaven::Error("injected read failure"));
        }
        const auto available = std::min(bytes_.size(), truncate_at);
        const auto count = position >= available ? 0 :
            std::min({destination.size(), chunk_limit, available - position});
        if (count != 0) std::memcpy(destination.data(), bytes_.data() + position, count);
        position += count;
        return count;
    }
    heaven::Result<void> seek(int64_t offset, heaven::AssetSeekOrigin origin) override {
        ++seeks;
        if (fail_seek) {
            if (advance_on_failed_seek) ++position;
            return std::unexpected(heaven::Error("injected seek failure"));
        }
        const auto base = origin == heaven::AssetSeekOrigin::Begin ? int64_t{0} :
            static_cast<int64_t>(origin == heaven::AssetSeekOrigin::Current ? position : bytes_.size());
        if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
            (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset))
            return std::unexpected(heaven::Error("seek overflow"));
        const auto target = base + offset;
        if (target < 0 || std::cmp_greater(target, bytes_.size()))
            return std::unexpected(heaven::Error("seek outside stream"));
        position = static_cast<std::size_t>(target);
        return {};
    }
    std::size_t position = 0, reads = 0, seeks = 0, largest_read = 0;
    std::size_t chunk_limit = std::numeric_limits<std::size_t>::max();
    std::size_t truncate_at = std::numeric_limits<std::size_t>::max();
    std::size_t fail_read_call = std::numeric_limits<std::size_t>::max();
    bool fail_seek = false, advance_on_failed_seek = false, write_on_failed_read = false;
private:
    Bytes bytes_;
};

bool read_matches(AssetBinaryReader& reader, const Bytes& bytes, std::size_t begin, std::size_t count) {
    Bytes actual(count);
    return reader.read_bytes(actual) && reader.tell() == begin + count &&
        std::equal(actual.begin(), actual.end(), bytes.begin() + static_cast<std::ptrdiff_t>(begin));
}

bool scalar_reads_are_bounded() {
    static_assert(sizeof(AssetBinaryReader) <= 128,
        "AssetBinaryReader must not carry a 64 KiB inline move/stack buffer");
    static_assert(std::is_nothrow_move_constructible_v<AssetBinaryReader>);
    static_assert(std::is_nothrow_move_assignable_v<AssetBinaryReader>);
    const auto bytes = payload(300'000);
    auto stream = std::make_unique<CountedStream>(bytes);
    auto* observed = stream.get();
    AssetBinaryReader reader(std::move(stream));
    for (std::size_t i = 0; i < 10'000; ++i) {
        std::byte byte{};
        if (!check(reader.read_object(byte) && byte == bytes[i] && reader.tell() == i + 1,
                "scalar output/position")) return false;
    }
    if (!check(observed->reads <= 4 && observed->largest_read <= std::size_t{64} * 1024,
            "scalar parsing must coalesce bounded backend reads")) return false;

    AssetBinaryReader moved(std::move(reader));
    if (!check(read_matches(moved, bytes, 10'000, 70'000), "move construction retains unread bytes"))
        return false;
    AssetBinaryReader assigned(std::make_unique<CountedStream>(Bytes{}));
    assigned = std::move(moved);
    return check(read_matches(assigned, bytes, 80'000, 120'000),
        "move assignment and large direct reads retain byte order");
}

bool short_reads_and_failures() {
    const auto bytes = payload(200'000);
    for (const auto chunk : {std::size_t{1}, std::size_t{7}, std::size_t{1023}, std::size_t{65535}}) {
        auto stream = std::make_unique<CountedStream>(bytes);
        stream->chunk_limit = chunk;
        AssetBinaryReader reader(std::move(stream));
        if (!check(read_matches(reader, bytes, 0, 31) && read_matches(reader, bytes, 31, 130'000),
                "varied backend short reads")) return false;
    }
    {
        auto stream = std::make_unique<CountedStream>(bytes);
        stream->chunk_limit = 17;
        stream->truncate_at = 53;
        AssetBinaryReader reader(std::move(stream));
        Bytes out(100);
        if (!check(!reader.read_bytes(out) && reader.tell() == 53,
                "truncated stream must fail with consumed position")) return false;
    }
    {
        auto stream = std::make_unique<CountedStream>(bytes);
        stream->chunk_limit = 17;
        stream->fail_read_call = 3;
        AssetBinaryReader reader(std::move(stream));
        Bytes out(100);
        if (!check(!reader.read_bytes(out) && reader.tell() == 34,
                "backend error must not become successful data")) return false;
    }
    {
        auto stream = std::make_unique<CountedStream>(bytes);
        auto* observed = stream.get();
        stream->fail_read_call = 1;
        stream->write_on_failed_read = true;
        AssetBinaryReader reader(std::move(stream));
        std::byte value{0xff};
        if (!check(!reader.read_object(value) && value == std::byte{0xff} && reader.tell() == 1 &&
                reader.read_object(value) && value == bytes[1] && observed->reads == 2,
                "failed read-ahead must not publish partially written cache bytes")) return false;
    }
    return true;
}

bool seeks_and_logical_position() {
    const auto bytes = payload(300'000);
    auto stream = std::make_unique<CountedStream>(bytes);
    auto* observed = stream.get();
    AssetBinaryReader reader(std::move(stream));
    if (!check(read_matches(reader, bytes, 0, 17) && reader.seek_relative(300) &&
            read_matches(reader, bytes, 317, 31) && reader.seek_relative(-25) &&
            read_matches(reader, bytes, 323, 9), "forward/backward buffered seeks")) return false;
    if (!check(!reader.seek_relative(-400) && reader.tell() == 332 &&
            !reader.seek_relative(std::numeric_limits<int64_t>::min()) && reader.tell() == 332 &&
            !reader.seek_relative(std::numeric_limits<int64_t>::max()) && reader.tell() == 332,
            "invalid and signed-boundary seeks preserve cached position")) return false;
    observed->fail_seek = true;
    if (!check(!reader.seek_relative(200'000) && reader.tell() == 332 &&
            read_matches(reader, bytes, 332, 3), "failed native seek preserves unread buffer")) return false;
    observed->fail_seek = false;
    if (!check(reader.seek_relative(80'000) && read_matches(reader, bytes, 80'335, 100'000) &&
            reader.seek_relative(-150'000) && read_matches(reader, bytes, 30'335, 11),
            "out-of-buffer seeks use logical position")) return false;
    observed->fail_seek = observed->advance_on_failed_seek = true;
    const auto changed_native = observed->position + 1;
    if (!check(!reader.seek_relative(200'000) && reader.tell() == changed_native,
            "partially advanced failed seek invalidates retired buffer")) return false;
    observed->fail_seek = false;
    if (!check(read_matches(reader, bytes, changed_native, 13),
            "read after changed failed seek must use actual stream position")) return false;
    return true;
}

bool exercise_mount(const heaven::AssetFileSystem& assets, const Bytes& bytes) {
    auto opened = AssetBinaryReader::open("Data_Root/heaven/payload.bin", std::cref(assets), "test payload");
    if (!opened) return false;
    auto reader = std::move(*opened);
    if (reader.size() != bytes.size() || !read_matches(reader, bytes, 0, 17) ||
        !reader.seek_relative(300) || !read_matches(reader, bytes, 317, 31) ||
        !reader.seek_relative(-25) || !read_matches(reader, bytes, 323, 9) ||
        reader.seek_relative(-400) || reader.tell() != 332 ||
        !reader.seek_relative(80'000) || !read_matches(reader, bytes, 80'332, 100'000) ||
        !reader.seek_relative(-150'000) || !read_matches(reader, bytes, 30'332, 7)) return false;
    std::size_t position = 30'339;
    const std::array<std::size_t, 6> sizes{1, 3, 17, 4093, 65536, 70001};
    std::size_t iteration = 0;
    while (position < bytes.size()) {
        const auto count = std::min(sizes[iteration++ % sizes.size()], bytes.size() - position);
        if (!read_matches(reader, bytes, position, count)) return false;
        position += count;
    }
    std::byte byte{};
    return !reader.read_object(byte) && reader.tell() == bytes.size() && reader.read_bytes({});
}

// Damage both ZIP-header CRC fields without changing valid compressed payload
// bytes. Both STORE and DEFLATE therefore reach the real EOF/CRC check.
bool corrupt_payload_crc(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(input), {}};
    const std::string target = "Data_Root/heaven/payload.bin";
    bool local = false, central = false;
    const auto u16 = [&](std::size_t offset) {
        return static_cast<std::size_t>(static_cast<unsigned char>(raw[offset])) |
            (static_cast<std::size_t>(static_cast<unsigned char>(raw[offset + 1])) << 8);
    };
    for (std::size_t i = 0; i + 46 <= raw.size(); ++i) {
        if (raw[i] != 'P' || raw[i + 1] != 'K') continue;
        const bool is_local = raw[i + 2] == 3 && raw[i + 3] == 4;
        const bool is_central = raw[i + 2] == 1 && raw[i + 3] == 2;
        if (!is_local && !is_central) continue;
        const auto header = is_local ? std::size_t{30} : std::size_t{46};
        const auto name_size = u16(i + (is_local ? 26 : 28));
        if (i + header + name_size > raw.size() || name_size != target.size() ||
            std::string_view(raw.data() + i + header, name_size) != target) continue;
        raw[i + (is_local ? 14 : 16)] ^= 1;
        local |= is_local;
        central |= is_central;
    }
    if (!local || !central) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    return output.good();
}

bool mounted_stream_parity_and_crc() {
    namespace fs = std::filesystem;
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{fs::temp_directory_path() / ("heaven-binary-reader-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto data = cleanup.path / "Data_Root";
    fs::create_directories(data / "heaven");
    const auto bytes = payload(300'000);
    {
        std::ofstream output(data / "heaven/payload.bin", std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::ofstream(data / "heaven/heaven.world", std::ios::binary) << "world";
    }
    auto directory = heaven::AssetFileSystem::open(data.string());
    auto memory = heaven::AssetFileSystem::create_memory("Data_Root");
    auto memory_bytes = std::make_shared<heaven::AssetBuffer>(bytes.size());
    std::memcpy(memory_bytes->data(), bytes.data(), bytes.size());
    if (!check(directory && memory && memory->mount_memory("heaven/payload.bin", memory_bytes) &&
            exercise_mount(*directory, bytes) && exercise_mount(*memory, bytes),
            "directory and memory mount parity")) return false;

    const std::string manifest = R"({"schema_version":1,"data_root_prefix":"Data_Root"})";
    const std::string world = "world";
    for (const auto compression : {heaven::ZipCompression::Store, heaven::ZipCompression::Deflate}) {
        const auto zip_path = cleanup.path / (compression == heaven::ZipCompression::Store ? "stored.zip" : "deflated.zip");
        {
            auto writer = heaven::ZipArchiveWriter::create(zip_path, false);
            if (!check(writer && writer->add_bytes(std::string(heaven::kAssetArchiveManifest),
                    std::as_bytes(std::span(manifest)), heaven::ZipCompression::Store) &&
                    writer->add_bytes("Data_Root/heaven/heaven.world", std::as_bytes(std::span(world)),
                        heaven::ZipCompression::Store) &&
                    writer->add_bytes("Data_Root/heaven/payload.bin", bytes, compression) && writer->finish(),
                    "ZIP fixture creation")) return false;
        }
        {
            auto zip = heaven::AssetFileSystem::open(zip_path.string());
            if (!check(zip && exercise_mount(*zip, bytes), "stored/deflated mount parity")) return false;
        }
        if (!check(corrupt_payload_crc(zip_path) && !heaven::validate_zip_archive(zip_path, true),
                "CRC fixture must independently fail validation")) return false;
        auto corrupt = heaven::AssetFileSystem::open(zip_path.string());
        auto reader = corrupt ? AssetBinaryReader::open("Data_Root/heaven/payload.bin", std::cref(*corrupt), "corrupt") :
            heaven::Result<AssetBinaryReader>(std::unexpected(heaven::Error("mount failed")));
        if (!check(reader.has_value(), "corrupt payload retains valid ZIP directory")) return false;
        Bytes out(bytes.size());
        if (!check(!reader->read_bytes(out), "buffered reads must preserve final-byte CRC failure")) return false;
        auto scalar = AssetBinaryReader::open("Data_Root/heaven/payload.bin", std::cref(*corrupt), "corrupt");
        if (!check(scalar.has_value(), "reopen corrupt scalar stream")) return false;
        std::size_t successful = 0;
        for (; successful < bytes.size(); ++successful) {
            std::byte value{0xff};
            if (!scalar->read_object(value)) {
                if (!check(value == std::byte{0xff}, "failed CRC refill must not publish invalid bytes")) return false;
                break;
            }
            if (!check(value == bytes[successful], "corrupt stream prefix before deferred CRC")) return false;
        }
        if (!check(successful < bytes.size(), "scalar buffering must not hide final CRC failure")) return false;
    }
    return true;
}

bool malformed_meshes_remain_rejected() {
    auto memory = heaven::AssetFileSystem::create_memory("Data_Root");
    if (!memory) return false;
    auto bytes = std::make_shared<heaven::AssetBuffer>();
    const auto append = [&]<class T>(T value) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
        bytes->insert(bytes->end(), first, first + sizeof(T));
    };
    for (const char c : std::string_view("mi08")) append(c);
    for (int i = 0; i < 10; ++i) append(0.0f);
    append(int32_t{1});
    append(int32_t{5});
    for (const char c : std::string_view("quad\0", 5)) append(c);
    for (int i = 0; i < 10; ++i) append(0.0f);
    append(int32_t{3});
    for (int vertex = 0; vertex < 3; ++vertex) {
        append(static_cast<float>(vertex == 1));
        append(static_cast<float>(vertex == 2));
        append(0.0f);
        append(uint16_t{32768}); append(uint16_t{32768}); append(uint16_t{65535});
    }
    append(int32_t{3});
    for (int vertex = 0; vertex < 3; ++vertex) {
        append(static_cast<float>(vertex == 1)); append(static_cast<float>(vertex == 2));
    }
    append(int32_t{0}); append(int32_t{1});
    append(uint16_t{0}); append(uint16_t{1}); append(uint16_t{2});
    if (!check(memory->mount_memory("heaven/valid.mesh", bytes).has_value() &&
            heaven::load_mi08("Data_Root/heaven/valid.mesh", std::cref(*memory)).has_value(),
            "valid loader control")) return false;
    for (std::size_t length = 0; length < bytes->size(); ++length) {
        auto truncated = std::make_shared<heaven::AssetBuffer>(bytes->begin(),
            bytes->begin() + static_cast<std::ptrdiff_t>(length));
        const auto path = "Data_Root/heaven/truncated-" + std::to_string(length) + ".mesh";
        if (!check(memory->mount_memory(path, truncated).has_value() &&
                !heaven::load_mi08(path, std::cref(*memory)),
                "every truncated valid-mesh prefix must remain rejected")) return false;
    }
    auto bad_count = std::make_shared<heaven::AssetBuffer>(*bytes);
    const auto invalid = std::numeric_limits<int32_t>::max();
    std::memcpy(bad_count->data() + 4 + 10 * sizeof(float), &invalid, sizeof(invalid));
    return check(memory->mount_memory("heaven/bad-count.mesh", bad_count).has_value() &&
        !heaven::load_mi08("Data_Root/heaven/bad-count.mesh", std::cref(*memory)),
        "complete mesh with invalid count remains rejected");
}

bool unit_asset_binary_reader() {
    return scalar_reads_are_bounded() && short_reads_and_failures() &&
        seeks_and_logical_position() && mounted_stream_parity_and_crc() &&
        malformed_meshes_remain_rejected();
}
const heaven::testing::Register registered(unit_asset_binary_reader);
} // namespace
