#include "engine/bounded_displacement_surface.h"

#include <opensubdiv/far/patchBasis.h>
#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/sdc/crease.h>

#include <bit>
#include <deque>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace heaven {
namespace {

namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;
using Point = glm::dvec3;
using IndexTriangle = std::array<std::uint32_t, 3>;
constexpr double kHeightZero = 127.0 / 255.0;
constexpr std::array<Point, 7> kSamples{{
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
    {.5, .5, 0}, {0, .5, .5}, {.5, 0, .5},
    {1.0 / 3, 1.0 / 3, 1.0 / 3}}};

bool finite(Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
double length(Point p) { return glm::length(p); }
Point cross(Point a, Point b) { return glm::cross(a, b); }
double dot(Point a, Point b) { return glm::dot(a, b); }
Point affine(const std::array<Point, 3>& p, Point b) {
    return p[0] + b.y * (p[1] - p[0]) + b.z * (p[2] - p[0]);
}
std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
}
Point bounded(Point p, Point reference, double limit) {
    Point delta = p - reference;
    double distance = length(delta);
    return reference + delta * (distance > limit ? limit / distance : 1.0);
}
double beta(std::size_t valence) {
    double n = static_cast<double>(valence);
    double c = .375 + .25 * std::cos(2.0 * std::numbers::pi / n);
    return (.625 - c * c) / n;
}

struct Node {
    Point neutral{}, reference{}, desired{};
    glm::dvec2 uv0{}, uv1{};
    std::uint32_t source_parent = 0;
    Point source_bary{};
    bool pinned = false;
};
struct Face {
    IndexTriangle nodes{};
    std::uint32_t parent = 0;
    std::array<Point, 3> bary{};
};
struct Edge {
    std::uint32_t a = 0, b = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> incident;
    bool feature = false;
};
using Edges = std::map<std::uint64_t, Edge>;
Edges edges_of(std::span<const Face> faces, const std::set<std::uint64_t>& features) {
    Edges edges;
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        const auto& t = faces[f].nodes;
        for (int e = 0; e < 3; ++e) {
            auto key = edge_key(t[e], t[(e + 1) % 3]);
            auto& row = edges[key];
            row.a = static_cast<std::uint32_t>(key >> 32);
            row.b = static_cast<std::uint32_t>(key);
            row.feature = features.contains(key);
            row.incident.emplace_back(f, t[(e + 2) % 3]);
        }
    }
    return edges;
}
std::vector<std::vector<std::uint32_t>> neighbors_of(const Edges& edges, std::size_t count) {
    std::vector<std::vector<std::uint32_t>> neighbors(count);
    for (const auto& [key, edge] : edges) {
        neighbors[edge.a].push_back(edge.b);
        neighbors[edge.b].push_back(edge.a);
    }
    return neighbors;
}

// Exact P/UV duplicates remain separate indexed charts. Pinning them protects
// the existing seam; this builder never welds or changes its bandwidth fields.
void pin_duplicates(std::vector<Node>& nodes) {
    using Key = std::array<std::uint64_t, 5>;
    std::map<Key, std::vector<std::uint32_t>> groups;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        Key key{std::bit_cast<std::uint64_t>(n.neutral.x),
            std::bit_cast<std::uint64_t>(n.neutral.y), std::bit_cast<std::uint64_t>(n.neutral.z),
            std::bit_cast<std::uint64_t>(n.uv0.x), std::bit_cast<std::uint64_t>(n.uv0.y)};
        groups[key].push_back(i);
    }
    for (const auto& [key, ids] : groups) if (ids.size() > 1) {
        for (auto id : ids) {
            nodes[id].pinned = true;
            nodes[id].desired = nodes[id].reference;
        }
    }
}

void subdivide(std::vector<Node>& nodes, std::vector<Face>& faces,
        std::set<std::uint64_t>& features, double bound) {
    auto edges = edges_of(faces, features);
    auto neighbors = neighbors_of(edges, nodes.size());
    std::vector<Point> updated(nodes.size());
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        Point p = n.desired;
        if (!n.pinned && neighbors[i].size() >= 3) {
            double w = beta(neighbors[i].size());
            p *= 1.0 - static_cast<double>(neighbors[i].size()) * w;
            for (auto j : neighbors[i]) p += w * nodes[j].desired;
        }
        updated[i] = n.pinned ? n.reference : bounded(p, n.reference, bound);
    }
    std::map<std::uint64_t, std::uint32_t> mids;
    std::set<std::uint64_t> next_features;
    const std::size_t original_count = nodes.size();
    for (const auto& [key, edge] : edges) {
        const auto& a = nodes[edge.a];
        const auto& b = nodes[edge.b];
        Node n;
        n.neutral = .5 * (a.neutral + b.neutral);
        n.reference = .5 * (a.reference + b.reference);
        n.uv0 = .5 * (a.uv0 + b.uv0);
        n.uv1 = .5 * (a.uv1 + b.uv1);
        n.pinned = edge.feature || edge.incident.size() != 2;
        auto owner = edge.incident.front().first;
        n.source_parent = faces[owner].parent;
        for (int e = 0; e < 3; ++e) if (edge_key(faces[owner].nodes[e],
                faces[owner].nodes[(e + 1) % 3]) == key) {
            n.source_bary = .5 * (faces[owner].bary[e] + faces[owner].bary[(e + 1) % 3]);
        }
        Point p = n.reference;
        if (!n.pinned) p = .375 * (a.desired + b.desired) + .125 *
            (nodes[edge.incident[0].second].desired + nodes[edge.incident[1].second].desired);
        n.desired = n.pinned ? n.reference : bounded(p, n.reference, bound);
        auto id = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back(n);
        mids.emplace(key, id);
        if (edge.feature) {
            next_features.insert(edge_key(edge.a, id));
            next_features.insert(edge_key(id, edge.b));
        }
    }
    for (std::uint32_t i = 0; i < original_count; ++i) nodes[i].desired = updated[i];
    pin_duplicates(nodes);
    std::vector<Face> children;
    children.reserve(faces.size() * 4);
    for (const auto& f : faces) {
        auto [a, b, c] = f.nodes;
        auto ab = mids.at(edge_key(a, b));
        auto bc = mids.at(edge_key(b, c));
        auto ca = mids.at(edge_key(c, a));
        std::array<std::uint32_t, 6> ids{a, b, c, ab, bc, ca};
        std::array<Point, 6> bary{f.bary[0], f.bary[1], f.bary[2],
            .5 * (f.bary[0] + f.bary[1]), .5 * (f.bary[1] + f.bary[2]),
            .5 * (f.bary[2] + f.bary[0])};
        for (auto t : std::array<IndexTriangle, 4>{{{0, 3, 5}, {3, 1, 4},
                {5, 4, 2}, {3, 4, 5}}}) {
            children.push_back({{ids[t[0]], ids[t[1]], ids[t[2]]}, f.parent,
                {bary[t[0]], bary[t[1]], bary[t[2]]}});
        }
    }
    faces.swap(children);
    features.swap(next_features);
}

// Solving for common controls preserves native patch joins. Pins are eliminated
// from the symmetric positive definite free-vertex system, not clamped later.
std::vector<Point> inverse_controls(const std::vector<Node>& nodes,
        const std::vector<std::vector<std::uint32_t>>& neighbors) {
    std::vector<Point> result(nodes.size());
    std::vector<std::uint32_t> free;
    std::vector<double> diagonal(nodes.size());
    std::vector<Point> rhs(nodes.size());
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        result[i] = nodes[i].desired;
        if (nodes[i].pinned) continue;
        auto n = neighbors[i].size();
        if (n < 3) throw std::runtime_error("Unprotected vertex has unsupported valence");
        diagonal[i] = 3.0 / (8.0 * beta(n));
        Point sum{};
        for (auto j : neighbors[i]) sum += nodes[j].desired;
        rhs[i] = static_cast<double>(n) * nodes[i].desired - sum;
        free.push_back(i);
    }
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<double> x(nodes.size()), r(nodes.size()), p(nodes.size()), z(nodes.size()), ap(nodes.size());
        double rz = 0.0, b2 = 0.0;
        for (auto i : free) {
            r[i] = rhs[i][axis]; z[i] = r[i] / diagonal[i]; p[i] = z[i];
            rz += r[i] * z[i]; b2 += r[i] * r[i];
        }
        double tolerance = std::max(1e-24, b2 * 4e-24);
        bool converged = b2 <= tolerance;
        for (int iteration = 0; iteration < 300 && !converged; ++iteration) {
            double pap = 0.0;
            for (auto i : free) {
                ap[i] = diagonal[i] * p[i];
                for (auto j : neighbors[i]) if (!nodes[j].pinned) ap[i] += p[j];
                pap += p[i] * ap[i];
            }
            if (!(pap > 0.0) || !std::isfinite(pap)) throw std::runtime_error("Common-control solve lost positive definiteness");
            double a = rz / pap, residual2 = 0.0;
            for (auto i : free) { x[i] += a * p[i]; r[i] -= a * ap[i]; residual2 += r[i] * r[i]; }
            if (residual2 <= tolerance) { converged = true; break; }
            double next_rz = 0.0;
            for (auto i : free) { z[i] = r[i] / diagonal[i]; next_rz += r[i] * z[i]; }
            double b = next_rz / rz;
            for (auto i : free) p[i] = z[i] + b * p[i];
            rz = next_rz;
        }
        if (!converged) throw std::runtime_error("Common-control solve exceeded its iteration budget");
        for (auto i : free) result[i][axis] += x[i];
    }
    return result;
}

