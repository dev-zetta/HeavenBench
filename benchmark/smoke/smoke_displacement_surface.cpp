/* HeavenBench — native contract for the reusable bounded displacement surface. */
#include "smoke_displacement_surface.h"

#include "rhi/displacement_surface_preparer.h"
#include "rhi/displacement_carrier.h"
#include "rhi/filament_backend.h"
#include "rhi/filament_util.h"
#include "rhi/material_instance_cache.h"
#include "rhi/materials_data.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/ColorSpace.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/ToneMapper.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_displacement_surface]";
constexpr std::uint32_t kExtent = 192;
constexpr int kGrid = 9;
constexpr float kReferenceAmplitude = 0.7f;
constexpr double kBound = 0.020;
constexpr double kDepthTolerance = 2e-5;
constexpr double kInteriorMargin = 1.5; // pixels; CPU oracle excludes raster edges
constexpr double kHalfExtent = 0.335;
constexpr double kSunRadius = 3.0 * std::numbers::pi / 180.0;
constexpr double kSunElevation = 2.0 * std::numbers::pi / 180.0;
using D3 = glm::dvec3;
using D4 = glm::dvec4;
using F3 = filament::math::float3;
using F4 = filament::math::float4;
using Extra = std::array<F4, 7>;
static_assert(sizeof(Extra) == 112);

struct PointUV { F3 position; filament::math::float2 uv0, uv1; };
static_assert(sizeof(PointUV) == 28);
struct DrawMesh {
    filament::VertexBuffer* vb = nullptr;
    filament::IndexBuffer* ib = nullptr;
    std::vector<PointUV> points;
    std::vector<filament::math::short4> quats;
    std::vector<F4> custom0;
    std::vector<Extra> extra;
    std::vector<std::uint32_t> indices;
    // Independent physical oracle arrays, not decoded shader metadata.
    std::vector<D3> physical_positions;
    bool owns_buffers = true;
};
enum class Shape { Original, Zero, Bounded };
struct State {
    float amplitude = kReferenceAmplitude;
    float power = 0.7f;
    float threshold = 1000;
    float inverse_range = 0.001f;
    F3 fade_origin{0, 0, 0};
};
struct Capture {
    std::vector<std::uint8_t> mask;
    // Shipping depth stores the pre-divide view Z. Keep that native value and
    // separately convert it to physical metres for every numerical gate.
    std::vector<float> raw_depth;
    std::vector<float> depth;
    double homogeneous_w = 1;
};
struct Oracle {
    std::vector<double> depth;
    // Diagnostic model only: projected vertices rounded to a 1/256 pixel grid.
    // It does not replace the exact CPU oracle or any acceptance threshold.
    std::vector<double> quantized_depth;
    std::vector<glm::dvec2> depth_gradient;
    std::vector<double> edge_margin;
    std::vector<std::uint32_t> owner;
    std::vector<D3> point;
    std::vector<bool> blocker;
};
struct GeometryCheck {
    std::size_t interior_coverage = 0, pass_mismatch = 0, samples = 0;
    std::size_t excluded_edges = 0, blocker_samples = 0;
    double error = 0;
};
D3 d3(F3 p) { return {p.x, p.y, p.z}; }
F3 f3(D3 p) { return {float(p.x), float(p.y), float(p.z)}; }
bool finite(D3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

template<std::size_t Components, class Vector>
bool same_components(const Vector& a, const Vector& b) {
    using Scalar = std::remove_cvref_t<decltype(a[0])>;
    static_assert(sizeof(Vector) == Components * sizeof(Scalar));
    // Compare serialized scalar bits, including signed zero and NaN payloads.
    for (std::size_t i = 0; i < Components; ++i) {
        using Bits = std::array<std::byte, sizeof(Scalar)>;
        if (std::bit_cast<Bits>(a[i]) != std::bit_cast<Bits>(b[i])) return false;
    }
    return true;
}

template<class T> bool same(std::span<const T> a, std::span<const T> b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}
bool same_packed(const PackedStaticSurface& a, const PackedStaticSurface& b) {
    return same<F3>(a.positions, b.positions) &&
        same<filament::math::float2>(a.uv0, b.uv0) &&
        same<filament::math::float2>(a.uv1, b.uv1) &&
        same<filament::math::short4>(a.tangent_frames, b.tangent_frames) &&
        same<F4>(a.displacement_custom, b.displacement_custom) &&
        same<mesh_displacement::Footprint>(a.footprints, b.footprints) &&
        same<F3>(a.unit_displacement_directions, b.unit_displacement_directions) &&
        a.source_signature == b.source_signature &&
        a.deformation_signature == b.deformation_signature &&
        a.tangent_packing == b.tangent_packing &&
        a.displacement_directions_valid == b.displacement_directions_valid;
}

// This is a separate spherical ring/azimuth integration. It does not use the
// shader's analytic cap CDF or its conditional Hammersley samples.
std::vector<D3> oracle_directions() {
    constexpr int rings = 6, azimuths = 24;
    const D3 center{std::cos(kSunElevation), 0, std::sin(kSunElevation)};
    const D3 tangent{0, 1, 0}, second = glm::cross(center, tangent);
    std::vector<D3> result;
    for (int ring = 0; ring < rings; ++ring) {
        const double cosine = 1 - (1 - std::cos(kSunRadius)) * (ring + .5) / rings;
        const double sine = std::sqrt(std::max(0.0, 1 - cosine * cosine));
        for (int azimuth = 0; azimuth < azimuths; ++azimuth) {
            const double phi = 2 * std::numbers::pi * (azimuth + .5) / azimuths;
            result.push_back(center * cosine +
                (tangent * std::cos(phi) + second * std::sin(phi)) * sine);
        }
    }
    return result;
}
double ray_hit(D3 origin, D3 direction, D3 a, D3 b, D3 c) {
    const D3 e1 = b-a, e2 = c-a, h = glm::cross(direction, e2);
    const double determinant = glm::dot(e1, h);
    if (std::abs(determinant) < 1e-14) return 0;
    const D3 s = origin-a, q = glm::cross(s, e1);
    const double u = glm::dot(s, h)/determinant;
    const double v = glm::dot(direction, q)/determinant;
    const double t = glm::dot(e2, q)/determinant;
    return u >= -1e-10 && v >= -1e-10 && u+v <= 1+1e-10 && t > 1e-7 ? t : 0;
}
double segment_distance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b) {
    const auto edge = b-a;
    const double length2 = glm::dot(edge, edge);
    const double t = length2 > 0 ? std::clamp(glm::dot(p-a, edge)/length2, 0.0, 1.0) : 0;
    return glm::length(p-a-t*edge);
}

class DisplacementSurfaceFixture {
public:
    explicit DisplacementSurfaceFixture(filament::Engine& engine, bool colored,
            const glm::mat4& model_transform = glm::mat4{1}, bool mixed_homogeneous_sharing=false)
        : engine_(engine), textures_(engine), materials_(engine, textures_), meshes_(engine) {
        try {
        transforms_[0]=model_transform;
        homogeneous_w_=double(model_transform[3][3]);
        if(mixed_homogeneous_sharing) {
            auto other=model_transform;other[3][3]=1;transforms_.push_back(other);
        }
        for(int column=0;column<3;++column) rotation_[column]=glm::normalize(D3(model_transform[column]));
        half_extent_=kHalfExtent*glm::length(D3(model_transform[0]))/homogeneous_w_;
        textures_.create_placeholders();
        materials_.set_colored_penumbra(colored);
        if (!materials_.load_base_materials()) throw std::runtime_error("shipping lit packages unavailable");
        color_material_ = materials_.get_or_create("displacement-surface-fixture", "receiver", GpuMaterial{});
        blocker_material_ = materials_.get_or_create("displacement-surface-fixture", "blocker", GpuMaterial{});
        const std::string_view expected = colored ? "heaven_lit_colored_penumbra" : "heaven_lit";
        if (!color_material_ || color_material_->getMaterial()->getName() != expected)
            throw std::runtime_error("receiver did not route to the shipping lit family");
        depth_base_ = filament::Material::Builder().package(k_heaven_depth_filamat, k_heaven_depth_filamat_size).build(engine_);
        masked_base_ = filament::Material::Builder().package(k_heaven_depth_masked_filamat, k_heaven_depth_masked_filamat_size).build(engine_);
        if (!depth_base_ || !masked_base_) throw std::runtime_error("shipping depth packages unavailable");
        const std::array<const filament::Material*,3> required{color_material_->getMaterial(),depth_base_,masked_base_};
        for (const auto* material : required) {
            if (!material->getRequiredAttributes().test(std::size_t(filament::VertexAttribute::CUSTOM7)))
                throw std::runtime_error("pending integration: shipping package lacks parent carrier CUSTOM7");
        }
        depth_material_ = depth_base_->createInstance();
        masked_material_ = masked_base_->createInstance();
        blocker_depth_ = depth_base_->createInstance();
        masked_material_->setParameter("baseColorMap", textures_.placeholder(), sampler());
        set_emission(color_material_, true);
        set_emission(blocker_material_, true);
        blocker_material_->setParameter("geometryDisplacement", false);
        blocker_depth_->setParameter("geometryDisplacement", false);
        blocker_depth_->setParameter("geometryHeightMap", textures_.placeholder(), sampler());
        blocker_depth_->setParameter("baseTransform", F4{1,1,0,0});
        blocker_depth_->setParameter("displacementParams", F4{0,1,1000,.001f});

        create_height();
        make_source();
        Mesh source_mesh;source_mesh.name="displacement-surface-native";
        Surface surface;surface.vertices=vertices_;surface.indices=indices_;
        source_mesh.surfaces.push_back(std::move(surface));
        const auto uploaded=meshes_.upload(source_mesh);
        if(uploaded.surfaces.size()!=1) throw std::runtime_error("original MeshStore upload failed");
        original_buffers_=meshes_.find(uploaded.surfaces.front().handle);
        if(!original_buffers_.vb||!original_buffers_.ib) throw std::runtime_error("original MeshStore buffers are missing");
        auto packed = meshes_.take_original_packing(uploaded.surfaces.front().handle,vertices_,indices_);
        if (!packed) throw std::runtime_error(packed.error().message);
        packed_ = std::move(*packed);
        auto batch = HeightSampleBatch::create(engine_, {k_heaven_height_sample_filamat, k_heaven_height_sample_filamat_size});
        if (!batch) throw std::runtime_error(batch.error().message);
        height_batch_ = std::move(*batch);
        DisplacementPreparationPolicy policy;
        // The same geometry operator is used. Only the synthetic workload cap
        // is smaller; an explicit budget-limited result remains visible.
        policy.geometry.maximum_triangles = 65'536;
        policy.geometry.maximum_patch_evaluations = 2'000'000;
        policy.geometry.maximum_work_bytes = 128ull*1024*1024;
        preparer_ = std::make_unique<DisplacementSurfacePreparer>(*height_batch_, policy);
        request_.vertices = vertices_; request_.indices = indices_;
        request_.uploaded_source = &packed_; request_.instance_transforms = transforms_;
        request_.static_vertices = true; request_.selected_height = height_;
        request_.displacement = {true, kReferenceAmplitude, .7f, {1000,1000}};
        request_.base_transform = {.93f,.87f,.013f,.047f};
        auto sampled=preparer_->sample_source(request_);
        if(!sampled||!sampled->source_ready||sampled->ready()||sampled->geometry_integrity_valid||
            sampled->disposition!=DisplacementPreparationDisposition::SourceSampled||
            !sampled->geometry.nodes.empty()||!same_packed(sampled->original_packed,packed_))
            throw std::runtime_error("native source sampling claimed a target or changed original packing");
        const double minimum_w=std::min(homogeneous_w_,mixed_homogeneous_sharing?1.0:homogeneous_w_);
        const double bound_scale=std::min(1.0,minimum_w);
        if(sampled->minimum_homogeneous_w!=minimum_w||
            std::abs(sampled->effective_correspondence_bound-kBound*bound_scale)>1e-12)
            throw std::runtime_error("source sampling lost the minimum sharing W or physical movement bound");
        auto prepared = preparer_->refine_source(std::move(*sampled));
        if (!prepared) throw std::runtime_error(prepared.error().message);
        prepared_ = std::move(*prepared);
        if (!prepared_.ready()) throw std::runtime_error("preparer rejected synthetic curved support: " + prepared_.reason);
        if (!same_packed(packed_, prepared_.original_packed)) throw std::runtime_error("preparer changed original upload bytes");
        if (prepared_.native_height.size() != vertices_.size() || prepared_.source_triangles.size()*3 != indices_.size())
            throw std::runtime_error("preparer lost ordered source samples or triangles");
        validate_preparation();
        validate_fallbacks();
        auto carrier=make_displacement_carrier(vertices_,prepared_);
        if(!carrier) throw std::runtime_error(carrier.error().message);
        carrier_=std::move(*carrier);
        validate_carrier();
        source_ = original_draw(); zero_ = refined_draw(false); bounded_ = refined_draw(true);
        blocker_ = blocker_draw();
        for (auto* mesh : {&zero_, &blocker_}) upload(*mesh);
        upload_bounded_via_mesh_store(source_mesh);
        const F4 transform{.93f,.87f,.013f,.047f};
        color_material_->setParameter("hasHeight", true);
        color_material_->setParameter("heightMap", height_, sampler());
        color_material_->setParameter("geometryHeightMap", height_, sampler());
        for (auto* material : {color_material_, depth_material_, masked_material_}) {
            material->setParameter("geometryDisplacement", true);
            material->setParameter("baseTransform", transform);
        }
        for (auto* material : {depth_material_, masked_material_}) material->setParameter("geometryHeightMap", height_, sampler());
        create_views();
        } catch (...) {
            release();
            throw;
        }
    }

