#include "height_sample_batch_fixture.h"
#include "rhi/height_sample_batch.h"
#include "engine/closed_displacement_seams.h"
#include "engine/zip_archive.h"
#include "rhi/mesh_displacement.h"
#include "rhi/texture_store.h"

#include <filament/Engine.h>
#include <filament/Texture.h>
#include <backend/PixelBufferDescriptor.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
struct FixtureTexture {
    explicit FixtureTexture(filament::Engine& value) : engine(value) {}
    ~FixtureTexture() { if (texture) engine.destroy(texture); }
    filament::Engine& engine;
    filament::Texture* texture = nullptr;

    bool upload(std::uint32_t width, std::uint32_t height,
                std::vector<std::vector<std::uint8_t>> red, bool rgba) {
        texture = filament::Texture::Builder().width(width).height(height)
            .levels(static_cast<std::uint8_t>(red.size())).sampler(filament::Texture::Sampler::SAMPLER_2D)
            .format(rgba ? filament::Texture::InternalFormat::RGBA8 : filament::Texture::InternalFormat::R8).build(engine);
        if (!texture) return false;
        for (std::uint32_t level = 0; level < red.size(); ++level) {
            const auto size = texture->getWidth(level) * texture->getHeight(level);
            if (red[level].size() != size) return false;
            auto* pixels = new std::vector<std::uint8_t>;
            if (rgba) {
                pixels->reserve(size * 4);
                for (auto value : red[level]) pixels->insert(pixels->end(), {value, 231, 53, 255});
            } else *pixels = std::move(red[level]);
            texture->setImage(engine, level, filament::backend::PixelBufferDescriptor(
                pixels->data(), pixels->size(),
                rgba ? filament::backend::PixelDataFormat::RGBA : filament::backend::PixelDataFormat::R,
                filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) { delete static_cast<std::vector<std::uint8_t>*>(user); }, pixels));
        }
        return true;
    }
};