struct Primvar {
    Point p{};
    void Clear(void* /*unused*/ = nullptr) { p = {}; }
    void AddWithWeight(const Primvar& other, double weight) { p += weight * other.p; }
};
struct Frame { Point p{}, du{}, dv{}; };
struct ComponentRejected : std::runtime_error {
    BoundedDisplacementStatus reason;
    ComponentRejected(BoundedDisplacementStatus status, const char* message)
        : std::runtime_error(message), reason(status) {}
};
struct Patch {
    int array = 0, index = 0, type = 0;
    std::uint32_t face = 0;
    std::array<Point, 3> domain{};
};
struct FrozenPatch {
    Patch patch;
    Far::PatchParam parameter;
    std::array<std::uint32_t, 18> controls{};
    unsigned control_count = 0;
};
struct FrozenNativeSurface {
    std::vector<Point> controls;
    std::vector<FrozenPatch> patches;
};

// Application-side product rule for the rational Gregory triangle basis.
// The unmodified upstream position basis and common-control construction stay
// authoritative; upstream v3_6_1 omits these terms in its derivative arrays.
void gregory_derivatives(Far::PatchParam param, double u, double v,
        double* du, double* dv) {
    param.NormalizeTriangle(u, v);
    double bp[15]; Far::internal::EvalBasisBezierTri(u, v, bp);
    double q[3]{u + v, 1 - u, 1 - v};
    double ds[3]{}, dt[3]{};
    if (q[0] > 0) { ds[0] = v / (q[0] * q[0]); dt[0] = -u / (q[0] * q[0]); }
    if (q[1] > 0) { ds[1] = v / (q[1] * q[1]); dt[1] = 1 / q[1]; }
    if (q[2] > 0) { ds[2] = -1 / q[2]; dt[2] = -u / (q[2] * q[2]); }
    double scale = (param.IsTriangleRotated() ? -1.0 : 1.0) * (1 << param.GetDepth());
    constexpr int bezier[3]{6, 7, 10}, gregory[3]{3, 8, 13};
    for (int i = 0; i < 3; ++i) {
        double a = bp[bezier[i]] * ds[i] * scale;
        double b = bp[bezier[i]] * dt[i] * scale;
        du[gregory[i]] += a; du[gregory[i] + 1] -= a;
        dv[gregory[i]] += b; dv[gregory[i] + 1] -= b;
    }
}

class NativeSurface {
public:
    NativeSurface(const std::vector<Node>& nodes, const std::vector<Face>& faces,
            const std::vector<Point>& common) {
        std::vector<int> counts(faces.size(), 3), indices, corners, creases;
        indices.reserve(faces.size() * 3);
        for (const auto& f : faces) for (auto i : f.nodes) indices.push_back(static_cast<int>(i));
        for (int i = 0; std::cmp_less(i, nodes.size()); ++i) if (nodes[i].pinned) corners.push_back(i);
        for (const auto& [key, edge] : edges_of(faces, {})) if (edge.incident.size() == 1) {
            creases.push_back(static_cast<int>(edge.a)); creases.push_back(static_cast<int>(edge.b));
        }
        std::vector<float> corner_weights(corners.size(), Sdc::Crease::SHARPNESS_INFINITE);
        std::vector<float> crease_weights(creases.size() / 2, Sdc::Crease::SHARPNESS_INFINITE);
        Far::TopologyDescriptor d;
        d.numVertices = static_cast<int>(nodes.size()); d.numFaces = static_cast<int>(faces.size());
        d.numVertsPerFace = counts.data(); d.vertIndicesPerFace = indices.data();
        d.numCorners = static_cast<int>(corners.size()); d.cornerVertexIndices = corners.data();
        d.cornerWeights = corner_weights.data();
        d.numCreases = static_cast<int>(crease_weights.size()); d.creaseVertexIndexPairs = creases.data();
        d.creaseWeights = crease_weights.data();
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options options(Sdc::SCHEME_LOOP);
        options.schemeOptions.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        refiner_.reset(Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(d, options));
        if (!refiner_) throw std::runtime_error("OpenSubdiv rejected the indexed topology");
        Far::PatchTableFactory::Options po(4);
        po.SetPatchPrecision<double>(); po.useInfSharpPatch = true;
        po.generateLegacySharpCornerPatches = false; po.generateVaryingTables = false;
        po.endCapType = Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS;
        refiner_->RefineAdaptive(po.GetRefineAdaptiveOptions());
        table_.reset(Far::PatchTableFactory::Create(*refiner_, po));
        if (!table_) throw std::runtime_error("OpenSubdiv patch construction failed");
        int n = refiner_->GetNumVerticesTotal();
        controls_.resize(n + table_->GetNumLocalPoints());
        update_common(common);
        for (int array = 0; array < table_->GetNumPatchArrays(); ++array) {
            int type = table_->GetPatchArrayDescriptor(array).GetType();
            if (type != Far::PatchDescriptor::LOOP && type != Far::PatchDescriptor::GREGORY_TRIANGLE)
                throw std::runtime_error("Unsupported native patch descriptor");
            for (int index = 0; index < table_->GetNumPatches(array); ++index) {
                auto param = table_->GetPatchParam(array, index);
                Patch p{array, index, type, static_cast<std::uint32_t>(param.GetFaceId())};
                for (int k = 0; k < 3; ++k) {
                    double u = k == 1 ? 1.0 : 0.0, v = k == 2 ? 1.0 : 0.0;
                    param.UnnormalizeTriangle(u, v); p.domain[k] = {1 - u - v, u, v};
                }
                patches.push_back(p);
            }
        }
    }
    Frame evaluate(const Patch& p, Point b) const {
        auto param = table_->GetPatchParam(p.array, p.index);
        double w[20]{}, du[20]{}, dv[20]{};
        Far::internal::EvaluatePatchBasis(p.type, param, b.y, b.z, w, du, dv);
        if (p.type == Far::PatchDescriptor::GREGORY_TRIANGLE)
            gregory_derivatives(param, b.y, b.z, du, dv);
        auto ids = table_->GetPatchVertices(p.array, p.index);
        Frame frame;
        const Point origin = controls_[ids[0]].p;
        frame.p = origin;
        for (int i = 0; i < ids.size(); ++i) {
            Point delta = controls_[ids[i]].p - origin;
            frame.p += w[i] * delta; frame.du += du[i] * delta; frame.dv += dv[i] * delta;
        }
        return frame;
    }
    std::vector<Point> patch_controls(const Patch& p) const {
        std::vector<Point> out;
        for (auto id : table_->GetPatchVertices(p.array, p.index)) out.push_back(controls_[id].p);
        return out;
    }
    FrozenNativeSurface freeze(const std::vector<Face>& faces, const std::vector<double>& weights) const {
        FrozenNativeSurface out;
        std::vector<std::uint32_t> control_ids(controls_.size(), UINT32_MAX);
        for (const auto& p : patches) {
            const auto& t = faces[p.face].nodes;
            if (weights[t[0]] == 0 && weights[t[1]] == 0 && weights[t[2]] == 0) continue;
            FrozenPatch frozen;
            frozen.patch = p; frozen.parameter = table_->GetPatchParam(p.array, p.index);
            auto ids = table_->GetPatchVertices(p.array, p.index);
            frozen.control_count = static_cast<unsigned>(ids.size());
            for (unsigned k = 0; k < frozen.control_count; ++k) {
                auto id = static_cast<std::uint32_t>(ids[static_cast<int>(k)]);
                if (control_ids[id] == UINT32_MAX) {
                    control_ids[id] = static_cast<std::uint32_t>(out.controls.size());
                    out.controls.push_back(controls_[id].p);
                }
                frozen.controls[k] = control_ids[id];
            }
            out.patches.push_back(frozen);
        }
        return out;
    }
    void update_common(const std::vector<Point>& common) {
        for (std::size_t i = 0; i < common.size(); ++i) controls_[i].p = common[i];
        Far::PrimvarRefinerReal<double> primvar(*refiner_);
        Primvar* source = controls_.data();
        for (int level = 1; level < refiner_->GetNumLevels(); ++level) {
            auto* dest = source + refiner_->GetLevel(level - 1).GetNumVertices();
            primvar.Interpolate(level, source, dest); source = dest;
        }
        if (table_->GetNumLocalPoints()) table_->GetLocalPointStencilTable<double>()->UpdateValues(
            controls_.data(), controls_.data() + refiner_->GetNumVerticesTotal());
    }
    std::map<std::uint32_t, std::pair<double, double>> derivative_rows(const Patch& p, Point b) {
        if (!stencils_) {
            Far::StencilTableFactoryReal<double>::Options options;
            options.generateControlVerts = true; options.generateOffsets = true;
            std::unique_ptr<const Far::StencilTableReal<double>> base(
                Far::StencilTableFactoryReal<double>::Create(*refiner_, options));
            stencils_.reset(Far::StencilTableFactoryReal<double>::AppendLocalPointStencilTable(
                *refiner_, base.get(), table_->GetLocalPointStencilTable<double>(), true));
            if (!stencils_ || stencils_->GetNumStencils() != static_cast<int>(controls_.size()))
                throw std::runtime_error("Native common-control stencil index mismatch");
        }
        auto param = table_->GetPatchParam(p.array, p.index);
        double w[20]{}, du[20]{}, dv[20]{};
        Far::internal::EvaluatePatchBasis(p.type, param, b.y, b.z, w, du, dv);
        if (p.type == Far::PatchDescriptor::GREGORY_TRIANGLE) gregory_derivatives(param, b.y, b.z, du, dv);
        std::map<std::uint32_t, std::pair<double, double>> rows;
        auto ids = table_->GetPatchVertices(p.array, p.index);
        for (int i = 0; i < ids.size(); ++i) {
            auto stencil = stencils_->GetStencil(ids[i]);
            for (int j = 0; j < stencil.GetSize(); ++j) {
                auto& row = rows[static_cast<std::uint32_t>(stencil.GetVertexIndices()[j])];
                row.first += du[i] * stencil.GetWeights()[j]; row.second += dv[i] * stencil.GetWeights()[j];
            }
        }
        return rows;
    }
    std::vector<Patch> patches;
private:
    std::unique_ptr<Far::TopologyRefiner> refiner_;
    std::unique_ptr<Far::PatchTable> table_;
    std::vector<Primvar> controls_;
    std::unique_ptr<const Far::StencilTableReal<double>> stencils_;
};

