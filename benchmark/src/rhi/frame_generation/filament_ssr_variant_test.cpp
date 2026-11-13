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
                uint32_t(filament::UserVariantFilterBit::FOG),
                uint32_t(filament::UserVariantFilterBit::MOTION_VECTORS)}) {
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

    // This independent six-bit oracle follows the new package contract, including the
    // STE discriminator that separates mono motion vectors from the SSR sentinel.
    // SSR is not instanced stereo even though its sentinel contains the STE bit.
    unsigned valid_count = 0, depth_count = 0, ssr_count = 0, motion_count = 0;
    check(filament::VARIANT_COUNT == 64, "six-bit material variant space", 0);
    for (unsigned key = 0; key < 64; ++key) {
        Variant v(static_cast<Variant::type_t>(key));
        const bool dep = (key & 0x08) != 0;
        const bool expected_ssr = (key & 0x3b) == 0x39;
        const bool expected_motion = (key & 0x3b) == 0x38;
        const bool expected_depth = dep && !(key & 0x02) && !expected_ssr;
        const bool expected_standard = !dep && (!(key & 0x20) || (key & 0x02));
        const bool expected_valid = expected_standard || expected_depth || expected_ssr;
        check(Variant::isSSRVariant(v) == expected_ssr, "SSR classification", key);
        check(Variant::isMotionVectorsVariant(v) == expected_motion,
                "motion vectors classification", key);
        check(Variant::isValidDepthVariant(v) == expected_depth, "depth classification", key);
        check(Variant::isValid(v) == expected_valid, "valid classification", key);
        check(Variant::isReserved(v) == !expected_valid, "reserved classification", key);
        valid_count += expected_valid;
        depth_count += expected_depth;
        ssr_count += expected_ssr;
        motion_count += expected_motion;
    }
    check(valid_count == 40 && depth_count == 14 && ssr_count == 2 && motion_count == 2,
            "complete variant partition", 0);

    const auto lit = filament::VariantUtils::getLitVariants();
    const auto unlit = filament::VariantUtils::getUnlitVariants();
    const auto depth = filament::VariantUtils::getDepthVariants();
    for (unsigned geometry : {0u, 0x04u}) {
        const unsigned key = 0x39 | geometry;
        Variant v(static_cast<Variant::type_t>(key));
        check(contains(lit, key) && contains(unlit, key), "program variants include SSR", key);
        check(!contains(depth, key), "depth program list excludes SSR", key);
        check(!Variant::isStereoVariant(v), "SSR discriminator is not instanced stereo", key);
        for (bool is_lit : {false, true}) {
            Variant filtered = Variant::filterVariant(v, is_lit);
            check(filtered.key == key, "material filter preserves SSR skinning", key);
            check(Variant::filterVariantVertex(filtered).key == geometry,
                    "vertex filter preserves skinning and removes SSR discriminator", key);
            check(Variant::filterVariantFragment(filtered).key == 0x39,
                    "fragment filter selects FLOAT SSR shader", key);
        }
        check(!Variant::isPickingVariant(v) && !Variant::isDepthMomentsVariant(v) &&
                !Variant::isMotionVectorsVariant(v),
                "SSR does not alias picking, shadow moments or motion vectors", key);
        check(!Variant::isFogVariant(v) && !Variant::isShadowSampler2DVariant(v),
                "SSR does not alias fog or shadow sampling", key);

        // Match the reflection RenderPass: start at the SSR sentinel, then mutate the
        // per-renderable skinning/shadow-receiver state. The pass does not set stereo.
        Variant from_pass(Variant::SPECIAL_SSR_VARIANT);
        from_pass.setSkinning((geometry & 0x04) != 0);
        from_pass.setShadowReceiver(false);
        from_pass.setFog(false);
        check(Variant::filterVariant(from_pass, true).key == key,
                "RenderPass mutation retains SSR identity", key);

        for (auto mask : {filament::UserVariantFilterBit::FOG,
                filament::UserVariantFilterBit::VSM, filament::UserVariantFilterBit::STE,
                filament::UserVariantFilterBit::SHADOW_RECEIVER,
                filament::UserVariantFilterBit::MOTION_VECTORS}) {
            check(Variant::filterUserVariant(v, uint32_t(mask)).key == key,
                    "unrelated user filter preserves SSR sentinel", key);
        }
        check(Variant::filterUserVariant(v,
                uint32_t(filament::UserVariantFilterBit::SSR)).key == geometry,
                "SSR user filter removes only SSR sentinel bits", key);
        check(Variant::filterUserVariant(v,
                uint32_t(filament::UserVariantFilterBit::SKINNING)).key == (key & ~0x04u),
                "skinning user filter retains SSR", key);

        filament::DynamicSpecConstKey lights_and_fog(0x0f);
        check(filament::DynamicSpecConstKey::filterProgramSpecKey(v, lights_and_fog,
                filament::MaterialDomain::SURFACE, true).key == 0,
                "SSR excludes standard lighting and fog specialization", key);
        const auto shaders = filamat::determineSurfaceVariants(0, true, false);
        auto has_shader = [&](unsigned shader_key, filament::backend::ShaderStage stage) {
            return std::any_of(shaders.begin(), shaders.end(), [&](const auto& shader) {
                return shader.variant.key == shader_key && shader.stage == stage;
            });
        };
        check(has_shader(geometry, filament::backend::ShaderStage::VERTEX),
                "package generator provides SSR geometry shader", key);
        check(has_shader(0x39, filament::backend::ShaderStage::FRAGMENT),
                "package generator provides shared SSR fragment shader", key);
        if (geometry) {
            check(!has_shader(key, filament::backend::ShaderStage::FRAGMENT),
                    "no additional fragment package keys required", key);
        }

        const unsigned motion_key = 0x38 | geometry;
        Variant motion(static_cast<Variant::type_t>(motion_key));
        check(contains(depth, motion_key) && !contains(lit, motion_key),
                "motion vectors have a separate depth program", motion_key);
        check(Variant::filterVariantVertex(motion).key == motion_key &&
                Variant::filterVariantFragment(motion).key == 0x38,
                "motion vertex and fragment package keys remain distinct from SSR", motion_key);
        check(has_shader(motion_key, filament::backend::ShaderStage::VERTEX) &&
                has_shader(0x38, filament::backend::ShaderStage::FRAGMENT),
                "package generator provides motion vectors shaders", motion_key);
        check(Variant::filterUserVariant(motion,
                uint32_t(filament::UserVariantFilterBit::MOTION_VECTORS)).key == (0x08 | geometry),
                "motion filter retains ordinary depth and skinning", motion_key);
        check(Variant::filterUserVariant(motion,
                uint32_t(filament::UserVariantFilterBit::VSM)).key == motion_key,
                "VSM filter does not corrupt motion vectors", motion_key);
        check(filament::DynamicSpecConstKey::filterProgramSpecKey(motion, lights_and_fog,
                filament::MaterialDomain::SURFACE, true).key == 0,
                "motion vectors exclude color specializations", motion_key);
    }

    for (unsigned geometry : {0u, 0x01u, 0x04u, 0x05u}) {
        for (unsigned pass : {0x08u, 0x18u, 0x28u}) {
            Variant other(static_cast<Variant::type_t>(pass | geometry));
            check(Variant::isValidDepthVariant(other), "ordinary depth remains valid", other.key);
            check(Variant::filterVariantVertex(other).key == ((pass & 0x28) | geometry),
                    "depth vertex filter preserves moments and geometry", other.key);
            check(Variant::filterVariantFragment(other).key == pass,
                    "depth fragment filter retains picking or moments", other.key);
        }
    }

    // Fog is a color specialization now. It must neither duplicate packaged standard
    // variants nor be removed from the aliased picking/SSR sentinel bits.
    for (bool is_lit : {false, true}) {
        Variant standard(static_cast<Variant::type_t>(0x12));
        check(Variant::filterVariant(standard, is_lit).key == (is_lit ? 0x02 : 0x00),
                "material filter removes standard fog bit", standard.key);
        filament::DynamicSpecConstKey all(0x0f);
        check(filament::DynamicSpecConstKey::filterProgramSpecKey(standard, all,
                filament::MaterialDomain::SURFACE, is_lit).key == (is_lit ? 0x0f : 0x08),
                "fog specialization supports lit and unlit color", standard.key);
    }
    check(filament::DynamicSpecConstKey::filterUserVariant(
            filament::DynamicSpecConstKey(0x0f),
            uint32_t(filament::UserVariantFilterBit::FOG)).key == 0x07,
            "public fog filter clears only fog specialization", 0);
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dump-material-variants") == 0) {
        dump_material_variants();
        return 0;
    }
    if (!run_contract()) return 1;
    std::puts("PASS: 64-key partition, SSR skinning, separate motion/picking/moments, package keys and fog/lighting specialization; no GPU");
    return 0;
}
