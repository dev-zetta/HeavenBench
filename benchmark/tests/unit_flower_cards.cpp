/*
 * HeavenBench — authored flower-card geometry regression test
 *
 * Verifies that massive foliage flowers are crossed alpha-masked cards with
 * normalized UVs and deterministic geometry, rather than ordinary randomized
 * grass blades that cannot display the authored flower atlas.
 */
#include "engine/grass_cpu.h"

#include <algorithm>
#include <cmath>
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

bool same_vertex(const heaven::GrassVertex& a,
                 const heaven::GrassVertex& b) {
    return a.px == b.px && a.py == b.py && a.pz == b.pz &&
           a.nx == b.nx && a.ny == b.ny && a.nz == b.nz &&
           a.u == b.u && a.v == b.v &&
           a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool same_tint(const heaven::GrassVertex& vertex,
               const heaven::render_tuning::Float3& tint) {
    return vertex.r == tint.x && vertex.g == tint.y && vertex.b == tint.z;
}

bool unit_flower_cards() {
    const std::vector<heaven::GrassRoot> roots{
        {{1.0f, 2.0f, 3.0f}},
        {{-4.0f, 5.0f, 0.5f}},
    };
    const auto first = heaven::generate_flower_cards(roots);
    const auto second = heaven::generate_flower_cards(roots);

    bool ok = first.size() ==
                  roots.size() * heaven::FLOWER_VERTS_PER_CARD &&
              first.size() == second.size();
    for (std::size_t i = 0; ok && i < first.size(); ++i) {
        const auto& vertex = first[i];
        ok = same_vertex(vertex, second[i]) &&
             (vertex.u == 0.0f || vertex.u == 1.0f) &&
             (vertex.v == 0.0f || vertex.v == 1.0f) &&
             std::isfinite(vertex.px) && std::isfinite(vertex.py) &&
             std::isfinite(vertex.pz) && vertex.a == 1.0f;
    }

    for (std::size_t root_index = 0; ok && root_index < roots.size();
         ++root_index) {
        const std::size_t base = root_index * heaven::FLOWER_VERTS_PER_CARD;
        float min_z = first[base].pz;
        float max_z = first[base].pz;
        for (std::size_t i = 1; i < heaven::FLOWER_VERTS_PER_CARD; ++i) {
            min_z = std::min(min_z, first[base + i].pz);
            max_z = std::max(max_z, first[base + i].pz);
        }
        ok = min_z == roots[root_index].position.z && max_z > min_z;
    }

    std::vector<heaven::GrassRoot> palette_roots;
    palette_roots.reserve(512);
    for (int i = 0; i < 512; ++i) {
        palette_roots.push_back({{
            static_cast<float>(i % 32), static_cast<float>(i / 32), 0.0f}});
    }
    const auto palette_cards = heaven::generate_flower_cards(palette_roots);
    int yellow = 0;
    int blue = 0;
    int red = 0;
    for (std::size_t root_index = 0;
         ok && root_index < palette_roots.size(); ++root_index) {
        const std::size_t base = root_index * heaven::FLOWER_VERTS_PER_CARD;
        const auto& first_vertex = palette_cards[base];
        if (same_tint(first_vertex,
                      heaven::render_tuning::grass.flower_tint_yellow)) {
            ++yellow;
        } else if (same_tint(
                       first_vertex,
                       heaven::render_tuning::grass.flower_tint_blue)) {
            ++blue;
        } else if (same_tint(
                       first_vertex,
                       heaven::render_tuning::grass.flower_tint_red)) {
            ++red;
        } else {
            ok = false;
        }
        for (std::size_t i = 1; ok && i < heaven::FLOWER_VERTS_PER_CARD; ++i) {
            ok = first_vertex.r == palette_cards[base + i].r &&
                 first_vertex.g == palette_cards[base + i].g &&
                 first_vertex.b == palette_cards[base + i].b;
        }
    }
    ok = ok && yellow > blue && yellow > red && blue > 0 && red > 0;

    if (!ok) std::printf("[unit_flower_cards] FAILED\n");
    return ok;
}

const heaven::testing::Register registered(unit_flower_cards);

} // namespace