    ~DisplacementSurfaceFixture() { release(); }

    void release() noexcept {
        for (auto* view : {color_view_, depth_view_, visibility_view_}) if (view) view->setRenderTarget(nullptr);
        for (auto entity : entities_) {
            if (color_scene_) color_scene_->remove(entity);
            if (depth_scene_) depth_scene_->remove(entity);
            engine_.destroy(entity); utils::EntityManager::get().destroy(entity);
        }
        for (auto* view : {color_view_, depth_view_, visibility_view_}) if (view) engine_.destroy(view);
        if (color_scene_) engine_.destroy(color_scene_);
        if (depth_scene_) engine_.destroy(depth_scene_);
        for (auto* target : {color_target_, depth_target_, visibility_target_}) if (target) engine_.destroy(target);
        for (auto* texture : {color_texture_, linear_texture_, visibility_texture_}) if (texture) engine_.destroy(texture);
        for (auto* texture : attachments_) if (texture) engine_.destroy(texture);
        if (grading_) engine_.destroy(grading_);
        if (camera_) { engine_.destroyCameraComponent(camera_entity_); utils::EntityManager::get().destroy(camera_entity_); }
        if (renderer_) engine_.destroy(renderer_);
        if (swapchain_) engine_.destroy(swapchain_);
        for (auto* mesh : {&source_, &zero_, &bounded_, &blocker_}) {
            if (mesh->owns_buffers && mesh->vb) engine_.destroy(mesh->vb);
            if (mesh->owns_buffers && mesh->ib) engine_.destroy(mesh->ib);
        }
        for (auto* material : {depth_material_,masked_material_,blocker_depth_}) if (material) engine_.destroy(material);
        if (depth_base_) engine_.destroy(depth_base_);
        if (masked_base_) engine_.destroy(masked_base_);
        materials_.clear_instances();
        preparer_.reset(); height_batch_.reset();
        if (height_) engine_.destroy(height_);
    }

    bool capture(Shape shape, const State& state, bool masked, bool blocker, Capture& result) {
        select(shape, state, masked, blocker);
        set_emission(color_material_, true); set_emission(blocker_material_, true);
        if (!renderer_->beginFrame(swapchain_)) return false;
        renderer_->render(color_view_); renderer_->render(depth_view_);
        result.mask.resize(std::size_t(kExtent)*kExtent*4);
        result.homogeneous_w=homogeneous_w_;
        result.raw_depth.assign(std::size_t(kExtent)*kExtent, std::numeric_limits<float>::quiet_NaN());
        std::atomic_bool color_done{false}, depth_done{false};
        renderer_->readPixels(color_target_,0,0,kExtent,kExtent,filament::backend::PixelBufferDescriptor(
            result.mask.data(),result.mask.size(),filament::backend::PixelDataFormat::RGBA,
            filament::backend::PixelDataType::UBYTE,completed,&color_done));
        renderer_->readPixels(depth_target_,0,0,kExtent,kExtent,filament::backend::PixelBufferDescriptor(
            result.raw_depth.data(),result.raw_depth.size()*sizeof(float),filament::backend::PixelDataFormat::R,
            filament::backend::PixelDataType::FLOAT,completed,&depth_done));
        renderer_->endFrame(); engine_.flushAndWait();
        result.depth.resize(result.raw_depth.size());
        for(std::size_t i=0;i<result.depth.size();++i)
            result.depth[i]=float(double(result.raw_depth[i])/result.homogeneous_w);
        return color_done.load() && depth_done.load();
    }

