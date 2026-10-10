/*
    Copyright 2012-2019 Luigi Auriemma

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

    http://www.gnu.org/licenses/gpl-2.0.txt
*/

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <zlib.h>

#define LZ4F_DISABLE_OBSOLETE_ENUMS
#include "lz4.h"

namespace {

using Byte = std::uint8_t;
struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file) {
            std::fclose(file);
        }
    }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

constexpr std::string_view kVersion = "0.3.1";
constexpr std::uint32_t kDefaultSeed = 0xa13cdbdeu;
constexpr std::size_t kFilenameCapacity = 0x401u;
constexpr std::size_t kKeyCapacity = 257u;

// Preserve the archive's full 32-bit field so unknown values remain
// distinguishable and produce an accurate unsupported-compression diagnostic.
enum class Compression : std::uint32_t { // NOLINT(performance-enum-size)
    None = 0,
    Lz4 = 1,
    Zlib = 2,
};

struct KnownKey {
    std::string_view name;
    std::string_view key;
};

constexpr std::array kKnownKeys = {
    KnownKey{"Cradle", "fS5g_2gDc*"},
    KnownKey{"Sumoman", "1EuCM90tADY6uxkJHXyCGj0PcJ1EWzvL"},
    KnownKey{"Superposition", "ieNgeivaep9eima7ze8m"},
    KnownKey{"Oil Rush", "CF29hity73GT640LP1dlfu55gzxyeuas"},
};
static_assert(!kKnownKeys.empty());

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

[[noreturn]] void fail_system(std::string_view action) {
    throw std::system_error(errno, std::generic_category(), std::string(action));
}

[[nodiscard]] File open_file(const std::filesystem::path& path,
                             const char* mode) {
    File file(std::fopen(path.string().c_str(), mode));
    if (!file) {
        fail_system("cannot open " + path.string());
    }
    return file;
}

void read_exact(std::FILE& file, std::span<Byte> output) {
    const std::size_t read =
        std::fread(output.data(), 1u, output.size(), &file);
    if (read != output.size()) {
        fail("incomplete input file, can't read " +
             std::to_string(output.size() - read) + " bytes");
    }
}

[[nodiscard]] std::uint64_t tell(std::FILE& file) {
    const long position = std::ftell(&file);
    if (position < 0) {
        fail_system("cannot determine archive position");
    }
    return static_cast<std::uint64_t>(position);
}

void seek(std::FILE& file, std::int64_t offset, int origin) {
    if (offset < std::numeric_limits<long>::min() ||
        offset > std::numeric_limits<long>::max()) {
        fail("archive offset is outside the platform file range");
    }
    if (std::fseek(&file, static_cast<long>(offset), origin) != 0) {
        fail_system("cannot seek archive");
    }
}

[[nodiscard]] constexpr std::uint64_t
little_endian_value(std::span<const Byte> bytes) {
    std::uint64_t value = 0;
    const std::size_t count = std::min(bytes.size(), sizeof(value));
    for (std::size_t index = 0; index < count; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8u);
    }
    return value;
}