void repair_common_controls(NativeSurface& native, std::vector<Point>& common,
        const std::vector<Node>& nodes, const std::vector<Face>& faces,
        const std::vector<std::vector<std::uint32_t>>& neighbors, const std::vector<double>& weights,
        BoundedDisplacementMetrics& metrics, std::uint64_t maximum_evaluations) {
    const auto original = common;
    struct Witness {
        std::map<std::uint32_t, std::pair<double, double>> rows;
        Point axis{}; bool gregory = false;
    };
    std::vector<Point> samples(kSamples.begin(), kSamples.end());
    for (int c = 0; c <= 4; ++c) for (int b = 0; b <= 4 - c; ++b)
        samples.emplace_back((4 - b - c) / 4.0, b / 4.0, c / 4.0);
    for (int iteration = 0; iteration < 32; ++iteration) {
        std::vector<Witness> witnesses;
        for (const auto& p : native.patches) {
            const auto& t = faces[p.face].nodes;
            if (weights[t[0]] == 0 && weights[t[1]] == 0 && weights[t[2]] == 0) continue;
            Point axis = cross(nodes[t[1]].reference - nodes[t[0]].reference,
                nodes[t[2]].reference - nodes[t[0]].reference);
            axis /= length(axis);
            bool gregory = p.type == Far::PatchDescriptor::GREGORY_TRIANGLE;
            for (auto b : samples) {
                if (metrics.patch_evaluations >= maximum_evaluations)
                    throw std::runtime_error("Common orientation evaluation budget exhausted");
                ++metrics.patch_evaluations;
                Point chart = affine(p.domain, b); auto f = native.evaluate(p, chart);
                Point normal = cross(f.du, f.dv); double size = length(normal);
                double cosine = size > 1e-24 ? dot(axis, normal) / size : -1;
                if (cosine >= (gregory ? .12 : .025)) continue;
                auto rows = native.derivative_rows(p, chart);
                // The regular projection is the ordinary unprotected Loop
                // control stage. Native protected corners stay fixed and their
                // boundary target is handled by the later common band.
                if (!gregory && std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
                        return nodes[row.first].pinned || neighbors[row.first].size() != 6;
                    })) continue;
                witnesses.push_back({std::move(rows), axis, gregory});
            }
        }
        if (witnesses.empty()) break;
        bool improved = false;
        for (const auto& w : witnesses) {
            auto state = [&] {
                Frame f; Point origin = common[w.rows.begin()->first];
                for (const auto& [id, coefficient] : w.rows) {
                    Point p = common[id] - origin; f.du += coefficient.first * p; f.dv += coefficient.second * p;
                }
                return f;
            };
            auto f = state(); Point n = cross(f.du, f.dv); double size = length(n);
            if (!(size > 1e-24)) throw std::runtime_error("Degenerate common-control orientation witness");
            double before = dot(w.axis, n) / size;
            double threshold = w.gregory ? .12 : .025, goal = w.gregory ? .15 : .035;
            if (before >= threshold) continue;
            Point tangent = w.gregory ? (w.axis - before * n / size) / size : w.axis;
            std::map<std::uint32_t, Point> gradient; double square = 0;
            for (const auto& [id, coefficient] : w.rows) if (!nodes[id].pinned) {
                auto g = coefficient.first * cross(f.dv, tangent) + coefficient.second * cross(tangent, f.du);
                gradient.emplace(id, g); square += dot(g, g);
            }
            if (!(square > 1e-30)) throw std::runtime_error("Common orientation infeasible under protected pins");
            double step = w.gregory ? (goal - before) / square : (goal - before) * size / square;
            std::map<std::uint32_t, Point> saved;
            for (const auto& [id, g] : gradient) saved[id] = common[id];
            bool accepted = false;
            for (int half = 0; half < 24; ++half) {
                for (const auto& [id, g] : gradient) common[id] = saved.at(id) + step * g;
                auto next = state(); auto nn = cross(next.du, next.dv); double l = length(nn);
                if (l > 1e-24 && dot(w.axis, nn) / l > before) { accepted = true; improved = true; break; }
                step *= .5;
            }
            if (!accepted) {
                for (const auto& [id, p] : saved) common[id] = p;
                throw std::runtime_error("Common orientation backtracking made no progress");
            }
        }
        if (!improved) throw std::runtime_error("Common orientation projection stalled");
        native.update_common(common);
        if (iteration == 31) throw std::runtime_error("Common orientation projection exceeded its iteration budget");
    }
    for (std::size_t i = 0; i < common.size(); ++i) {
        double delta = length(common[i] - original[i]);
        if (delta > 0) ++metrics.common_control_adjustments;
        metrics.maximum_common_control_adjustment = std::max(metrics.maximum_common_control_adjustment, delta);
    }
}

struct Target {
    const NativeSurface& native;
    const std::vector<Node>& nodes;
    const std::vector<Face>& faces;
    const std::vector<double>& weights;
    BoundedDisplacementMetrics& metrics;
    std::uint64_t maximum_evaluations;

    Frame evaluate(std::uint32_t patch, Point b) const {
        if (metrics.patch_evaluations >= maximum_evaluations)
            throw std::runtime_error("Patch evaluation work budget exhausted");
        ++metrics.patch_evaluations;
        const auto& p = native.patches[patch];
        const auto& t = faces[p.face].nodes;
        std::array<Point, 3> ref{nodes[t[0]].reference, nodes[t[1]].reference, nodes[t[2]].reference};
        double w = b.x * weights[t[0]] + b.y * weights[t[1]] + b.z * weights[t[2]];
        Point ru = ref[1] - ref[0], rv = ref[2] - ref[0];
        Point q0 = affine(ref, b);
        if (w == 0.0 && weights[t[0]] == 0.0 && weights[t[1]] == 0.0 && weights[t[2]] == 0.0)
            return {q0, ru, rv};
        Frame f = native.evaluate(p, b);
        Point delta = f.p - q0;
        double wu = weights[t[1]] - weights[t[0]], wv = weights[t[2]] - weights[t[0]];
        return {q0 + w * delta, ru + w * (f.du - ru) + wu * delta,
            rv + w * (f.dv - rv) + wv * delta};
    }
    Point source_bary(std::uint32_t patch, Point b) const {
        return affine(faces[native.patches[patch].face].bary, b);
    }
    Point reference(std::uint32_t patch, Point b) const {
        const auto& t = faces[native.patches[patch].face].nodes;
        return affine({nodes[t[0]].reference, nodes[t[1]].reference, nodes[t[2]].reference}, b);
    }
};

// Sampling charts use the original parent domains. Native patch boundaries are
// evaluation detail, not mandatory raster edges: a coarse cell may span several
// regular native patches while retaining the original affine correspondence.
class SourceTarget {
public:
    SourceTarget(std::vector<Node> local_nodes, std::vector<Face> local_faces,
            std::vector<double> local_weights, FrozenNativeSurface surface,
            std::span<const IndexTriangle> source_triangles,
            std::span<const Point> source_positions, BoundedDisplacementMetrics& counters,
            std::uint64_t evaluation_limit)
        : nodes(std::move(local_nodes)), faces(std::move(local_faces)), weights(std::move(local_weights)),
          native(std::move(surface)), triangles(source_triangles), original(source_positions),
          metrics(counters), maximum_evaluations(evaluation_limit) {
        face_charts.reserve(faces.size()); face_patches.resize(faces.size());
        for (std::uint32_t i = 0; i < faces.size(); ++i) {
            face_charts.push_back(glm::inverse(glm::dmat3(faces[i].bary[0], faces[i].bary[1], faces[i].bary[2])));
            parent_faces[faces[i].parent].push_back(i);
        }
        patch_charts.reserve(native.patches.size());
        for (std::uint32_t i = 0; i < native.patches.size(); ++i) {
            const auto& p = native.patches[i].patch;
            patch_charts.push_back(glm::inverse(glm::dmat3(p.domain[0], p.domain[1], p.domain[2])));
            face_patches[p.face].push_back(i);
        }
        for (const auto& entry : parent_faces) { parents.push_back(entry.first); }
        std::sort(parents.begin(), parents.end());
    }

