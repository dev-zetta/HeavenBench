/* Actual NOOP Engine resources; visual feature coverage is tested separately. */
#include "smoke_cases.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/materials_data.h"
#include "rhi/material_tuning_bindings.h"

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>

namespace heaven::smoke {
namespace {
CaseResult verify_render_test_assets() {
    using Texture = filament::Texture;
    auto* raw_engine = filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!raw_engine) {
        std::printf("[smoke_render_test_assets] NOOP Engine creation failed\n");
        return CaseResult::Fail;
    }
    const auto destroy_engine = [](filament::Engine* p) { filament::Engine::destroy(&p); };
    std::unique_ptr<filament::Engine, decltype(destroy_engine)> engine(raw_engine, destroy_engine);
    auto created = RenderTestSceneAssets::create(*engine);
    if (!created) {
        std::printf("[smoke_render_test_assets] %s\n", created.error().message.c_str());
        return CaseResult::Fail;
    }
    auto assets = std::move(*created);
    bool ok = true;
    std::set<const Texture*> identities;
    const auto check_texture = [&](const Texture* texture, Texture::InternalFormat format) {
        if (!texture || texture->getWidth() != 128 || texture->getHeight() != 128 ||
            texture->getLevels() != 8 || texture->getFormat() != format ||
            texture->getWidth(7) != 1 || texture->getHeight(7) != 1) {
            ok = false;
        }
        if (!identities.insert(texture).second) ok = false;
    };
    for (const auto asset : {FeatureAsset::GrassDiffuse, FeatureAsset::GrassIrregularity,
            FeatureAsset::GrassClutter, FeatureAsset::CloudLayer1, FeatureAsset::CloudLayer2,
            FeatureAsset::Glow, FeatureAsset::RainDrop}) {
        check_texture(assets->feature_texture(asset), Texture::InternalFormat::SRGB8_A8);
    }
    check_texture(assets->feature_texture(FeatureAsset::CloudMask), Texture::InternalFormat::RGBA8);
    check_texture(assets->sky_texture(SkyAsset::MoonDisk), Texture::InternalFormat::SRGB8_A8);
    check_texture(assets->sky_texture(SkyAsset::MoonNormal), Texture::InternalFormat::RGBA8);
    check_texture(assets->sky_texture(SkyAsset::MilkyWay), Texture::InternalFormat::SRGB8_A8);
    check_texture(assets->lens_dirt_texture(), Texture::InternalFormat::RGBA16F);
    ok &= identities.size() == 12;

    const glm::vec2 minimum{-4.0f, -2.0f}, maximum{4.0f, 6.0f};
    if (!assets->set_ground_patch(3, minimum, maximum, 0.125f, 1800, 70)) ok = false;
    auto patches = assets->grass_patches();
    if (patches.size() != 1 || patches[0].instance_index != 3 ||
        patches[0].roots.size() != 1800 || patches[0].flowers.size() != 70) {
        std::printf("[smoke_render_test_assets] explicit root counts/parent are wrong\n");
        return CaseResult::Fail;
    }
    const auto snapshot = patches[0];
    const auto check_roots = [&](const auto& roots) {
        for (const auto& root : roots) {
            const auto p = root.position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                p.x < minimum.x || p.x >= maximum.x || p.y < minimum.y ||
                p.y >= maximum.y || p.z != 0.125f) ok = false;
        }
    };
    check_roots(snapshot.roots);
    check_roots(snapshot.flowers);
    if (!assets->set_ground_patch(3, minimum, maximum, 0.125f, 1800, 70)) ok = false;
    patches = assets->grass_patches();
    for (std::size_t i = 0; i < snapshot.roots.size(); ++i)
        ok &= snapshot.roots[i].position == patches[0].roots[i].position;
    for (std::size_t i = 0; i < snapshot.flowers.size(); ++i)
        ok &= snapshot.flowers[i].position == patches[0].flowers[i].position;
    // Rejected setup must leave the current patch available and unchanged.
    ok &= !assets->set_ground_patch(3, maximum, minimum);
    ok &= !assets->set_ground_patch(3, minimum, maximum, 0.0f, 0);
    ok &= !assets->set_ground_patch(3, minimum, maximum,
        std::numeric_limits<float>::quiet_NaN());
    ok &= assets->grass_patches().size() == 1 &&
          assets->grass_patches()[0].roots[0].position == snapshot.roots[0].position;

    // A real production grass material borrows both typed texture inputs.
    // Release its descriptor owner before releasing the procedural texture owner.
    auto* raw_material = filament::Material::Builder()
        .package(k_heaven_grass_filamat, k_heaven_grass_filamat_size).build(*engine);
    const auto destroy_material = [&engine](filament::Material* p) { engine->destroy(p); };
    std::unique_ptr<filament::Material, decltype(destroy_material)> material(raw_material, destroy_material);
    if (!material) {
        std::printf("[smoke_render_test_assets] production grass material failed\n");
        return CaseResult::Fail;
    }
    const auto destroy_instance = [&engine](filament::MaterialInstance* p) { engine->destroy(p); };
    std::unique_ptr<filament::MaterialInstance, decltype(destroy_instance)> instance(
        material->createInstance(), destroy_instance);
    if (!instance) return CaseResult::Fail;
    material_tuning::bind_grass_visible(*instance);
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    instance->setParameter("grassDiffuse", assets->feature_texture(FeatureAsset::GrassDiffuse), sampler);
    instance->setParameter("grassVariation", assets->feature_texture(FeatureAsset::GrassIrregularity), sampler);
    instance.reset();
    material.reset();
    assets.reset();
    engine->flushAndWait();
    // Recreate after borrower-first teardown to exercise allocation reuse.
    auto recreated = RenderTestSceneAssets::create(*engine);
    ok &= recreated.has_value();
    if (recreated) recreated->reset();
    engine->flushAndWait();
    std::printf("[smoke_render_test_assets] %s: 12 actual typed textures/full mip chains, "
                "finite bounded deterministic roots, rejected-input preservation and "
                "production material borrower-first teardown (NOOP; no pixel claim)\n",
                ok ? "PASS" : "FAIL");
    return ok ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace

CaseResult run_render_test_assets_case(const CaseContext& /*ctx*/) {
    return verify_render_test_assets();
}
} // namespace heaven::smoke