[[nodiscard]] bool wildcard_matches(std::string_view filename,
                                    std::string_view wildcard) {
    std::size_t file_index = 0;
    std::size_t pattern_index = 0;
    std::size_t star_pattern = std::string_view::npos;
    std::size_t star_file = 0;

    const auto equal_character = [](char lhs, char rhs) {
        if ((lhs == '/' || lhs == '\\') && (rhs == '/' || rhs == '\\')) {
            return true;
        }
        return std::tolower(static_cast<unsigned char>(lhs)) ==
               std::tolower(static_cast<unsigned char>(rhs));
    };

    while (file_index < filename.size()) {
        if (pattern_index < wildcard.size() &&
            (wildcard[pattern_index] == '?' ||
             equal_character(filename[file_index], wildcard[pattern_index]))) {
            ++file_index;
            ++pattern_index;
        } else if (pattern_index < wildcard.size() &&
                   wildcard[pattern_index] == '*') {
            star_pattern = ++pattern_index;
            star_file = file_index;
        } else if (star_pattern != std::string_view::npos) {
            pattern_index = star_pattern;
            file_index = ++star_file;
        } else {
            return false;
        }
    }
    while (pattern_index < wildcard.size() &&
           wildcard[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == wildcard.size();
}

[[nodiscard]] std::expected<std::uint32_t, std::string>
parse_seed(std::string_view token) {
    int base = 10;
    if (token.starts_with("0x") || token.starts_with("0X")) {
        base = 16;
        token.remove_prefix(2u);
    }
    if (token.empty()) {
        return std::unexpected("empty seed");
    }
    std::uint32_t value = 0;
    const auto [next, error] =
        std::from_chars(token.data(), token.data() + token.size(), value, base);
    if (error == std::errc::result_out_of_range) {
        return std::unexpected("seed is out of range");
    }
    if (error != std::errc{} || next != token.data() + token.size()) {
        return std::unexpected("invalid seed");
    }
    return value;
}

[[nodiscard]] constexpr std::uint8_t decode_base64_character(Byte character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<std::uint8_t>(character - 'A');
    }
    if (character >= 'a' && character <= 'z') {
        return static_cast<std::uint8_t>(character - 'a' + 26u);
    }
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0' + 52u);
    }
    if (character == '+' || character == '-' || character == '[') {
        return 62u;
    }
    if (character == '/' || character == ']' || character == '_') {
        return 63u;
    }
    return 0u;
}

[[nodiscard]] std::expected<std::vector<Byte>, std::string>
decode_base64(std::string_view input) {
    std::vector<Byte> output;
    output.reserve((input.size() / 4u) * 3u + 1u);
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::size_t step = 0;
    for (const unsigned char character : input) {
        if (character == '=' || character == '_') {
            break;
        }
        if (character <= ' ' || character > 0x7fu) {
            continue;
        }
        switch (step & 3u) {
            case 0:
                a = decode_base64_character(character);
                break;
            case 1:
                b = decode_base64_character(character);
                output.push_back(static_cast<Byte>((a << 2u) | (b >> 4u)));
                break;
            case 2:
                a = decode_base64_character(character);
                output.push_back(
                    static_cast<Byte>(((b & 15u) << 4u) | (a >> 2u)));
                break;
            case 3:
                output.push_back(static_cast<Byte>(
                    ((a & 3u) << 6u) |
                    decode_base64_character(character)));
                break;
            default:
                std::unreachable();
        }
        ++step;
    }
    return output;
}