    Oracle oracle(bool reverse_y) const {
        const auto model = camera_->getModelMatrix();
        const D3 eye{model[3].x,model[3].y,model[3].z};
        const D3 right{model[0].x,model[0].y,model[0].z};
        const D3 up{model[1].x,model[1].y,model[1].z};
        const D3 forward{-model[2].x,-model[2].y,-model[2].z};
        Oracle result;
        const std::size_t count = std::size_t(kExtent)*kExtent;
        result.depth.assign(count,0); result.edge_margin.assign(count,0);
        result.quantized_depth.assign(count,std::numeric_limits<double>::quiet_NaN());
        result.depth_gradient.resize(count);
        result.owner.assign(count,std::numeric_limits<std::uint32_t>::max());
        result.point.resize(count); result.blocker.assign(count,false);
        auto raster = [&](const DrawMesh& mesh, bool blocker) {
            for (std::size_t i=0;i<mesh.indices.size();i+=3) {
                std::array<D3,3> p;
                std::array<glm::dvec2,3> screen;
                std::array<double,3> depth;
                for (std::size_t k=0;k<3;++k) {
                    p[k]=mesh.physical_positions[mesh.indices[i+k]];
                    const D3 q=p[k]-eye;
                    screen[k]={(glm::dot(q,right)/half_extent_+1)*.5*kExtent,
                        (glm::dot(q,up)/half_extent_*(reverse_y?-1:1)+1)*.5*kExtent};
                    depth[k]=glm::dot(q,forward);
                }
                if (!blocker && glm::dot(glm::cross(p[1]-p[0],p[2]-p[0]),-forward)<=1e-14)
                    throw std::runtime_error("oracle receiver has a back-facing or degenerate triangle");
                const auto e1=screen[1]-screen[0],e2=screen[2]-screen[0];
                const double determinant=e1.x*e2.y-e1.y*e2.x;
                if (std::abs(determinant)<1e-14) continue;
                const double z1=depth[1]-depth[0],z2=depth[2]-depth[0];
                const glm::dvec2 gradient{(z1*e2.y-z2*e1.y)/determinant,
                    (e1.x*z2-e2.x*z1)/determinant};
                std::array<glm::dvec2,3> quantized_screen;
                for(std::size_t k=0;k<3;++k) quantized_screen[k]={
                    std::nearbyint(screen[k].x*256)/256,std::nearbyint(screen[k].y*256)/256};
                const auto qe1=quantized_screen[1]-quantized_screen[0],qe2=quantized_screen[2]-quantized_screen[0];
                const double quantized_determinant=qe1.x*qe2.y-qe1.y*qe2.x;
                const int x0=std::max(0,int(std::floor(std::min({screen[0].x,screen[1].x,screen[2].x}))));
                const int x1=std::min(int(kExtent)-1,int(std::ceil(std::max({screen[0].x,screen[1].x,screen[2].x}))));
                const int y0=std::max(0,int(std::floor(std::min({screen[0].y,screen[1].y,screen[2].y}))));
                const int y1=std::min(int(kExtent)-1,int(std::ceil(std::max({screen[0].y,screen[1].y,screen[2].y}))));
                for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
                    const glm::dvec2 pixel{x+.5,y+.5}, q=pixel-screen[0];
                    const double b1=(q.x*e2.y-q.y*e2.x)/determinant;
                    const double b2=(e1.x*q.y-e1.y*q.x)/determinant, b0=1-b1-b2;
                    if(std::min({b0,b1,b2}) < -1e-10) continue;
                    const double d=b0*depth[0]+b1*depth[1]+b2*depth[2];
                    const auto at=std::size_t(y)*kExtent+x;
                    if(d<=0 || (result.depth[at]>0 && d>=result.depth[at])) continue;
                    result.depth[at]=d; result.blocker[at]=blocker;
                    result.depth_gradient[at]=gradient;
                    result.quantized_depth[at]=std::numeric_limits<double>::quiet_NaN();
                    if(std::abs(quantized_determinant)>1e-14) {
                        const auto qq=pixel-quantized_screen[0];
                        const double qb1=(qq.x*qe2.y-qq.y*qe2.x)/quantized_determinant;
                        const double qb2=(qe1.x*qq.y-qe1.y*qq.x)/quantized_determinant,qb0=1-qb1-qb2;
                        if(std::min({qb0,qb1,qb2})>=0)
                            result.quantized_depth[at]=qb0*depth[0]+qb1*depth[1]+qb2*depth[2];
                    }
                    result.owner[at]=std::uint32_t(i/3);
                    result.point[at]=b0*p[0]+b1*p[1]+b2*p[2];
                    result.edge_margin[at]=std::min({segment_distance(pixel,screen[0],screen[1]),
                        segment_distance(pixel,screen[1],screen[2]),segment_distance(pixel,screen[2],screen[0])});
                }
            }
        };
        raster(*active_,false); if(blocker_enabled_) raster(blocker_,true);
        // Shared internal triangle edges are not silhouettes. Their depth is
        // continuous and remains part of the oracle. Only the outer indexed
        // grid boundary and the independent blocker's outline are excluded
        // from exact CPU raster classification near a subpixel edge.
        std::vector<std::pair<glm::dvec2,glm::dvec2>> outline;
        auto project=[&](D3 p) {
            const D3 q=p-eye;
            return glm::dvec2{(glm::dot(q,right)/half_extent_+1)*.5*kExtent,
                (glm::dot(q,up)/half_extent_*(reverse_y?-1:1)+1)*.5*kExtent};
        };
        auto edge=[&](std::uint32_t a,std::uint32_t b) {
            outline.emplace_back(project(source_.physical_positions[a]),project(source_.physical_positions[b]));
        };
        for(int i=0;i<kGrid-1;++i) {
            edge(i,i+1);edge((kGrid-1)*kGrid+i,(kGrid-1)*kGrid+i+1);
            edge(i*kGrid,(i+1)*kGrid);edge(i*kGrid+kGrid-1,(i+1)*kGrid+kGrid-1);
        }
        if(blocker_enabled_) for(std::size_t i=0;i<4;++i)
            outline.emplace_back(project(blocker_.physical_positions[i]),project(blocker_.physical_positions[(i+1)%4]));
        for(std::uint32_t y=0;y<kExtent;++y) for(std::uint32_t x=0;x<kExtent;++x) {
            double margin=std::numeric_limits<double>::infinity();
            for(const auto& [a,b]:outline) margin=std::min(margin,segment_distance({x+.5,y+.5},a,b));
            result.edge_margin[std::size_t(y)*kExtent+x]=margin;
        }
        return result;
    }

    // This final-geometry reference includes both actual t=0 horizon and real
    // intersections within the unchanged 0.3 m contact interval. The caller
    // distinguishes these terms instead of labelling all darkness an artifact.
    struct Visibility { double own=0, external=0, blocker=0, total=0, central_dot=0; };
    Visibility visibility(const Oracle& reference, std::size_t pixel, bool blocker) const {
        Visibility result;
        if(reference.owner[pixel]==std::numeric_limits<std::uint32_t>::max() || reference.blocker[pixel]) return result;
        const auto triangle=std::size_t(reference.owner[pixel])*3;
        const D3 a=active_->physical_positions[active_->indices[triangle]],
            b=active_->physical_positions[active_->indices[triangle+1]],
            c=active_->physical_positions[active_->indices[triangle+2]];
        const D3 normal=glm::normalize(glm::cross(b-a,c-a));
        result.central_dot=glm::dot(normal,D3{std::cos(kSunElevation),0,std::sin(kSunElevation)});
        const D3 origin=reference.point[pixel];
        const auto directions=oracle_directions();
        for(const auto direction : directions) {
            if(glm::dot(normal,direction)<=0) {++result.own; ++result.total; continue;}
            bool hit=false, independent=false;
            for(std::size_t i=0;i<active_->indices.size()&&!hit;i+=3) {
                if(i==triangle) continue;
                const double distance=ray_hit(origin,direction,active_->physical_positions[active_->indices[i]],
                    active_->physical_positions[active_->indices[i+1]],active_->physical_positions[active_->indices[i+2]]);
                hit=distance>0&&distance<=.3;
            }
            if(blocker) for(std::size_t i=0;i<blocker_.indices.size();i+=3) {
                const double distance=ray_hit(origin,direction,blocker_.physical_positions[blocker_.indices[i]],
                    blocker_.physical_positions[blocker_.indices[i+1]],blocker_.physical_positions[blocker_.indices[i+2]]);
                independent|=distance>0&&distance<=.3;
            }
            result.external+=hit; result.blocker+=independent; result.total+=hit||independent;
        }
        const double inverse=1.0/static_cast<double>(directions.size());
        result.own*=inverse;result.external*=inverse;result.blocker*=inverse;result.total*=inverse;
        return result;
    }

    bool capture_visibility(Shape shape, bool blocker, bool contact, std::vector<float>& average) {
        select(shape,State{},false,blocker);
        set_emission(color_material_,false);set_emission(blocker_material_,false);
        auto& lights=engine_.getLightManager(); const auto instance=lights.getInstance(light_);
        auto options=lights.getShadowOptions(instance);
        options.screenSpaceContactShadows=contact;options.stepCount=32;options.maxShadowDistance=.3f;
        lights.setShadowOptions(instance,options);
        const auto actual=lights.getShadowOptions(instance);
        if(actual.stepCount!=32||std::abs(actual.maxShadowDistance-.3f)>1e-6f||actual.screenSpaceContactShadows!=contact)
            throw std::runtime_error("contact options changed the original interval or step count");
        // TAA receives the same fixed geometry. It is reset between controls so
        // an old contact history cannot make a disabled control look shadowed.
        auto taa=visibility_view_->getTemporalAntiAliasingOptions();taa.enabled=false;
        visibility_view_->setTemporalAntiAliasingOptions(taa);taa.enabled=true;
        visibility_view_->setTemporalAntiAliasingOptions(taa);
        average.assign(std::size_t(kExtent)*kExtent*4,0);
        constexpr int warmup=64, samples=8;
        int accepted=0;
        for(int attempt=0;accepted<warmup+samples&&attempt<4*(warmup+samples);++attempt) {
            if(!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(visibility_view_);
            std::vector<float> pixels;
            std::atomic_bool done{false};
            if(accepted>=warmup) {
                pixels.assign(average.size(),std::numeric_limits<float>::quiet_NaN());
                renderer_->readPixels(visibility_target_,0,0,kExtent,kExtent,filament::backend::PixelBufferDescriptor(
                    pixels.data(),pixels.size()*sizeof(float),filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::FLOAT,completed,&done));
            }
            renderer_->endFrame();engine_.flushAndWait();
            if(accepted>=warmup) {
                if(!done.load()) return false;
                for(std::size_t i=0;i<pixels.size();++i) {
                    if(!std::isfinite(pixels[i])) return false;
                    average[i]+=pixels[i]/samples;
                }
            }
            ++accepted;
        }
        return accepted==warmup+samples;
    }

    const PreparedDisplacementSurface& prepared() const { return prepared_; }

    struct Horizon {
        std::size_t fully_front=0,partial=0,fully_back=0;
        double maximum_adjacent_mass_jump=0;
    };
    std::size_t clamped_correction_records() const { return clamped_correction_records_; }
    double maximum_unclamped_physical_correction() const { return maximum_unclamped_physical_correction_; }

    void condition_cap_measurement_camera(const State& state) {
        // The high-amplitude cap control keeps its source, target and fade.
        // Measure it normal to the transformed neutral plane, avoiding the
        // depth-plane slope amplification of the earlier oblique camera.
        select(Shape::Bounded,state,true,false);
        const auto report=[&](const char* label) {
            const auto reference=oracle(false);
            double slope=0,quantization=0;std::size_t samples=0;
            for(std::size_t i=0;i<reference.depth.size();++i) {
                if(reference.depth[i]<=.001||reference.edge_margin[i]<kInteriorMargin) continue;
                const auto g=reference.depth_gradient[i];
                slope=std::max(slope,std::abs(g.x)+std::abs(g.y));
                if(std::isfinite(reference.quantized_depth[i])) {
                    quantization=std::max(quantization,std::abs(reference.quantized_depth[i]-reference.depth[i]));
                    ++samples;
                }
            }
            std::printf("%s cap camera %s: maximum physical depth slope %.9g m/pixel, diagnostic nearest1/256-vertex depth residual %.9g m over %zu same-owner samples; exact oracle unchanged\n",
                kTag,label,slope,quantization,samples);
        };
        report("prior oblique");
        const auto eye=world_position({0,0,1.8}),target=world_position({0,0,0});
        const auto up=rotation_*D3{0,1,0};
        camera_->lookAt({eye.x,eye.y,eye.z},{target.x,target.y,target.z},{up.x,up.y,up.z});
        report("neutral normal");
    }
    Horizon horizon() const {
        Horizon result;
        const auto directions=oracle_directions();
        std::unordered_map<std::uint64_t,double> first_mass;
        for(std::size_t i=0;i<active_->indices.size();i+=3) {
            std::array<std::uint32_t,3> nodes{};
            std::array<D3,3> p;
            for(std::size_t c=0;c<3;++c) {
                p[c]=active_->physical_positions[active_->indices[i+c]];
                nodes[c]=active_==&source_?active_->indices[i+c]:prepared_.geometry.triangles[i/3].nodes[c];
            }
            const D3 normal=glm::normalize(glm::cross(p[1]-p[0],p[2]-p[0]));
            std::size_t count=0;for(const auto direction:directions) count+=glm::dot(normal,direction)<=0;
            const double mass=double(count)/static_cast<double>(directions.size());
            result.fully_front+=count==0;result.fully_back+=count==directions.size();
            result.partial+=count>0&&count<directions.size();
            for(std::size_t c=0;c<3;++c) {
                const auto a=std::min(nodes[c],nodes[(c+1)%3]),b=std::max(nodes[c],nodes[(c+1)%3]);
                const auto key=(std::uint64_t(a)<<32)|b;
                const auto [it,inserted]=first_mass.emplace(key,mass);
                if(!inserted) result.maximum_adjacent_mass_jump=std::max(result.maximum_adjacent_mass_jump,std::abs(mass-it->second));
            }
        }
        return result;
    }

private:
    static void completed(void* /*buffer*/,std::size_t /*size*/,void* user) {static_cast<std::atomic_bool*>(user)->store(true);}
    static filament::TextureSampler sampler() {
        return {filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,filament::TextureSampler::WrapMode::REPEAT};
    }
    void set_emission(filament::MaterialInstance* material,bool enabled) {
        if(!material) throw std::runtime_error("lit instance missing");
        material->setParameter("hasEmissive",enabled);
        material->setParameter("emissiveMap",textures_.placeholder(),sampler());
        material->setParameter("emissionTint",F3{1,1,1});
        material->setParameter("emissionScale",enabled?100.0f:0.0f);
        material->setParameter("surfaceEmissionTuning",F4{0,1,0,.5f});
    }
    void create_height() {
        height_=filament::Texture::Builder().width(64).height(64).levels(7)
            .format(filament::Texture::InternalFormat::R32F).build(engine_);
        if(!height_) throw std::runtime_error("height texture allocation failed");
        std::vector<float> values(std::size_t{64}*64);
        for(int y=0;y<64;++y) for(int x=0;x<64;++x) {
            const double u=(x+.5)/64, v=(y+.5)/64;
            values[y*64+x]=float(127.0/255.0+.08*std::pow(std::sin(std::numbers::pi*u),2)*
                std::pow(std::sin(std::numbers::pi*v),2));
        }
        for(std::uint8_t level=0;level<7;++level) {
            const int size=64>>level;
            auto* copy=new float[values.size()];std::copy(values.begin(),values.end(),copy);
            height_->setImage(engine_,level,filament::backend::PixelBufferDescriptor(copy,values.size()*sizeof(float),
                filament::backend::PixelDataFormat::R,filament::backend::PixelDataType::FLOAT,
                [](void* data,std::size_t,void*){delete[] static_cast<float*>(data);}));
            if(size>1) {
                std::vector<float> next(std::size_t(size/2)*(size/2));
                for(int y=0;y<size/2;++y) for(int x=0;x<size/2;++x)
                    next[y*(size/2)+x]=.25f*(values[(2*y)*size+2*x]+values[(2*y)*size+2*x+1]+
                        values[(2*y+1)*size+2*x]+values[(2*y+1)*size+2*x+1]);
                values=std::move(next);
            }
        }
    }
    void make_source() {
        for(int y=0;y<kGrid;++y) for(int x=0;x<kGrid;++x) {
            Vertex v{};v.px=-.25f+.5f*static_cast<float>(x)/(kGrid-1);v.py=-.25f+.5f*static_cast<float>(y)/(kGrid-1);
            v.nz=1;v.u=float(x)/(kGrid-1);v.v=float(y)/(kGrid-1);v.u1=v.u;v.v1=v.v;
            vertices_.push_back(v);
        }
        for(int y=0;y<kGrid-1;++y) for(int x=0;x<kGrid-1;++x) {
            const auto a=std::uint32_t(y*kGrid+x),b=a+1,c=a+kGrid,d=c+1;
            indices_.insert(indices_.end(),{a,b,d,a,d,c});
        }
    }
    void validate_preparation() {
        const auto& geometry=prepared_.geometry;
        if(geometry.nodes.size()<vertices_.size()||geometry.triangles.empty())
            throw std::runtime_error("operator has no usable refined geometry");
        double movement=0;
        for(const auto& node:geometry.nodes) {
            if(!finite(node.target_position)||!finite(node.old_raster_position)||!finite(node.correction)||
                node.source_parent>=prepared_.source_triangles.size()||
                std::abs(node.source_barycentric.x+node.source_barycentric.y+node.source_barycentric.z-1)>1e-8||
                std::min({node.source_barycentric.x,node.source_barycentric.y,node.source_barycentric.z})< -1e-8)
                throw std::runtime_error("operator node lost finite ordered parent ancestry");
            movement=std::max(movement,glm::length(node.target_position-node.old_raster_position));
            if(glm::length(node.target_position-node.old_raster_position-node.correction)>1e-8)
                throw std::runtime_error("operator correction is inconsistent with final target");
        }
        if(movement>prepared_.effective_correspondence_bound+1e-8||
            movement/prepared_.minimum_homogeneous_w>kBound+1e-8)
            throw std::runtime_error("operator exceeded the physical world correspondence bound");
        if(movement<1e-5) throw std::runtime_error("curved stimulus produced an inert bounded target");
        for(std::size_t i=0;i<prepared_.native_height.size();++i) {
            const auto& sample=prepared_.native_height[i];
            if(sample.id!=i||!std::isfinite(sample.height)||sample.mesh_lod<1||
                std::abs(sample.mesh_lod-std::round(sample.mesh_lod))<.05f)
                throw std::runtime_error("actual source height IDs or noninteger mesh LOD stimulus failed");
        }
        std::printf("%s preparer disposition %d, quality %d, source %zu/%zu, final %zu/%zu, movement %.9g m, unresolved %llu\n",
            kTag,int(prepared_.disposition),prepared_.requested_quality_met,vertices_.size(),indices_.size()/3,
            geometry.nodes.size(),geometry.triangles.size(),movement,
            static_cast<unsigned long long>(geometry.metrics.sampled_quality_failures));
        std::printf("%s source sharing %zu instances, minimum W %.9g, preclip bound %.9g m, physical movement %.9g m\n",
            kTag,transforms_.size(),prepared_.minimum_homogeneous_w,prepared_.effective_correspondence_bound,
            movement/prepared_.minimum_homogeneous_w);
    }
    void validate_fallbacks() {
        auto check=[&](StaticDisplacementSurfaceRequest request,DisplacementPreparationDisposition expected) {
            auto result=preparer_->prepare(request);
            if(!result||result->disposition!=expected||result->ready()||
                !same_packed(packed_,result->original_packed)||!result->geometry.nodes.empty()||!result->geometry.triangles.empty())
                throw std::runtime_error("unsupported request did not retain the original upload identity");
        };
        auto request=request_;request.static_vertices=false;check(request,DisplacementPreparationDisposition::NonStatic);
        request=request_;request.height_route=DisplacementHeightRoute::ManualWindow;
        check(request,DisplacementPreparationDisposition::UnsupportedHeightRoute);
        request=request_;request.displacement.enabled=false;check(request,DisplacementPreparationDisposition::Disabled);
        std::array<glm::mat4,1> nonuniform{glm::mat4{1}};nonuniform[0][0][0]=1.1f;
        request=request_;request.instance_transforms=nonuniform;
        check(request,DisplacementPreparationDisposition::UnsupportedInstanceMetric);
        std::array<glm::mat4,2> mixed{glm::mat4{1},glm::mat4{1}};
        mixed[1][0][0]=mixed[1][1][1]=mixed[1][2][2]=1.1f;
        request=request_;request.instance_transforms=mixed;
        check(request,DisplacementPreparationDisposition::IncompatibleInstanceScale);
        request=request_;request.base_transform={.01f,.01f,0,0};
        check(request,DisplacementPreparationDisposition::MeshLodBelowOne);
        std::array<glm::mat4,1> projective{transforms_[0]};projective[0][0][3]=.01f;
        request=request_;request.instance_transforms=projective;
        check(request,DisplacementPreparationDisposition::UnsupportedInstanceMetric);
        for(const float w:{0.0f,-.8f}) {
            auto invalid=transforms_[0];invalid[3][3]=w;
            std::array<glm::mat4,1> invalid_w{invalid};request=request_;request.instance_transforms=invalid_w;
            check(request,DisplacementPreparationDisposition::UnsupportedInstanceMetric);
        }
        std::printf("%s unsupported nonstatic/manual/nonuniform/mixed-scale/distance-LOD/spatial-projective/nonpositive-W and disabled paths retain exact packed bytes\n",kTag);
    }
    void validate_carrier() const {
        if(carrier_.indices.size()!=prepared_.geometry.triangles.size()*3||
            carrier_.parents.size()!=carrier_.packed.positions.size())
            throw std::runtime_error("shared carrier lost ordered triangles or parent records");
        std::vector<std::uint32_t> representative(prepared_.geometry.nodes.size(),UINT32_MAX);
        for(std::size_t i=0;i<prepared_.geometry.triangles.size();++i) {
            const auto& triangle=prepared_.geometry.triangles[i];
            const auto source=prepared_.source_triangles[triangle.source_parent];
            for(std::size_t c=0;c<3;++c) {
                const auto record=carrier_.indices[3*i+c],node=triangle.nodes[c];
                if(record>=carrier_.parents.size()) throw std::runtime_error("shared carrier index escaped its records");
                const auto& attributes=carrier_.parents[record];
                for(std::size_t j=0;j<3;++j) {
                    const auto& p=packed_.positions[source[j]];
                    if(attributes[j].x!=p.x||attributes[j].y!=p.y||attributes[j].z!=p.z||
                        attributes[j].w!=float(triangle.source_barycentric[c][static_cast<int>(j)]))
                        throw std::runtime_error("shared carrier changed ordered original parent ancestry");
                    const auto& original=prepared_.source_vertices[source[j]];
                    const D3 displacement=original.displacement_direction*
                        ((double(original.height)-127.0/255.0)*original.displacement_magnitude);
                    if(attributes[j+3].x!=float(displacement.x)||attributes[j+3].y!=float(displacement.y)||
                        attributes[j+3].z!=float(displacement.z))
                        throw std::runtime_error("shared carrier changed the authoritative original height vector");
                }
                const auto previous=representative[node];
                if(previous!=UINT32_MAX) {
                    if(!same_components<3>(carrier_.packed.positions[record],carrier_.packed.positions[previous])||
                        !same_components<2>(carrier_.packed.uv0[record],carrier_.packed.uv0[previous])||
                        !same_components<2>(carrier_.packed.uv1[record],carrier_.packed.uv1[previous])||
                        !same_components<4>(carrier_.packed.tangent_frames[record],carrier_.packed.tangent_frames[previous])||
                        !same_components<4>(carrier_.packed.displacement_custom[record],carrier_.packed.displacement_custom[previous])||
                        !same_components<4>(attributes[6],carrier_.parents[previous][6]))
                        throw std::runtime_error("shared carrier split a canonical node's geometry or shading bytes");
                } else representative[node]=record;
                if(node<vertices_.size() && !same_components<4>(carrier_.packed.tangent_frames[record],packed_.tangent_frames[node]))
                    throw std::runtime_error("shared carrier repacked an original corner tangent frame");
            }
        }
        // A repeated attachment is a single GPU record. Changing only its
        // later barycentric spelling must be rejected, not silently reuse the
        // first record and conceal a geometric disagreement between triangles.
        auto corrupted=prepared_;
        std::unordered_map<std::uint64_t,bool> seen;
        bool changed=false;
        for(auto& triangle:corrupted.geometry.triangles) {
            for(std::size_t c=0;c<3&&!changed;++c) {
                const auto key=(std::uint64_t(triangle.source_parent)<<32)|triangle.nodes[c];
                if(seen.emplace(key,true).second) continue;
                auto& bary=triangle.source_barycentric[c];
                int from=0;for(int j=1;j<3;++j) if(bary[j]>bary[from]) from=j;
                const auto to=(from+1)%3;
                bary[from]-=1e-4;bary[to]+=1e-4;changed=true;
            }
            if(changed) break;
        }
        if(!changed||make_displacement_carrier(vertices_,corrupted))
            throw std::runtime_error("shared carrier accepted conflicting repeated parent weights");
        std::printf("%s carrier rejects a conflicting repeated attachment with valid-range unit-sum weights\n",kTag);
    }
    DrawMesh original_draw() {
        DrawMesh mesh;mesh.indices=indices_;mesh.quats=packed_.tangent_frames;mesh.custom0=packed_.displacement_custom;
        for(std::size_t i=0;i<vertices_.size();++i) mesh.points.push_back({packed_.positions[i],packed_.uv0[i],packed_.uv1[i]});
        mesh.extra.resize(mesh.points.size());mesh.physical_positions.resize(mesh.points.size());
        mesh.vb=original_buffers_.vb;mesh.ib=original_buffers_.ib;mesh.owns_buffers=false;return mesh;
    }
    DrawMesh refined_draw(bool bounded) {
        DrawMesh mesh;mesh.indices=carrier_.indices;mesh.quats=carrier_.packed.tangent_frames;
        mesh.custom0=carrier_.packed.displacement_custom;mesh.extra=carrier_.parents;
        for(std::size_t i=0;i<carrier_.packed.positions.size();++i)
            mesh.points.push_back({carrier_.packed.positions[i],carrier_.packed.uv0[i],carrier_.packed.uv1[i]});
        if(!bounded) for(auto& attributes:mesh.extra) attributes[6]={0,0,0,kReferenceAmplitude};
        mesh.physical_positions.resize(mesh.points.size());return mesh;
    }
    void upload_bounded_via_mesh_store(const Mesh& original) {
        // A valid carrier must not replace another uploaded owner. Rejecting
        // this request must leave that owner's buffers and metadata untouched.
        Mesh wrong=original;
        wrong.surfaces.front().vertices.front().px+=.001f;
        auto wrong_gpu=meshes_.upload(wrong);
        if(wrong_gpu.surfaces.size()!=1) throw std::runtime_error("wrong-owner control upload failed");
        auto& wrong_surface=wrong_gpu.surfaces.front();const auto saved_wrong=wrong_surface;
        const auto wrong_before=meshes_.find(wrong_surface.handle);
        if(meshes_.replace_displacement_surface(wrong_surface,vertices_,prepared_))
            throw std::runtime_error("MeshStore accepted a carrier for another uploaded source");
        const auto wrong_after=meshes_.find(wrong_surface.handle);
        const auto equal_bounds=[](const glm::vec3& a,const glm::vec3& b) {
            return a.x==b.x&&a.y==b.y&&a.z==b.z;
        };
        if(wrong_after.vb!=wrong_before.vb||wrong_after.ib!=wrong_before.ib||
            wrong_surface.handle!=saved_wrong.handle||wrong_surface.index_count!=saved_wrong.index_count||
            !equal_bounds(wrong_surface.bbox_min,saved_wrong.bbox_min)||!equal_bounds(wrong_surface.bbox_max,saved_wrong.bbox_max))
            throw std::runtime_error("wrong-owner rejection changed the uploaded source");
        auto wrong_packing=meshes_.take_original_packing(wrong_surface.handle,wrong.surfaces.front().vertices,
            wrong.surfaces.front().indices);
        if(!wrong_packing||wrong_packing->source_signature==prepared_.original_packed.source_signature)
            throw std::runtime_error("wrong-owner rejection lost its distinct original packing");

        // Replacement precedes renderable creation. The original fixture VB
        // remains alive for the unchanged/zero-S counterfactual, while bounded
        // draws borrow the actual production upload through the stable handle.
        auto replacement=meshes_.upload(original);
        if(replacement.surfaces.size()!=1) throw std::runtime_error("replacement source upload failed");
        auto& surface=replacement.surfaces.front();const auto handle=surface.handle;
        auto packing=meshes_.take_original_packing(handle,vertices_,indices_);
        if(!packing||!same_packed(*packing,packed_))
            throw std::runtime_error("replacement source differs from the authoritative original upload");
        auto result=meshes_.replace_displacement_surface(surface,vertices_,prepared_);
        if(!result) throw std::runtime_error(result.error().message);
        const auto buffers=meshes_.find(handle);
        if(!buffers.vb||!buffers.ib||surface.handle!=handle||*result!=carrier_.packed.positions.size()||
            surface.index_count!=bounded_.indices.size()||
            !equal_bounds(surface.bbox_min,carrier_.bounds_min)||!equal_bounds(surface.bbox_max,carrier_.bounds_max))
            throw std::runtime_error("MeshStore replacement changed carrier counts, bounds or handle ownership");
        bounded_.vb=buffers.vb;bounded_.ib=buffers.ib;bounded_.owns_buffers=false;
        std::printf("%s MeshStore replacement: stable handle %llu, %u vertices/%zu triangles, wrong-source owner rejected unchanged\n",
            kTag,static_cast<unsigned long long>(handle),*result,bounded_.indices.size()/3);
    }
    DrawMesh blocker_draw() {
        // The light travels toward +X. This wall is within .3 m of west-side
        // receivers and camera-visible; it never passes through the smoother.
        std::vector<Vertex> vertices;
        for(F3 p:{F3{.11f,-.21f,-.015f},F3{.11f,-.08f,-.015f},F3{.11f,-.08f,.085f},F3{.11f,-.21f,.085f}}) {
            Vertex v{};v.px=p.x;v.py=p.y;v.pz=p.z;v.nx=-1;vertices.push_back(v);
        }
        const std::vector<std::uint32_t> indices{0,2,1,0,3,2};
        auto packed=pack_static_surface(vertices,indices);if(!packed) throw std::runtime_error(packed.error().message);
        DrawMesh mesh;mesh.indices=indices;mesh.quats=packed->tangent_frames;mesh.custom0=packed->displacement_custom;
        for(std::size_t i=0;i<vertices.size();++i) {
            mesh.points.push_back({packed->positions[i],packed->uv0[i],packed->uv1[i]});
            mesh.physical_positions.push_back(world_position(d3(packed->positions[i])));mesh.extra.push_back({});
        }
        return mesh;
    }
    template<class T> void buffer(filament::VertexBuffer* vb,std::uint8_t slot,const std::vector<T>& values) {
        auto* copy=new std::uint8_t[values.size()*sizeof(T)];std::memcpy(copy,values.data(),values.size()*sizeof(T));
        vb->setBufferAt(engine_,slot,filament::VertexBuffer::BufferDescriptor(copy,values.size()*sizeof(T),
            [](void* data,std::size_t,void*){delete[] static_cast<std::uint8_t*>(data);}));
    }
    void upload(DrawMesh& mesh) {
        auto builder=filament::VertexBuffer::Builder().vertexCount(mesh.points.size()).bufferCount(4)
            .attribute(filament::VertexAttribute::POSITION,0,filament::VertexBuffer::AttributeType::FLOAT3,0,sizeof(PointUV))
            .attribute(filament::VertexAttribute::UV0,0,filament::VertexBuffer::AttributeType::FLOAT2,offsetof(PointUV,uv0),sizeof(PointUV))
            .attribute(filament::VertexAttribute::UV1,0,filament::VertexBuffer::AttributeType::FLOAT2,offsetof(PointUV,uv1),sizeof(PointUV))
            .attribute(filament::VertexAttribute::TANGENTS,1,filament::VertexBuffer::AttributeType::SHORT4,0,8)
            .normalized(filament::VertexAttribute::TANGENTS)
            .attribute(filament::VertexAttribute::CUSTOM0,2,filament::VertexBuffer::AttributeType::FLOAT4,0,16);
        for(std::uint8_t i=0;i<7;++i) builder.attribute(static_cast<filament::VertexAttribute>(
            std::uint8_t(filament::VertexAttribute::CUSTOM1)+i),3,filament::VertexBuffer::AttributeType::FLOAT4,i*16,112);
        mesh.vb=builder.build(engine_);
        mesh.ib=filament::IndexBuffer::Builder().indexCount(mesh.indices.size()).bufferType(filament::IndexBuffer::IndexType::UINT).build(engine_);
        if(!mesh.vb||!mesh.ib) throw std::runtime_error("fixture buffer allocation failed");
        buffer(mesh.vb,0,mesh.points);buffer(mesh.vb,1,mesh.quats);buffer(mesh.vb,2,mesh.custom0);buffer(mesh.vb,3,mesh.extra);
        auto* copy=new std::uint32_t[mesh.indices.size()];std::copy(mesh.indices.begin(),mesh.indices.end(),copy);
        mesh.ib->setBuffer(engine_,filament::IndexBuffer::BufferDescriptor(copy,mesh.indices.size()*sizeof(std::uint32_t),
            [](void* data,std::size_t,void*){delete[] static_cast<std::uint32_t*>(data);}));
    }
    filament::View* view(filament::Scene* scene,filament::RenderTarget* target) {
        auto* result=engine_.createView();result->setScene(scene);result->setCamera(camera_);result->setRenderTarget(target);
        result->setViewport({0,0,kExtent,kExtent});result->setPostProcessingEnabled(false);result->setShadowingEnabled(false);
        result->setAntiAliasing(filament::AntiAliasing::NONE);result->setMultiSampleAntiAliasingOptions({.enabled=false});return result;
    }
    utils::Entity entity(const DrawMesh& mesh,filament::MaterialInstance* material,filament::Scene* scene,bool receiver) {
        const auto result=utils::EntityManager::get().create();
        if(filament::RenderableManager::Builder(1).boundingBox({{0,0,0},{.5f,.5f,.25f}}).culling(false)
            .geometry(0,filament::RenderableManager::PrimitiveType::TRIANGLES,mesh.vb,mesh.ib)
            .material(0,material).castShadows(false).receiveShadows(receiver).screenSpaceContactShadows(receiver)
            .build(engine_,result)!=filament::RenderableManager::Builder::Success)
            throw std::runtime_error("fixture renderable allocation failed");
        auto& manager=engine_.getTransformManager();
        const auto instance=manager.getInstance(result);
        if(instance) manager.setTransform(instance,glm_to_filament(transforms_[0]));
        else manager.create(result,{},glm_to_filament(transforms_[0]));
        entities_.push_back(result);if(scene) scene->addEntity(result);return result;
    }
    void create_views() {
        renderer_=engine_.createRenderer();renderer_->setDisplayInfo({.refreshRate=0});renderer_->setFrameRateOptions({.interval=0});
        renderer_->setClearOptions({.clearColor={0,0,0,0},.clear=true,.discard=true});
        swapchain_=engine_.createSwapChain(kExtent,kExtent);camera_entity_=utils::EntityManager::get().create();
        camera_=engine_.createCamera(camera_entity_);
        camera_->setProjection(filament::Camera::Projection::ORTHO,-half_extent_,half_extent_,-half_extent_,half_extent_,.01,10);
        // A wall almost parallel to the camera ray amplifies raster subpixel
        // setup into a large linear-depth slope. This oblique pose conditions
        // that independent control without changing its geometry or oracle.
        const auto eye=world_position({-.85,-.25,1.8}),look_target=world_position({0,0,0});
        const auto up=rotation_*D3{0,1,0};
        camera_->lookAt({eye.x,eye.y,eye.z},{look_target.x,look_target.y,look_target.z},{up.x,up.y,up.z});
        camera_->setExposure(16.0f,1.0f/125,100);
        color_scene_=engine_.createScene();depth_scene_=engine_.createScene();
        const auto usage=filament::Texture::Usage::COLOR_ATTACHMENT|filament::Texture::Usage::BLIT_SRC;
        color_texture_=filament::Texture::Builder().width(kExtent).height(kExtent).levels(1).format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        linear_texture_=filament::Texture::Builder().width(kExtent).height(kExtent).levels(1).format(filament::Texture::InternalFormat::R32F).usage(usage).build(engine_);
        visibility_texture_=filament::Texture::Builder().width(kExtent).height(kExtent).levels(1).format(filament::Texture::InternalFormat::RGBA16F).usage(usage).build(engine_);
        for(auto& attachment:attachments_) attachment=filament::Texture::Builder().width(kExtent).height(kExtent).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F).usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        auto target=[&](filament::Texture* texture,std::size_t index) {return filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0,texture)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH,attachments_[index]).build(engine_);};
        color_target_=target(color_texture_,0);depth_target_=target(linear_texture_,1);visibility_target_=target(visibility_texture_,2);
        color_view_=view(color_scene_,color_target_);depth_view_=view(depth_scene_,depth_target_);visibility_view_=view(color_scene_,visibility_target_);
        visibility_view_->setShadowingEnabled(true);visibility_view_->setPostProcessingEnabled(true);
        filament::AmbientOcclusionOptions ao;ao.enabled=false;ao.resolution=1;visibility_view_->setAmbientOcclusionOptions(ao);
        filament::TemporalAntiAliasingOptions taa;taa.enabled=true;visibility_view_->setTemporalAntiAliasingOptions(taa);
        visibility_view_->setDithering(filament::Dithering::NONE);
        filament::LinearToneMapper mapper;
        grading_=filament::ColorGrading::Builder().toneMapper(&mapper)
            .outputColorSpace(filament::color::Rec709-filament::color::Linear-filament::color::D65).build(engine_);
        visibility_view_->setColorGrading(grading_);
        color_entity_=entity(source_,color_material_,color_scene_,true);depth_entity_=entity(source_,depth_material_,depth_scene_,false);
        blocker_color_entity_=entity(blocker_,blocker_material_,nullptr,false);blocker_depth_entity_=entity(blocker_,blocker_depth_,nullptr,false);
        light_=utils::EntityManager::get().create();entities_.push_back(light_);
        filament::LightManager::Builder(filament::LightManager::Type::SUN).color({1,1,1}).intensity(50000)
            .sunAngularRadius(3).direction({float(-std::cos(kSunElevation)),0,float(-std::sin(kSunElevation))})
            .castShadows(true).build(engine_,light_);
        color_scene_->addEntity(light_);
    }
    D4 world_position4(D3 position) const {
        return glm::dmat4(transforms_[0])*D4(position,1);
    }
    D3 world_position(D3 position) const {
        const auto p=world_position4(position);return D3(p)/p.w;
    }
    double fade(std::uint32_t source,const State& state) const {
        const double distance=glm::length(D3(world_position4(d3(packed_.positions[source])))-d3(state.fade_origin));
        return std::pow(std::max(0.0,1-std::clamp((distance-state.threshold)*state.inverse_range,0.0,8.0)),state.power);
    }
    void select(Shape shape,const State& state,bool masked,bool blocker) {
        active_=shape==Shape::Original?&source_:shape==Shape::Zero?&zero_:&bounded_;blocker_enabled_=blocker;
        clamped_correction_records_=0;maximum_unclamped_physical_correction_=0;
        const F4 parameters{state.amplitude,state.power,state.threshold,state.inverse_range};
        for(auto* material:{color_material_,depth_material_,masked_material_}) material->setParameter("displacementParams",parameters);
        for(auto* view:{color_view_,depth_view_,visibility_view_}) view->setMaterialGlobal(1,F4{state.fade_origin,1});
        auto& manager=engine_.getRenderableManager();
        for(auto entity:{color_entity_,depth_entity_}) manager.setGeometryAt(manager.getInstance(entity),0,
            filament::RenderableManager::PrimitiveType::TRIANGLES,active_->vb,active_->ib,0,active_->indices.size());
        manager.setMaterialInstanceAt(manager.getInstance(depth_entity_),0,masked?masked_material_:depth_material_);
        if(blocker) {color_scene_->addEntity(blocker_color_entity_);depth_scene_->addEntity(blocker_depth_entity_);}
        else {color_scene_->remove(blocker_color_entity_);depth_scene_->remove(blocker_depth_entity_);}
        std::vector<D3> corners(vertices_.size());std::vector<double> fades(vertices_.size());
        for(std::size_t i=0;i<vertices_.size();++i) {
            fades[i]=fade(std::uint32_t(i),state);
            const auto neutral=world_position4(d3(packed_.positions[i]));
            const D4 displaced=neutral+D4((rotation_*d3(packed_.unit_displacement_directions[i]))*
                (packed_.displacement_custom[i].x*double(state.amplitude)*fades[i]*
                    (double(prepared_.native_height[i].height)-double(127.0f/255.0f))),0);
            corners[i]=D3(displaced)/displaced.w;
        }
        source_.physical_positions=corners;
        if(shape==Shape::Original) active_->physical_positions=corners;
        else for(std::size_t i=0;i<prepared_.geometry.triangles.size();++i) {
            const auto& triangle=prepared_.geometry.triangles[i];const auto ids=prepared_.source_triangles[triangle.source_parent];
            for(std::size_t corner=0;corner<3;++corner) {
                const auto b=triangle.source_barycentric[corner];D3 p{0};double f=0;
                for(std::size_t k=0;k<3;++k) {p+=corners[ids[k]]*b[static_cast<int>(k)];f+=fades[ids[k]]*b[static_cast<int>(k)];}
                if(shape==Shape::Bounded) {
                    D3 correction=(rotation_*prepared_.geometry.nodes[triangle.nodes[corner]].correction)*
                        (double(state.amplitude)/kReferenceAmplitude);
                    const double raw_bound=kBound*std::min(1.0,homogeneous_w_);
                    const double length=glm::length(correction);
                    maximum_unclamped_physical_correction_=std::max(maximum_unclamped_physical_correction_,length/homogeneous_w_);
                    if(length>raw_bound) {correction*=raw_bound/length;++clamped_correction_records_;}
                    if(glm::length(correction)/homogeneous_w_>kBound+1e-8)
                        throw std::runtime_error("homogeneous smoothing exceeded the physical world bound");
                    p+=correction*(f/homogeneous_w_);
                }
                active_->physical_positions[active_->indices[3*i+corner]]=p;
            }
            std::array<D3,3> old,target;
            for(std::size_t c=0;c<3;++c) {
                old[c]={0,0,0};
                for(std::size_t k=0;k<3;++k) old[c]+=corners[ids[k]]*triangle.source_barycentric[c][static_cast<int>(k)];
                target[c]=active_->physical_positions[active_->indices[3*i+c]];
            }
            const D3 before=glm::cross(old[1]-old[0],old[2]-old[0]);
            const D3 after=glm::cross(target[1]-target[0],target[2]-target[0]);
            if(!finite(after)||glm::length(after)<1e-15||glm::dot(before,after)<=0)
                throw std::runtime_error("amplitude/fade state reversed or degenerated a final triangle");
            for(std::size_t c=0;c<3;++c) if(glm::length(target[c]-old[c])>kBound+1e-8)
                throw std::runtime_error("amplitude/fade state exceeded the world movement bound");
        }
    }

    filament::Engine& engine_;TextureStore textures_;MaterialInstanceCache materials_;MeshStore meshes_;
    MeshStore::SurfaceBuffers original_buffers_;
    std::vector<Vertex> vertices_;std::vector<std::uint32_t> indices_;PackedStaticSurface packed_;
    std::vector<glm::mat4> transforms_{glm::mat4{1}};
    glm::dmat3 rotation_{1};double half_extent_=kHalfExtent,homogeneous_w_=1;
    std::size_t clamped_correction_records_=0;double maximum_unclamped_physical_correction_=0;
    StaticDisplacementSurfaceRequest request_;PreparedDisplacementSurface prepared_;
    DisplacementCarrier carrier_;
    std::unique_ptr<HeightSampleBatch> height_batch_;std::unique_ptr<DisplacementSurfacePreparer> preparer_;
    filament::MaterialInstance *color_material_=nullptr,*blocker_material_=nullptr,*depth_material_=nullptr,*masked_material_=nullptr,*blocker_depth_=nullptr;
    filament::Material *depth_base_=nullptr,*masked_base_=nullptr;
    filament::Texture *height_=nullptr,*color_texture_=nullptr,*linear_texture_=nullptr,*visibility_texture_=nullptr;
    std::array<filament::Texture*,3> attachments_{};
    filament::RenderTarget *color_target_=nullptr,*depth_target_=nullptr,*visibility_target_=nullptr;
    filament::Scene *color_scene_=nullptr,*depth_scene_=nullptr;
    filament::View *color_view_=nullptr,*depth_view_=nullptr,*visibility_view_=nullptr;
    filament::Camera* camera_=nullptr;filament::ColorGrading* grading_=nullptr;
    filament::Renderer* renderer_=nullptr;filament::SwapChain* swapchain_=nullptr;
    utils::Entity camera_entity_,color_entity_,depth_entity_,blocker_color_entity_,blocker_depth_entity_,light_;
    std::vector<utils::Entity> entities_;
    DrawMesh source_,zero_,bounded_,blocker_;DrawMesh* active_=nullptr;bool blocker_enabled_=false;
};

