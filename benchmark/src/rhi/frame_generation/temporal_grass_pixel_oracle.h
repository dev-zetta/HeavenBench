/* Independent CPU raster reference for the native grass-motion regression. */
#ifndef HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_GRASS_PIXEL_ORACLE_H_
#define HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_GRASS_PIXEL_ORACLE_H_

#include "engine/grass_cpu.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace heaven::temporal_grass_oracle {

struct Frame {
    glm::dmat4 clip{1.0};
    glm::dmat4 model{1.0};
    glm::dmat3 wind_to_user{1.0};
    double time = 0.0;
};

enum class Coverage { outside, uncertain, opaque, cutout };

struct Pixel {
    Coverage coverage = Coverage::outside;
    double depth = 0.0;
    glm::dvec2 motion{};
    glm::dvec2 uv{};
};

// This is the visible grass material's documented physical displacement,
// evaluated on original CPU vertices. It does not inspect proxy uniforms,
// temporal shader outputs, or TemporalGrass's accepted-history state.
inline glm::dvec3 wind(const GrassVertex& vertex, const glm::dvec4& tuning, double time) {
    const double phase = double(vertex.px) * 0.5 + double(vertex.py) * 0.3;
    const double force = std::sin(time * tuning.x + phase) * tuning.z +
        std::sin(time * tuning.y + phase * 2.0) * tuning.w;
    const double height_squared = double(vertex.v) * double(vertex.v);
    return height_squared * glm::dvec3{0.8 * force, 0.3 * force, -0.2 * std::abs(force)};
}

inline glm::dvec4 position(const GrassVertex& vertex, const Frame& frame,
                           const glm::dvec4& tuning) {
    auto world = frame.model * glm::dvec4{vertex.px, vertex.py, vertex.pz, 1.0};
    world += glm::dvec4(frame.wind_to_user * wind(vertex, tuning, frame.time), 0.0);
    return frame.clip * world;
}

// The controlled RGBA8 atlas has a fully transparent central square and
// opaque surroundings. Ignore a four-texel band on each mask boundary, and
// ignore translucent vertex-alpha regions. This checks real discard and real
// opaque pixels without approximating derivatives or implementation-specific
// raster edge rules. The production sampler's repeat wrap is retained.
inline Coverage alpha_coverage(glm::dvec2 uv, double vertex_alpha) {
    uv -= glm::floor(uv);
    constexpr double margin = 4.0 / 128.0;
    const auto in_hole = [](double value) { return value > 0.25 + margin && value < 0.75 - margin; };
    const auto outside_hole = [](double value) { return value < 0.25 - margin || value > 0.75 + margin; };
    if (in_hole(uv.x) && in_hole(uv.y)) return Coverage::cutout;
    if ((outside_hole(uv.x) || outside_hole(uv.y)) && vertex_alpha > 0.999)
        return Coverage::opaque;
    return Coverage::uncertain;
}