[[nodiscard]] std::filesystem::path
safe_relative_archive_path(std::string_view authored_name) {
    std::string cleaned(authored_name);
    if (cleaned.size() >= 2u && cleaned[1] == ':') {
        cleaned.erase(0u, 2u);
    }
    constexpr std::string_view invalid = "?%*:|\"<>";
    for (char& character : cleaned) {
        if (character == '\\') {
            character = '/';
        } else if (invalid.contains(character) ||
                   character == '\n' || character == '\r') {
            character = '_';
        }
    }
    while (!cleaned.empty() &&
           (cleaned.back() == ' ' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }

    std::filesystem::path result;
    for (const auto& component : std::filesystem::path(cleaned)) {
        std::string part = component.string();
        if (part.empty() || part == "." || part == ".." || part == "/") {
            part = "__";
        }
        for (char& character : part) {
            if (character == ':' || character == '\t' ||
                character == ' ') {
                character = '_';
            }
        }
        result /= part;
    }
    if (result.empty()) {
        return "__";
    }
    return result.relative_path();
}

class Extractor {
public:
    int run(std::span<const std::string_view> args) {
        print_banner();
        if (args.size() < 2u) {
            print_usage(args.empty() ? "uniginex" : args.front());
            return 1;
        }

        bool encrypted = true;
        std::size_t index = 1u;
        for (; index < args.size(); ++index) {
            const std::string_view argument = args[index];
            if (argument.size() != 2u ||
                (argument.front() != '-' && argument.front() != '/')) {
                break;
            }
            const auto next_value = [&]() -> std::string_view {
                if (++index >= args.size()) {
                    fail("option " + std::string(argument) +
                         " requires a value");
                }
                return args[index];
            };
            switch (argument[1]) {
                case 'l': list_only_ = true; break;
                case 'f': filter_ = std::string(next_value()); break;
                case 'o': force_overwrite_ = true; break;
                case 'k': set_key(next_value()); break;
                case 'K': key_file_ = std::string(next_value()); break;
                case 's': {
                    const auto parsed = parse_seed(next_value());
                    if (!parsed) {
                        fail(parsed.error());
                    }
                    seed_ = *parsed;
                    break;
                }
                case 'v': verbose_ = true; break;
                case 'd': encrypted = false; break;
                default:
                    fail("wrong argument (" + std::string(argument) + ')');
            }
        }

        if (index >= args.size() || args[index].starts_with('-')) {
            fail("you must specify also the output folder, use . for current one");
        }
        const std::filesystem::path archive_path{std::string(args[index++])};
        if (index < args.size()) {
            output_directory_ = std::string(args[index]);
        }

        std::printf("- open file %s\n", archive_path.string().c_str());
        File archive = open_file(archive_path, "rb");
        if (!list_only_) {
            std::printf("- set output folder %s\n",
                        output_directory_.string().c_str());
            std::error_code error;
            std::filesystem::create_directories(output_directory_, error);
            if (error) {
                fail("cannot create output directory '" +
                     output_directory_.string() + "': " + error.message());
            }
            output_directory_ = std::filesystem::canonical(output_directory_);
        }

        const std::uint32_t extracted = extract(*archive, encrypted);
        std::printf("\n- %u files extracted\n- done\n", extracted);
        return 0;
    }

private:
    void print_banner() const {
        std::printf(
            "\nUnigine ung files extractor %.*s\n"
            "by Luigi Auriemma\n"
            "e-mail: aluigi@autistici.org\n"
            "web:    aluigi.org\n\n",
            static_cast<int>(kVersion.size()), kVersion.data());
    }

    void print_usage(std::string_view executable) const {
        std::printf(
            "\nUsage: %.*s [options] <file.UNG> [output_folder]\n\n"
            "Options:\n"
            "-l      list the files without extracting them, you can use . as output folder\n"
            "-f W    filter the files to extract using the W wildcard, example -f \"*.oga\"\n"
            "-o      overwrite existing output files without prompting\n"
            "-k K    specify a custom key, default is %s\n"
            "-K FILE scan a text file looking for the custom key\n"
            "-s K    specify a custom seed, default is 0x%08x\n"
            "-v      verbose debug information\n"
            "-d      disable encryption in file data\n\n"
            "The tool automatically tries the known keys of the following games:\n",
            static_cast<int>(executable.size()), executable.data(),
            key_.empty() ? "(none)" : key_.c_str(), seed_);
        for (const KnownKey& known : kKnownKeys) {
            std::printf("  %.*s\n", static_cast<int>(known.name.size()),
                        known.name.data());
        }
        std::putchar('\n');
    }

    void set_key(std::string_view key) {
        if (key.empty()) {
            return;
        }
        key_ = key;
        std::printf("- set key: %s\n", key_.c_str());
    }

    void set_encoded_key(std::string_view encoded) {
        const auto decoded = decode_base64(encoded);
        if (!decoded) {
            return;
        }
        std::array<Byte, kKeyCapacity> inflated{};
        uLongf inflated_size = inflated.size() - 1u;
        std::string candidate;
        if (::uncompress(inflated.data(), &inflated_size, decoded->data(),
                         decoded->size()) == Z_OK) {
            candidate.assign(reinterpret_cast<const char*>(inflated.data()),
                             static_cast<std::size_t>(inflated_size));
        } else {
            candidate.assign(reinterpret_cast<const char*>(decoded->data()),
                             decoded->size());
        }
        const auto terminator = candidate.find('\0');
        candidate.resize(std::min(candidate.size(), terminator));
        if (std::ranges::any_of(candidate, [](unsigned char character) {
                return character < ' ';
            })) {
            return;
        }
        set_key(candidate);
    }

    std::uint32_t next_seed(bool scramble) {
        // The cipher wraps its multiply to 32 bits BEFORE the modulus. A
        // widened product generates a different stream and rejects real UNGs.
        seed_ *= 0x6b982u;
        seed_ %= 0x7fffffabu;
        if (scramble && !key_.empty()) {
            const std::size_t key_index =
                (seed_ >> 0xdu) % key_.size();
            const auto key_byte =
                static_cast<std::int8_t>(key_[key_index]);
            if (archive_version_ <= 3) {
                seed_ += static_cast<std::uint32_t>(
                    (key_byte +
                     static_cast<std::int64_t>(key_.size() << 8u)) *
                    8);
            } else {
                seed_ += static_cast<std::uint32_t>(
                    (key_.size() << 11u) +
                    static_cast<std::int64_t>(key_byte) * 8);
            }
        }
        return seed_;
    }

    static void rotate_seed_byte(Byte& value, Byte amount, bool left) {
        const int shift = static_cast<int>(amount & 7u);
        value = left ? std::rotl(value, shift) : std::rotr(value, shift);
    }

    [[nodiscard]] std::uint64_t read_plain_integer(std::FILE& file,
                                                   int bits) const {
        const std::size_t count = static_cast<std::size_t>(bits / 8);
        std::array<Byte, 8> bytes{};
        const std::uint64_t old_offset = tell(file);
        read_exact(file, std::span(bytes).first(count));
        const std::uint64_t value =
            little_endian_value(std::span(bytes).first(count));
        if (verbose_) {
            std::printf("- fget%-2d %010llx: %016llx\n", bits,
                        static_cast<unsigned long long>(old_offset),
                        static_cast<unsigned long long>(value));
        }
        return value;
    }

    [[nodiscard]] std::uint32_t read_encrypted32(std::FILE& file) {
        return read_encrypted32_impl(file);
    }

    void skip_encrypted32(std::FILE& file) {
        read_encrypted32_impl(file);
    }

    std::uint32_t read_encrypted32_impl(std::FILE& file) {
        std::array<Byte, 4> bytes{};
        const std::uint64_t old_offset = tell(file);
        read_exact(file, std::span<Byte>(&bytes[3], 1u));
        read_exact(file, std::span<Byte>(&bytes[2], 1u));
        read_exact(file, std::span<Byte>(bytes.data(), 1u));
        read_exact(file, std::span<Byte>(&bytes[1], 1u));

        rotate_seed_byte(bytes[1], static_cast<Byte>(seed_), false); next_seed(true);
        rotate_seed_byte(bytes[3], static_cast<Byte>(seed_), true);  next_seed(true);
        rotate_seed_byte(bytes[2], static_cast<Byte>(seed_), false); next_seed(true);
        rotate_seed_byte(bytes[0], static_cast<Byte>(seed_), true);  next_seed(true);
        bytes[3] ^= static_cast<Byte>(seed_); next_seed(true);
        bytes[0] ^= static_cast<Byte>(seed_); next_seed(true);
        bytes[2] ^= static_cast<Byte>(seed_); next_seed(true);
        bytes[1] ^= static_cast<Byte>(seed_); next_seed(true);

        const auto value = static_cast<std::uint32_t>(
            little_endian_value(bytes));
        if (verbose_) {
            std::printf("- get32  %010llx: %016llx\n",
                        static_cast<unsigned long long>(old_offset),
                        static_cast<unsigned long long>(value));
        }
        return value;
    }

    [[nodiscard]] std::uint64_t read_encrypted64(std::FILE& file) {
        std::array<Byte, 8> bytes{};
        const std::uint64_t old_offset = tell(file);
        constexpr std::array order = {7u, 6u, 4u, 5u, 3u, 2u, 0u, 1u};
        constexpr std::array xor_order = {7u, 6u, 4u, 5u, 3u, 0u, 2u, 1u};
        for (const std::size_t index : order) {
            read_exact(file, std::span<Byte>(&bytes[index], 1u));
        }
        constexpr std::array rotates = {
            std::pair{5u, false}, std::pair{7u, true},
            std::pair{4u, false}, std::pair{6u, true},
            std::pair{1u, false}, std::pair{3u, true},
            std::pair{2u, false}, std::pair{0u, true},
        };
        for (const auto& [index, left] : rotates) {
            rotate_seed_byte(bytes[index], static_cast<Byte>(seed_), left);
            next_seed(true);
        }
        for (const std::size_t index : xor_order) {
            bytes[index] ^= static_cast<Byte>(seed_);
            next_seed(true);
        }
        const std::uint64_t value = little_endian_value(bytes);
        if (verbose_) {
            std::printf("- get64  %010llx: %016llx\n",
                        static_cast<unsigned long long>(old_offset),
                        static_cast<unsigned long long>(value));
        }
        return value;
    }

    [[nodiscard]] std::string read_encrypted_string(std::FILE& file) {
        std::string result;
        result.reserve(kFilenameCapacity - 1u);
        Byte previous = 0;
        const std::uint64_t old_offset = tell(file);
        for (std::size_t index = 0;; ++index) {
            Byte character = 0;
            read_exact(file, std::span<Byte>(&character, 1u));
            rotate_seed_byte(
                character, static_cast<Byte>(seed_ + previous),
                ((index + previous) & 1u) != 0u);
            character ^= static_cast<Byte>(next_seed(false) + previous);
            next_seed(false);
            previous = character;
            if (character == 0u) {
                break;
            }
            if (result.size() + 1u < kFilenameCapacity) {
                result.push_back(static_cast<char>(character));
            }
        }
        if (verbose_) {
            std::printf("- getss  %08llx: %s\n",
                        static_cast<unsigned long long>(old_offset),
                        result.c_str());
        }
        return result;
    }

    void read_encrypted_data(std::FILE& file, std::span<Byte> data,
                             bool encrypted) {
        read_exact(file, data);
        if (!encrypted) {
            return;
        }
        Byte previous = 0;
        for (std::size_t index = 0; index < data.size(); ++index) {
            Byte character = data[index];
            const bool rotate_left = ((index ^ previous) & 1u) != 0u;
            rotate_seed_byte(
                character,
                static_cast<Byte>(rotate_left ? seed_ + previous
                                              : seed_ - previous),
                rotate_left);
            character ^= static_cast<Byte>(next_seed(false) - previous);
            next_seed(false);
            previous = character;
            data[index] = character;
        }
    }

    [[nodiscard]] std::string next_key_from_file() {
        if (!key_stream_.is_open()) {
            key_stream_.open(key_file_);
            if (!key_stream_) {
                fail("cannot open key list file " + key_file_.string());
            }
        }
        std::string key;
        if (!std::getline(key_stream_, key)) {
            fail("no key found in the provided list file " +
                 key_file_.string());
        }
        while (!key.empty() &&
               static_cast<unsigned char>(key.back()) <= ' ') {
            key.pop_back();
        }
        return key;
    }

    [[nodiscard]] bool should_overwrite(
        const std::filesystem::path& path) {
        if (force_overwrite_ || !std::filesystem::exists(path)) {
            return true;
        }
        std::printf(
            "- the file \"%s\" already exists\n"
            "  do you want to overwrite it (y/N/all)? ",
            path.string().c_str());
        std::array<char, 16> answer{};
        if (!std::fgets(answer.data(), static_cast<int>(answer.size()), stdin)) {
            return false;
        }
        const int first =
            std::tolower(static_cast<unsigned char>(answer.front()));
        if (first == 'a') {
            force_overwrite_ = true;
            return true;
        }
        return first == 'y';
    }

    void write_output(std::string_view authored_name,
                      std::span<const Byte> data) {
        const std::filesystem::path relative =
            safe_relative_archive_path(authored_name);
        const std::filesystem::path path = output_directory_ / relative;

        // Sanitizing '..' does not prevent an existing symlink from escaping
        // the output tree. Check each component before creating directories or
        // opening the file, including dangling links and special-file targets.
        std::filesystem::path checked = output_directory_;
        for (const auto& component : relative) {
            checked /= component;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(checked, error);
            if (error && error != std::errc::no_such_file_or_directory) {
                fail("cannot inspect output path '" + checked.string() +
                     "': " + error.message());
            }
            if (std::filesystem::is_symlink(status)) {
                fail("refusing output symlink '" + checked.string() + "'");
            }
            if (checked != path) {
                if (!std::filesystem::exists(status)) {
                    std::filesystem::create_directory(checked);
                } else if (!std::filesystem::is_directory(status)) {
                    fail("output parent is not a directory: '" + checked.string() + "'");
                }
            } else if (std::filesystem::exists(status) &&
                       !std::filesystem::is_regular_file(status)) {
                fail("output is not a regular file: '" + checked.string() + "'");
            }
        }
        if (!should_overwrite(path)) {
            return;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            fail("cannot create '" + path.string() + "'");
        }
        if (data.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max())) {
            fail("output is too large: '" + path.string() + "'");
        }
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        output.close();
        if (!output) {
            fail("impossible to write " + std::to_string(data.size()) +
                 " bytes");
        }
    }

    [[nodiscard]] std::uint32_t extract(std::FILE& file, bool encrypted) {
        seek(file, 0, SEEK_END);
        std::uint64_t maximum_offset = tell(file);
        std::uint64_t data_end = maximum_offset;
        seek(file, 0, SEEK_SET);

        std::array<Byte, 4> signature{};
        read_exact(file, signature);
        if (std::memcmp(signature.data(), "ar", 2u) != 0) {
            fail("invalid signature, not an UNG archive");
        }
        if (!std::isdigit(signature[2]) || !std::isdigit(signature[3])) {
            fail("invalid archive version");
        }
        archive_version_ =
            (signature[2] - static_cast<Byte>('0')) * 10 +
            signature[3] - static_cast<Byte>('0');

        Compression compression = Compression::Zlib;
        if (archive_version_ == 4) {
            if (maximum_offset < 16u) {
                fail("incomplete version 4 archive header/footer");
            }
            compression =
                static_cast<Compression>(read_plain_integer(file, 32));
            std::printf("- compression %u\n",
                        static_cast<unsigned>(compression));
            seek(file, -8, SEEK_END);
            maximum_offset = tell(file);
            const std::uint64_t index_offset =
                read_plain_integer(file, 64);
            if (index_offset < 8u || index_offset > maximum_offset) {
                fail("archive index offset is outside the data region");
            }
            data_end = index_offset;
            seek(file, static_cast<std::int64_t>(index_offset), SEEK_SET);
        } else if (archive_version_ != 3) {
            fail("archive version (" + std::to_string(archive_version_) +
                 ") is not supported yet");
        }

        std::printf(
            "\n  offset     filesize   filename\n"
            "--------------------------------\n");

        std::uint32_t info_seed = seed_;
        std::uint32_t data_seed = seed_;
        std::uint32_t extracted_files = 0;
        std::size_t file_count = 0;
        std::size_t known_key_index = 0;
        int key_mode = 0;
        std::vector<Byte> compressed;
        std::vector<Byte> uncompressed;

        while (tell(file) < maximum_offset) {
            const std::uint64_t entry_offset = tell(file);
            seed_ = info_seed;
            const std::uint32_t name_crc = read_encrypted32(file);
            const std::string filename = read_encrypted_string(file);
            const auto actual_crc = static_cast<std::uint32_t>(
                ::crc32(0u, reinterpret_cast<const Byte*>(filename.data()),
                        static_cast<uInt>(filename.size())));
            if (name_crc != actual_crc) {
                if (file_count != 0u) {
                    fail("the crc of a filename doesn't match the one in the file");
                }
                if (!key_.empty()) {
                    ++key_mode;
                }
                if ((key_mode & 1) != 0) {
                    set_encoded_key(key_);
                } else {
                    key_mode = 0;
                    if (!key_file_.empty()) {
                        set_key(next_key_from_file());
                    } else if (known_key_index < kKnownKeys.size()) {
                        set_key(kKnownKeys[known_key_index++].key);
                    } else {
                        fail("the filename CRC does not match; provide the correct key with -k");
                    }
                }
                seek(file, static_cast<std::int64_t>(entry_offset), SEEK_SET);
                continue;
            }

            const std::uint32_t size = read_encrypted32(file);
            info_seed = seed_;
            std::uint32_t compressed_size = read_encrypted32(file);
            std::uint64_t data_offset = 0;
            std::uint64_t next_offset = 0;
            if (archive_version_ <= 3) {
                std::uint32_t block_size = read_encrypted32(file);
                if (block_size < compressed_size) {
                    std::swap(block_size, compressed_size);
                }
                skip_encrypted32(file);
                data_offset = tell(file);
                if (data_offset > data_end || block_size > data_end - data_offset) {
                    fail("archive data block extends beyond the input file");
                }
                next_offset = data_offset + block_size;
                data_seed = seed_;
            } else {
                data_offset = read_encrypted64(file);
                next_offset = tell(file);
                info_seed = seed_;
                const std::uint64_t header_size = encrypted ? 12u : 8u;
                if (next_offset > maximum_offset || data_offset < 8u ||
                    data_offset > data_end || header_size > data_end - data_offset) {
                    fail("archive data/index offset is outside the input file");
                }
            }

            const bool selected =
                filter_.empty() || wildcard_matches(filename, filter_);
            if (selected) {
                std::printf("  %010llx %-10u %s\n",
                            static_cast<unsigned long long>(data_offset), size,
                            filename.c_str());
                if (!list_only_) {
                    seek(file, static_cast<std::int64_t>(data_offset), SEEK_SET);
                    seed_ = data_seed;
                    if (archive_version_ > 3) {
                        compressed_size = read_encrypted32(file);
                        skip_encrypted32(file);
                        if (encrypted) {
                            skip_encrypted32(file);
                        }
                    }
                    const std::uint64_t payload_offset = tell(file);
                    if (payload_offset > data_end ||
                        compressed_size > data_end - payload_offset) {
                        fail("archive data block extends beyond the data region");
                    }
                    compressed.resize(compressed_size);
                    read_encrypted_data(file, compressed, encrypted);

                    if (compression == Compression::None) {
                        if (compressed_size != size) {
                            fail("stored data size does not match the index");
                        }
                        write_output(filename, compressed);
                    } else {
                        uncompressed.resize(size);
                        std::uint32_t produced = 0;
                        if (compression == Compression::Lz4) {
                            if (compressed.size() >
                                    static_cast<std::size_t>(INT_MAX) ||
                                uncompressed.size() >
                                    static_cast<std::size_t>(INT_MAX)) {
                                fail("LZ4 block exceeds supported size");
                            }
                            const int decoded = LZ4_decompress_safe_partial(
                                reinterpret_cast<const char*>(compressed.data()),
                                reinterpret_cast<char*>(uncompressed.data()),
                                static_cast<int>(compressed.size()),
                                static_cast<int>(uncompressed.size()),
                                static_cast<int>(uncompressed.size()));
                            if (decoded >= 0) {
                                produced = static_cast<std::uint32_t>(decoded);
                            } else {
                                produced = std::numeric_limits<std::uint32_t>::max();
                            }
                        } else if (compression == Compression::Zlib) {
                            uLongf output_size = uncompressed.size();
                            if (::uncompress(uncompressed.data(), &output_size,
                                             compressed.data(),
                                             compressed.size()) == Z_OK &&
                                output_size <=
                                    std::numeric_limits<std::uint32_t>::max()) {
                                produced =
                                    static_cast<std::uint32_t>(output_size);
                            } else {
                                produced =
                                    std::numeric_limits<std::uint32_t>::max();
                            }
                        } else {
                            fail("compression " +
                                 std::to_string(
                                     static_cast<std::uint32_t>(compression)) +
                                 " is not supported yet");
                        }
                        if (produced != size) {
                            fail("decompression failed: expected " +
                                 std::to_string(size) + " bytes, got " +
                                 std::to_string(produced));
                        }
                        write_output(filename, uncompressed);
                    }
                }
                ++extracted_files;
            }
            // Version 4 assigns a data seed to every index entry, including
            // entries skipped by -f. Filtering must not change decryption.
            if (archive_version_ > 3) {
                seed_ = data_seed;
                data_seed = next_seed(true);
            }
            ++file_count;
            seek(file, static_cast<std::int64_t>(next_offset), SEEK_SET);
        }
        return extracted_files;
    }

    std::string key_;
    std::string filter_;
    std::filesystem::path key_file_;
    std::ifstream key_stream_;
    std::filesystem::path output_directory_ = ".";
    std::uint32_t seed_ = kDefaultSeed;
    int archive_version_ = 0;
    bool list_only_ = false;
    bool force_overwrite_ = false;
    bool verbose_ = false;
};

} // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span raw_args(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> args;
        args.reserve(raw_args.size());
        std::ranges::transform(raw_args, std::back_inserter(args),
                               [](const char* argument) {
                                   return std::string_view(argument);
                               });
        Extractor extractor;
        return extractor.run(args);
    } catch (const std::system_error& error) {
        std::fprintf(stderr, "\nError: %s\n", error.what());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "\nError: %s\n", error.what());
    }
    return 1;
}
