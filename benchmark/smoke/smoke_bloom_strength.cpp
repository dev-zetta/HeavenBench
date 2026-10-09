/*
 * HeavenBench — real View bloom strength ownership and cache regression
 */
#include "smoke_cases.h"
#include "rhi/backend_core.h"
#include "rhi/lighting_system.h"
#include "rhi/texture_store.h"
#include "dds_loader_cpu.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/View.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace heaven::smoke {

CaseResult run_bloom_strength_case(const CaseContext& /*unused*/) {
    auto* engine = filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) return CaseResult::Skip;
    auto* scene = engine->createScene();
    auto* view = engine->createView();
    view->setScene(scene);
    bool ok = true;
    const auto require = [&](bool condition, const char* message) {
        if (!condition) std::printf("[smoke_bloom_strength] FAIL: %s\n", message);
        ok &= condition;
    };
    auto assets = AssetFileSystem::create_memory("bloom-strength-smoke");
    require(assets.has_value(), "generated-only dirt namespace failed");
    dds_detail::Header header{};
    header.size = sizeof(header);
    header.flags = dds_detail::DDSD_PITCH;
    header.width = 2; header.height = 1; header.mipMapCount = 2;
    header.pitchOrLinearSize = 8;
    header.pixelFormat.size = sizeof(header.pixelFormat);
    header.pixelFormat.flags = dds_detail::DDPF_RGB | dds_detail::DDPF_ALPHAPIXELS;
    header.pixelFormat.rgbBitCount = 32;
    header.pixelFormat.rBitMask = 0xff; header.pixelFormat.gBitMask = 0xff00;
    header.pixelFormat.bBitMask = 0xff0000; header.pixelFormat.aBitMask = 0xff000000;
    const std::array<uint8_t, 12> pixels{0, 0, 0, 255, 255, 255, 255, 255,
                                      128, 128, 128, 255};
    auto bytes = std::make_shared<AssetBuffer>(4 + sizeof(header) + pixels.size());
    const uint32_t magic = dds_detail::DDS_MAGIC;
    std::memcpy(bytes->data(), &magic, 4);
    std::memcpy(bytes->data() + 4, &header, sizeof(header));
    std::memcpy(bytes->data() + 4 + sizeof(header), pixels.data(), pixels.size());
    if (assets) require(assets->mount_memory("dirt.dds", bytes).has_value(), "DDS mount failed");
    auto textures = std::make_unique<TextureStore>(*engine,
        assets ? AssetFileSystemRef{std::cref(*assets)} : std::nullopt);
    const auto gain_handle = textures->load_bloom_dirt_gain("dirt.dds");
    auto* dirt = textures->get(gain_handle);
    require(dirt && dirt->getWidth() == 2 && dirt->getHeight() == 1 &&
        dirt->getLevels() == 2 && dirt->getFormat() == filament::Texture::InternalFormat::RGBA16F,
        "authored dirt gain is missing, UNORM/sRGB or has an incomplete mip chain");
    require(textures->load_bloom_dirt_gain("dirt.dds") == gain_handle &&
        textures->stats().hdr == 1 && textures->stats().originals == 1 &&
        textures->stats().selected_mip_bytes == 24,
        "gain cache reloads the source or reports incorrect half-float upload bytes");
    const auto color_handle = textures->load("dirt.dds", TextureSemantic::ColorSRGB);
    const auto* color = textures->get(color_handle);
    require(color_handle != gain_handle && color && color != dirt &&
        color->getFormat() == filament::Texture::InternalFormat::SRGB8_A8,
        "derived gain aliases the ordinary sRGB DDS cache entry");
    require(textures->load_bloom_dirt_gain("missing.dds") == 0,
        "missing dirt fails to retain the clean-bloom fallback");
    {
        BackendCore core{.engine=engine, .scene=scene, .view=view};
        LightingSystem lighting(core);
        const auto night = lighting_for_time_of_day(18.5f);
        FeatureToggles features;
        features.bloom = true;
        QualityKnobs quality;
        auto options = view->getBloomOptions();
        options.threshold = true;
        options.dirt = dirt;
        options.dirtStrength = 0.17f;
        options.resolution = 640;
        options.levels = 7;
        options.quality = filament::QualityLevel::HIGH;
        view->setBloomOptions(options);
        const float baseline = render_tuning::lighting.bloom_strength.x +
            std::clamp(night.sun_intensity / render_tuning::lighting.reference_sun_lux,
                       0.0f, 1.0f) * render_tuning::lighting.bloom_strength.y;
        const auto check = [&](float expected) {
            const auto actual = view->getBloomOptions();
            require(std::fabs(actual.strength - expected) < 1e-6f,
                    "caller strength is ignored or strength-only update was cached out");
            require(actual.enabled == features.bloom, "feature activation changed");
            require(actual.threshold && actual.dirt == dirt && actual.dirtStrength == 0.17f &&
                actual.resolution == 640 && actual.levels == 7 &&
                actual.quality == filament::QualityLevel::HIGH,
                "strength update overwrote dirt, threshold or quality");
        };
        lighting.setup(night, true, quality, features);
        require(std::bit_cast<uint32_t>(view->getBloomOptions().strength) ==
                std::bit_cast<uint32_t>(baseline), "default curve is not bit exact");
        check(baseline);
        // Lighting, weather and toggles stay fixed. This specifically catches
        // a missing requested-strength input in the steady-state cache key.
        lighting.update(night, true, features, 0.9f);
        check(baseline * (0.9f / 0.3f));
        lighting.update(night, true, features, 0.0f);
        check(0.0f);
        lighting.update(night, true, features, 0.6f);
        check(baseline * 2.0f);
        features.bloom = false;
        lighting.update(night, true, features, 0.6f);
        check(baseline * 2.0f);
        features.bloom = true;
        lighting.setup(night, true, quality, features, 0.3f);
        check(baseline);
    }
    auto options = view->getBloomOptions();
    options.dirt = nullptr;
    view->setBloomOptions(options);
    textures->clear_loaded_textures();
    require(textures->get(gain_handle) == nullptr && textures->get(color_handle) == nullptr,
        "dirt gain ownership survived explicit cache invalidation");
    const auto reloaded = textures->load_bloom_dirt_gain("dirt.dds");
    require(reloaded != 0 && reloaded != gain_handle && textures->get(reloaded) != nullptr,
        "gain cache did not safely reload after borrower-first retirement");
    textures.reset();
    engine->destroy(view);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);
    if (ok) std::printf("[smoke_bloom_strength] default curve/caller-only updates and actual linear dirt gain DDS/cache/lifecycle PASS; threshold/quality retained (NOOP state, no pixel claim)\n");
    return ok ? CaseResult::Pass : CaseResult::Fail;
}

} // namespace heaven::smoke