    Point reference(std::uint32_t parent, Point b) const {
        const auto& t = triangles[parent];
        return affine({original[t[0]], original[t[1]], original[t[2]]}, b);
    }
    Frame evaluate(std::uint32_t parent, Point b, Point interior = Point(-1)) const {
        if (metrics.patch_evaluations >= maximum_evaluations)
            throw std::runtime_error("Source-chart evaluation work budget exhausted");
        ++metrics.patch_evaluations;
        // A C0 protected-band boundary has two legitimate one-sided normals.
        // Choose the limiting chart toward this cell's interior, while still
        // evaluating the exact requested position and derivatives at b.
        Point owner = interior.x >= 0 ? b + 1e-6 * (interior - b) : b;
        std::uint32_t fi = UINT32_MAX; Point chart{}, owner_chart{};
        for (auto candidate : parent_faces.at(parent)) {
            auto actual = face_charts[candidate] * b;
            if (std::min({actual.x, actual.y, actual.z}) < -1e-12) continue;
            auto local = face_charts[candidate] * owner;
            if (fi == UINT32_MAX) { fi = candidate; chart = actual; owner_chart = local; }
            if (std::min({local.x, local.y, local.z}) >= -1e-12) {
                fi = candidate; chart = actual; owner_chart = local; break;
            }
        }
        if (fi == UINT32_MAX) throw std::runtime_error("Source point has no original subdivision chart");
        const auto& t = faces[fi].nodes;
        std::array<Point, 3> ref{nodes[t[0]].reference, nodes[t[1]].reference, nodes[t[2]].reference};
        double w = chart.x * weights[t[0]] + chart.y * weights[t[1]] + chart.z * weights[t[2]];
        Point ru = ref[1] - ref[0], rv = ref[2] - ref[0];
        Frame f{affine(ref, chart), ru, rv};
        if (weights[t[0]] != 0 || weights[t[1]] != 0 || weights[t[2]] != 0) {
            std::uint32_t pi = UINT32_MAX;
            for (auto candidate : face_patches[fi]) {
                auto actual = patch_charts[candidate] * chart;
                if (std::min({actual.x, actual.y, actual.z}) < -1e-12) continue;
                auto local = patch_charts[candidate] * owner_chart;
                if (pi == UINT32_MAX) pi = candidate;
                if (std::min({local.x, local.y, local.z}) >= -1e-12) { pi = candidate; break; }
            }
            if (pi == UINT32_MAX) throw std::runtime_error("Source point has no native patch");
            const auto& p = native.patches[pi];
            double basis[20]{}, du[20]{}, dv[20]{};
            Far::internal::EvaluatePatchBasis(p.patch.type, p.parameter, chart.y, chart.z, basis, du, dv);
            if (p.patch.type == Far::PatchDescriptor::GREGORY_TRIANGLE)
                gregory_derivatives(p.parameter, chart.y, chart.z, du, dv);
            Frame smooth; smooth.p = native.controls[p.controls[0]];
            const auto origin = smooth.p;
            for (unsigned k = 0; k < p.control_count; ++k) {
                auto delta = native.controls[p.controls[k]] - origin;
                smooth.p += basis[k] * delta; smooth.du += du[k] * delta; smooth.dv += dv[k] * delta;
            }
            auto delta = smooth.p - f.p;
            f = {f.p + w * delta,
                ru + w * (smooth.du - ru) + (weights[t[1]] - weights[t[0]]) * delta,
                rv + w * (smooth.dv - rv) + (weights[t[2]] - weights[t[0]]) * delta};
        }
        // Chain native face derivatives into the original parent coordinates.
        auto bu = face_charts[fi] * Point(-1, 1, 0), bv = face_charts[fi] * Point(-1, 0, 1);
        return {f.p, bu.y * f.du + bu.z * f.dv, bv.y * f.du + bv.z * f.dv};
    }
    std::uint64_t working_bytes() const {
        return nodes.size() * sizeof(Node) + faces.size() * sizeof(Face) + weights.size() * sizeof(double)
            + native.controls.size() * sizeof(Point) + native.patches.size() * sizeof(FrozenPatch)
            + (face_charts.size() + patch_charts.size()) * sizeof(glm::dmat3)
            + native.patches.size() * sizeof(std::uint32_t) + faces.size() * sizeof(std::uint32_t);
    }

    std::vector<Node> nodes;
    std::vector<Face> faces;
    std::vector<double> weights;
    FrozenNativeSurface native;
    std::span<const IndexTriangle> triangles;
    std::span<const Point> original;
    std::vector<glm::dmat3> face_charts, patch_charts;
    std::vector<std::vector<std::uint32_t>> face_patches;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parent_faces;
    std::vector<std::uint32_t> parents;
    BoundedDisplacementMetrics& metrics;
    std::uint64_t maximum_evaluations;
};

struct Exponent { int a, b, c; };
std::vector<Exponent> exponents(int degree) {
    std::vector<Exponent> out;
    for (int c = 0; c <= degree; ++c) for (int b = 0; b <= degree - c; ++b)
        out.push_back({degree - b - c, b, c});
    return out;
}
double factorial(int n) { double v = 1; for (int i = 2; i <= n; ++i) v *= i; return v; }
double multinomial(Exponent e) { return factorial(e.a + e.b + e.c) / (factorial(e.a) * factorial(e.b) * factorial(e.c)); }
double bernstein(Exponent e, Point b) {
    return multinomial(e) * std::pow(b.x, e.a) * std::pow(b.y, e.b) * std::pow(b.z, e.c);
}
std::size_t exponent_index(const std::vector<Exponent>& net, Exponent e) {
    for (std::size_t i = 0; i < net.size(); ++i)
        if (net[i].a == e.a && net[i].b == e.b && net[i].c == e.c) return i;
    throw std::runtime_error("Invalid Bernstein exponent");
}

// The quartic collocation matrix is constant. Inverting it once recovers exact
// polynomial controls to double precision; the conservative bound includes a
// roundoff allowance and never silently clips independently constructed patches.
const std::array<std::array<double, 15>, 15>& quartic_inverse() {
    static const auto inverse = [] {
        auto e = exponents(4);
        std::array<std::array<double, 30>, 15> a{};
        for (int i = 0; i < 15; ++i) {
            Point b(e[i].a / 4.0, e[i].b / 4.0, e[i].c / 4.0);
            for (int j = 0; j < 15; ++j) a[i][j] = bernstein(e[j], b);
            a[i][i + 15] = 1;
        }
        for (int k = 0; k < 15; ++k) {
            int pivot = k;
            for (int i = k + 1; i < 15; ++i) if (std::abs(a[i][k]) > std::abs(a[pivot][k])) pivot = i;
            std::swap(a[k], a[pivot]);
            double divisor = a[k][k];
            if (std::abs(divisor) < 1e-14) throw std::runtime_error("Singular quartic collocation matrix");
            for (auto& v : a[k]) v /= divisor;
            for (int i = 0; i < 15; ++i) if (i != k) {
                double w = a[i][k];
                for (int j = 0; j < 30; ++j) a[i][j] -= w * a[k][j];
            }
        }
        std::array<std::array<double, 15>, 15> out{};
        for (int i = 0; i < 15; ++i) for (int j = 0; j < 15; ++j) out[i][j] = a[i][j + 15];
        return out;
    }();
    return inverse;
}

double patch_bound(const Target& target, std::uint32_t id) {
    const auto& patch = target.native.patches[id];
    const auto& t = target.faces[patch.face].nodes;
    bool zero = target.weights[t[0]] == 0 && target.weights[t[1]] == 0 && target.weights[t[2]] == 0;
    if (zero) return 0.0;
    auto e = exponents(4);
    std::array<Point, 15> sample{}, delta{};
    const Point origin = target.nodes[t[0]].reference;
    for (int i = 0; i < 15; ++i) {
        Point b(e[i].a / 4.0, e[i].b / 4.0, e[i].c / 4.0);
        Point chart = affine(patch.domain, b);
        if (target.metrics.patch_evaluations >= target.maximum_evaluations)
            throw std::runtime_error("Patch evaluation work budget exhausted");
        ++target.metrics.patch_evaluations;
        sample[i] = target.native.evaluate(patch, chart).p - origin;
    }
    if (patch.type == Far::PatchDescriptor::LOOP) {
        for (int i = 0; i < 15; ++i) {
            for (int j = 0; j < 15; ++j) delta[i] += quartic_inverse()[i][j] * sample[j];
            Point b(e[i].a / 4.0, e[i].b / 4.0, e[i].c / 4.0);
            delta[i] -= target.reference(id, affine(patch.domain, b)) - origin;
        }
        // Multiplying the quartic correction by common affine W gives a quintic
        // Bernstein net. Its convex hull bounds the entire protected band patch.
        double bound = 0;
        auto elevated = exponents(5);
        double w[3]{};
        for (int k = 0; k < 3; ++k) {
            auto b = patch.domain[k];
            w[k] = b.x * target.weights[t[0]] + b.y * target.weights[t[1]] + b.z * target.weights[t[2]];
        }
        for (auto alpha : elevated) {
            int powers[3]{alpha.a, alpha.b, alpha.c}; Point correction{};
            for (int k = 0; k < 3; ++k) if (powers[k]) {
                Exponent lower = alpha;
                if (k == 0) --lower.a; else if (k == 1) --lower.b; else --lower.c;
                correction += (powers[k] / 5.0) * w[k] * delta[exponent_index(e, lower)];
            }
            bound = std::max(bound, length(correction));
        }
        return bound + 1e-11;
    }
    // Gregory's 18 basis functions are nonnegative and partition unity. An
    // affine reference is reproduced by duplicating its three rational-pair
    // Bernstein controls. Retrieve the native control net rather than fitting
    // rational positions with a polynomial.
    constexpr int mapping[18]{0, 1, 5, 6, 6, 4, 8, 3, 7, 7, 14, 12, 13, 10, 10, 2, 11, 9};
    auto controls = target.native.patch_controls(patch);
    if (controls.size() != 18) throw std::runtime_error("Unexpected Gregory control count");
    double max_weight = 0;
    for (auto b : patch.domain) max_weight = std::max(max_weight,
        b.x * target.weights[t[0]] + b.y * target.weights[t[1]] + b.z * target.weights[t[2]]);
    double bound = 0;
    for (int i = 0; i < 18; ++i) {
        auto power = e[mapping[i]];
        Point b(power.a / 4.0, power.b / 4.0, power.c / 4.0);
        bound = std::max(bound, length(controls[i] - target.reference(id, affine(patch.domain, b))));
    }
    return max_weight * bound + 1e-11;
}

