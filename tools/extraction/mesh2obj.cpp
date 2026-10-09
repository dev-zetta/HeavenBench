/*
 * Unigine mi08 mesh to Wavefront OBJ converter.
 *
 * The file format is little-endian and consists of a mesh header, per-surface
 * headers, and tightly packed vertex/index streams. Parsing is intentionally
 * separated from output so malformed data cannot leave a partial OBJ behind.
 */

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Vec2 {
    float u{};
    float v{};
};

struct Triangle {
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
};

struct Surface {
    std::string name;
    std::vector<Vec3> vertices;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords0;
    std::vector<Vec2> texcoords1;
    std::vector<Triangle> triangles;
};

constexpr std::uint32_t kMi08Magic =
    static_cast<std::uint32_t>('m') |
    (static_cast<std::uint32_t>('i') << 8u) |
    (static_cast<std::uint32_t>('0') << 16u) |
    (static_cast<std::uint32_t>('8') << 24u);
constexpr std::size_t kBoundingVolumeBytes = 10u * sizeof(float);
constexpr std::int32_t kMaxSurfaceCount = 32'767;
constexpr std::int32_t kMaxElementCount = 10'000'000;
constexpr std::int32_t kMaxStringBytes = 1'024;
constexpr std::uint32_t kU16IndexLimit =
    static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) + 1u;

static_assert(kBoundingVolumeBytes == 40u);
static_assert(kMaxElementCount > kMaxSurfaceCount);

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename Value>
    requires std::is_trivially_copyable_v<Value>
    [[nodiscard]] std::expected<Value, std::string> read(std::string_view what) {
        if (remaining() < sizeof(Value)) {
            return std::unexpected("unexpected EOF reading " +
                                   std::string(what));
        }
        Value value{};
        std::memcpy(&value, bytes_.data() + cursor_, sizeof(value));
        cursor_ += sizeof(value);
        if constexpr (std::endian::native == std::endian::big) {
            value = byteswap(value);
        }
        return value;
    }

    [[nodiscard]] std::expected<void, std::string>
    skip(std::size_t count, std::string_view what) {
        if (remaining() < count) {
            return std::unexpected("unexpected EOF reading " +
                                   std::string(what));
        }
        cursor_ += count;
        return {};
    }

    [[nodiscard]] bool contains_elements(std::size_t count,
                                         std::size_t stride) const noexcept {
        return count <= remaining() / stride;
    }

    [[nodiscard]] std::expected<std::string, std::string> read_string() {
        const auto length_result = read<std::int32_t>("string length");
        if (!length_result) {
            return std::unexpected(length_result.error());
        }
        const std::int32_t length = *length_result;
        if (length <= 0 || length > kMaxStringBytes) {
            return std::unexpected("invalid string length " +
                                   std::to_string(length));
        }
        const auto byte_count = static_cast<std::size_t>(length);
        if (remaining() < byte_count) {
            return std::unexpected("unexpected EOF reading string");
        }
        const char* const text =
            reinterpret_cast<const char*>(bytes_.data() + cursor_);
        cursor_ += byte_count;
        if (text[length - 1] != '\0') {
            return std::unexpected("surface name is not null-terminated");
        }
        return std::string(text, byte_count - 1u);
    }

private:
    template <typename Value>
    [[nodiscard]] static Value byteswap(Value value) {
        auto representation = std::bit_cast<std::array<std::byte, sizeof(Value)>>(value);
        std::ranges::reverse(representation);
        return std::bit_cast<Value>(representation);
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - cursor_;
    }

    std::span<const std::byte> bytes_;
    std::size_t cursor_ = 0;
};

[[nodiscard]] std::expected<std::size_t, std::string>
read_count(BinaryReader& reader, std::string_view label, std::int32_t maximum) {
    const auto result = reader.read<std::int32_t>(label);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (*result < 0 || *result > maximum) {
        return std::unexpected("invalid " + std::string(label) + " " +
                               std::to_string(*result));
    }
    return static_cast<std::size_t>(*result);
}