GeometryCheck check_geometry(const Capture& capture,const Oracle& reference,bool report_diagnostics=true) {
    GeometryCheck result;
    std::size_t diagnostics=0;
    std::size_t worst_pixel=0;
    for(std::size_t i=0;i<capture.depth.size();++i) {
        if(!std::isfinite(capture.depth[i])) {result.error=1e100;continue;}
        const bool depth=capture.depth[i]>.001f,color=capture.mask[4*i]>127,expected=reference.depth[i]>.001;
        result.pass_mismatch+=depth!=color;
        if(depth!=color&&report_diagnostics&&diagnostics++<8)
            std::printf("%s pass mismatch pixel %zu,%zu color %u,%u,%u,%u depth %.9g CPU %.9g margin %.6g px\n",
                kTag,i%kExtent,i/kExtent,capture.mask[4*i],capture.mask[4*i+1],capture.mask[4*i+2],capture.mask[4*i+3],
                capture.depth[i],reference.depth[i],reference.edge_margin[i]);
        if(reference.edge_margin[i]<kInteriorMargin) {++result.excluded_edges;continue;}
        result.interior_coverage+=depth!=expected;
        if(depth&&expected) {
            ++result.samples;const double error=std::abs(double(capture.depth[i])-reference.depth[i]);
            if(error>result.error) {result.error=error;worst_pixel=i;}
            result.blocker_samples+=reference.blocker[i];
        }
    }
    if(report_diagnostics&&result.error>kDepthTolerance) {
        const auto p=reference.point[worst_pixel];
        std::printf("%s maximum depth witness %zu,%zu measured %.9g CPU %.9g error %.9g m, blocker %d, owner %u, margin %.6g px, world %.9g %.9g %.9g\n",
            kTag,worst_pixel%kExtent,worst_pixel/kExtent,capture.depth[worst_pixel],reference.depth[worst_pixel],result.error,
            int(bool(reference.blocker[worst_pixel])),reference.owner[worst_pixel],reference.edge_margin[worst_pixel],p.x,p.y,p.z);
        const auto gradient=reference.depth_gradient[worst_pixel];
        std::printf("%s witness physical depth gradient %.9g/%.9g m/pixel, diagnostic nearest1/256-vertex depth %.9g m\n",
            kTag,gradient.x,gradient.y,reference.quantized_depth[worst_pixel]);
    }
    return result;
}
bool valid(const GeometryCheck& check) {
    return check.interior_coverage==0&&check.pass_mismatch==0&&check.samples>=100&&check.error<=kDepthTolerance;
}
double channel_mean(const std::vector<float>& pixels,std::size_t pixel) {
    return (double(pixels[4*pixel])+pixels[4*pixel+1]+pixels[4*pixel+2])/3;
}

