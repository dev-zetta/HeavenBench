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
    expect(texture_semantic_for_binding("_d", "", false),
           TextureSemantic::ScalarLinear,
           "authored diffuse without format metadata must stay linear");
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
    expect(texture_semantic_for_binding("_n", "", true),
           TextureSemantic::NormalLinear,
           "normal maps stay linear under auto-discovery");
    expect(texture_semantic_for_binding("_s", "", true),
           TextureSemantic::ScalarLinear,
           "unspecified specular maps stay linear");

    if (ok) std::printf("[unit_texture_semantics] PASSED\n");
    return ok;
}

const heaven::testing::Register reg_unit_texture_semantics{
    unit_texture_semantics};

} // namespace
