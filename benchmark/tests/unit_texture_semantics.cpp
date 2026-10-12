/*
 * HeavenBench — texture semantic provenance tests
 */
#include "engine/material_rhi.h"

#include <cstdio>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_texture_semantics() {
    using heaven::TextureSemantic;
    using heaven::texture_semantic_for_binding;

    bool ok = true;
    auto expect = [&](TextureSemantic actual, TextureSemantic expected,
                      const char* label) {
        if (actual != expected) {
            std::printf("[unit_texture_semantics] FAILED: %s\n", label);
            ok = false;
        }
    };

    expect(texture_semantic_for_binding("_d", "srgb", false),
           TextureSemantic::ColorSRGB,
           "authored format=srgb diffuse must decode as sRGB");
    expect(texture_semantic_for_binding("_dd", "srgb", false),
           TextureSemantic::ColorSRGB,
           "explicit sRGB detail must retain its authored encoding");
    expect(texture_semantic_for_binding("_d", "", false),
           TextureSemantic::ColorLegacyGamma2,
           "authored unmarked diffuse must retain post-composition gamma-2 provenance");
    expect(texture_semantic_for_binding("_dd", "", false),
           TextureSemantic::ColorLegacyGamma2,
           "authored detail must retain encoded overlay values until gamma-2 decoding");
    expect(texture_semantic_for_binding("_e", "", false),
           TextureSemantic::ScalarLinear,
           "authored emission without format metadata must stay linear");
    expect(texture_semantic_for_binding("_d", "", true),
           TextureSemantic::ColorSRGB,
           "auto-discovered diffuse must use the color-role sRGB guess");
    expect(texture_semantic_for_binding("_dd", "", true),
           TextureSemantic::ColorSRGB,
           "auto-discovered detail diffuse must use sRGB");
    expect(texture_semantic_for_binding("_e", "", true),
           TextureSemantic::ColorSRGB,
           "auto-discovered emission must use sRGB");
    expect(texture_semantic_for_binding("_n", "srgb", false),
           TextureSemantic::ColorSRGB,
           "explicit authored format metadata takes precedence");
    expect(texture_semantic_for_binding("_s", "srgb", false),
           TextureSemantic::ColorSRGB,
           "explicit sRGB metadata also takes precedence for a data slot");
    expect(texture_semantic_for_binding("_e", "srgb", false),
           TextureSemantic::ColorSRGB,
           "explicit authored sRGB emission must retain its existing policy");
    expect(texture_semantic_for_binding("_d", "signed", false),
           TextureSemantic::ScalarLinear,
           "explicit signed data must not acquire diffuse color decoding");
    expect(texture_semantic_for_binding("_dd", "signed", false),
           TextureSemantic::ScalarLinear,
           "explicit signed detail data must stay linear");
    expect(texture_semantic_for_binding("_n", "signed", false),
           TextureSemantic::NormalLinear,
           "authored signed normals must retain their normal role");
    expect(texture_semantic_for_binding("_dn", "", false),
           TextureSemantic::NormalLinear,
           "authored unmarked detail normals must stay linear");
    expect(texture_semantic_for_binding("_n", "", true),
           TextureSemantic::NormalLinear,
           "normal maps stay linear under auto-discovery");
    expect(texture_semantic_for_binding("_s", "", true),
           TextureSemantic::ScalarLinear,
           "unspecified specular maps stay linear");
    for (const auto suffix : {"_s", "_ds", "_h", "_t"}) {
        expect(texture_semantic_for_binding(suffix, "", false),
               TextureSemantic::ScalarLinear,
               "authored unmarked specular/height/displacement data must stay linear");
        expect(texture_semantic_for_binding(suffix, "", true),
               TextureSemantic::ScalarLinear,
               "discovered specular/height/displacement data must stay linear");
    }
    const heaven::GpuMaterial procedural{};
    expect(procedural.diffuse_semantic, TextureSemantic::ScalarLinear,
           "procedural base color must not acquire legacy decoding by default");
    expect(procedural.detail_diffuse_semantic, TextureSemantic::ScalarLinear,
           "procedural detail must not acquire legacy decoding by default");

    if (ok) std::printf("[unit_texture_semantics] PASSED\n");
    return ok;
}

const heaven::testing::Register reg_unit_texture_semantics{
    unit_texture_semantics};

} // namespace
