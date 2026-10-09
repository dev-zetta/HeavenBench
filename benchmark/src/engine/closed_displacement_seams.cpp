#include "engine/closed_displacement_seams.h"
#include "engine/dds_block_codec.h"
#include "dds_loader_cpu.h"
#include "rhi/mesh_displacement.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <unordered_map>

namespace heaven {
namespace {

constexpr double kMaximumAddedSlope = 1.0;
constexpr double kSlopeSafetyMargin = 0.98;

glm::dvec3 position(const Vertex& v) { return {v.px, v.py, v.pz}; }
bool finite(const glm::dvec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
using PositionKey = std::array<std::uint32_t, 3>;
PositionKey position_key(const Vertex& v) {
    const auto bits = [](float x) { return std::bit_cast<std::uint32_t>(x == 0 ? 0.0f : x); };
    return {bits(v.px), bits(v.py), bits(v.pz)};
}

struct BoundaryEdge { ClosedSeamSegment segment; std::uint32_t a = 0, b = 0; };
std::vector<BoundaryEdge> indexed_boundaries(const Surface& surface) {
    struct IndexedEdge { std::size_t count = 0; std::uint32_t a = 0, b = 0; };
    std::map<std::uint64_t, IndexedEdge> indexed;
    for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            auto a = surface.indices[i + k], b = surface.indices[i + (k + 1) % 3];
            if (a >= surface.vertices.size() || b >= surface.vertices.size()) return {};
            const auto low = std::min(a, b), high = std::max(a, b);
            auto& entry = indexed[(std::uint64_t(low) << 32u) | high];
            ++entry.count; entry.a = a; entry.b = b;
        }
    }
    std::vector<BoundaryEdge> result;
    for (const auto& entry_pair : indexed) {
        const auto& edge = entry_pair.second;
        if (edge.count != 1) continue;
        const auto a = position(surface.vertices[edge.a]), b = position(surface.vertices[edge.b]);
        if (finite(a) && finite(b) && glm::dot(b - a, b - a) > 1e-24)
            result.push_back({{a, b}, edge.a, edge.b});
    }
    return result;
}

std::vector<ClosedSeamSegment> boundaries(const Surface& surface) {
    // Remove internal duplicated UV-chart edges. A flat neighbor must be a
    // geometric source boundary, not merely an index-chart boundary.
    using EdgeKey = std::array<PositionKey, 2>;
    struct Entry { std::size_t count = 0; ClosedSeamSegment segment; };
    std::map<EdgeKey, Entry> geometric;
    for (const auto& edge : indexed_boundaries(surface)) {
        const auto& a = surface.vertices[edge.a];
        const auto& b = surface.vertices[edge.b];
        auto ka = position_key(a), kb = position_key(b);
        if (kb < ka) std::swap(ka, kb);
        auto& entry = geometric[{ka, kb}];
        ++entry.count;
        entry.segment = {position(a), position(b)};
    }
    std::vector<ClosedSeamSegment> result;
    for (const auto& [key, entry] : geometric) {
        const auto& s = entry.segment;
        if (entry.count == 1 && finite(s.a) && finite(s.b) &&
            glm::dot(s.b - s.a, s.b - s.a) > 1e-24) result.push_back(s);
    }
    return result;
}

using Cell = std::array<std::int64_t, 3>;
struct CellHash {
    std::size_t operator()(const Cell& c) const noexcept {
        std::size_t h = 0;
        for (const auto x : c) h ^= std::hash<std::int64_t>{}(x) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
        return h;
    }
};
Cell cell(const glm::dvec3& p, double tolerance) {
    return {static_cast<std::int64_t>(std::floor(p.x / tolerance)),
        static_cast<std::int64_t>(std::floor(p.y / tolerance)),
        static_cast<std::int64_t>(std::floor(p.z / tolerance))};
}
double distance_squared(const glm::dvec3& p, const ClosedSeamSegment& segment) {
    const auto edge = segment.b - segment.a;
    const double t = std::clamp(glm::dot(p - segment.a, edge) / glm::dot(edge, edge), 0.0, 1.0);
    const auto delta = p - (segment.a + t * edge);
    return glm::dot(delta, delta);
}

class SegmentTree {
public:
    explicit SegmentTree(std::span<const ClosedSeamSegment> segments) : segments_(segments) {
        order_.resize(segments.size());
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        build(0, order_.size());
    }
    double nearest_squared(const glm::dvec3& p, double limit) const {
        query(0, p, limit);
        return limit;
    }
    std::vector<std::size_t> nearby(const ClosedSeamSegment& segment, double tolerance) const {
        std::vector<std::size_t> result;
        overlap(0, glm::min(segment.a, segment.b) - tolerance,
            glm::max(segment.a, segment.b) + tolerance, result);
        return result;
    }
private:
    struct Node {
        glm::dvec3 lo{0}, hi{0};
        std::size_t begin = 0, end = 0, left = 0, right = 0;
    };
    std::size_t build(std::size_t begin, std::size_t end) {
        Node n;
        n.begin = begin; n.end = end;
        n.lo = glm::dvec3(std::numeric_limits<double>::infinity()); n.hi = -n.lo;
        for (auto i = begin; i < end; ++i) {
            const auto& s = segments_[order_[i]];
            n.lo = glm::min(n.lo, glm::min(s.a, s.b)); n.hi = glm::max(n.hi, glm::max(s.a, s.b));
        }
        const auto index = nodes_.size(); nodes_.push_back(n);
        if (end - begin > 8) {
            const auto extent = n.hi - n.lo;
            const std::size_t axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
            const auto mid = begin + (end - begin) / 2;
            std::nth_element(order_.begin() + static_cast<std::ptrdiff_t>(begin),
                order_.begin() + static_cast<std::ptrdiff_t>(mid),
                order_.begin() + static_cast<std::ptrdiff_t>(end), [&](auto a, auto b) {
                    const auto& sa = segments_[a]; const auto& sb = segments_[b];
                    const double ca = sa.a[static_cast<glm::length_t>(axis)] + sa.b[static_cast<glm::length_t>(axis)];
                    const double cb = sb.a[static_cast<glm::length_t>(axis)] + sb.b[static_cast<glm::length_t>(axis)];
                    return ca != cb ? ca < cb : a < b;
                });
            nodes_[index].left = build(begin, mid); nodes_[index].right = build(mid, end);
        }
        return index;
    }
    void query(std::size_t index, const glm::dvec3& p, double& best) const {
        const auto& n = nodes_[index];
        const auto delta = glm::max(glm::max(n.lo - p, p - n.hi), glm::dvec3{0});
        if (glm::dot(delta, delta) > best) return;
        if (n.left == 0) {
            for (auto i = n.begin; i < n.end; ++i) best = std::min(best, distance_squared(p, segments_[order_[i]]));
        } else { query(n.left, p, best); query(n.right, p, best); }
    }
    void overlap(std::size_t index, const glm::dvec3& lo, const glm::dvec3& hi,
                 std::vector<std::size_t>& result) const {
        const auto& n = nodes_[index];
        if (glm::any(glm::greaterThan(n.lo, hi)) || glm::any(glm::lessThan(n.hi, lo))) return;
        if (n.left == 0) {
            for (auto i = n.begin; i < n.end; ++i) {
                const auto id = order_[i]; const auto& s = segments_[id];
                if (!glm::any(glm::greaterThan(glm::min(s.a, s.b), hi)) &&
                    !glm::any(glm::lessThan(glm::max(s.a, s.b), lo))) result.push_back(id);
            }
        } else { overlap(n.left, lo, hi, result); overlap(n.right, lo, hi, result); }
    }
    std::span<const ClosedSeamSegment> segments_;
    std::vector<std::size_t> order_;
    std::vector<Node> nodes_;
};

struct ChartEdge { BoundaryEdge edge; std::size_t surface = 0; };
struct Overlap { std::size_t other = 0; double begin = 0, end = 0; bool incompatible = false; };

bool compatible_at(const Surface& a, const BoundaryEdge& ae, double at,
                   const Surface& b, const BoundaryEdge& be, double bt) {
    const auto interpolate = [](const Surface& surface, const BoundaryEdge& edge, double t) {
        const auto& x = surface.vertices[edge.a]; const auto& y = surface.vertices[edge.b];
        const std::array xv{x.px, x.py, x.pz, x.u, x.v, x.nx, x.ny, x.nz, x.displacement_normal_scale};
        const std::array yv{y.px, y.py, y.pz, y.u, y.v, y.nx, y.ny, y.nz, y.displacement_normal_scale};
        std::array<std::uint32_t, xv.size()> bits{};
        for (std::size_t i = 0; i < xv.size(); ++i) {
            const float value = static_cast<float>(static_cast<double>(xv[i]) * (1 - t) + static_cast<double>(yv[i]) * t);
            bits[i] = std::bit_cast<std::uint32_t>(value == 0 ? 0.0f : value);
        }
        return bits;
    };
    return interpolate(a, ae, at) == interpolate(b, be, bt);
}

void collect_chart_seams(const Mesh& source, std::span<const SeamSurfaceKind> kinds,
                        ClosedDisplacementSeams& result) {
    std::vector<ChartEdge> edges;
    std::vector<ClosedSeamSegment> segments;
    for (std::size_t si = 0; si < kinds.size(); ++si) if (kinds[si] == SeamSurfaceKind::Displaced)
        for (const auto& e : indexed_boundaries(source.surfaces[si])) {
            edges.push_back({e, si}); segments.push_back(e.segment);
        }
    if (edges.empty()) return;
    SegmentTree tree(segments);
    std::vector<std::vector<Overlap>> matches(edges.size());
    std::vector<bool> closed(edges.size(), false);
    const double tolerance = result.chart_matching_tolerance;
    const double tolerance2 = tolerance * tolerance;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const auto& a = edges[i]; const auto& s = a.edge.segment;
        const auto d = s.b - s.a; const double length2 = glm::dot(d, d), length = std::sqrt(length2);
        for (const auto j : tree.nearby(s, tolerance)) {
            if (i == j) continue;
            const auto& b = edges[j]; const auto& t = b.edge.segment;
            const auto td = t.b - t.a;
            if (glm::dot(d, td) >= 0) continue;
            // A tiny overlap at a gable apex is not a collinear edge chain.
            // The symmetric direction test excludes crossing boundary edges.
            const auto cross = glm::cross(d, td);
            if (glm::dot(cross, cross) > 1e-4 * length2 * glm::dot(td, td)) continue;
            const double pa = glm::dot(t.a - s.a, d) / length2;
            const double pb = glm::dot(t.b - s.a, d) / length2;
            const double begin = std::max(0.0, pb), end = std::min(1.0, pa);
            if ((end - begin) * length <= 1e-8) continue;
            const auto parameter = [&](double p) { return std::clamp(glm::dot(s.a + p * d - t.a, td) / glm::dot(td, td), 0.0, 1.0); };
            // Compare only the overlapping portions, never the extrapolated
            // far end of a longer edge against a short chart segment's line.
            const auto da = s.a + begin * d - (t.a + parameter(begin) * td);
            const auto db = s.a + end * d - (t.a + parameter(end) * td);
            if (glm::dot(da, da) > tolerance2 || glm::dot(db, db) > tolerance2) continue;
            // Only exact P+UV duplicates within one surface share MeshStore's
            // incident height footprint. Cross-surface equality cannot prove it.
            const bool compatible = a.surface == b.surface &&
                begin == 0 && end == 1 && pa == 1 && pb == 0 &&
                compatible_at(source.surfaces[a.surface], a.edge, begin,
                    source.surfaces[b.surface], b.edge, parameter(begin)) &&
                compatible_at(source.surfaces[a.surface], a.edge, end,
                    source.surfaces[b.surface], b.edge, parameter(end));
            matches[i].push_back({j, begin, end, !compatible});
        }
        auto& intervals = matches[i];
        std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
            return a.begin != b.begin ? a.begin < b.begin : a.other < b.other;
        });
        // Full coverage admits differently segmented chains, but rejects
        // partial open joints and any overlapping/ambiguous third chart.
        const double allowance = std::min(tolerance / length, 0.01);
        double covered = 0; bool valid = !intervals.empty();
        for (const auto& interval : intervals) {
            if (interval.begin > covered + allowance || interval.begin < covered - allowance) { valid = false; break; }
            covered = interval.end;
        }
        closed[i] = valid && covered >= 1 - allowance;
    }
    for (std::size_t i = 0; i < edges.size(); ++i) if (closed[i])
        for (const auto& overlap : matches[i])
            if (std::none_of(matches[overlap.other].begin(), matches[overlap.other].end(),
                [&](const auto& reverse) { return reverse.other == i; })) {
                closed[i] = false; break;
            }
    // A short edge fully overlapped by an otherwise open long edge must not
    // pin only one side. Propagate rejection until every match is reciprocal.
    std::vector<std::vector<std::size_t>> dependents(edges.size());
    std::vector<std::size_t> rejected;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (!closed[i]) rejected.push_back(i);
        for (const auto& overlap : matches[i]) dependents[overlap.other].push_back(i);
    }
    for (std::size_t next = 0; next < rejected.size(); ++next) {
        for (const auto i : dependents[rejected[next]]) if (closed[i]) {
            closed[i] = false; rejected.push_back(i);
        }
    }
    for (std::size_t i = 0; i < edges.size(); ++i) if (closed[i]) {
        const auto& edge = edges[i]; const auto& s = edge.edge.segment;
        if (std::any_of(matches[i].begin(), matches[i].end(), [](const auto& overlap) {
            return overlap.incompatible;
        })) {
            // Full reciprocal coverage already qualified this source edge.
            // Retain its actual endpoints, rather than trimming a micron-sized
            // residual from rounding differences in projected chain coverage.
            result.surfaces[edge.surface].push_back(s);
            ++result.matched_segments;
        }
    }
}

} // namespace