// Project each wind-deformed triangle in top-left image coordinates, then
// perspective-correctly interpolate prior positions and UVs at pixel centers.
// The winning opaque fragment is chosen independently by inverted depth.
// Only stable triangle/mask interiors qualify for strict pixel comparisons.
inline std::vector<Pixel> raster(std::span<const GrassVertex> vertices,
        const Frame& current, const Frame& previous, const glm::dvec4& tuning,
        const glm::dvec4& diffuse_uv, unsigned extent, bool valid) {
    struct Vertex {
        glm::dvec4 current;
        glm::dvec4 previous;
        glm::dvec2 screen;
        glm::dvec2 uv;
        double alpha;
    };
    std::vector<Pixel> result(size_t(extent) * extent);
    std::vector<double> uncertain_depth(result.size(), 0.0);
    const auto cross = [](const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; };
    for (size_t base = 0; base + 2 < vertices.size(); base += 3) {
        std::array<Vertex, 3> triangle{};
        bool in_front = true;
        for (size_t i = 0; i < 3; ++i) {
            const auto& source = vertices[base + i];
            auto& vertex = triangle[i];
            vertex.current = position(source, current, tuning);
            vertex.previous = position(source, previous, tuning);
            in_front &= vertex.current.w > 0.0 && vertex.previous.w > 0.0;
            vertex.screen = glm::dvec2(vertex.current) / vertex.current.w * glm::dvec2{0.5, -0.5} + 0.5;
            vertex.screen *= double(extent);
            vertex.uv = {source.u, source.v};
            vertex.alpha = source.a;
        }
        if (!in_front) continue;
        const auto& a = triangle[0].screen;
        const auto& b = triangle[1].screen;
        const auto& c = triangle[2].screen;
        const double area = cross(b - a, c - a);
        if (std::abs(area) < 1e-8) continue;
        const int min_x = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int min_y = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int max_x = std::min(int(extent) - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int max_y = std::min(int(extent) - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = min_y; y <= max_y; ++y) for (int x = min_x; x <= max_x; ++x) {
            const glm::dvec2 point{double(x) + 0.5, double(y) + 0.5};
            const std::array barycentric{cross(b - point, c - point) / area,
                cross(c - point, a - point) / area, cross(a - point, b - point) / area};
            if (*std::min_element(barycentric.begin(), barycentric.end()) < -1e-7) continue;
            glm::dvec4 current_clip{}, previous_clip{};
            glm::dvec2 uv{};
            double alpha = 0.0, denominator = 0.0;
            for (size_t i = 0; i < 3; ++i) {
                const double weight = barycentric[i] / triangle[i].current.w;
                denominator += weight;
                current_clip += weight * triangle[i].current;
                previous_clip += weight * triangle[i].previous;
                uv += weight * triangle[i].uv;
                alpha += weight * triangle[i].alpha;
            }
            uv = uv / denominator * glm::dvec2(diffuse_uv) + glm::dvec2(diffuse_uv.z, diffuse_uv.w);
            alpha /= denominator;
            const double depth = 0.5 - 0.5 * current_clip.z / current_clip.w;
            if (depth <= 0.0 || depth >= 1.0) continue;
            auto kind = alpha_coverage(uv, alpha);
            // A constant atlas texel at one sample does not imply a constant
            // alpha derivative: a thin card's adjacent helper lane may sample
            // the opaque region. Qualify only whole constant-alpha 2x2 quads,
            // using this primitive's perspective UV interpolation even for
            // helper lanes outside its triangle. No shader fwidth/cutoff math
            // is copied into the oracle.
            if (kind != Coverage::uncertain) {
                for (int qy = y & ~1; qy < (y & ~1) + 2; ++qy) {
                    for (int qx = x & ~1; qx < (x & ~1) + 2; ++qx) {
                        const glm::dvec2 lane{double(qx) + 0.5,double(qy) + 0.5};
                        const std::array lane_barycentric{cross(b - lane,c - lane) / area,
                            cross(c - lane,a - lane) / area,cross(a - lane,b - lane) / area};
                        glm::dvec2 lane_uv{};
                        double lane_alpha = 0.0, lane_denominator = 0.0;
                        for (size_t i = 0; i < 3; ++i) {
                            const double weight = lane_barycentric[i] / triangle[i].current.w;
                            lane_denominator += weight;
                            lane_uv += weight * triangle[i].uv;
                            lane_alpha += weight * triangle[i].alpha;
                        }
                        lane_uv = lane_uv / lane_denominator * glm::dvec2(diffuse_uv) +
                            glm::dvec2(diffuse_uv.z,diffuse_uv.w);
                        if (alpha_coverage(lane_uv,lane_alpha / lane_denominator) != kind)
                            kind = Coverage::uncertain;
                    }
                }
            }
            const bool interior = *std::min_element(barycentric.begin(), barycentric.end()) > 0.045;
            const size_t index = size_t(y) * extent + unsigned(x);
            auto& pixel = result[index];
            if (kind == Coverage::uncertain || (!interior && kind != Coverage::cutout)) {
                uncertain_depth[index] = std::max(uncertain_depth[index], depth);
                continue;
            }
            if (kind == Coverage::cutout) {
                if (pixel.coverage == Coverage::outside) {
                    pixel.coverage = Coverage::cutout;
                    pixel.uv = uv;
                }
                continue;
            }
            if (depth <= pixel.depth) continue;
            pixel.coverage = Coverage::opaque;
            pixel.depth = depth;
            pixel.uv = uv;
            pixel.motion = valid ? (glm::dvec2(previous_clip) / previous_clip.w -
                glm::dvec2(current_clip) / current_clip.w) * glm::dvec2{0.5, -0.5} : glm::dvec2{};
        }
    }
    for (size_t i = 0; i < result.size(); ++i) {
        if (uncertain_depth[i] >= result[i].depth && uncertain_depth[i] > 0.0)
            result[i].coverage = Coverage::uncertain;
    }
    return result;
}

} // namespace heaven::temporal_grass_oracle
#endif