[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected("cannot open '" + path.string() + "'");
    }
    const std::streampos end = input.tellg();
    if (end < 0) {
        return std::unexpected("cannot determine size of '" + path.string() + "'");
    }
    const auto size = static_cast<std::size_t>(end);
    if (size > static_cast<std::size_t>(
                   std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected("file is too large: '" + path.string() + "'");
    }
    std::vector<std::byte> bytes(size);
    input.seekg(0);
    if (size != 0u &&
        !input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(size))) {
        return std::unexpected("cannot read '" + path.string() + "'");
    }
    return bytes;
}

[[nodiscard]] std::expected<std::vector<Surface>, std::string>
parse_mesh(std::span<const std::byte> bytes) {
    BinaryReader reader(bytes);
    const auto magic = reader.read<std::uint32_t>("magic");
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (*magic != kMi08Magic) {
        std::ostringstream message;
        message << "not a mi08 mesh (magic: 0x" << std::hex << std::setw(8)
                << std::setfill('0') << *magic << ", expected: 0x"
                << std::setw(8) << kMi08Magic << ')';
        return std::unexpected(message.str());
    }
    if (auto skipped = reader.skip(kBoundingVolumeBytes, "mesh bounds");
        !skipped) {
        return std::unexpected(skipped.error());
    }

    const auto surface_count_result =
        read_count(reader, "surface count", kMaxSurfaceCount);
    if (!surface_count_result) {
        return std::unexpected(surface_count_result.error());
    }
    if (*surface_count_result == 0u) {
        return std::unexpected("invalid surface count 0");
    }
    if (!reader.contains_elements(*surface_count_result, 4u + 1u + kBoundingVolumeBytes)) {
        return std::unexpected("unexpected EOF reading surface headers");
    }

    std::vector<Surface> surfaces(*surface_count_result);
    for (Surface& surface : surfaces) {
        auto name = reader.read_string();
        if (!name) {
            return std::unexpected(name.error());
        }
        surface.name = std::move(*name);
        if (auto skipped =
                reader.skip(kBoundingVolumeBytes, "surface bounds");
            !skipped) {
            return std::unexpected(skipped.error());
        }
    }

    for (Surface& surface : surfaces) {
        const auto vertex_count =
            read_count(reader, "vertex count", kMaxElementCount);
        if (!vertex_count) {
            return std::unexpected(vertex_count.error());
        }
        if (!reader.contains_elements(*vertex_count, 18u)) {
            return std::unexpected("unexpected EOF reading vertices");
        }
        surface.vertices.resize(*vertex_count);
        surface.normals.resize(*vertex_count);

        for (std::size_t index = 0; index < *vertex_count; ++index) {
            auto x = reader.read<float>("vertex x");
            auto y = reader.read<float>("vertex y");
            auto z = reader.read<float>("vertex z");
            auto nx = reader.read<std::uint16_t>("normal x");
            auto ny = reader.read<std::uint16_t>("normal y");
            auto nz = reader.read<std::uint16_t>("normal z");
            if (!x || !y || !z || !nx || !ny || !nz) {
                return std::unexpected("unexpected EOF reading vertex");
            }
            surface.vertices[index] = {*x, *y, *z};
            constexpr float kNormalScale =
                2.0f / static_cast<float>(
                           std::numeric_limits<std::uint16_t>::max());
            surface.normals[index] = {
                static_cast<float>(*nx) * kNormalScale - 1.0f,
                static_cast<float>(*ny) * kNormalScale - 1.0f,
                static_cast<float>(*nz) * kNormalScale - 1.0f,
            };
        }

        const auto texcoord0_count =
            read_count(reader, "texcoord0 count", kMaxElementCount);
        if (!texcoord0_count) {
            return std::unexpected(texcoord0_count.error());
        }
        if (!reader.contains_elements(*texcoord0_count, 8u)) {
            return std::unexpected("unexpected EOF reading texcoord0");
        }
        surface.texcoords0.resize(*texcoord0_count);
        for (Vec2& texcoord : surface.texcoords0) {
            auto u = reader.read<float>("texcoord0 u");
            auto v = reader.read<float>("texcoord0 v");
            if (!u || !v) {
                return std::unexpected("unexpected EOF reading texcoord0");
            }
            texcoord = {*u, *v};
        }

        const auto texcoord1_count =
            read_count(reader, "texcoord1 count", kMaxElementCount);
        if (!texcoord1_count) {
            return std::unexpected(texcoord1_count.error());
        }
        if (!reader.contains_elements(*texcoord1_count, 8u)) {
            return std::unexpected("unexpected EOF reading texcoord1");
        }
        surface.texcoords1.resize(*texcoord1_count);
        for (Vec2& texcoord : surface.texcoords1) {
            auto u = reader.read<float>("texcoord1 u");
            auto v = reader.read<float>("texcoord1 v");
            if (!u || !v) {
                return std::unexpected("unexpected EOF reading texcoord1");
            }
            texcoord = {*u, *v};
        }

        const auto triangle_count =
            read_count(reader, "triangle count", kMaxElementCount);
        if (!triangle_count) {
            return std::unexpected(triangle_count.error());
        }
        const bool narrow_indices = *vertex_count < kU16IndexLimit;
        if (!reader.contains_elements(*triangle_count, narrow_indices ? 6u : 12u)) {
            return std::unexpected("unexpected EOF reading triangles");
        }
        surface.triangles.resize(*triangle_count);
        for (Triangle& triangle : surface.triangles) {
            if (narrow_indices) {
                auto a = reader.read<std::uint16_t>("triangle index");
                auto b = reader.read<std::uint16_t>("triangle index");
                auto c = reader.read<std::uint16_t>("triangle index");
                if (!a || !b || !c) {
                    return std::unexpected("unexpected EOF reading triangle");
                }
                triangle = {*a, *b, *c};
            } else {
                auto a = reader.read<std::uint32_t>("triangle index");
                auto b = reader.read<std::uint32_t>("triangle index");
                auto c = reader.read<std::uint32_t>("triangle index");
                if (!a || !b || !c) {
                    return std::unexpected("unexpected EOF reading triangle");
                }
                triangle = {*a, *b, *c};
            }
            if (triangle.a >= *vertex_count ||
                triangle.b >= *vertex_count ||
                triangle.c >= *vertex_count) {
                return std::unexpected("triangle index exceeds vertex count");
            }
            if (*texcoord0_count != 0u &&
                (triangle.a >= *texcoord0_count ||
                 triangle.b >= *texcoord0_count ||
                 triangle.c >= *texcoord0_count)) {
                return std::unexpected("triangle index exceeds texcoord0 count");
            }
        }
    }
    return surfaces;
}