ClosedDisplacementSeams collect_closed_displacement_seams(
    const Mesh& source, std::span<const SeamSurfaceKind> kinds) {
    ClosedDisplacementSeams result;
    result.surfaces.resize(source.surfaces.size());
    if (kinds.size() != source.surfaces.size()) return result;
    if (std::none_of(kinds.begin(), kinds.end(), [](auto kind) {
        return kind == SeamSurfaceKind::Displaced;
    })) return result;
    glm::dvec3 lo(std::numeric_limits<double>::infinity()), hi = -lo;
    for (std::size_t i = 0; i < kinds.size(); ++i) if (kinds[i] != SeamSurfaceKind::Inactive)
        for (const auto& v : source.surfaces[i].vertices) {
            const auto p = position(v); if (!finite(p)) return result;
            lo = glm::min(lo, p); hi = glm::max(hi, p);
        }
    const double diagonal = glm::length(hi - lo);
    if (!std::isfinite(diagonal) || diagonal <= 0) return result;
    result.matching_tolerance = std::clamp(diagonal * 1e-6, 2e-6, 5e-5);
    result.chart_matching_tolerance = std::clamp(diagonal * 1e-4, 2e-6, 5e-4);
    result.fade_distance = std::clamp(diagonal * 0.005, 0.01, 0.05);
    // Avoid integer conversion overflow for invalid extreme source coordinates.
    const double coordinate_limit = result.matching_tolerance * static_cast<double>(std::numeric_limits<std::int64_t>::max() / 4);
    if (glm::any(glm::greaterThan(glm::abs(lo), glm::dvec3(coordinate_limit))) ||
        glm::any(glm::greaterThan(glm::abs(hi), glm::dvec3(coordinate_limit)))) return result;
    std::vector<ClosedSeamSegment> flat;
    std::unordered_map<Cell, std::vector<std::size_t>, CellHash> grid;
    for (std::size_t i = 0; i < kinds.size(); ++i) if (kinds[i] == SeamSurfaceKind::Flat)
        for (const auto& s : boundaries(source.surfaces[i])) {
            const auto id = flat.size(); flat.push_back(s);
            grid[cell(s.a, result.matching_tolerance)].push_back(id);
            grid[cell(s.b, result.matching_tolerance)].push_back(id);
        }
    const double tolerance2 = result.matching_tolerance * result.matching_tolerance;
    for (std::size_t i = 0; i < kinds.size(); ++i) if (kinds[i] == SeamSurfaceKind::Displaced)
        for (const auto& s : boundaries(source.surfaces[i])) {
            const auto origin = cell(s.a, result.matching_tolerance);
            std::optional<std::size_t> match;
            bool ambiguous = false;
            for (std::int64_t x = -1; x <= 1 && !ambiguous; ++x)
                for (std::int64_t y = -1; y <= 1 && !ambiguous; ++y)
                    for (std::int64_t z = -1; z <= 1 && !ambiguous; ++z) {
                        auto it = grid.find({origin[0] + x, origin[1] + y, origin[2] + z});
                        if (it == grid.end()) continue;
                        for (auto id : it->second) {
                            const auto& f = flat[id];
                            const auto near = [&](const auto& a, const auto& b) { const auto d = a - b; return glm::dot(d, d) <= tolerance2; };
                            // Opposite boundary winding establishes adjacent
                            // faces rather than overlapping same-side copies.
                            if (near(s.a, f.b) && near(s.b, f.a)) {
                                if (match && *match != id) { ambiguous = true; break; }
                                match = id;
                            }
                        }
                    }
            if (match && !ambiguous) { result.surfaces[i].push_back(s); ++result.matched_segments; }
        }
    collect_chart_seams(source, kinds, result);
    return result;
}

