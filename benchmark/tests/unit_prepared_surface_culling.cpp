#include "rhi/prepared_surface_culling.h"
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
} // namespace heaven::testing

namespace {
bool prepared_surface_culling_test() {
    using heaven::prepared_surface_culling_enabled;
    glm::mat4 transform(1.0f);
    bool ok = prepared_surface_culling_enabled(true, transform) &&
        !prepared_surface_culling_enabled(false, transform);
    // Proper uniform placement and translation do not change the affine row.
    transform[0][0] = transform[1][1] = transform[2][2] = 1.25f;
    transform[3][0] = 17;
    transform[3][1] = -23;
    ok &= prepared_surface_culling_enabled(true, transform);
    // Even the adjacent representable W values require homogeneous division.
    for (const float w : {.8f, 0.f, -1.f, .9999949932098389f,
            std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f),
            std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        transform[3][3] = w;
        ok &= !prepared_surface_culling_enabled(true, transform);
    }
    transform[3][3] = 1;
    for (int axis = 0; axis < 3; ++axis) {
        transform[axis][3] = std::numeric_limits<float>::denorm_min();
        ok &= !prepared_surface_culling_enabled(true, transform);
        transform[axis][3] = -0.f;
        ok &= prepared_surface_culling_enabled(true, transform);
    }
    // The same predicate reevaluates disabled -> enabled after a dirty transform.
    transform[3][3] = .8f;
    ok &= !prepared_surface_culling_enabled(true, transform);
    transform[3][3] = 1;
    ok &= prepared_surface_culling_enabled(true, transform) &&
        !prepared_surface_culling_enabled(false, transform);
    if (!ok) std::fprintf(stderr, "[prepared_surface_culling] affine row guard failed\n");
    return ok;
}
heaven::testing::Register prepared_surface_culling_registration(prepared_surface_culling_test);

heaven::PreparedSurfaceCullingBounds culling_bounds(glm::vec3 lo, glm::vec3 hi) {
    const glm::vec3 outer_lo = lo - glm::vec3(1);
    const glm::vec3 outer_hi = hi + glm::vec3(1);
    return {lo, hi, (outer_lo + outer_hi) * .5f, (outer_hi - outer_lo) * .5f};
}

bool sampled_containment(const glm::mat4& placement,
        const heaven::PreparedSurfaceCullingBounds& bounds, glm::dvec3 camera) {
    // An independent corner replay exercises different world-origin rotations
    // and FP32 world-matrix/Box arithmetic. It is not a GPU precision oracle.
    for (unsigned i = 0; i < 24; ++i) {
        const auto rotation = glm::rotate(glm::dmat4(1), .31 * i,
            glm::normalize(glm::dvec3(1 + i % 3, 2 + i % 5, 3 + i % 7)));
        for (const auto origin : {glm::dvec3(0), camera}) {
            const glm::mat4 m(rotation * glm::translate(glm::dmat4(1), -origin) * glm::dmat4(placement));
            const glm::vec3 c = glm::mat3(m) * bounds.outer_center + glm::vec3(m[3]);
            glm::mat3 absolute(m);
            for (int j = 0; j < 3; ++j) absolute[j] = glm::abs(absolute[j]);
            const glm::vec3 h = absolute * bounds.outer_half_extent;
            for (unsigned corner = 0; corner < 8; ++corner) {
                const glm::vec3 p((corner & 1) ? bounds.inner_max.x : bounds.inner_min.x,
                    (corner & 2) ? bounds.inner_max.y : bounds.inner_min.y,
                    (corner & 4) ? bounds.inner_max.z : bounds.inner_min.z);
                const glm::vec4 raw = m * glm::vec4(p, 1);
                const glm::vec3 actual = glm::vec3(raw) / raw.w;
                for (int axis = 0; axis < 3; ++axis)
                    if (actual[axis] < c[axis] - h[axis] || actual[axis] > c[axis] + h[axis]) return false;
            }
        }
    }
    return true;
}

bool prepared_surface_containment_test() {
    using heaven::prepared_surface_culling_enabled;
    const auto small = culling_bounds(glm::vec3(-.1f), glm::vec3(.1f));
    glm::mat4 m(1);
    m[3][3] = .8f;
    bool ok = prepared_surface_culling_enabled(true, m, small, {0,0,0}, true);
    ok &= sampled_containment(m, small, {0,0,0});
    // No entity changed: moving the camera invalidates a previously safe box.
    ok &= !prepared_surface_culling_enabled(true, m, small, {100,0,0}, true);
    ok &= !sampled_containment(m, small, {100,0,0});
    ok &= !prepared_surface_culling_enabled(true, m, small, {0,0,0}, false);
    ok &= !prepared_surface_culling_enabled(false, m, small, {0,0,0}, true);
    const auto no_margin = heaven::PreparedSurfaceCullingBounds{{-.1f,-.1f,-.1f},
        {.1f,.1f,.1f}, {}, {.1f,.1f,.1f}};
    ok &= !prepared_surface_culling_enabled(true, m, no_margin, {}, true);
    // Actual ninth house2 transform from the primary scene-loader receipt.
    const float ninth_values[16] = {-0.969268322f,0.235708922f,-0.0704230592f,0,
        -0.23795262f,-0.970947504f,0.0252683479f,0,
        -0.0624212101f,0.0412492231f,0.997195959f,0,
        37.9739265f,71.5950851f,20.3841724f,0.9999949932098389f};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) m[column][row] = ninth_values[4*column+row];
    const auto roof = culling_bounds({-5.7f,-4.2f,2.1f}, {5.8f,5.5f,9.f});
    for (const glm::dvec3 camera : {glm::dvec3(-9.8,24.3,27.5), glm::dvec3(-28,1,23),
            glm::dvec3(2000,-2000,2000), glm::dvec3(-2000,2000,-2000)}) {
        ok &= prepared_surface_culling_enabled(true, m, roof, camera, true);
        ok &= sampled_containment(m, roof, camera);
    }
    ok &= !prepared_surface_culling_enabled(true, m, roof, {1e6,1e6,1e6}, true);
    // Rotation, scale, W and camera sweeps: every permitted case must contain
    // all replayed corners. Some unsafe cases must remain disabled.
    unsigned permitted = 0, rejected = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const float angle = .13f * float(i);
        const float scale = .5f + .05f * float(i % 8);
        m = glm::scale(glm::rotate(glm::mat4(1), angle,
            glm::normalize(glm::vec3(1,2,3))), glm::vec3(scale));
        m[3] = glm::vec4(float(i)-32, 3, -7, i % 3 ? .8f : (i % 2 ? 1.000005f : .999995f));
        const glm::dvec3 camera(2*i, -3, 11);
        if (prepared_surface_culling_enabled(true, m, small, camera, true)) {
            ++permitted;
            ok &= sampled_containment(m, small, camera);
        } else ++rejected;
    }
    ok &= permitted > 0 && rejected > 0;
    m = glm::mat4(1); m[3][3] = .8f;
    for (const float w : {0.f,-1.f,std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        m[3][3] = w;
        ok &= !prepared_surface_culling_enabled(true,m,small,{},true);
    }
    m[3][3] = .8f; m[0][3] = std::numeric_limits<float>::denorm_min();
    ok &= !prepared_surface_culling_enabled(true,m,small,{},true);
    m = glm::mat4(1); m[0][0] = 0; m[3][3] = .8f;
    ok &= !prepared_surface_culling_enabled(true,m,small,{},true);
    // Canonical affine W keeps the existing fast path under grid mode.
    ok &= prepared_surface_culling_enabled(true,glm::mat4(1),no_margin,{},false);
    if (!ok) std::fprintf(stderr,"[prepared_surface_culling] containment or camera sweep failed\n");
    return ok;
}
heaven::testing::Register prepared_surface_containment_registration(prepared_surface_containment_test);
} // namespace