Result<void> check(HeightSampleBatch& sampler, filament::Texture* texture,
                   std::span<const HeightSampleQuery> queries, std::array<float, 4> transform,
                   std::span<const float> heights, std::span<const float> lods) {
    auto result = sampler.sample(texture, queries, transform);
    if (!result) return std::unexpected(result.error());
    if (result->size() != queries.size() || heights.size() != queries.size() || lods.size() != queries.size())
        return std::unexpected(Error{"Height fixture result count differs"});
    for (std::size_t i = 0; i < queries.size(); ++i) {
        // Small uncompressed maps use a conservative two-UNORM8-step scalar
        // allowance. Wrong selected texture/mip/UV/channel yields much larger
        // differences. This does not assert a compressed BC4 decoder profile.
        if ((*result)[i].id != queries[i].id || std::abs((*result)[i].height - heights[i]) > 2.0f / 255.0f ||
            std::abs((*result)[i].mesh_lod - lods[i]) > 2e-5f || (*result)[i].selected_lod != (*result)[i].mesh_lod)
            return std::unexpected(Error{"Height fixture independent scalar/LOD/ID oracle failed at query " + std::to_string(i)});
    }
    return {};
}
// Qualify the declared CPU envelope against the real selected native texture,
// without consulting proprietary assets or changing any sampler shader route.
Result<void> check_closed_seam_height_envelopes(filament::Engine& engine, HeightSampleBatch& sampler) {
    const auto pack = std::filesystem::temp_directory_path() /
        ("heaven-native-height-envelope-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zip");
    struct RemovePack {
        std::filesystem::path path;
        ~RemovePack() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{pack};
    const auto dds = [](std::uint8_t high, std::uint8_t low) {
        std::vector<std::uint8_t> bytes(128);
        const auto put = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned j = 0; j < 4; ++j) bytes[offset + j] = static_cast<std::uint8_t>(value >> (8u * j));
        };
        put(0, 0x20534444); put(4, 124); put(12, 8); put(16, 8); put(28, 3);
        put(76, 32); put(80, 4); put(84, 0x31495441); // BC4, partial 8->4->2 chain
        std::uint64_t selectors = 0;
        for (unsigned j = 0; j < 16; ++j) selectors |= std::uint64_t(j % 8) << (3u * j);
        for (unsigned block = 0; block < 6; ++block) {
            bytes.insert(bytes.end(), {high, low});
            for (unsigned j = 0; j < 6; ++j) bytes.push_back(static_cast<std::uint8_t>(selectors >> (8u * j)));
        }
        return bytes;
    };
    const auto original = dds(160, 96), variant = dds(180, 74);
    std::array<std::vector<std::uint8_t>, 8> constants;
    for (std::size_t selector = 0; selector < constants.size(); ++selector) {
        constants[selector] = dds(255, 0);
        std::uint64_t repeated = 0;
        for (unsigned j = 0; j < 16; ++j) repeated |= static_cast<std::uint64_t>(selector) << (3u * j);
        for (std::size_t block = 128; block < constants[selector].size(); block += 8)
            for (unsigned j = 0; j < 6; ++j)
                constants[selector][block + 2 + j] = static_cast<std::uint8_t>(repeated >> (8u * j));
    }
    const std::string hash(64, '0'), world = "fixture";
    const std::string manifest =
        R"({"schema_version":2,"data_root_prefix":"Data_Root","texture_sets":{"estimated_gpu_bytes":{"original":48,"2k":16,"4k":48},"textures":{"Data_Root/heaven/h.dds":{"source_sha256":")" +
        hash + R"(","variant":{"path":"Data_Root/texture_sets/4k/h.dds","sha256":")" + hash +
        R"(","format":"BC4","width":4096,"height":4096,"gpu_bytes_2k":16,"gpu_bytes_4k":48}}}}})";
    auto writer = ZipArchiveWriter::create(pack, false);
    if (!writer ||
        !writer->add_bytes(std::string(kAssetArchiveManifest), std::as_bytes(std::span(manifest)), ZipCompression::Store) ||
        !writer->add_bytes("Data_Root/heaven/heaven.world", std::as_bytes(std::span(world)), ZipCompression::Store) ||
        !writer->add_bytes("Data_Root/heaven/h.dds", std::as_bytes(std::span(original)), ZipCompression::Store) ||
        !writer->add_bytes("Data_Root/texture_sets/4k/h.dds", std::as_bytes(std::span(variant)), ZipCompression::Store))
        return std::unexpected(Error{"Cannot write native envelope fixture archive"});
    for (std::size_t selector = 0; selector < constants.size(); ++selector)
        if (!writer->add_bytes("Data_Root/heaven/constant" + std::to_string(selector) + ".dds",
            std::as_bytes(std::span(constants[selector])), ZipCompression::Store))
            return std::unexpected(Error{"Cannot add constant BC4 selector fixture"});
    if (!writer->finish()) return std::unexpected(Error{"Cannot finish native envelope fixture archive"});
    auto assets = AssetFileSystem::open(pack.string());
    if (!assets) return std::unexpected(assets.error());
    TextureStore textures(engine, *assets);
    ClosedSeamHeightSampler envelopes(*assets);
    Surface surface;
    for (const auto uv : {std::array{-0.125f, -0.125f}, std::array{1.25f, -3.5f}, std::array{0.125f, 0.375f}}) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (unsigned corner = 0; corner < 3; ++corner) {
            Vertex vertex{}; vertex.px = static_cast<float>(first + corner); vertex.nz = 1;
            vertex.u = uv[0] + (corner == 1 ? 0.5f : 0);
            vertex.v = uv[1] + (corner == 2 ? 0.5f : 0);
            surface.vertices.push_back(vertex);
        }
        surface.indices.insert(surface.indices.end(), {first, first + 1, first + 2});
    }
    const auto footprints = mesh_displacement::calculate_footprints<Vertex>(surface.vertices, surface.indices);
    std::vector<HeightSampleQuery> queries;
    queries.reserve(surface.vertices.size());
    for (std::size_t i = 0; i < surface.vertices.size(); ++i)
        queries.push_back({static_cast<std::uint64_t>(i), {surface.vertices[i].u, surface.vertices[i].v}, footprints[i]});
    const std::array<float, 4> transform{-1, 2, 1, -0.25f};
    std::size_t qualified = 0;
    for (const auto quality : {TextureQuality::Original, TextureQuality::High2K, TextureQuality::Ultra4K}) {
        textures.set_texture_quality(quality);
        auto* texture = textures.get(textures.load("Data_Root/heaven/h.dds", TextureSemantic::ScalarLinear));
        if (!texture) return std::unexpected(Error{"Cannot upload selected native envelope fixture texture"});
        const auto bounds = envelopes.bounds(surface, "Data_Root/heaven/h.dds", transform, quality);
        if (bounds.universal_vertices != 0 || bounds.candidate_chains != (quality == TextureQuality::Original ? 1u : 2u))
            return std::unexpected(Error{"Native envelope fixture did not resolve its finite original/variant union"});
        const auto native = sampler.sample(texture, queries, transform);
        if (!native) return std::unexpected(native.error());
        if (native->size() != queries.size()) return std::unexpected(Error{"Native envelope result count differs"});
        for (std::size_t i = 0; i < native->size(); ++i) {
            if ((*native)[i].id != queries[i].id || !std::isfinite((*native)[i].height) ||
                std::abs(static_cast<double>((*native)[i].height) - 127.0 / 255.0) > bounds.maximum_height_delta[i] + 1e-6 ||
                bounds.maximum_height_delta[i] >= 0.26)
                return std::unexpected(Error{"Native BC4/periodic/partial-mip selected sample lies outside informative CPU envelope"});
            ++qualified;
        }
    }
    if (textures.stats().originals != 1 || textures.stats().hires != 2 || textures.stats().fallbacks != 0)
        return std::unexpected(Error{"Native envelope fixture selected texture provenance differs"});
    textures.set_texture_quality(TextureQuality::Original);
    for (std::size_t selector = 0; selector < constants.size(); ++selector) {
        const auto path = "Data_Root/heaven/constant" + std::to_string(selector) + ".dds";
        auto* texture = textures.get(textures.load(path, TextureSemantic::ScalarLinear));
        const auto bounds = envelopes.bounds(surface, path, transform, TextureQuality::Original);
        const auto native = sampler.sample(texture, queries, transform);
        if (!native || bounds.universal_vertices != 0 || native->size() != queries.size())
            return std::unexpected(Error{"Constant BC4 interpolation envelope fixture could not be sampled"});
        for (std::size_t i = 0; i < native->size(); ++i) {
            const double delta = std::abs(static_cast<double>((*native)[i].height) - 127.0 / 255.0);
            if (!std::isfinite(delta) || delta > bounds.maximum_height_delta[i] + 1e-6) {
                std::printf("[smoke_height_sample_batch] BC4 selector%zu nativeH=%.9g centered=%.9g bound=%.9g\n",
                    selector, (*native)[i].height, delta, bounds.maximum_height_delta[i]);
                return std::unexpected(Error{"Constant BC4 selector exceeds declared interpolation envelope"});
            }
            ++qualified;
        }
    }
    std::printf("[smoke_height_sample_batch] %zu native selected/constant-selector BC4 samples inside CPU envelopes; manual window route remains excluded\n", qualified);
    return {};
}
} // namespace