double closed_seam_minimum_spatial_scale(const glm::mat4& transform) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        if (!std::isfinite(transform[c][r])) return 0;
    if (transform[0][3] != 0 || transform[1][3] != 0 || transform[2][3] != 0 || transform[3][3] <= 0) return 0;
    glm::dmat3 a(transform);
    auto gram = glm::transpose(a) * a;
    // Orthogonal Jacobi sweeps followed by a Gershgorin lower bound. Residual
    // off-diagonal entries are subtracted rather than assumed to be zero.
    for (int sweep = 0; sweep < 12; ++sweep) {
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            const double off = gram[p][q];
            if (off == 0) continue;
            const double angle = 0.5 * std::atan2(2 * off, gram[q][q] - gram[p][p]);
            glm::dmat3 rotation(1);
            const double c = std::cos(angle), s = std::sin(angle);
            rotation[p][p] = rotation[q][q] = c;
            rotation[p][q] = -s; rotation[q][p] = s;
            gram = glm::transpose(rotation) * gram * rotation;
        }
    }
    const double largest = std::max({gram[0][0], gram[1][1], gram[2][2]});
    double lower = largest;
    for (int i = 0; i < 3; ++i) {
        double radius = 0;
        for (int j = 0; j < 3; ++j) if (i != j) radius += std::abs(gram[i][j]);
        lower = std::min(lower, gram[i][i] - radius);
    }
    lower -= 128 * std::numeric_limits<double>::epsilon() * largest;
    return std::isfinite(lower) && lower > 0 ? std::sqrt(lower) : 0;
}