[[nodiscard]] std::string sanitize_name(std::string_view name) {
    std::string result(name);
    std::ranges::replace_if(
        result,
        [](char c) {
            constexpr std::string_view invalid = " /\\:*?<>|";
            return invalid.contains(c);
        },
        '_');
    return result;
}

[[nodiscard]] std::expected<void, std::string>
write_mesh(const std::filesystem::path& input_path,
           const std::filesystem::path& obj_path,
           std::span<const Surface> surfaces) {
    const std::filesystem::path mtl_path =
        obj_path.parent_path() / (obj_path.stem().string() + ".mtl");

    std::ofstream mtl(mtl_path);
    if (!mtl) {
        return std::unexpected("cannot create '" + mtl_path.string() + "'");
    }
    mtl << "# Generated by mesh2obj from " << input_path.string() << '\n';
    for (const Surface& surface : surfaces) {
        const std::string material = sanitize_name(surface.name);
        mtl << "\nnewmtl " << material
            << "\nKa 0.2 0.2 0.2"
            << "\nKd 0.8 0.8 0.8"
            << "\nKs 0.0 0.0 0.0"
            << "\nd 1.0\n";
    }
    mtl.close();
    if (!mtl) {
        return std::unexpected("cannot write '" + mtl_path.string() + "'");
    }

    std::ofstream obj(obj_path);
    if (!obj) {
        return std::unexpected("cannot create '" + obj_path.string() + "'");
    }
    obj << "# Generated by mesh2obj from " << input_path.string()
        << "\n# Surfaces: " << surfaces.size()
        << "\nmtllib " << mtl_path.filename().string() << "\n\n"
        << std::fixed << std::setprecision(6);

    std::uint64_t vertex_offset = 0;
    std::uint64_t normal_offset = 0;
    std::uint64_t texcoord_offset = 0;
    for (const Surface& surface : surfaces) {
        const std::string material = sanitize_name(surface.name);
        obj << "o " << material << "\nusemtl " << material << '\n';
        for (const Vec3& vertex : surface.vertices) {
            obj << "v " << vertex.x << ' ' << vertex.y << ' ' << vertex.z << '\n';
        }
        for (const Vec3& normal : surface.normals) {
            obj << "vn " << normal.x << ' ' << normal.y << ' ' << normal.z << '\n';
        }
        for (const Vec2& texcoord : surface.texcoords0) {
            obj << "vt " << texcoord.u << ' ' << 1.0f - texcoord.v << '\n';
        }

        const bool has_uv = !surface.texcoords0.empty();
        for (const Triangle& triangle : surface.triangles) {
            const std::array indices = {triangle.a, triangle.b, triangle.c};
            obj << "f";
            for (const std::uint32_t index : indices) {
                const std::uint64_t vertex = vertex_offset + index + 1u;
                const std::uint64_t normal = normal_offset + index + 1u;
                if (has_uv) {
                    const std::uint64_t texcoord =
                        texcoord_offset + index + 1u;
                    obj << ' ' << vertex << '/' << texcoord << '/' << normal;
                } else {
                    obj << ' ' << vertex << "//" << normal;
                }
            }
            obj << '\n';
        }
        obj << '\n';
        vertex_offset += surface.vertices.size();
        normal_offset += surface.normals.size();
        texcoord_offset += surface.texcoords0.size();
    }
    obj.close();
    if (!obj) {
        return std::unexpected("cannot write '" + obj_path.string() + "'");
    }
    return {};
}