constexpr std::int64_t kChartDenominator = 1ll << 28;
struct NodeKey {
    std::array<std::uint32_t, 3> ids{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::array<std::uint32_t, 3> weights{};
    auto operator<=>(const NodeKey&) const = default;
};
NodeKey chart_key(const IndexTriangle& t, Point b, const std::vector<std::uint32_t>& global_ids) {
    std::array<std::pair<std::uint32_t, std::uint32_t>, 3> pairs{};
    auto y = std::llround(b.y * kChartDenominator), z = std::llround(b.z * kChartDenominator);
    std::int64_t weights[3]{kChartDenominator - y - z, y, z};
    for (int i = 0; i < 3; ++i) {
        if (weights[i] < 0 || weights[i] > kChartDenominator ||
                std::abs(b[i] - static_cast<double>(weights[i]) / double(kChartDenominator)) > 1e-12)
            throw std::runtime_error("Native sampling point is not in the dyadic source chart");
        pairs[i] = {global_ids[t[i]], static_cast<std::uint32_t>(weights[i])};
    }
    std::sort(pairs.begin(), pairs.end());
    NodeKey key; int used = 0;
    for (auto [id, weight] : pairs) if (weight) { key.ids[used] = id; key.weights[used++] = weight; }
    return key;
}

struct Cell {
    IndexTriangle nodes{};
    std::uint32_t patch = 0;
    std::array<Point, 3> domain{};
    std::uint32_t depth = 0;
    double position_error = 0, normal_error = 0, area = 0, priority = 0;
    bool quality_valid = false;
};

double chart_area(Point a, Point b, Point c) {
    return (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
}

class SourceSampler {
public:
    SourceSampler(const SourceTarget& target,
            BoundedDisplacementSurface& output, const BoundedDisplacementPolicy& policy)
        : target_(target), output_(output), policy_(policy) {
        source_ids_.resize(target.original.size());
        std::iota(source_ids_.begin(), source_ids_.end(), 0);
    }

    std::vector<Cell> initial_cells() {
        std::vector<Cell> cells; cells.reserve(target_.parents.size());
        for (auto parent : target_.parents) {
            IndexTriangle ids{};
            for (int k = 0; k < 3; ++k) ids[k] = assign(parent, kSamples[k]);
            cells.push_back({ids, parent, {kSamples[0], kSamples[1], kSamples[2]}});
        }
        return cells;
    }

    void quality(Cell& c) {
        if (c.quality_valid) return;
        const auto& a = output_.nodes[c.nodes[0]].target_position;
        const auto& b = output_.nodes[c.nodes[1]].target_position;
        const auto& d = output_.nodes[c.nodes[2]].target_position;
        Point normal = cross(b - a, d - a); double size = length(normal);
        c.area = .5 * size; c.position_error = 0; c.normal_error = 0;
        if (!(size > 1e-24)) { c.priority = std::numeric_limits<double>::infinity(); return; }
        normal /= size;
        std::array<Point, 3> q{a, b, d};
        Point interior = affine(c.domain, kSamples[6]);
        for (auto sample : kSamples) {
            auto chart = affine(c.domain, sample); auto f = target_.evaluate(c.patch, chart, interior);
            Point analytic = cross(f.du, f.dv); double n = length(analytic);
            c.position_error = std::max(c.position_error, length(f.p - affine(q, sample)));
            double angle = n > 1e-24 ? std::acos(std::clamp(dot(normal, analytic / n), -1.0, 1.0))
                * 180.0 / std::numbers::pi : 180.0;
            c.normal_error = std::max(c.normal_error, angle);
        }
        double score = std::max(c.position_error / policy_.position_error,
            c.normal_error / policy_.normal_error_degrees);
        c.priority = c.area * score * score;
        c.quality_valid = true;
    }


    bool guard_winding(std::vector<Cell>& cells, std::size_t budget) {
        for (std::uint32_t pass = 0; pass <= policy_.maximum_sample_depth; ++pass) {
            std::set<std::uint64_t> requested;
            std::map<std::uint64_t, std::uint32_t> incidence;
            for (const auto& c : cells) {
                for (int e = 0; e < 3; ++e) ++incidence[edge_key(c.nodes[e], c.nodes[(e + 1) % 3])];
                std::array<Point, 3> old{}, q{};
                for (int k = 0; k < 3; ++k) {
                    old[k] = target_.reference(c.patch, c.domain[k]);
                    q[k] = output_.nodes[c.nodes[k]].target_position;
                }
                Point before = cross(old[1] - old[0], old[2] - old[0]);
                Point after = cross(q[1] - q[0], q[2] - q[0]);
                if (length(after) > 1e-24 && dot(before, after) >
                        64 * std::numeric_limits<double>::epsilon() * length(before) * length(after)) continue;
                if (c.depth >= policy_.maximum_sample_depth) return false;
                for (int e = 0; e < 3; ++e) requested.insert(edge_key(c.nodes[e], c.nodes[(e + 1) % 3]));
            }
            if (requested.empty()) return true;
            std::size_t added = 0;
            for (auto edge : requested) added += incidence.at(edge);
            if (cells.size() + added > budget || pass == policy_.maximum_sample_depth) return false;
            split(cells, requested, added);
        }
        return false;
    }

    void split(std::vector<Cell>& cells, const std::set<std::uint64_t>& requested, std::size_t added) {
        std::map<std::uint64_t, std::uint32_t> mid;
        std::vector<Cell> next; next.reserve(cells.size() + added);
        for (const auto& c : cells) {
            unsigned mask = 0; std::array<std::uint32_t, 6> ids{};
            std::copy(c.nodes.begin(), c.nodes.end(), ids.begin());
            std::array<Point, 6> chart{c.domain[0], c.domain[1], c.domain[2],
                .5 * (c.domain[0] + c.domain[1]), .5 * (c.domain[1] + c.domain[2]),
                .5 * (c.domain[2] + c.domain[0])};
            for (int e = 0; e < 3; ++e) {
                auto key = edge_key(c.nodes[e], c.nodes[(e + 1) % 3]);
                if (!requested.contains(key)) continue;
                mask |= 1u << e;
                auto found = mid.find(key);
                if (found == mid.end()) found = mid.emplace(key, assign(c.patch, chart[e + 3])).first;
                else check_shared(found->second, c.patch, chart[e + 3]);
                ids[e + 3] = found->second;
            }
            if (!mask) { next.push_back(c); continue; }
            for (auto t : pattern(mask)) {
                Cell child{{ids[t[0]], ids[t[1]], ids[t[2]]}, c.patch,
                    {chart[t[0]], chart[t[1]], chart[t[2]]}, c.depth + 1};
                next.push_back(child);
            }
        }
        if (next.size() != cells.size() + added) throw std::runtime_error("Conforming split budget accounting failed");
        cells.swap(next);
    }
    static std::vector<IndexTriangle> pattern(unsigned mask) {
        if (mask == 7) return {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
        int count = std::popcount(mask);
        if (count == 1) {
            int e = std::countr_zero(mask), j = (e + 1) % 3, k = (e + 2) % 3;
            return {{{std::uint32_t(e), std::uint32_t(3 + e), std::uint32_t(k)}},
                {{std::uint32_t(3 + e), std::uint32_t(j), std::uint32_t(k)}}};
        }
        int a = 0; while (mask & (1u << a)) ++a;
        int b = (a + 1) % 3, c = (a + 2) % 3;
        int ma = 3 + c, mb = 3 + b;
        return {{{std::uint32_t(c), std::uint32_t(ma), std::uint32_t(mb)}},
            {{std::uint32_t(a), std::uint32_t(b), std::uint32_t(mb)}},
            {{std::uint32_t(a), std::uint32_t(mb), std::uint32_t(ma)}}};
    }
    void check_shared(std::uint32_t id, std::uint32_t patch, Point chart) const {
        auto p = target_.evaluate(patch, chart).p;
        if (length(p - output_.nodes[id].target_position) > 1e-9)
            throw std::runtime_error("Native shared-node position mismatch");
    }
    std::uint32_t assign(std::uint32_t parent, Point chart) {
        auto key = chart_key(target_.triangles[parent], chart, source_ids_);
        auto found = points_.find(key);
        if (found != points_.end()) { check_shared(found->second, parent, chart); return found->second; }
        auto frame = target_.evaluate(parent, chart); Point reference = target_.reference(parent, chart);
        BoundedDisplacementNode n{reference, frame.p, frame.p - reference, parent, chart};
        std::uint32_t id = static_cast<std::uint32_t>(output_.nodes.size());
        if (key.weights[0] == kChartDenominator) {
            id = key.ids[0]; output_.nodes[id] = n;
        } else output_.nodes.push_back(n);
        points_.emplace(key, id); return id;
    }
private:
    const SourceTarget& target_;
    BoundedDisplacementSurface& output_;
    const BoundedDisplacementPolicy& policy_;
    std::vector<std::uint32_t> source_ids_;
    std::map<NodeKey, std::uint32_t> points_;
};

struct SampleWork {
    std::unique_ptr<SourceTarget> target;
    std::unique_ptr<SourceSampler> sampler;
    std::vector<Cell> cells;
    double bound = 0;
    std::uint64_t regular_patches = 0, extraordinary_patches = 0;
    bool active = true;
};

// All components enter one geometry-error queue. Work is not allocated by
// source order or a separate per-component area quota.
template <typename Reject>
bool refine_global(std::vector<SampleWork>& work, std::size_t current_total,
        std::size_t budget, const BoundedDisplacementPolicy& policy,
        BoundedDisplacementMetrics& metrics, Reject&& reject) {
    struct Candidate { std::size_t component, cell; double priority; };
    std::vector<Candidate> order;
    std::vector<std::map<std::uint64_t, std::uint32_t>> incidence(work.size());
    for (std::size_t ci = 0; ci < work.size(); ++ci) if (work[ci].active) {
        auto& w = work[ci];
        auto before = order.size();
        try {
            for (std::size_t i = 0; i < w.cells.size(); ++i) {
                auto& c = w.cells[i]; w.sampler->quality(c);
                for (int e = 0; e < 3; ++e) ++incidence[ci][edge_key(c.nodes[e], c.nodes[(e + 1) % 3])];
                if ((c.position_error > policy.position_error || c.normal_error > policy.normal_error_degrees ||
                        c.area <= 1e-24) && c.depth < policy.maximum_sample_depth)
                    order.push_back({ci, i, c.priority});
            }
        } catch (const std::bad_alloc&) { throw; }
        catch (const std::exception& e) {
            current_total -= w.cells.size() - w.target->parents.size();
            order.resize(before); incidence[ci].clear();
            reject(w, BoundedDisplacementStatus::NumericalLimit, e.what());
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.priority > b.priority;
    });
    std::vector<std::set<std::uint64_t>> requested(work.size());
    std::vector<std::size_t> additions(work.size());
    std::size_t added = 0, requested_edges = 0;
    for (const auto& candidate : order) {
        auto ci = candidate.component; const auto& c = work[ci].cells[candidate.cell];
        std::array<std::uint64_t, 3> keys{}; std::size_t cost = 0, new_edges = 0;
        for (int e = 0; e < 3; ++e) {
            keys[e] = edge_key(c.nodes[e], c.nodes[(e + 1) % 3]);
            if (!requested[ci].contains(keys[e])) { cost += incidence[ci].at(keys[e]); ++new_edges; }
        }
        if (current_total + added + cost > budget) break;
        if (metrics.patch_evaluations + 14 * (added + cost) + 4 * (requested_edges + new_edges)
                > policy.maximum_patch_evaluations) break;
        added += cost; additions[ci] += cost; requested_edges += new_edges;
        requested[ci].insert(keys.begin(), keys.end());
    }
    if (!added) return false;
    for (std::size_t ci = 0; ci < work.size(); ++ci) if (!requested[ci].empty()) {
        try { work[ci].sampler->split(work[ci].cells, requested[ci], additions[ci]); }
        catch (const std::bad_alloc&) { throw; }
        catch (const std::exception& e) { reject(work[ci], BoundedDisplacementStatus::NumericalLimit, e.what()); }
    }
    return true;
}

void guard_initial_nodes(std::vector<Node>& nodes, std::span<const Face> faces, double limit) {
    std::vector<bool> bad(nodes.size());
    for (int iteration = 0; iteration < 64; ++iteration) {
        std::fill(bad.begin(), bad.end(), false); bool failed = false;
        // Generic startup states contain no light, camera or asset information.
        // The final native target is validated separately; these states only
        // protect the bounded Loop seed against obvious orientation reversal.
        for (double factor : {0.0, .5, 1.0, 2.0}) for (double fade : {0.0, .25, .5, .75, 1.0}) {
            for (const auto& f : faces) {
                std::array<Point, 3> old{}, q{};
                for (int k = 0; k < 3; ++k) {
                    const auto& n = nodes[f.nodes[k]];
                    old[k] = n.neutral + factor * fade * (n.reference - n.neutral);
                    Point delta = bounded(factor * (n.desired - n.reference), Point{}, limit);
                    q[k] = old[k] + fade * delta;
                }
                Point a = cross(old[1] - old[0], old[2] - old[0]);
                Point b = cross(q[1] - q[0], q[2] - q[0]);
                if (dot(a, b) <= 0 || length(b) <= 2e-14) {
                    failed = true; for (auto i : f.nodes) bad[i] = true;
                }
            }
        }
        if (!failed) return;
        for (std::uint32_t i = 0; i < nodes.size(); ++i) if (bad[i])
            nodes[i].desired = .5 * (nodes[i].reference + nodes[i].desired);
    }
    throw std::runtime_error("Bounded Loop seed could not preserve sampled state orientation");
}

std::vector<double> protected_weights(const std::vector<Node>& nodes,
        const std::vector<std::vector<std::uint32_t>>& neighbors, const BoundedDisplacementPolicy& policy) {
    std::vector<std::uint32_t> distance(nodes.size(), UINT32_MAX);
    std::deque<std::uint32_t> queue;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) if (nodes[i].pinned) { distance[i] = 0; queue.push_back(i); }
    while (!queue.empty()) {
        auto i = queue.front(); queue.pop_front();
        for (auto j : neighbors[i]) if (distance[j] == UINT32_MAX) {
            distance[j] = distance[i] + 1; queue.push_back(j);
        }
    }
    std::vector<double> weights(nodes.size());
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        double t = distance[i] == UINT32_MAX ? 1.0 : std::clamp(
            (double(distance[i]) - policy.protected_zero_rings) /
            double(policy.protected_full_rings - policy.protected_zero_rings), 0.0, 1.0);
        weights[i] = t * t * t * (10.0 + t * (-15.0 + 6.0 * t));
    }
    return weights;
}

void validate_input(const BoundedDisplacementInput& input, const BoundedDisplacementPolicy& p) {
    if (input.vertices.empty() || input.triangles.empty() || input.vertices.size() > INT32_MAX ||
            input.triangles.size() > INT32_MAX / 3) throw std::runtime_error("Empty or oversized displacement surface");
    if (!std::isfinite(input.uniform_scale) || input.uniform_scale <= 0 ||
            !std::isfinite(input.reference_amplitude) || input.reference_amplitude < 0)
        throw std::runtime_error("Invalid displacement metric or amplitude");
    if (!std::isfinite(p.correspondence_bound) || p.correspondence_bound <= 0 ||
            !std::isfinite(p.initial_node_bound) || p.initial_node_bound <= 0 ||
            p.initial_node_bound > p.correspondence_bound || !std::isfinite(p.position_error) || p.position_error <= 0 ||
            !std::isfinite(p.normal_error_degrees) || p.normal_error_degrees <= 0 || p.normal_error_degrees >= 90 ||
            !std::isfinite(p.neutral_feature_degrees) || p.neutral_feature_degrees <= 0 || p.neutral_feature_degrees >= 180 ||
            !std::isfinite(p.common_control_blend) || p.common_control_blend < 0 || p.common_control_blend > 1 ||
            p.initial_levels > 3 || p.maximum_sample_depth > 20 ||
            p.protected_full_rings <= p.protected_zero_rings || p.maximum_triangles < input.triangles.size() ||
            p.maximum_patch_evaluations == 0 || p.maximum_work_bytes < 1024)
        throw std::runtime_error("Invalid bounded displacement policy");
    for (const auto& v : input.vertices) {
        if (!finite(v.neutral_position) || !finite(v.displacement_direction) ||
                std::abs(length(v.displacement_direction) - 1.0) > 1e-5 ||
                !std::isfinite(v.displacement_magnitude) || v.displacement_magnitude < 0 ||
                !std::isfinite(v.height) || v.height < 0 || v.height > 1 ||
                !std::isfinite(v.uv0.x) || !std::isfinite(v.uv0.y) ||
                !std::isfinite(v.uv1.x) || !std::isfinite(v.uv1.y) ||
                !std::isfinite(v.footprint.x) || !std::isfinite(v.footprint.y) ||
                v.footprint.x < 0 || v.footprint.y < 0)
            throw std::runtime_error("Invalid authoritative displacement vertex");
    }
    for (auto t : input.triangles) {
        for (auto i : t) if (i >= input.vertices.size()) throw std::runtime_error("Displacement triangle index out of range");
        if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0]) throw std::runtime_error("Repeated displacement triangle corner");
        Point a = input.vertices[t[0]].neutral_position, b = input.vertices[t[1]].neutral_position,
            c = input.vertices[t[2]].neutral_position;
        if (length(cross(b - a, c - a)) <= 1e-24) throw std::runtime_error("Degenerate neutral displacement triangle");
    }
}