struct ClosedSeamHeightSampler::Chain {
    struct Mip { std::uint32_t width = 0, height = 0; std::vector<std::uint8_t> red, upper; };
    std::vector<Mip> mips;
    bool compressed = false;
};

std::shared_ptr<const ClosedSeamHeightSampler::Chain> ClosedSeamHeightSampler::chain(
    const std::string& path, std::uint32_t starting_mip) {
    const auto key = std::pair{path, starting_mip};
    if (const auto found = chains_.find(key); found != chains_.end()) return found->second;
    auto result = std::make_shared<Chain>();
    const auto dds = load_dds_cpu(path, assets_, starting_mip);
    if (!dds || dds.is_cubemap || dds.mip_data.size() > 32 || std::uint64_t(dds.width) * dds.height > std::uint64_t{64} * 1024u * 1024u)
        return chains_.emplace(key, result).first->second;
    result->compressed = dds.format != DDSFormatCPU::RGBA8 && dds.format != DDSFormatCPU::RGB8;
    for (std::size_t i = 0; i < dds.mip_data.size(); ++i) {
        Chain::Mip mip{std::max(dds.width >> i, 1u), std::max(dds.height >> i, 1u), {}, {}};
        const auto count = static_cast<std::size_t>(mip.width) * mip.height;
        const auto bytes = dds.mip_data[i].bytes();
        mip.red.resize(count); mip.upper.resize(count);
        if (!result->compressed) {
            const std::size_t stride = dds.format == DDSFormatCPU::RGBA8 ? 4 : 3;
            if (bytes.size() != count * stride) { result->mips.clear(); break; }
            for (std::size_t j = 0; j < count; ++j) mip.red[j] = bytes[j * stride];
        } else {
            // Other compressed red-channel representations require their own
            // endpoint contract. Retain the explicit unsigned fallback.
            if (dds.format != DDSFormatCPU::BC4 && dds.format != DDSFormatCPU::BC5) { result->mips.clear(); break; }
            const auto decoded = decode_dds_blocks_rgba(bytes, mip.width, mip.height, dds.format);
            if (!decoded) { result->mips.clear(); break; }
            for (std::size_t j = 0; j < count; ++j) mip.red[j] = decoded->pixels[j * 4];
        }
        mip.upper = mip.red;
        // Unsigned RGTC interpolants are convex combinations of the two
        // stored endpoints. Bound that entire hull instead of assuming a
        // particular hardware palette precision relative to bcdec's bytes.
        // Endpoint selectors and six-entry mode's explicit 0/1 stay exact.
        if (result->compressed) {
            const std::size_t stride = dds.format == DDSFormatCPU::BC4 ? 8 : 16;
            const std::size_t blocks_wide = (mip.width + 3u) / 4u;
            for (std::size_t y = 0; y < mip.height; ++y) for (std::size_t x = 0; x < mip.width; ++x) {
                const auto offset = (y / 4 * blocks_wide + x / 4) * stride;
                std::uint64_t selectors = 0;
                for (unsigned j = 0; j < 6; ++j) selectors |= std::uint64_t(bytes[offset + 2 + j]) << (8u * j);
                const auto selector = (selectors >> (3u * ((y % 4) * 4 + x % 4))) & 7u;
                if (selector < 2 || (bytes[offset] <= bytes[offset + 1] && selector >= 6)) continue;
                mip.red[y * mip.width + x] = std::min(bytes[offset], bytes[offset + 1]);
                mip.upper[y * mip.width + x] = std::max(bytes[offset], bytes[offset + 1]);
            }
        }
        result->mips.push_back(std::move(mip));
    }
    return chains_.emplace(key, result).first->second;
}

