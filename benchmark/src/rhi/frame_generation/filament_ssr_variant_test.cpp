// Host-only regression for SSR geometry variants. No graphics API is initialized.
#include "DynamicSpecConstKey.h"
#include "MaterialVariants.h"

#include <private/filament/Variant.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace {
using filament::Variant;

template<typename Range>
bool contains(const Range& variants, unsigned key) {
    return std::any_of(variants.begin(), variants.end(), [key](Variant v) {
        return v.key == key;
    });
}

void dump_material_variants() {
    for (bool lit : {false, true}) {
        for (auto filter : {0u, uint32_t(filament::UserVariantFilterBit::SSR),
                uint32_t(filament::UserVariantFilterBit::SKINNING),
                uint32_t(filament::UserVariantFilterBit::STE),
                uint32_t(filament::UserVariantFilterBit::VSM),
                uint32_t(filament::UserVariantFilterBit::FOG)}) {
            std::printf("lit=%d filter=%u:", int(lit), filter);
            for (const auto& shader : filamat::determineSurfaceVariants(filter, lit, false)) {
                std::printf(" %u/%u", unsigned(shader.variant.key), unsigned(shader.stage));
            }
            std::puts("");
        }
    }
}

bool run_contract() {
    bool ok = true;
    auto check = [&](bool condition, const char* label, unsigned key) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s, key=0x%02x\n", label, key);
            ok = false;
        }
    };

    // Independent bit-level oracle: SSR needs all three pass bits and no light/shadow
    // receiver bits. Geometry bits are orthogonal. The other depth modes require at
    // most one of the two overloaded moment/picking bits.
    unsigned valid_count = 0, depth_count = 0, ssr_count = 0;
    for (unsigned key = 0; key < 128; ++key) {
        Variant v(static_cast<Variant::type_t>(key));
        const bool dep = (key & 0x10) != 0;
        const bool expected_ssr = (key & 0x75) == 0x70;
        const bool expected_depth = dep && !(key & 0x05) && (key & 0x60) != 0x60;
        const bool expected_standard = !dep && (!(key & 0x40) || (key & 0x04));
        const bool expected_valid = expected_standard || expected_depth || expected_ssr;
        check(Variant::isSSRVariant(v) == expected_ssr, "SSR classification", key);
        check(Variant::isValidDepthVariant(v) == expected_depth, "depth classification", key);
        check(Variant::isValid(v) == expected_valid, "valid classification", key);
        check(Variant::isReserved(v) == !expected_valid, "reserved classification", key);
        valid_count += expected_valid;
        depth_count += expected_depth;
        ssr_count += expected_ssr;
    }
    check(valid_count == 64 && depth_count == 12 && ssr_count == 4,
            "complete variant partition", 0);

    const auto lit = filament::VariantUtils::getLitVariants();
    const auto unlit = filament::VariantUtils::getUnlitVariants();
    const auto depth = filament::VariantUtils::getDepthVariants();
    for (unsigned geometry : {0u, 0x02u, 0x08u, 0x0au}) {
        const unsigned key = 0x70 | geometry;
        Variant v(static_cast<Variant::type_t>(key));
        check(contains(lit, key) && contains(unlit, key), "program variants include SSR", key);
        check(!contains(depth, key), "depth program list excludes SSR", key);
        for (bool is_lit : {false, true}) {
            Variant filtered = Variant::filterVariant(v, is_lit);
            check(filtered.key == key, "material filter preserves SSR geometry", key);
            check(Variant::filterVariantVertex(filtered).key == geometry,
                    "vertex filter preserves skinning and stereo", key);
            check(Variant::filterVariantFragment(filtered).key == 0x70,
                    "fragment filter selects FLOAT SSR shader", key);
        }
        check(!Variant::isPickingVariant(v) && !Variant::isDepthMomentsVariant(v),
                "SSR does not alias picking or shadow moments", key);
        check(!Variant::isFogVariant(v) && !Variant::isShadowSampler2DVariant(v),
                "SSR does not alias fog or shadow sampling", key);

        // Match the RenderPass path: geometry mutation followed by per-material filtering.
        Variant from_pass(Variant::SPECIAL_SSR_VARIANT);
        from_pass.setSkinning((geometry & 0x08) != 0);
        from_pass.setStereo((geometry & 0x02) != 0);
        from_pass.setShadowReceiver(false);
        from_pass.setFog(false);
        check(Variant::filterVariant(from_pass, true).key == key,
                "RenderPass mutation retains SSR identity", key);

        for (auto mask : {filament::UserVariantFilterBit::FOG,
                filament::UserVariantFilterBit::VSM}) {
            check(Variant::filterUserVariant(v, uint32_t(mask)).key == key,
                    "unrelated user filter preserves SSR pass bits", key);
        }
        check(Variant::filterUserVariant(v,
                uint32_t(filament::UserVariantFilterBit::SSR)).key == geometry,
                "SSR user filter removes only SSR pass bits", key);
        check(Variant::filterUserVariant(v,
                uint32_t(filament::UserVariantFilterBit::SKINNING)).key == (key & ~0x08u),
                "skinning user filter retains SSR", key);
        check(Variant::filterUserVariant(v,
                uint32_t(filament::UserVariantFilterBit::STE)).key == (key & ~0x02u),
                "stereo user filter retains SSR", key);

        filament::DynamicSpecConstKey lights(0x07);
        check(filament::DynamicSpecConstKey::filterProgramSpecKey(v, lights,
                filament::MaterialDomain::SURFACE, true).key == 0,
                "SSR excludes standard lighting specialization", key);
        const auto shaders = filamat::determineSurfaceVariants(0, true, false);
        auto has_shader = [&](unsigned shader_key, filament::backend::ShaderStage stage) {
            return std::any_of(shaders.begin(), shaders.end(), [&](const auto& shader) {
                return shader.variant.key == shader_key && shader.stage == stage;
            });
        };
        check(has_shader(geometry, filament::backend::ShaderStage::VERTEX),
                "package generator provides SSR geometry shader", key);
        check(has_shader(0x70, filament::backend::ShaderStage::FRAGMENT),
                "package generator provides shared SSR fragment shader", key);
        if (geometry) {
            check(!has_shader(key, filament::backend::ShaderStage::FRAGMENT),
                    "no additional fragment package keys required", key);
        }

        for (unsigned pass : {0x10u, 0x30u, 0x50u}) {
            Variant other(static_cast<Variant::type_t>(pass | geometry));
            check(Variant::isValidDepthVariant(other), "ordinary depth remains valid", other.key);
            check(Variant::filterVariantVertex(other).key == ((pass & 0x50) | geometry),
                    "depth vertex filter preserves moments and geometry", other.key);
            check(Variant::filterVariantFragment(other).key == pass,
                    "depth fragment filter retains picking or moments", other.key);
        }
    }
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dump-material-variants") == 0) {
        dump_material_variants();
        return 0;
    }
    if (!run_contract()) return 1;
    std::puts("PASS: 128-key partition, SSR skinning/stereo, picking/moments, package keys and lighting specialization; no GPU");
    return 0;
}