[[nodiscard]] std::filesystem::path
default_output_path(const std::filesystem::path& input) {
    std::filesystem::path output = input;
    output.replace_extension(".obj");
    return output;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::span raw_args(argv, static_cast<std::size_t>(argc));
    std::vector<std::string_view> args;
    args.reserve(raw_args.size());
    std::ranges::transform(raw_args, std::back_inserter(args),
                           [](const char* argument) {
                               return std::string_view(argument);
                           });

    std::cout << "Unigine mi08 mesh to OBJ converter\n"
                 "-----------------------------------\n";
    if (args.size() < 2u) {
        std::cerr
            << "Usage: " << args.front() << " <input.mesh> [output.obj]\n\n"
            << "If output is omitted, writes to <input_basename>.obj in the same directory.\n"
            << "Batch mode: " << args.front() << " *.mesh  (shell expands the glob)\n";
        return 1;
    }

    bool explicit_output = false;
    std::filesystem::path output_path;
    std::size_t last_input = args.size();
    if (args.size() == 3u &&
        !std::string_view(args[2]).ends_with(".mesh")) {
        explicit_output = true;
        output_path = std::string(args[2]);
        last_input = 2u;
    }

    std::size_t converted = 0;
    int exit_code = 0;
    for (const std::string_view input_arg :
         std::span(args).subspan(1u, last_input - 1u)) {
        const std::filesystem::path input_path{std::string(input_arg)};
        const auto bytes = read_file(input_path);
        if (!bytes) {
            std::cerr << "Error: " << bytes.error() << '\n';
            exit_code = 1;
            continue;
        }
        const auto surfaces = parse_mesh(*bytes);
        if (!surfaces) {
            std::cerr << "Error: " << surfaces.error() << " in '"
                      << input_path.string() << "'\n";
            exit_code = 1;
            continue;
        }

        const std::filesystem::path obj_path =
            explicit_output ? output_path : default_output_path(input_path);
        if (auto written = write_mesh(input_path, obj_path, *surfaces);
            !written) {
            std::cerr << "Error: " << written.error() << '\n';
            exit_code = 1;
            continue;
        }

        std::size_t total_vertices = 0;
        std::size_t total_triangles = 0;
        for (const Surface& surface : *surfaces) {
            total_vertices += surface.vertices.size();
            total_triangles += surface.triangles.size();
        }
        std::cout << "  " << input_path.string() << " -> "
                  << obj_path.string() << " (" << surfaces->size()
                  << " surfaces, " << total_vertices << " vertices, "
                  << total_triangles << " triangles)\n";
        ++converted;
    }

    std::cout << '\n' << converted << " mesh(es) converted.\n";
    return exit_code;
}