ClosedSeamHeightBounds ClosedSeamHeightSampler::bounds(const Surface& surface,
    const std::string& original_path, const std::array<float, 4>& transform, TextureQuality quality) {
    constexpr double universal = 128.0 / 255.0;
    ClosedSeamHeightBounds result;
    result.maximum_height_delta.assign(surface.vertices.size(), universal);
    for (const auto id : surface.indices) if (id >= surface.vertices.size()) {
        result.universal_vertices = surface.vertices.size(); return result;
    }
    std::vector<std::shared_ptr<const Chain>> candidates{chain(original_path, 0)};
    if (quality != TextureQuality::Original && assets_) {
        if (const auto variant = assets_->get().texture_variant(original_path)) {
            std::uint32_t starting_mip = 0;
            if (quality == TextureQuality::High2K) {
                auto edge = std::max(variant->width, variant->height);
                while (edge > 2048u) { ++starting_mip; edge >>= 1u; }
            }
            // Invalid variant loading uses the original in TextureStore. An
            // undecodable but valid format is conservatively universal here.
            candidates.push_back(chain(variant->variant_path, starting_mip));
        }
    }
    result.candidate_chains = candidates.size();
    const auto footprints = mesh_displacement::calculate_footprints<Vertex>(surface.vertices, surface.indices);
    const auto interval = [](const Chain::Mip& mip, double u, double v, double du, double dv) {
        const double x = (u - std::floor(u)) * mip.width - 0.5;
        const double y = (v - std::floor(v)) * mip.height - 0.5;
        const double ex = du * mip.width + 1.0 / 16.0, ey = dv * mip.height + 1.0 / 16.0;
        std::array<double, 4> xs{x - ex, x + ex, 0, 0}, ys{y - ey, y + ey, 0, 0};
        std::size_t nx = 2, ny = 2;
        for (auto k = static_cast<std::int64_t>(std::ceil(xs[0])); static_cast<double>(k) < xs[1]; ++k) xs[nx++] = static_cast<double>(k);
        for (auto k = static_cast<std::int64_t>(std::ceil(ys[0])); static_cast<double>(k) < ys[1]; ++k) ys[ny++] = static_cast<double>(k);
        const auto texel = [&](std::int64_t ix, std::int64_t iy, bool upper) {
            const auto w = static_cast<std::int64_t>(mip.width), h = static_cast<std::int64_t>(mip.height);
            ix = (ix % w + w) % w; iy = (iy % h + h) % h;
            return static_cast<double>((upper ? mip.upper : mip.red)[static_cast<std::size_t>(iy * w + ix)]) / 255.0;
        };
        std::array<double, 2> range{1, 0};
        for (std::size_t i = 0; i < nx; ++i) for (std::size_t j = 0; j < ny; ++j) {
            const auto ix = static_cast<std::int64_t>(std::floor(xs[i])), iy = static_cast<std::int64_t>(std::floor(ys[j]));
            const double fx = xs[i] - static_cast<double>(ix), fy = ys[j] - static_cast<double>(iy);
            for (const bool upper : {false, true}) {
                const double a = std::lerp(texel(ix, iy, upper), texel(ix + 1, iy, upper), fx);
                const double b = std::lerp(texel(ix, iy + 1, upper), texel(ix + 1, iy + 1, upper), fx);
                const double sample = std::lerp(a, b, fy);
                range[0] = std::min(range[0], sample); range[1] = std::max(range[1], sample);
            }
        }
        return range;
    };
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const auto& vertex = surface.vertices[i];
        const double u = static_cast<double>(vertex.u) * transform[0] + transform[2];
        const double v = static_cast<double>(vertex.v) * transform[1] + transform[3];
        const double du = 4 * std::numeric_limits<float>::epsilon() *
            (std::abs(static_cast<double>(vertex.u) * transform[0]) + std::abs(transform[2]) + 1);
        const double dv = 4 * std::numeric_limits<float>::epsilon() *
            (std::abs(static_cast<double>(vertex.v) * transform[1]) + std::abs(transform[3]) + 1);
        double bound = 0; bool valid = std::isfinite(u) && std::isfinite(v);
        if (!valid) { ++result.universal_vertices; continue; }
        for (const auto& candidate : candidates) {
            if (candidate->mips.empty()) { valid = false; break; }
            const auto& base = candidate->mips.front();
            if (du * base.width > 0.25 || dv * base.height > 0.25) { valid = false; break; }
            const double footprint = std::max({static_cast<double>(footprints[i][0]) * std::abs(transform[0]) * base.width,
                static_cast<double>(footprints[i][1]) * std::abs(transform[1]) * base.height, 1.0});
            const double mesh_lod = std::log2(footprint);
            if (!std::isfinite(mesh_lod)) { valid = false; break; }
            const double last = static_cast<double>(candidate->mips.size() - 1);
            const double lo = std::clamp(mesh_lod - 1.0 / 16.0 - 1e-5, 0.0, last);
            const double hi = std::clamp(std::max(mesh_lod, 1.0) + 1.0 / 16.0 + 1e-5, 0.0, last);
            const auto sample_lod = [&](double lod) {
                const auto a = static_cast<std::size_t>(std::floor(lod));
                const auto b = std::min(a + 1, candidate->mips.size() - 1);
                const auto ar = interval(candidate->mips[a], u, v, du, dv);
                const auto br = interval(candidate->mips[b], u, v, du, dv);
                const double f = lod - static_cast<double>(a);
                const double low = std::lerp(ar[0], br[0], f), high = std::lerp(ar[1], br[1], f);
                bound = std::max({bound, std::abs(low - 127.0 / 255.0), std::abs(high - 127.0 / 255.0)});
            };
            sample_lod(lo); sample_lod(hi);
            for (auto level = static_cast<std::size_t>(std::ceil(lo)); static_cast<double>(level) < hi; ++level) sample_lod(static_cast<double>(level));
            // Endpoint conversion and filtering arithmetic allowance, beyond
            // the full RGTC interpolation hull. Clamp to unsigned [0,1].
            bound = std::min(universal, bound + (candidate->compressed ? 1.0 / 255.0 : 0) + 1.0 / 1024.0);
        }
        if (valid) result.maximum_height_delta[i] = bound;
        else ++result.universal_vertices;
    }
    return result;
}