void validate_topology(const Edges& edges, std::span<const Face> faces) {
    for (const auto& [key, e] : edges) {
        if (e.incident.size() > 2) throw std::runtime_error("Nonmanifold displacement edge");
        if (e.incident.size() == 2) {
            auto direction = [&](std::uint32_t face) {
                auto t = faces[face].nodes;
                for (int k = 0; k < 3; ++k) if (t[k] == e.a && t[(k + 1) % 3] == e.b) return 1;
                return -1;
            };
            if (direction(e.incident[0].first) == direction(e.incident[1].first))
                throw std::runtime_error("Inconsistent displacement triangle winding");
        }
    }
}

void final_topology(const BoundedDisplacementSurface& surface, const BoundedDisplacementInput& input) {
    std::map<std::uint64_t, std::vector<std::pair<std::size_t, int>>> incidence;
    std::map<std::uint64_t, unsigned> original;
    for (auto t : input.triangles) for (int e = 0; e < 3; ++e) ++original[edge_key(t[e], t[(e + 1) % 3])];
    std::vector<double> coverage(input.triangles.size());
    for (std::size_t i = 0; i < surface.triangles.size(); ++i) {
        const auto& t = surface.triangles[i];
        for (int e = 0; e < 3; ++e) {
            auto& row = incidence[edge_key(t.nodes[e], t.nodes[(e + 1) % 3])];
            row.emplace_back(i, e);
            if (row.size() > 2) throw std::runtime_error("Generated displacement edge is nonmanifold");
        }
        if (t.source_parent >= input.triangles.size()) throw std::runtime_error("Generated parent ancestry out of range");
        for (auto b : t.source_barycentric) if (b.x < -1e-12 || b.y < -1e-12 || b.z < -1e-12 ||
                std::abs(b.x + b.y + b.z - 1) > 1e-12)
            throw std::runtime_error("Generated source barycentric ancestry is invalid");
        double signed_area = chart_area(t.source_barycentric[0], t.source_barycentric[1], t.source_barycentric[2]);
        if (!(signed_area > 0)) throw std::runtime_error("Generated parent chart orientation is invalid");
        coverage[t.source_parent] += signed_area;
    }
    for (double value : coverage) if (std::abs(value - 1) > 1e-9)
        throw std::runtime_error("Generated triangles do not cover every original parent domain");
    for (const auto& [key, row] : incidence) if (row.size() == 1) {
        auto [id, edge] = row.front(); const auto& t = surface.triangles[id];
        Point a = t.source_barycentric[edge], b = t.source_barycentric[(edge + 1) % 3];
        bool inherited = false;
        for (int k = 0; k < 3; ++k) if (std::abs(a[k]) < 1e-12 && std::abs(b[k]) < 1e-12) {
            auto parent = input.triangles[t.source_parent];
            inherited |= original.at(edge_key(parent[(k + 1) % 3], parent[(k + 2) % 3])) == 1;
        }
        if (!inherited) throw std::runtime_error("Generated displacement mesh has an internal opening");
    }
}

} // namespace