Result<void> verify_height_sample_batch_fixture(filament::Engine& engine,
                                               std::span<const std::uint8_t> package) {
    try {
        HeightSampleBatchOptions options;
        options.max_queries_per_chunk = 256;
        options.completion_timeout = std::chrono::seconds(20);
        auto created = HeightSampleBatch::create(engine, package, options);
        if (!created) return std::unexpected(created.error());
        auto& sampler = **created;
        FixtureTexture scalar(engine), fallback_rgba(engine), nonsquare(engine), constant(engine), one_level(engine);
        const std::vector<std::vector<std::uint8_t>> values{{std::vector<std::uint8_t>(16, 0)}, {0, 64, 128, 255}, {192}};
        if (!scalar.upload(4, 4, values, false) || !fallback_rgba.upload(4, 4, values, true) ||
            !nonsquare.upload(8, 4, {std::vector<std::uint8_t>(32, 0), {0, 32, 64, 96, 128, 160, 192, 224}, {64, 64}, {224}}, false) ||
            !constant.upload(4, 4, {std::vector<std::uint8_t>(16, 17), std::vector<std::uint8_t>(4, 17), {17}}, false) ||
            !one_level.upload(1, 1, {{37}}, false))
            return std::unexpected(Error{"Cannot create native height fixture textures"});
        const float half_fractional = std::numbers::sqrt2_v<float> / 2;
        const std::array queries{
            HeightSampleQuery{91, {.25f, .25f}, {.5f, .5f}},
            HeightSampleQuery{7, {.75f, .25f}, {.5f, .5f}},
            HeightSampleQuery{0, {.25f, .75f}, {.5f, .5f}},
            HeightSampleQuery{std::numeric_limits<std::uint64_t>::max(), {.75f, .75f}, {.5f, .5f}},
            HeightSampleQuery{12345, {.5f, .5f}, {.5f, .5f}},
            HeightSampleQuery{555, {.25f, .25f}, {1, .5f}},
            HeightSampleQuery{444, {.25f, .25f}, {half_fractional, .5f}},
            HeightSampleQuery{333, {1.25f, -.25f}, {.5f, .5f}}};
        const std::array expected{0.0f, 64.0f/255, 128.0f/255, 1.0f, 447.0f/1020, 192.0f/255, 96.0f/255, 128.0f/255};
        const std::array lod{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 1.5f, 1.0f};
        for (auto* texture : {scalar.texture, fallback_rgba.texture})
            if (auto result = check(sampler, texture, queries, {1, 1, 0, 0}, expected, lod); !result) return result;
        const std::array transformed{
            HeightSampleQuery{902, {.25f, .25f}, {.5f, .25f}},
            HeightSampleQuery{903, {.75f, .5f}, {.5f, .25f}}};
        const std::array transformed_h{64.0f/255, 128.0f/255};
        const std::array transformed_lod{1.0f, 1.0f};
        if (auto result = check(sampler, scalar.texture, transformed, {-1, 2, 1, -.25f}, transformed_h, transformed_lod); !result) return result;
        const std::array rectangular{
            HeightSampleQuery{801, {.125f, .25f}, {.25f, 0}},
            HeightSampleQuery{802, {.875f, .75f}, {0, .5f}},
            HeightSampleQuery{803, {.125f, .75f}, {.125f, .5f}},
            HeightSampleQuery{804, {.125f, .25f}, {.5f, 0}}};
        const std::array rectangular_h{0.0f, 224.0f/255, 128.0f/255, 64.0f/255};
        const std::array rectangular_lod{1.0f, 1.0f, 1.0f, 2.0f};
        if (auto result = check(sampler, nonsquare.texture, rectangular, {1, 1, 0, 0}, rectangular_h, rectangular_lod); !result) return result;
        std::array<float, 8> constant_h;constant_h.fill(17.0f/255);
        if (auto result = check(sampler, constant.texture, queries, {1, 1, 0, 0}, constant_h, lod); !result) return result;
        const std::array clamped{HeightSampleQuery{771, {-3.25f, 2.75f}, {2, 0}}};
        const std::array clamped_h{37.0f/255}, clamped_lod{1.0f};
        if (auto result = check(sampler, one_level.texture, clamped, {1,1,0,0}, clamped_h, clamped_lod); !result) return result;
        // Force a second chunk and mostly empty final target. External IDs are
        // not ordinal, and ordering must survive both slots and pixel Y routes.
        std::vector<HeightSampleQuery> many;
        std::vector<float> many_h(259), many_lod(259, 1);
        for (std::uint64_t i = 0; i < 259; ++i) {
            auto q = queries[i % 4];q.id = 10'000 + i * 17;many.push_back(q);many_h[i] = expected[i % 4];
        }
        if (auto result = check(sampler, scalar.texture, many, {1, 1, 0, 0}, many_h, many_lod); !result) return result;
        auto duplicates = transformed;duplicates[1].id = duplicates[0].id;
        auto sparse = transformed;sparse[0].footprint = {0, 0};
        if (sampler.sample(scalar.texture, duplicates, {1,1,0,0}) ||
            sampler.sample(scalar.texture, sparse, {1,1,0,0}) ||
            sampler.sample(scalar.texture, transformed, {1,1,std::numeric_limits<float>::quiet_NaN(),0}))
            return std::unexpected(Error{"Height sampler accepted invalid/LOD-dependent input"});
        options.max_queries_per_chunk = 1;
        auto single = HeightSampleBatch::create(engine, package, options);
        if (!single) return std::unexpected(single.error());
        if (auto result = check(**single, scalar.texture, transformed, {-1,2,1,-.25f}, transformed_h, transformed_lod); !result) return result;
        if (auto envelope = check_closed_seam_height_envelopes(engine, sampler); !envelope) return envelope;
        options.completion_timeout = std::chrono::milliseconds(0);
        if (HeightSampleBatch::create(engine, package, options))
            return std::unexpected(Error{"Height sampler accepted unbounded/invalid timeout"});
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(Error{std::string("Height sampler native fixture: ") + e.what()});
    }
}
} // namespace heaven::smoke