bool run_family(filament::Engine& engine,bool colored) {
    DisplacementSurfaceFixture fixture(engine,colored);
    Capture original,zero,bounded;
    if(!fixture.capture(Shape::Original,State{},false,false,original)) return false;
    const auto a=check_geometry(original,fixture.oracle(false),false),b=check_geometry(original,fixture.oracle(true),false);
    const bool reverse_y=b.interior_coverage<a.interior_coverage||
        (b.interior_coverage==a.interior_coverage&&b.error<a.error);
    if(!valid(reverse_y?b:a)) {
        static_cast<void>(check_geometry(original,fixture.oracle(reverse_y)));
        std::printf("%s original oracle failed: orientation0 %.9g/%zu, orientation1 %.9g/%zu\n",kTag,a.error,a.interior_coverage,b.error,b.interior_coverage);
        return false;
    }
    const std::array states{State{},State{.35f,.7f,1000,.001f,{0,0,0}},
        State{1.4f,.7f,1000,.001f,{0,0,0}},State{.7f,.7f,0,2,{-0.4f,0,0}}};
    double largest_motion=0,zero_error=0;
    for(std::size_t state=0;state<states.size();++state) {
        const auto s=states[state];
        for(const auto shape:{Shape::Original,Shape::Zero,Shape::Bounded}) {
            auto& pixels=shape==Shape::Original?original:shape==Shape::Zero?zero:bounded;
            if(!fixture.capture(shape,s,state%2,false,pixels)) return false;
            const auto reference=fixture.oracle(reverse_y);const auto check=check_geometry(pixels,reference);
            std::printf("%s family %d state %zu shape %d: strict pass mismatch %zu, interior coverage %zu, samples %zu, excluded %zu, max depth %.9g m\n",
                kTag,colored,state,int(shape),check.pass_mismatch,check.interior_coverage,check.samples,check.excluded_edges,check.error);
            if(!valid(check)) return false;
            if(shape==Shape::Zero) for(std::size_t i=0;i<zero.depth.size();++i)
                if(reference.depth[i]>.001&&reference.edge_margin[i]>=kInteriorMargin&&original.depth[i]>.001f)
                    zero_error=std::max(zero_error,std::abs(double(zero.depth[i])-original.depth[i]));
            if(shape==Shape::Bounded) for(std::size_t i=0;i<bounded.depth.size();++i)
                if(reference.depth[i]>.001&&reference.edge_margin[i]>=kInteriorMargin&&zero.depth[i]>.001f)
                    largest_motion=std::max(largest_motion,std::abs(double(bounded.depth[i])-zero.depth[i]));
        }
    }
    if(zero_error>kDepthTolerance||largest_motion<1e-5) return false;
    std::array<DisplacementSurfaceFixture::Horizon,3> horizons;
    for(const auto shape:{Shape::Original,Shape::Zero,Shape::Bounded}) {
        Capture pixels;
        if(!fixture.capture(shape,State{},false,false,pixels)) return false;
        horizons[std::size_t(shape)]=fixture.horizon();const auto& h=horizons[std::size_t(shape)];
        std::printf("%s family %d own horizon shape %d: front %zu partial %zu back %zu, adjacent cap jump %.6f\n",
            kTag,colored,int(shape),h.fully_front,h.partial,h.fully_back,h.maximum_adjacent_mass_jump);
        if(h.fully_front==0||h.partial==0||h.fully_back==0) return false;
    }
    // The source must actually exercise the coarse geometric-horizon problem.
    // A field that never crosses the Sun disc would be an inert regression.
    if(horizons[0].maximum_adjacent_mass_jump<.25) return false;
    Capture blocker_zero,blocker_bounded;
    if(!fixture.capture(Shape::Zero,State{},false,true,blocker_zero)) return false;
    // Ownership is evaluated separately for each receiver state. Smoothing can
    // uncover or hide wall pixels without changing the wall's physical depth.
    const auto zero_reference=fixture.oracle(reverse_y);
    const auto blocker_zero_check=check_geometry(blocker_zero,zero_reference);
    if(!fixture.capture(Shape::Bounded,State{},true,true,blocker_bounded)) return false;
    const auto reference=fixture.oracle(reverse_y);const auto blocker_check=check_geometry(blocker_bounded,reference);
    double blocker_error=0;std::size_t blocker_samples=0,blocker_ownership_changes=0;
    for(std::size_t i=0;i<reference.depth.size();++i) {
        blocker_ownership_changes+=bool(zero_reference.blocker[i])!=bool(reference.blocker[i]);
        if(zero_reference.blocker[i]&&reference.blocker[i]&&
            zero_reference.edge_margin[i]>=kInteriorMargin&&reference.edge_margin[i]>=kInteriorMargin) {
            ++blocker_samples;blocker_error=std::max(blocker_error,std::abs(double(blocker_zero.depth[i])-blocker_bounded.depth[i]));
        }
    }
    std::printf("%s family %d zero-S blocker oracle: pass mismatch %zu, interior coverage %zu, samples %zu, excluded %zu, max depth %.9g m\n",
        kTag,colored,blocker_zero_check.pass_mismatch,blocker_zero_check.interior_coverage,blocker_zero_check.samples,
        blocker_zero_check.excluded_edges,blocker_zero_check.error);
    std::printf("%s family %d blocker control: pass mismatch %zu, interior coverage %zu, samples %zu, excluded %zu, oracle error %.9g m, independent interior samples %zu, unchanged depth %.9g m\n",
        kTag,colored,blocker_check.pass_mismatch,blocker_check.interior_coverage,blocker_check.samples,blocker_check.excluded_edges,
        blocker_check.error,blocker_samples,blocker_error);
    std::printf("%s family %d blocker visibility ownership changed at %zu pixels; depth invariance uses %zu common interior wall pixels\n",
        kTag,colored,blocker_ownership_changes,blocker_samples);
    if(!valid(blocker_zero_check)||!valid(blocker_check)||blocker_samples<32||blocker_error>kDepthTolerance) return false;
    std::printf("%s family %d zero-S %.9g m, actual bounded depth motion %.9g m, independent blocker %zu pixels %.9g m\n",
        kTag,colored,zero_error,largest_motion,blocker_samples,blocker_error);

    // A few well-separated final-mesh receiver pixels bound the visibility
    // workload. No assertion claims that a particular smoothing model must
    // brighten or darken them. The CPU result specifies the physical target.
    std::vector<float> off,on;
    if(!fixture.capture_visibility(Shape::Bounded,true,false,off)||
       !fixture.capture_visibility(Shape::Bounded,true,true,on)) return false;
    const auto visibility_reference=fixture.oracle(reverse_y);
    std::size_t horizon=0,independent=0,evaluated=0,invalid=0,hard_back=0,clear_front=0;
    double maximum_error=0,blocker_motion=0,hard_back_error=0,clear_front_error=0;
    for(std::uint32_t y=16;y<kExtent-16;y+=16) for(std::uint32_t x=16;x<kExtent-16;x+=16) {
        const auto pixel=std::size_t(y)*kExtent+x;
        if(visibility_reference.depth[pixel]<.001||visibility_reference.blocker[pixel]||
            visibility_reference.edge_margin[pixel]<.35) continue;
        const auto physical=fixture.visibility(visibility_reference,pixel,true);
        const double unshadowed=channel_mean(off,pixel),shadowed=channel_mean(on,pixel);
        if(unshadowed<1e-4||unshadowed>.9||!std::isfinite(unshadowed)||!std::isfinite(shadowed)) {++invalid;continue;}
        const double occlusion=1-shadowed/unshadowed;
        const double error=std::abs(occlusion-physical.total);
        maximum_error=std::max(maximum_error,error);++evaluated;
        horizon+=physical.own>.05&&physical.own<.95;
        // Saturated own-horizon and clear-ray controls have a conservative
        // angular margin from the Sun rim. Fractional screen-space contacts
        // remain reported rather than forced to an unmeasured image target.
        if(physical.central_dot < -std::sin(kSunRadius)-.015) {
            ++hard_back;hard_back_error=std::max(hard_back_error,std::abs(1-occlusion));
        }
        if(physical.central_dot > std::sin(kSunRadius)+.015 && physical.total==0) {
            ++clear_front;clear_front_error=std::max(clear_front_error,std::abs(occlusion));
        }
        if(physical.blocker>.95&&physical.own<.05&&physical.external<.05) {
            ++independent;blocker_motion=std::max(blocker_motion,occlusion);
        }
        std::printf("%s visibility family %d pixel %u,%u CPU own %.6f surface %.6f blocker %.6f total %.6f GPU %.6f error %.6f\n",
            kTag,colored,x,y,physical.own,physical.external,physical.blocker,physical.total,occlusion,error);
    }
    std::printf("%s family %d final mesh visibility %zu probes, partial horizon %zu, independent full blocker %zu, invalid %zu, max reference error %.6g, hard back %zu/error %.6g, clear front %zu/error %.6g\n",
        kTag,colored,evaluated,horizon,independent,invalid,maximum_error,hard_back,hard_back_error,clear_front,clear_front_error);
    // The final triangle-ray reference and screen-space estimator have different
    // domains. Keep their discrepancy as evidence; do not tune a brightness
    // threshold to claim geometric convergence. The existing finite-Sun and
    // contact fixtures retain their numerical shader acceptance obligations.
    return evaluated>=8&&horizon>0&&independent>0&&blocker_motion>.5&&
        hard_back>0&&clear_front>0&&hard_back_error<=.2&&clear_front_error<=.2;
}