Result<BoundedDisplacementSurface> build_bounded_displacement_surface(
        const BoundedDisplacementInput& input, const BoundedDisplacementPolicy& policy) {
    try {
        validate_input(input, policy);
        BoundedDisplacementSurface out;
        out.parent_quality.resize(input.triangles.size());
        auto& m = out.metrics;
        double total_area = 0, total_failed_area = 0;
        m.input_vertices = input.vertices.size(); m.input_triangles = input.triangles.size();
        std::vector<Node> nodes(input.vertices.size()); std::vector<Face> faces;
        faces.reserve(input.triangles.size());
        for (std::uint32_t i = 0; i < input.vertices.size(); ++i) {
            const auto& v = input.vertices[i]; auto& n = nodes[i];
            n.neutral = input.uniform_scale * v.neutral_position;
            n.reference = n.neutral + input.reference_amplitude * double(v.displacement_magnitude)
                * (double(v.height) - kHeightZero) * v.displacement_direction;
            n.desired = n.reference; n.uv0 = v.uv0; n.uv1 = v.uv1;
        }
        std::vector<bool> owned(nodes.size());
        for (std::uint32_t i = 0; i < input.triangles.size(); ++i) {
            auto t = input.triangles[i]; faces.push_back({t, i, {kSamples[0], kSamples[1], kSamples[2]}});
            for (int k = 0; k < 3; ++k) if (!owned[t[k]]) {
                nodes[t[k]].source_parent = i; nodes[t[k]].source_bary = kSamples[k]; owned[t[k]] = true;
            }
            Point normal = cross(nodes[t[1]].reference - nodes[t[0]].reference,
                nodes[t[2]].reference - nodes[t[0]].reference);
            if (length(normal) <= 1e-24) throw std::runtime_error("Degenerate authoritative displaced triangle");
        }
        std::set<std::uint64_t> features;
        auto initial_edges = edges_of(faces, features); validate_topology(initial_edges, faces);
        for (auto& [key, e] : initial_edges) {
            bool feature = e.incident.size() != 2;
            if (!feature) {
                Point n[2];
                for (int j = 0; j < 2; ++j) {
                    auto t = faces[e.incident[j].first].nodes;
                    n[j] = cross(nodes[t[1]].neutral - nodes[t[0]].neutral,
                        nodes[t[2]].neutral - nodes[t[0]].neutral);
                    n[j] /= length(n[j]);
                }
                feature = dot(n[0], n[1]) < std::cos(policy.neutral_feature_degrees * std::numbers::pi / 180.0);
            }
            if (feature) { features.insert(key); nodes[e.a].pinned = nodes[e.b].pinned = true; }
        }
        std::map<std::uint64_t, std::vector<std::uint32_t>> seams;
        for (std::uint32_t i = 0; i < input.vertices.size(); ++i) if (input.vertices[i].compatible_seam)
            seams[input.vertices[i].compatible_seam].push_back(i);
        for (const auto& [token, group] : seams) if (group.size() > 1)
            for (auto i : group) nodes[i].pinned = true;
        pin_duplicates(nodes);
        out.nodes.reserve(input.vertices.size());
        for (const auto& n : nodes) out.nodes.push_back({n.reference, n.reference, Point{}, n.source_parent, n.source_bary});
        const auto original_faces = faces;
        for (std::uint32_t level = 0; level < policy.initial_levels; ++level) {
            std::uint64_t estimate = 4ull * faces.size() * 512 + nodes.size() * 512;
            if (estimate > policy.maximum_work_bytes) throw std::runtime_error("Initial subdivision working-set estimate exceeds budget");
            subdivide(nodes, faces, features, policy.initial_node_bound);
            guard_initial_nodes(nodes, faces, policy.initial_node_bound);
        }
        auto edges = edges_of(faces, features); auto neighbors = neighbors_of(edges, nodes.size());
        auto weights = protected_weights(nodes, neighbors, policy);
        m.pinned_nodes = std::count_if(nodes.begin(), nodes.end(), [](const auto& n) { return n.pinned; });
        std::vector<std::vector<std::uint32_t>> incident(nodes.size());
        for (std::uint32_t i = 0; i < faces.size(); ++i) for (auto n : faces[i].nodes) incident[n].push_back(i);
        std::vector<bool> visited(faces.size()), replaced(input.triangles.size());
        std::vector<std::vector<std::uint32_t>> components;
        for (std::uint32_t first = 0; first < faces.size(); ++first) if (!visited[first]) {
            std::vector<std::uint32_t> component; std::deque<std::uint32_t> queue{first}; visited[first] = true;
            bool active = false;
            while (!queue.empty()) {
                auto f = queue.front(); queue.pop_front(); component.push_back(f);
                for (auto n : faces[f].nodes) {
                    active |= weights[n] > 0;
                    for (auto adjacent : incident[n]) if (!visited[adjacent]) { visited[adjacent] = true; queue.push_back(adjacent); }
                }
            }
            // A globally affine physical chart has no geometric horizon cut to
            // improve. Keep its exact old topology instead of spending native
            // boundary patch work on a roundoff-only plane identity.
            const auto& first_face = faces[component.front()].nodes;
            Point origin = nodes[first_face[0]].reference;
            Point axis = cross(nodes[first_face[1]].reference - origin, nodes[first_face[2]].reference - origin);
            axis /= length(axis);
            bool affine_component = true;
            double epsilon = 1e-11 + 64 * std::numeric_limits<double>::epsilon() * (1 + length(origin));
            for (auto f : component) for (auto n : faces[f].nodes)
                affine_component &= std::abs(dot(axis, nodes[n].reference - origin)) <= epsilon;
            if (affine_component) ++m.affine_components;
            else if (active) components.push_back(std::move(component));
        }
        // Native charts certify the common target but do not impose a raster
        // tessellation floor. Every sampler starts on original parent triangles.
        m.eligible_components = components.size();
        m.minimum_conforming_triangles = input.triangles.size();
        std::vector<Point> original_positions;
        original_positions.reserve(out.nodes.size());
        for (const auto& n : out.nodes) original_positions.push_back(n.old_raster_position);
        std::vector<SampleWork> work;
        std::uint64_t stored_target_bytes = 0;
        auto reject_component = [&](std::span<const std::uint32_t> parents,
                BoundedDisplacementStatus reason, std::string diagnostic,
                std::uint64_t witnesses = 0, double bound = 0.0) {
            ++m.rejected_components; m.rejected_source_triangles += parents.size();
            m.rejected_orientation_failures += witnesses;
            m.rejected_bound_components += reason == BoundedDisplacementStatus::TargetBoundFailed;
            m.rejected_numerical_components += reason == BoundedDisplacementStatus::NumericalLimit;
            m.maximum_rejected_target_bound = std::max(m.maximum_rejected_target_bound, bound);
            for (auto parent : parents) for (auto id : input.triangles[parent]) {
                const auto& n = nodes[id];
                out.nodes[id] = {original_positions[id], original_positions[id], Point{}, n.source_parent, n.source_bary};
            }
            out.component_fallbacks.push_back({reason, {parents.begin(), parents.end()}, std::move(diagnostic)});
        };
        for (const auto& component : components) {
            std::set<std::uint32_t> used, parent_set;
            for (auto f : component) {
                parent_set.insert(faces[f].parent);
                for (auto n : faces[f].nodes) used.insert(n);
            }
            std::vector<std::uint32_t> parents(parent_set.begin(), parent_set.end());
            std::vector<std::uint32_t> globals(used.begin(), used.end()), local(nodes.size(), UINT32_MAX);
            std::vector<Node> cn; std::vector<Face> cf; std::vector<double> cw;
            cn.reserve(globals.size()); cw.reserve(globals.size());
            for (std::uint32_t i = 0; i < globals.size(); ++i) {
                local[globals[i]] = i; cn.push_back(nodes[globals[i]]); cw.push_back(weights[globals[i]]);
            }
            for (auto id : component) {
                Face f = faces[id]; for (auto& n : f.nodes) n = local[n]; cf.push_back(f);
            }
            double component_bound = 0; std::uint64_t witnesses = 0, regular = 0, extraordinary = 0;
            try {
                auto ce = edges_of(cf, {}); auto adjacency = neighbors_of(ce, cn.size());
                std::uint64_t estimate = cf.size() * 4096ull + cn.size() * 2048ull + stored_target_bytes;
                if (estimate > policy.maximum_work_bytes)
                    throw std::runtime_error("Native component working-set estimate exceeds budget");
                auto common = inverse_controls(cn, adjacency);
                NativeSurface native(cn, cf, common);
                repair_common_controls(native, common, cn, cf, adjacency, cw, m, policy.maximum_patch_evaluations);
                for (std::size_t i = 0; i < common.size(); ++i)
                    common[i] = (1 - policy.common_control_blend) * common[i] + policy.common_control_blend * cn[i].desired;
                native.update_common(common);
                Target target{native, cn, cf, cw, m, policy.maximum_patch_evaluations};
                for (std::uint32_t p = 0; p < native.patches.size(); ++p) {
                    const auto& patch = native.patches[p];
                    if (patch.type == Far::PatchDescriptor::LOOP) ++regular; else ++extraordinary;
                    const auto& t = cf[patch.face].nodes;
                    // An identically protected face is exactly affine. Its
                    // position and orientation follow the original reference.
                    if (cw[t[0]] == 0 && cw[t[1]] == 0 && cw[t[2]] == 0) continue;
                    double bound = patch_bound(target, p); component_bound = std::max(component_bound, bound);
                    if (!std::isfinite(bound) || bound > policy.correspondence_bound) {
                        if (m.first_failed_source_parent == UINT32_MAX) {
                            m.first_failed_source_parent = cf[patch.face].parent;
                            m.first_failed_source_barycentric = target.source_bary(p, affine(patch.domain, kSamples[6]));
                        }
                        throw ComponentRejected(BoundedDisplacementStatus::TargetBoundFailed,
                            "Native target correspondence envelope exceeds the movement bound");
                    }
                    for (auto b : kSamples) {
                        auto chart = affine(patch.domain, b); auto frame = target.evaluate(p, chart);
                        Point axis = cross(cn[t[1]].reference - cn[t[0]].reference,
                            cn[t[2]].reference - cn[t[0]].reference);
                        if (dot(axis, cross(frame.du, frame.dv)) <= 0) {
                            ++witnesses;
                            if (m.first_failed_source_parent == UINT32_MAX) {
                                m.first_failed_source_parent = cf[patch.face].parent;
                                m.first_failed_source_barycentric = target.source_bary(p, chart);
                            }
                        }
                    }
                }
                if (witnesses) throw ComponentRejected(BoundedDisplacementStatus::TargetOrientationFailed,
                    "Protected-band analytic target has an original-axis orientation reversal");
                auto frozen = native.freeze(cf, cw);
                auto source = std::make_unique<SourceTarget>(std::move(cn), std::move(cf), std::move(cw),
                    std::move(frozen), input.triangles, original_positions, m, policy.maximum_patch_evaluations);
                std::uint64_t future_output = policy.maximum_triangles * (sizeof(Cell) + sizeof(BoundedDisplacementTriangle))
                    + (input.vertices.size() + policy.maximum_triangles) * sizeof(BoundedDisplacementNode);
                if (stored_target_bytes + source->working_bytes() + future_output > policy.maximum_work_bytes)
                    throw std::runtime_error("Frozen target and sampling working-set estimate exceeds budget");
                stored_target_bytes += source->working_bytes();
                work.push_back({std::move(source), {}, {}, component_bound, regular, extraordinary});
            } catch (const ComponentRejected& e) {
                reject_component(parents, e.reason, e.what(), witnesses, component_bound);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& e) {
                reject_component(parents, BoundedDisplacementStatus::NumericalLimit, e.what(), witnesses, component_bound);
            }
        }
        m.target_patch_evaluations = m.patch_evaluations;
        auto total_triangles = [&] {
            std::size_t count = input.triangles.size();
            for (const auto& w : work) if (w.active && !w.cells.empty()) count += w.cells.size() - w.target->parents.size();
            return count;
        };
        auto reject_work = [&](SampleWork& w, BoundedDisplacementStatus reason, const char* message) {
            reject_component(w.target->parents, reason, message,
                reason == BoundedDisplacementStatus::TargetOrientationFailed ? 1 : 0, w.bound);
            w.active = false;
        };
        for (auto& w : work) {
            try {
                w.sampler = std::make_unique<SourceSampler>(*w.target, out, policy);
                w.cells = w.sampler->initial_cells();
                std::size_t available = policy.maximum_triangles - total_triangles() + w.cells.size();
                if (!w.sampler->guard_winding(w.cells, available))
                    reject_work(w, BoundedDisplacementStatus::TargetOrientationFailed,
                        "Original-parent starting chords cannot obtain positive orientation within their work budget");
            } catch (const std::bad_alloc&) { throw; }
            catch (const std::exception& e) { reject_work(w, BoundedDisplacementStatus::NumericalLimit, e.what()); }
        }
        std::size_t reserve = std::min<std::size_t>(512, (policy.maximum_triangles - input.triangles.size()) / 100);
        std::size_t quality_budget = std::max(total_triangles(), std::size_t(policy.maximum_triangles - reserve));
        for (std::uint32_t pass = 0; pass < policy.maximum_sample_depth; ++pass) {
            if (!refine_global(work, total_triangles(), quality_budget, policy, m, reject_work)) break;
        }
        for (auto& w : work) if (w.active) {
            try {
                std::size_t available = policy.maximum_triangles - total_triangles() + w.cells.size();
                if (!w.sampler->guard_winding(w.cells, available)) {
                    reject_work(w, BoundedDisplacementStatus::TargetOrientationFailed,
                        "Final conforming green chords cannot obtain positive orientation within their work budget");
                    continue;
                }
                bool invalid = false;
                for (auto& c : w.cells) {
                    w.sampler->quality(c);
                    auto a = out.nodes[c.nodes[0]].target_position, b = out.nodes[c.nodes[1]].target_position,
                        d = out.nodes[c.nodes[2]].target_position;
                    auto oa = w.target->reference(c.patch, c.domain[0]), ob = w.target->reference(c.patch, c.domain[1]),
                        od = w.target->reference(c.patch, c.domain[2]);
                    invalid |= c.area <= 1e-24 || dot(cross(b - a, d - a), cross(ob - oa, od - oa)) <= 0;
                }
                if (invalid) reject_work(w, BoundedDisplacementStatus::TargetOrientationFailed,
                    "Final accepted chord orientation check failed");
            } catch (const std::bad_alloc&) { throw; }
            catch (const std::exception& e) { reject_work(w, BoundedDisplacementStatus::NumericalLimit, e.what()); }
        }
        for (const auto& w : work) if (w.active) {
            ++m.accepted_components;
            m.native_regular_patches += w.regular_patches; m.native_extraordinary_patches += w.extraordinary_patches;
            m.maximum_target_bound = std::max(m.maximum_target_bound, w.bound);
            for (auto parent : w.target->parents) replaced[parent] = true;
            for (const auto& c : w.cells) {
                auto& parent = out.parent_quality[c.patch];
                parent.sampled_target = true; ++parent.output_triangles;
                parent.maximum_position_error = std::max(parent.maximum_position_error, c.position_error);
                parent.maximum_normal_error_degrees = std::max(parent.maximum_normal_error_degrees, c.normal_error);
                m.maximum_sampled_position_error = std::max(m.maximum_sampled_position_error, c.position_error);
                m.maximum_sampled_normal_error_degrees = std::max(m.maximum_sampled_normal_error_degrees, c.normal_error);
                total_area += c.area;
                if (c.position_error > policy.position_error || c.normal_error > policy.normal_error_degrees) {
                    ++parent.sampled_quality_failures;
                    ++m.sampled_quality_failures; total_failed_area += c.area;
                }
                m.minimum_triangle_area = m.minimum_triangle_area == 0 ? c.area : std::min(m.minimum_triangle_area, c.area);
                out.triangles.push_back({c.nodes, c.patch, c.domain});
            }
        }
        for (std::uint32_t i = 0; i < original_faces.size(); ++i) if (!replaced[i]) {
            out.triangles.push_back({original_faces[i].nodes, i, original_faces[i].bary});
            out.parent_quality[i].output_triangles = 1;
            const auto& t = original_faces[i].nodes;
            double area = .5 * length(cross(original_positions[t[1]] - original_positions[t[0]],
                original_positions[t[2]] - original_positions[t[0]]));
            m.minimum_triangle_area = m.minimum_triangle_area == 0 ? area : std::min(m.minimum_triangle_area, area);
        }
        // A rejected late sampler can leave unreferenced temporary nodes. Keep
        // the immutable original prefix, then compact only actually used nodes.
        std::vector<std::uint32_t> remap(out.nodes.size(), UINT32_MAX);
        for (std::uint32_t i = 0; i < input.vertices.size(); ++i) remap[i] = i;
        std::vector<BoundedDisplacementNode> compact(out.nodes.begin(),
            out.nodes.begin() + static_cast<std::ptrdiff_t>(input.vertices.size()));
        for (auto& t : out.triangles) for (auto& id : t.nodes) {
            if (remap[id] == UINT32_MAX) {
                remap[id] = static_cast<std::uint32_t>(compact.size()); compact.push_back(out.nodes[id]);
            }
            id = remap[id];
        }
        out.nodes.swap(compact);
        for (const auto& n : out.nodes) m.maximum_sampled_movement = std::max(m.maximum_sampled_movement, length(n.correction));
        m.output_nodes = out.nodes.size(); m.output_triangles = out.triangles.size();
        m.sampling_patch_evaluations = m.patch_evaluations - m.target_patch_evaluations;
        m.failing_area_fraction = total_area > 0 ? total_failed_area / total_area : 0;
        if (m.maximum_sampled_movement > policy.correspondence_bound + 1e-10)
            out.status = BoundedDisplacementStatus::TargetBoundFailed;
        else if (m.sampled_quality_failures || m.rejected_components) out.status = BoundedDisplacementStatus::BudgetLimited;
        final_topology(out, input);
        return out;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Bounded displacement builder allocation failed"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{e.what()});
    }
}

} // namespace heaven