std::size_t apply_closed_displacement_seams(Surface& surface,
    std::span<const ClosedSeamSegment> segments, double fade_distance, double matching_tolerance,
    double maximum_source_offset_per_magnitude, ClosedSeamFadeReport* report,
    std::span<const double> maximum_source_offsets) {
    if (report) *report = {};
    if (segments.empty() || !std::isfinite(fade_distance) || fade_distance <= 0 ||
        !std::isfinite(matching_tolerance) || matching_tolerance < 0 ||
        !std::isfinite(maximum_source_offset_per_magnitude) || maximum_source_offset_per_magnitude < 0) return 0;
    for (const auto& s : segments) if (!finite(s.a) || !finite(s.b) || glm::dot(s.b - s.a, s.b - s.a) <= 1e-24) return 0;
    for (const auto id : surface.indices) if (id >= surface.vertices.size()) return 0;
    if (!maximum_source_offsets.empty() && maximum_source_offsets.size() != surface.vertices.size()) return 0;
    SegmentTree tree(segments);
    std::vector<double> distances(surface.vertices.size()), weights(surface.vertices.size());
    std::vector<float> magnitudes(surface.vertices.size());
    std::vector<double> offset_bounds(surface.vertices.size());
    double offset_bound = 0, maximum_width = fade_distance;
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const auto& v = surface.vertices[i];
        if (!finite(position(v)) || !std::isfinite(v.displacement_normal_scale)) return 0;
        distances[i] = std::sqrt(tree.nearest_squared(position(v), std::numeric_limits<double>::infinity()));
        offset_bounds[i] = maximum_source_offsets.empty() ? maximum_source_offset_per_magnitude *
            std::abs(static_cast<double>(v.displacement_normal_scale)) : maximum_source_offsets[i];
        if (!std::isfinite(offset_bounds[i]) || offset_bounds[i] < 0) return 0;
        offset_bound = std::max(offset_bound, offset_bounds[i]);
    }
    // Smoothstep's derivative peaks at 1.5. Bound the additional slope from
    // the fade to the declared limit, independent of camera or height frequency.
    // Width is local to the actual filtered-height envelope at each node.
    // A high-offset shutter must not widen near-neutral roof tile interiors.
    // Match the compatibility contract: exact P/UV/raw-normal/magnitude
    // copies share their existing height footprint and must share this weight.
    const auto key = [&](std::size_t i) {
        const auto& v = surface.vertices[i];
        const std::array values{v.px, v.py, v.pz, v.u, v.v, v.nx, v.ny, v.nz, v.displacement_normal_scale};
        std::array<std::uint32_t, values.size()> bits{};
        for (std::size_t j = 0; j < values.size(); ++j)
            bits[j] = std::bit_cast<std::uint32_t>(values[j] == 0 ? 0.0f : values[j]);
        return bits;
    };
    std::vector<std::size_t> order(surface.vertices.size()), group(surface.vertices.size()), representative;
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return key(a) < key(b); });
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i == 0 || key(order[i]) != key(order[i - 1])) representative.push_back(order[i]);
        group[order[i]] = representative.size() - 1;
    }
    std::vector<double> group_weights(representative.size(), 1);
    const auto rounded_weight = [&](std::size_t g, double requested) {
        const auto original = surface.vertices[representative[g]].displacement_normal_scale;
        const float next = static_cast<float>(static_cast<double>(original) * requested);
        return original != 0 ? static_cast<double>(next) / original : requested;
    };
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const double local_width = std::max(fade_distance, (1.5 / kMaximumAddedSlope) * offset_bounds[i]);
        maximum_width = std::max(maximum_width, local_width);
        const double t = std::clamp((distances[i] - matching_tolerance) / local_width, 0.0, 1.0);
        group_weights[group[i]] = std::min(group_weights[group[i]], rounded_weight(group[i], t * t * (3 - 2 * t)));
    }
    const std::size_t triangle_count = surface.indices.size() / 3;
    std::vector<std::vector<std::size_t>> incident(representative.size());
    for (std::size_t t = 0; t < triangle_count; ++t)
        for (std::size_t corner = 0; corner < 3; ++corner)
            incident[group[surface.indices[t * 3 + corner]]].push_back(t);
    const auto triangle_slope = [&](std::size_t t) {
        const auto a = surface.indices[t * 3], b = surface.indices[t * 3 + 1], c = surface.indices[t * 3 + 2];
        const auto e = position(surface.vertices[b]) - position(surface.vertices[a]);
        const auto f = position(surface.vertices[c]) - position(surface.vertices[a]);
        const auto cross = glm::cross(e, f); const double area2 = glm::dot(cross, cross);
        const double minimum = std::min({group_weights[group[a]], group_weights[group[b]], group_weights[group[c]]});
        // New raster displacement = m * old displacement +
        // sum_i (w_i-m) * D_i * bary_i, where m=min(w_i). This bound
        // holds even when the unknown height/normal vectors D_i differ.
        // A scalar |grad(w)| alone could cancel on a sliver and understate it.
        const double extra =
            (group_weights[group[a]] - minimum) * offset_bounds[a] * glm::length(f - e) +
            (group_weights[group[b]] - minimum) * offset_bounds[b] * glm::length(f) +
            (group_weights[group[c]] - minimum) * offset_bounds[c] * glm::length(e);
        if (extra == 0) return 0.0;
        // Every nonzero-area triangle participates, including tiny slivers.
        // A truly degenerate triangle with unequal extra terms must equalize
        // locally or fail the final check; it cannot certify zero derivative.
        if (area2 == 0) return std::numeric_limits<double>::infinity();
        const double slope = extra / std::sqrt(area2);
        return std::isfinite(slope) ? slope : std::numeric_limits<double>::infinity();
    };
    // A common dyadic ratio is exact for ordinary float products. Verify it
    // on every original magnitude, including subnormal products; zero is the
    // exact conservative fallback. This avoids unequal rounded ratios on a
    // degenerate triangle and finite triangles stalled at float precision.
    const auto common_ratio = [&](const std::array<std::size_t, 3>& gs, double minimum) {
        int exponent = 0;
        std::frexp(minimum, &exponent);
        double common = minimum > 0 ? std::ldexp(1.0, exponent - 1) : 0;
        for (const auto g : gs) if (rounded_weight(g, common) != common) { common = 0; break; }
        return common;
    };
    std::size_t evaluated = 0, exact_relaxations = 0, stalls = 0;
    bool work_exhausted = false;
    if (offset_bound > 0) {
        std::deque<std::size_t> queue;
        std::vector<bool> queued(triangle_count, true);
        for (std::size_t t = 0; t < triangle_count; ++t) queue.push_back(t);
        // At most 64 evaluations per triangle, including the final validation.
        // Only incident triangles are revisited. Failure commits no attributes.
        const std::size_t work_limit = triangle_count * 64;
        while (!queue.empty() && evaluated < work_limit - triangle_count) {
            const auto t = queue.front(); queue.pop_front(); queued[t] = false;
            const double slope = triangle_slope(t); ++evaluated;
            if (slope <= kMaximumAddedSlope) continue;
            std::array<std::size_t, 3> gs{};
            for (std::size_t c = 0; c < 3; ++c) gs[c] = group[surface.indices[t * 3 + c]];
            const double minimum = std::min({group_weights[gs[0]], group_weights[gs[1]], group_weights[gs[2]]});
            const auto lower = [&](std::size_t g, double next) {
                if (next >= group_weights[g]) return false;
                group_weights[g] = next;
                for (const auto adjacent : incident[g]) if (!queued[adjacent]) {
                    queue.push_back(adjacent); queued[adjacent] = true;
                }
                return true;
            };
            if (!std::isfinite(slope)) {
                const double common = common_ratio(gs, minimum);
                for (const auto g : gs) static_cast<void>(lower(g, common));
                ++exact_relaxations;
                continue;
            }
            const double factor = kSlopeSafetyMargin * kMaximumAddedSlope / slope;
            bool advanced = false;
            for (const auto g : gs)
                advanced = lower(g, rounded_weight(g, minimum + factor * (group_weights[g] - minimum))) || advanced;
            if (!advanced) {
                const double common = common_ratio(gs, minimum);
                for (const auto g : gs) static_cast<void>(lower(g, common));
                ++exact_relaxations; ++stalls;
            }
        }
        work_exhausted = !queue.empty();
    }
    double maximum_slope = 0, affected_distance = 0;
    std::size_t unresolved = 0;
    if (offset_bound > 0) for (std::size_t t = 0; t < triangle_count; ++t) {
        const double slope = triangle_slope(t);
        maximum_slope = std::max(maximum_slope, slope); ++evaluated;
        if (slope > kMaximumAddedSlope) ++unresolved;
    }
    if (report) *report = {maximum_width, 0, maximum_slope, evaluated, maximum_slope <= kMaximumAddedSlope,
        exact_relaxations, stalls, unresolved, work_exhausted};
    if (maximum_slope > kMaximumAddedSlope) return 0;
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const auto original = surface.vertices[i].displacement_normal_scale;
        weights[i] = group_weights[group[i]];
        magnitudes[i] = static_cast<float>(static_cast<double>(original) * weights[i]);
        if (magnitudes[i] != original) affected_distance = std::max(affected_distance, distances[i]);
    }
    if (report) report->affected_distance = affected_distance;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        auto& v = surface.vertices[i];
        if (magnitudes[i] != v.displacement_normal_scale) {
            v.displacement_normal_scale = magnitudes[i]; ++changed;
        }
    }
    return changed;
}

} // namespace heaven