bool run_proper_uniform_pose(filament::Engine& engine,bool constant_w=false) {
    // The original source packing remains unchanged. Its instance metric is
    // rebuilt for this pose, while native-height displacement and smoothing
    // retain world-metre units rather than receiving uniform model scale.
    const auto rotation=glm::rotate(glm::mat4{1},.31f,glm::normalize(glm::vec3{1,2,3}));
    auto model=glm::translate(glm::mat4{1},glm::vec3{.12f,-.09f,.06f})*
        rotation*glm::scale(glm::mat4{1},glm::vec3{.75f});
    if(constant_w) model[3][3]=.8f;
    // The second sharing instance retains W=1. Its raw scale and source are
    // identical, so minW=.8 must bound the shared preclip correction.
    DisplacementSurfaceFixture fixture(engine,false,model,constant_w);
    const char* pose_label=constant_w?"constant-W":"proper-uniform";
    State pose_state{.7f,.7f,0,2,{0,0,0}};
    if(constant_w) pose_state.amplitude=1.4f;
    pose_state.fade_origin=f3(D3(glm::dmat4(model)*glm::dvec4(-.4,0,0,1)));
    const auto run_state = [&](const State& state, const char* label, bool require_clamp) {
        Capture original,zero,bounded;
        if(!fixture.capture(Shape::Original,state,false,false,original)) return false;
        const auto a=check_geometry(original,fixture.oracle(false),false),b=check_geometry(original,fixture.oracle(true),false);
        const bool reverse_y=b.interior_coverage<a.interior_coverage||
            (b.interior_coverage==a.interior_coverage&&b.error<a.error);
        const auto original_check=reverse_y?b:a;
        std::printf("%s %s pose scale .75 rotation .31 translation .12/-.09/.06 W %.9g shape 0: pass mismatch %zu, interior coverage %zu, samples %zu, max physical depth %.9g m\n",
            kTag,label,original.homogeneous_w,original_check.pass_mismatch,original_check.interior_coverage,original_check.samples,original_check.error);
        if(!valid(original_check)) {
            static_cast<void>(check_geometry(original,fixture.oracle(reverse_y)));
            return false;
        }
        const auto original_reference=fixture.oracle(reverse_y);
        double zero_error=0,bounded_motion=0;
        for(const auto shape:{Shape::Zero,Shape::Bounded}) {
            auto& pixels=shape==Shape::Zero?zero:bounded;
            if(!fixture.capture(shape,state,shape==Shape::Bounded,false,pixels)) return false;
            const auto reference=fixture.oracle(reverse_y);const auto check=check_geometry(pixels,reference);
            std::printf("%s %s pose shape %d: pass mismatch %zu, interior coverage %zu, samples %zu, max physical depth %.9g m\n",
                kTag,label,int(shape),check.pass_mismatch,check.interior_coverage,check.samples,check.error);
            if(shape==Shape::Bounded)
                std::printf("%s %s pose amplitude %.9g: unweighted attempted physical correction %.9g m, clamped parent-local records %zu\n",
                    kTag,label,state.amplitude,fixture.maximum_unclamped_physical_correction(),fixture.clamped_correction_records());
            if(!valid(check)) return false;
            if(shape==Shape::Bounded && require_clamp &&
                (fixture.maximum_unclamped_physical_correction()<=.0201||
                 fixture.clamped_correction_records()==0)) {
                std::printf("%s %s cap stimulus did not exercise the physical correction clamp\n",kTag,label);
                return false;
            }
            for(std::size_t i=0;i<reference.depth.size();++i) {
                if(reference.depth[i]<=.001||reference.edge_margin[i]<kInteriorMargin||
                    original_reference.depth[i]<=.001||original_reference.edge_margin[i]<kInteriorMargin) continue;
                if(shape==Shape::Zero) zero_error=std::max(zero_error,std::abs(double(zero.depth[i])-original.depth[i]));
                else if(zero.depth[i]>.001f) bounded_motion=std::max(bounded_motion,std::abs(double(bounded.depth[i])-zero.depth[i]));
            }
        }
        std::printf("%s %s pose with dynamic parent fade: zero-S physical %.9g m, bounded physical depth motion %.9g m, raw eyeDepth divided by W %.9g\n",
            kTag,label,zero_error,bounded_motion,original.homogeneous_w);
        return zero_error<=kDepthTolerance&&bounded_motion>=1e-5;
    };
    if(!run_state(pose_state,pose_label,false)) return false;
    if(!constant_w) return true;
    // This uses the same sampled source, target, uploaded buffers and pose.
    // Only amplitude changes: clamping occurs before the existing parent fade.
    State cap_state=pose_state;cap_state.amplitude=4.0f;
    fixture.condition_cap_measurement_camera(cap_state);
    return run_state(cap_state,"constant-W-cap",true);
}
} // namespace

CaseResult run_displacement_surface_case(const CaseContext& ctx) {
    if(ctx.backend.api()!=GraphicsApi::Filament) return CaseResult::Skip;
    if(auto initialized=ctx.backend.init_pipelines(ctx.config);!initialized) {
        std::printf("%s pipeline initialization failed: %s\n",kTag,initialized.error().message.c_str());return CaseResult::Fail;
    }
    auto* engine=static_cast<FilamentBackend&>(ctx.backend).debug_render_context().engine;
    if(!engine) {std::printf("%s initialized Engine is missing\n",kTag);return CaseResult::Fail;}
    try {return run_family(*engine,false)&&run_family(*engine,true)&&
        run_proper_uniform_pose(*engine)&&run_proper_uniform_pose(*engine,true)?CaseResult::Pass:CaseResult::Fail;}
    catch(const std::exception& error) {
        std::printf("%s failure: %s\n",kTag,error.what());return CaseResult::Fail;
    }
}
} // namespace heaven::smoke
