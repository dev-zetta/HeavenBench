// Requires public View prepass opt-in and actual GPU diagnostics.
// FLOAT final-color/coverage and geometric blocker controls observe main-depth
// behavior indirectly; this fixture does not read the private FG depth texture.
#include "smoke_cases.h"
#include "benchmark/microprofiler.h"
#include "benchmark/microprofile_report.h"
#include "rhi/filament_backend.h"
#include "rhi/displacement_surface_preparer.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/profile_bridge.h"
#include "rhi/texture_store.h"
#include "rhi/variable_rate_shading.h"

#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/ColorSpace.h>
#include <filament/Engine.h>
#include <filament/InstanceBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/ToneMapper.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef HEAVEN_SMOKE_VRS_UNLIT_PACKAGE
#define HEAVEN_SMOKE_VRS_UNLIT_PACKAGE ""
#endif

namespace heaven::smoke {
namespace {
constexpr auto kTag = "[smoke_opaque_depth_prepass]";
constexpr std::uint32_t kExtent = 192;
using Pixels = std::vector<float>;
using F3 = filament::math::float3;
using F4 = filament::math::float4;
using M4 = filament::math::mat4f;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Layer { float z; float u = .25f; };
struct Observed {
    Pixels pixels;
    std::uint64_t frame = 0, depth_commands = 0, depth_primitives = 0;
    std::uint64_t opaque_commands = 0, opaque_fragments = 0, coarse_draws = 0;
    bool prepass = false, invocation_counts = false, refraction = false, native_rates = false;
};

Mesh layers(std::string name, std::vector<Layer> const& records) {
    Mesh mesh; mesh.name = mesh.path = std::move(name);
    Surface surface; surface.name = "layers";
    for (auto const layer : records) {
        auto const base = static_cast<std::uint32_t>(surface.vertices.size());
        for (auto const p : std::array<glm::vec2, 4>{{{-.73f,-.67f},{.73f,-.67f},{.73f,.67f},{-.73f,.67f}}}) {
            Vertex v{}; v.px = p.x; v.py = p.y; v.pz = layer.z;
            v.nz = v.tx = v.tw = v.displacement_normal_scale = 1;
            v.u = v.u1 = layer.u; v.v = v.v1 = .5f;
            surface.vertices.push_back(v);
        }
        for (auto const i : {0u,1u,2u,0u,2u,3u}) surface.indices.push_back(base+i);
    }
    mesh.surfaces.push_back(std::move(surface)); return mesh;
}

SkinnedMesh skinned_card() {
    auto source = layers("prepass-one-bone-card", {{0}});
    SkinnedMesh mesh; mesh.name = mesh.path = source.path;
    mesh.bones.push_back({"root", -1}); mesh.num_frames = 2;
    BoneFrame rest; rest.position = {0,0,0,1}; rest.rotation = {1,0,0,0}; mesh.frames.assign(2, rest);
    SkinnedSurface surface; surface.name = "one-bone-card"; surface.indices = source.surfaces[0].indices;
    for (auto const& input : source.surfaces[0].vertices) {
        SkinnedVertex v{}; v.px=input.px; v.py=input.py; v.pz=input.pz;
        v.nz=1; v.u=v.u1=input.u; v.v=v.v1=input.v; v.bone_weights[0]=1;
        surface.vertices.push_back(v);
    }
    mesh.surfaces.push_back(std::move(surface)); return mesh;
}

class PrepassFixture {
public:
    explicit PrepassFixture(filament::Engine& engine)
        : engine_(engine), textures_(engine), materials_(engine,textures_), meshes_(engine) {}
    PrepassFixture(PrepassFixture const&) = delete;
    PrepassFixture& operator=(PrepassFixture const&) = delete;
    ~PrepassFixture() {
        engine_.flushAndWait(); bridge_.reset(); clear();
        if (view_) { view_->setRenderTarget(nullptr); view_->setScene(nullptr); view_->setCamera(nullptr); }
        if (emission_) {
            for (auto* mi : {lit_, other_}) if (mi) mi->setParameter("emissiveMap", textures_.placeholder(), sampler());
            engine_.flushAndWait();
        }
        engine_.destroy(view_); engine_.destroy(scene_); engine_.destroy(grading_);
        engine_.destroy(target_); engine_.destroy(color_);
        engine_.destroy(minimal_instance_); engine_.destroy(minimal_); engine_.destroy(emission_);
        if (camera_entity_) { engine_.destroyCameraComponent(camera_entity_); utils::EntityManager::get().destroy(camera_entity_); }
        engine_.destroy(renderer_); engine_.destroy(swapchain_);
    }
    void init() {
        textures_.create_placeholders(); materials_.set_colored_penumbra(false);
        require(materials_.load_base_materials().has_value(),"production packages unavailable");
        lit_=materials_.get_or_create("opaque-prepass-smoke","lit",GpuMaterial{});
        other_=materials_.get_or_create("opaque-prepass-smoke","other",GpuMaterial{});
        require(lit_ && other_,"production opaque instances unavailable");
        std::ifstream input(HEAVEN_SMOKE_VRS_UNLIT_PACKAGE,std::ios::binary);
        require(input.is_open(),"existing test-only default-depth package unavailable");
        std::vector<char> package{std::istreambuf_iterator<char>(input),{}};
        minimal_=filament::Material::Builder().package(package.data(),package.size()).build(engine_);
        require(minimal_,"default-depth package creation failed");
        minimal_instance_=minimal_->createInstance(); require(minimal_instance_,"default-depth instance unavailable");
        minimal_instance_->setParameter("pattern",0);
        emission_=filament::Texture::Builder().width(2).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
        require(emission_,"two-color emission texture unavailable");
        auto* bytes=new std::uint8_t[8]{255,8,8,255,8,255,8,255};
        emission_->setImage(engine_,0,filament::Texture::PixelBufferDescriptor(bytes,8,
            filament::Texture::Format::RGBA,filament::Texture::Type::UBYTE,
            [](void* p,std::size_t,void*){delete[] static_cast<std::uint8_t*>(p);}));
        for (auto* mi : {lit_,other_}) {
            mi->setParameter("hasEmissive",true); mi->setParameter("emissiveMap",emission_,sampler());
            // Keep both UV marker colors below the linear mapper's clipping
            // point so the independent depth/order winner stays observable.
            mi->setParameter("emissionTint",F3{1,1,1}); mi->setParameter("emissionScale",.5f);
            mi->setParameter("surfaceEmissionTuning",F4{0,1,0,.5f});
        }
        renderer_=engine_.createRenderer();swapchain_=engine_.createSwapChain(kExtent,kExtent);
        scene_=engine_.createScene();view_=engine_.createView();camera_entity_=utils::EntityManager::get().create();
        camera_=engine_.createCamera(camera_entity_);
        require(renderer_ && swapchain_ && scene_ && view_ && camera_,"fixture owners unavailable");
        renderer_->setDisplayInfo({.refreshRate=0});renderer_->setFrameRateOptions({.interval=0});
        renderer_->setClearOptions({.clearColor={0,0,0,0},.clear=true,.discard=true});
        camera_->setProjection(filament::Camera::Projection::ORTHO,-1,1,-1,1,1,9);
        camera_->lookAt({0,0,4},{0,0,0},{0,1,0});camera_->setExposure(16,1.0f/125,100);
        view_->setScene(scene_);view_->setCamera(camera_);view_->setPostProcessingEnabled(true);
        view_->setScreenSpaceRefractionEnabled(true);
        view_->setBlendMode(filament::View::BlendMode::OPAQUE);view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);view_->setTemporalAntiAliasingOptions({.enabled=false});
        view_->setAmbientOcclusionOptions({.enabled=false});view_->setDithering(filament::Dithering::NONE);
        view_->setDynamicResolutionOptions({.enabled=false});
        // Zero-strength bloom selects an ordinary post-process color pass at
        // both sample counts, avoiding the excluded color-grading callback.
        view_->setBloomOptions({.strength=0.0f,.enabled=true});
        filament::LinearToneMapper mapper;
        grading_=filament::ColorGrading::Builder().toneMapper(&mapper)
            .outputColorSpace(filament::color::Rec709-filament::color::Linear-filament::color::D65).build(engine_);
        require(grading_,"linear grading unavailable");view_->setColorGrading(grading_);resize(kExtent);
        bridge_=std::make_unique<ProfileBridge>(recorder_);
    }
    void clear() {
        engine_.flushAndWait();
        for (auto entity : objects_) {scene_->remove(entity);engine_.destroy(entity);utils::EntityManager::get().destroy(entity);}
        objects_.clear();for(auto const& mesh:uploaded_)meshes_.release(mesh);uploaded_.clear();
        for(auto* buffer:instances_)engine_.destroy(buffer);instances_.clear();
        if (lit_) {lit_->setParameter("geometryDisplacement",false);lit_->setDepthCulling(true);lit_->setDepthWrite(true);lit_->setColorWrite(true);lit_->setPolygonOffset(0,0);lit_->setStencilWrite(false);lit_->setCullingMode(filament::backend::CullingMode::BACK);}
        if (view_) {view_->setStencilBufferEnabled(false);view_->setGuardBandOptions({.enabled=false});view_->setFragmentShadingRate(filament::backend::FragmentShadingRate::FULL);}
    }
    void resize(std::uint32_t size) {
        engine_.flushAndWait();view_->setRenderTarget(nullptr);engine_.destroy(target_);engine_.destroy(color_);
        size_=size;
        // Texture usage is a documented combinable flag set.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage=static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT)|
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_=filament::Texture::Builder().width(size).height(size).levels(1)
            .format(filament::Texture::InternalFormat::RGBA16F).usage(usage).build(engine_);
        target_=filament::RenderTarget::Builder().texture(filament::RenderTarget::AttachmentPoint::COLOR0,color_).build(engine_);
        require(color_ && target_,"FLOAT target unavailable");view_->setRenderTarget(target_);view_->setViewport({0,0,size,size});
    }
    utils::Entity add(std::string name,std::vector<Layer> const& records,filament::MaterialInstance* material,
            M4 const& model=M4{1},bool skin=false,bool instance=false,bool carrier=false) {
        auto source=layers(std::move(name),records);
        auto gpu=skin?meshes_.upload_skinned(skinned_card()):meshes_.upload(source);
        require(gpu.surfaces.size()==1,"one-surface upload failed");
        if(carrier)prepare_carrier(gpu.surfaces[0],source.surfaces[0]);
        uploaded_.push_back(gpu);
        auto const& surface=gpu.surfaces[0];auto buffers=meshes_.find(surface.handle);
        require(buffers.vb && buffers.ib,"surface buffers unavailable");
        require(!records.empty(),"sort bounds need at least one authored layer");
        auto const z_range=std::minmax_element(records.begin(),records.end(),
            [](Layer const& left,Layer const& right){return left.z<right.z;});
        auto const center_z=(z_range.first->z+z_range.second->z)*.5f;
        auto entity=utils::EntityManager::get().create();
        filament::RenderableManager::Builder builder(1);
        builder.geometry(0,
            filament::RenderableManager::PrimitiveType::TRIANGLES,buffers.vb,buffers.ib,0,surface.index_count)
            .material(0,material).boundingBox({{0,0,center_z},{2,2,2}}).culling(false).castShadows(false).receiveShadows(false);
        if(skin)builder.skinning(1);
        if(instance) {
            std::array<M4,2> transforms{M4::translation(F3{-.35f,0,0})*M4::scaling(F3{.4f,.6f,1}),
                M4::translation(F3{.35f,0,0})*M4::scaling(F3{.4f,.6f,1})};
            auto* buffer=filament::InstanceBuffer::Builder(transforms.size()).localTransforms(transforms.data()).build(engine_);
            require(buffer,"actual instance buffer unavailable");instances_.push_back(buffer);builder.instances(transforms.size(),buffer);
        }
        require(builder.build(engine_,entity)==filament::RenderableManager::Builder::Success,"renderable creation failed");
        auto& tm=engine_.getTransformManager();auto transform=tm.getInstance(entity);
        if(transform)tm.setTransform(transform,model);else tm.create(entity,{},model);
        auto& rm=engine_.getRenderableManager();
        if(instance)require(rm.getInstanceCount(rm.getInstance(entity))==2,"instance buffer did not create two placements");
        if(skin) {
            auto const marker=meshes_.skinned_info(surface.handle);
            require(marker.skinned && marker.bone_count==1,"uploaded card did not use GPU skinning");
            auto const bone=M4::translation(F3{.125f,.0625f,0});rm.setBones(rm.getInstance(entity),&bone,1);
        }
        objects_.push_back(entity);scene_->addEntity(entity);return entity;
    }
    void samples(std::uint8_t count) {view_->setMultiSampleAntiAliasingOptions({.enabled=count>1,.sampleCount=count});}
    void guardband(bool enabled) {view_->setGuardBandOptions({.enabled=enabled});}
    bool coarse_rate(VariableRateShadingMode mode) {
        VariableRateShadingController controller({.engine=&engine_,.view=view_});
        controller.apply(mode);
        auto const status=controller.status();
        if(status.effective!=mode) {
            std::printf("%s VRS negative control SKIP: %s\n",kTag,status.reason.c_str());
            return false;
        }
        auto const expected=mode==VariableRateShadingMode::Rate2x1?
            filament::backend::FragmentShadingRate::RATE_2X1:filament::backend::FragmentShadingRate::RATE_2X2;
        require(view_->getFragmentShadingRate()==expected,"supported coarse VRS request was not applied");
        return true;
    }
    void set_channel(utils::Entity object,std::uint8_t channel) {auto& rm=engine_.getRenderableManager();rm.setChannel(rm.getInstance(object),channel);}
    void set_priority(utils::Entity object,std::uint8_t priority) {auto& rm=engine_.getRenderableManager();rm.setPriority(rm.getInstance(object),priority);}
    void displacement(bool enabled,bool frozen_carrier_control=false) {
        lit_->setParameter("geometryDisplacement",enabled);
        // The frozen-parent control binds near-neutral raw height while its
        // retained parent H is one. Taking the ordinary texture-height branch
        // would leave the independent blocker visible and fail the control.
        lit_->setParameter("geometryHeightMap",frozen_carrier_control?textures_.placeholder_normal():textures_.placeholder(),sampler());
        lit_->setParameter("displacementParams",F4{.2f,1,1000,0});
    }
    TraceCapabilities capabilities()const{return bridge_->capabilities();}
    filament::MaterialInstance* lit()const{return lit_;}
    filament::MaterialInstance* other()const{return other_;}
    filament::MaterialInstance* minimal()const{return minimal_instance_;}
    filament::MaterialInstance* shadow_only() {
        GpuMaterial material;material.two_sided=true;material.alpha_ref=.5f;
        auto* result=materials_.get_or_create_foliage_shadow(
            "opaque-prepass-smoke","forced-depth-shadow",material);
        require(result && !result->isColorWriteEnabled() && !result->isDepthWriteEnabled() &&
            result->getMaterial()->getBlendingMode()==filament::BlendingMode::TRANSPARENT,
            "production foliage shadow is not a colorless non-depth transparent control");
        return result;
    }
    filament::MaterialInstance* glass() {
        materials_.set_refraction_enabled(true);GpuMaterial m;m.glass=true;
        auto* result=materials_.get_or_create("opaque-prepass-smoke","refractor",m);
        require(result && result->getMaterial()->getRefractionMode()!=filament::RefractionMode::NONE,"refractive production package not routed");return result;
    }
    Observed capture(std::string const& label,bool enabled) {
        view_->setOpaqueDepthPrepassEnabled(enabled);recorder_.set_active(false);
        for(int i=0;i<2;++i)frame();recorder_.begin_capture(60'000'000'000ull);
        for(int i=0;i<7;++i)frame();
        Observed result;auto const measured_attempt=frame(&result.pixels);
        // These test-only frames poll delayed query completion. The selected
        // frame below must be the actual FLOAT readback frame, not an earlier
        // frame that happened to have its metadata ready.
        for(int i=0;i<4;++i)frame();
        require(std::ranges::all_of(result.pixels,[](float f){return std::isfinite(f);}),"FLOAT readback nonfinite");
        auto snapshot=recorder_.snapshot();recorder_.set_active(false);
        struct Group {Observed observed;bool color=false;};std::map<std::uint64_t,Group> groups;
        for(auto const& sample:snapshot.gpu_samples) {
            if(sample.context.view_id!=reinterpret_cast<std::uintptr_t>(view_)||
                    sample.context.attempt_id!=measured_attempt||sample.context.capture_id!=recorder_.session_id())continue;
            if(sample.name=="Opaque Z Prepass")result.prepass=true;
            bool const terminal=sample.status==profile::SampleStatus::Available||sample.status==profile::SampleStatus::Unsupported;
            if(!terminal)continue;
            auto& group=groups[sample.context.filament_frame_id];group.observed.frame=sample.context.filament_frame_id;
            if(sample.name=="Opaque Z Prepass") {
                group.observed.prepass=true;group.observed.depth_commands=sample.command_count;group.observed.depth_primitives=sample.primitive_count;
            }
            if(sample.name=="Color Pass"||sample.name=="Color Pass (opaque)") {
                group.color=true;group.observed.opaque_commands=sample.command_count;
                group.observed.invocation_counts=sample.pipeline_statistics_status==profile::SampleStatus::Available;
                group.observed.opaque_fragments=sample.fragment_shader_invocations;
                group.observed.native_rates=sample.native_draw_commands_available;
                group.observed.coarse_draws=sample.native_draw_commands_2x1+sample.native_draw_commands_2x2;
            }
            if(sample.name=="Color Pass (transparent)")group.observed.refraction=true;
        }
        bool complete=false;
        for (auto const& entry : groups) {
            auto const& group = entry.second;
            if(!group.color)continue;
            if(result.prepass && (!group.observed.prepass||!group.observed.depth_commands))continue;
            auto pixels = std::move(result.pixels);
            result = group.observed;
            result.pixels = std::move(pixels);
            complete = true;
        }
        require(complete,"no completed main-view color/prepass frame pair");
        if(const char* path=std::getenv("HEAVEN_SMOKE_OPAQUE_Z_OUTPUT")) {
            auto base=std::filesystem::path(path)/label;std::filesystem::create_directories(base.parent_path());
            for(auto const* extension:{".json",".html",".rgba32f"})require(!std::filesystem::exists(base.string()+extension),"fixture output would overwrite earlier evidence");
            profile::ReportWriter writer;require(writer.submit(std::move(snapshot),base),"profile export rejected");writer.wait();
            require(writer.status().state==profile::ReportState::Complete,"profile export failed");
            std::ofstream pixels(base.string()+".rgba32f",std::ios::binary);pixels.write(reinterpret_cast<char const*>(result.pixels.data()),static_cast<std::streamsize>(result.pixels.size()*sizeof(float)));require(pixels.good(),"FLOAT export failed");
        }
        return result;
    }
    Observed compare(std::string const& label,bool expected_prepass,bool reduction=false,bool expected_refraction=false,int expected_center=0) {
        auto off=capture(label+"-off",false);auto on=capture(label+"-on",true);auto off2=capture(label+"-off-return",false);
        require(!off.prepass && !off2.prepass,"disabled path generated named prepass");
        require(on.prepass==expected_prepass,"enabled/fallback path was not exercised");
        require(!expected_prepass||on.depth_commands>0,"prepass name had no actual depth commands");
        require(on.refraction==expected_refraction,"refraction branch was not exercised");
        float error=0,return_error=0;std::size_t nonblack=0;
        for(std::size_t i=0;i<off.pixels.size();++i) {error=std::max(error,std::abs(off.pixels[i]-on.pixels[i]));return_error=std::max(return_error,std::abs(off.pixels[i]-off2.pixels[i]));if(i%4!=3 && off.pixels[i]>.02f)++nonblack;}
        require(error==0 && return_error==0,"prepass or toggle return changed FLOAT pixels/coverage");
        require(std::memcmp(off.pixels.data(),on.pixels.data(),off.pixels.size()*sizeof(float))==0 &&
                std::memcmp(off.pixels.data(),off2.pixels.data(),off.pixels.size()*sizeof(float))==0,
                "prepass or toggle return changed FLOAT readback bits");
        require(nonblack>500,"color oracle is vacuous");
        if(expected_center) {
            auto const center=center_rgb(off.pixels);auto const winner=expected_center>0?0u:1u;
            require(center[winner]>.02f && center[winner]>4.0f*center[1u-winner],"coplanar primitive order positive control did not produce expected red/green winner");
        }
        if(reduction)require(off.invocation_counts && on.invocation_counts && on.opaque_fragments<off.opaque_fragments,"in-primitive overdraw did not reduce actual opaque fragment invocations");
        std::printf("%s %s frame=%llu prepass=%d depth=%llu/%llu FS=%llu->%llu FLOAT exact PASS\n",kTag,label.c_str(),static_cast<unsigned long long>(on.frame),on.prepass,
            static_cast<unsigned long long>(on.depth_commands),static_cast<unsigned long long>(on.depth_primitives),static_cast<unsigned long long>(off.opaque_fragments),static_cast<unsigned long long>(on.opaque_fragments));
        return on;
    }
    void compare_static_reference(std::string const& label,Observed const& actual) {
        auto reference=capture(label,false);
        require(!reference.prepass,"independent static reference unexpectedly used prepass");
        require(actual.pixels.size()==reference.pixels.size() &&
                std::memcmp(actual.pixels.data(),reference.pixels.data(),actual.pixels.size()*sizeof(float))==0,
                "GPU deformation/instance image differs from independently drawn static reference");
        std::printf("%s %s independent static-reference FLOAT bits exact PASS\n",kTag,label.c_str());
    }
    std::array<float,3> center_rgb(Pixels const& pixels) const {
        std::array<float,3> result{};
        for(std::uint32_t y=size_/2-2;y<size_/2+2;++y)for(std::uint32_t x=size_/2-2;x<size_/2+2;++x)
            for(std::size_t channel=0;channel<3;++channel)result[channel]+=pixels[(static_cast<std::size_t>(y)*size_+x)*4+channel]/16.0f;
        return result;
    }
private:
    static filament::TextureSampler sampler(){return {filament::TextureSampler::MinFilter::NEAREST,filament::TextureSampler::MagFilter::NEAREST};}
    void prepare_carrier(GpuSurface& gpu,Surface const& source) {
        require(source.vertices.size()==4 && source.indices.size()==6,"carrier control requires one source card");
        auto packed=meshes_.take_original_packing(gpu.handle,source.vertices,source.indices);
        require(packed.has_value(),"carrier authoritative original packing unavailable");
        // This small explicit valid attachment exercises production MeshStore
        // carrier upload and applyParentChord, not the quality of a fitted
        // smoother or native-height source acquisition.
        PreparedDisplacementSurface prepared;
        prepared.disposition=DisplacementPreparationDisposition::Prepared;
        prepared.source_ready=prepared.geometry_integrity_valid=true;
        prepared.original_packed=std::move(*packed);prepared.displacement.amplitude=.2f;
        for(auto const& vertex:source.vertices) {
            BoundedDisplacementVertex v;v.neutral_position={vertex.px,vertex.py,vertex.pz};
            v.displacement_direction={0,0,1};v.uv0={vertex.u,vertex.v};v.uv1={vertex.u1,vertex.v1};v.height=1;
            prepared.source_vertices.push_back(v);
            BoundedDisplacementNode node;node.old_raster_position=v.neutral_position+glm::dvec3(0,0,(1.0-127.0/255.0)*.2);
            node.correction={.004,0,.012};node.target_position=node.old_raster_position+node.correction;
            prepared.geometry.nodes.push_back(node);
        }
        for(std::size_t parent=0;parent<2;++parent) {
            std::array<std::uint32_t,3> triangle{source.indices[parent*3],source.indices[parent*3+1],source.indices[parent*3+2]};
            prepared.source_triangles.push_back(triangle);
            BoundedDisplacementTriangle attached;attached.nodes=triangle;attached.source_parent=static_cast<std::uint32_t>(parent);
            attached.source_barycentric={glm::dvec3(1,0,0),glm::dvec3(0,1,0),glm::dvec3(0,0,1)};
            prepared.geometry.triangles.push_back(attached);
        }
        auto replacement=meshes_.replace_displacement_surface(gpu,source.vertices,prepared);
        require(replacement.has_value() && meshes_.is_displacement_prepared(gpu.handle),"production prepared-carrier upload rejected fixture attachment");
    }
    std::uint64_t frame(Pixels* pixels=nullptr) {
        for(int retry=0;retry<16;++retry) {
            ++attempt_;bridge_->update({attempt_,attempt_,0,0,0,recorder_.session_id()});renderer_->setProfilerContext(utils::profiling::getContext());
            if(!renderer_->beginFrame(swapchain_))continue;
            renderer_->render(view_);std::atomic_bool completed{false};
            if(pixels) {
                pixels->assign(static_cast<std::size_t>(size_)*size_*4,std::numeric_limits<float>::quiet_NaN());
                renderer_->readPixels(target_,0,0,size_,size_,filament::backend::PixelBufferDescriptor(
                    pixels->data(),pixels->size()*sizeof(float),filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::FLOAT,[](void*,std::size_t,void* user){
                        static_cast<std::atomic_bool*>(user)->store(true);},&completed));
            }
            renderer_->endFrame();engine_.flushAndWait();
            require(!pixels||completed.load(),"FLOAT readback incomplete");return attempt_;
        }
        throw std::runtime_error("frame repeatedly rejected");
    }
    filament::Engine& engine_;TextureStore textures_;MaterialInstanceCache materials_;MeshStore meshes_;
    std::vector<GpuMesh> uploaded_;std::vector<utils::Entity> objects_;std::vector<filament::InstanceBuffer*> instances_;
    filament::Renderer* renderer_=nullptr;filament::SwapChain* swapchain_=nullptr;filament::Scene* scene_=nullptr;filament::View* view_=nullptr;
    filament::Camera* camera_=nullptr;utils::Entity camera_entity_;filament::Texture *color_=nullptr,*emission_=nullptr;
    filament::RenderTarget* target_=nullptr;filament::ColorGrading* grading_=nullptr;
    filament::Material* minimal_=nullptr;filament::MaterialInstance *minimal_instance_=nullptr,*lit_=nullptr,*other_=nullptr;
    profile::Recorder recorder_;std::unique_ptr<ProfileBridge> bridge_;std::uint64_t attempt_=0;std::uint32_t size_=kExtent;
};
} // namespace

CaseResult run_opaque_depth_prepass_case(CaseContext const& ctx) {
#ifdef __ANDROID__
    std::printf("%s SKIP: opaque depth prepass admission requires desktop Vulkan shaders\n",kTag);
    return CaseResult::Skip;
#endif
    if(ctx.backend.render_api_name()!="Vulkan")return CaseResult::Skip;
    auto* enabled=std::getenv("HEAVEN_SMOKE_GPU_DIAGNOSTICS");
    if(!enabled||std::string_view(enabled)!="1") {
        std::printf("%s SKIP: actual invocation controls require HEAVEN_SMOKE_GPU_DIAGNOSTICS=1\n",kTag);return CaseResult::Skip;
    }
    try {
        auto& backend=static_cast<FilamentBackend&>(ctx.backend);require(backend.init_pipelines(ctx.config).has_value(),"backend initialization failed");
        auto* engine=backend.debug_render_context().engine;require(engine,"active backend has no Engine");PrepassFixture f(*engine);f.init();
        // Probe actual capabilities through the fixture's observer. Backend
        // trace_capabilities() has no observer during standalone smoke cases.
        f.samples(1);f.add("invocation-capability-control",{{0}},f.lit());
        auto const probe=f.capture("invocation-capability-control-off",false);
        auto const capabilities=f.capabilities();
        require(capabilities.diagnostics_requested,"GPU diagnostic capabilities were not reported");
        if(!capabilities.pipeline_statistics_supported) {
            require(!capabilities.pipeline_statistics_reason.empty(),"unsupported invocation query has no reason");
            std::printf("%s SKIP: pipeline statistics unsupported: %s\n",kTag,
                capabilities.pipeline_statistics_reason.c_str());
            return CaseResult::Skip;
        }
        require(capabilities.pipeline_statistics_enabled && probe.invocation_counts,
            "supported invocation diagnostics did not complete the actual control");
        f.clear();
        for(auto const samples:{std::uint8_t{1},std::uint8_t{4}}) {
            f.samples(samples);std::string const suffix="-msaa"+std::to_string(samples);
            f.clear();f.add("in-primitive-back-to-front",{{-.7f},{-.6f},{-.5f},{-.4f},{-.3f},{-.2f},{-.1f},{0}},f.lit());f.compare("overdraw"+suffix,true,true);
            for(bool reverse:{false,true}) {
                f.clear();f.add("coplanar-UV-emission",reverse?std::vector<Layer>{{0,.75f},{0,.25f}}:std::vector<Layer>{{0,.25f},{0,.75f}},f.lit());
                f.compare(std::string(reverse?"ties-reversed":"ties-forward")+suffix,true,false,false,reverse?1:-1);
            }
            // 2^-21 world units span one nominal DEPTH32F ULP for this exact
            // orthographic eight-unit clip interval near depth .5-.625. This is
            // a geometric color-blocker oracle, not direct depth readback.
            unsigned z_index=0;std::array<std::array<float,3>,3> z_centers{};
            for(float z:{-0x1p-21f,0.0f,0x1p-21f}) {
                f.clear();f.add("custom-depth-profile",{{0}},f.lit());f.add("shared-depth-profile",{{z}},f.minimal());
                auto observed=f.compare("mixed-VS-z"+std::to_string(z_index)+suffix,true);z_centers[z_index++]=f.center_rgb(observed.pixels);
            }
            require(std::abs(z_centers[0][0]-z_centers[2][0])+std::abs(z_centers[0][1]-z_centers[2][1])>.01f,
                "nominal one-ULP depth competitors did not change visible winner");
            f.clear();f.add("instanced",{{0}},f.lit(),M4{1},false,true);auto instances=f.compare("instances"+suffix,true);
            f.clear();
            f.add("static-left",{{0}},f.lit(),M4::translation(F3{-.35f,0,0})*M4::scaling(F3{.4f,.6f,1}));
            f.add("static-right",{{0}},f.lit(),M4::translation(F3{.35f,0,0})*M4::scaling(F3{.4f,.6f,1}));
            f.compare_static_reference("instances-static-reference"+suffix,instances);
            f.clear();f.add("one-bone",{{0}},f.lit(),M4{1},true);auto skinning=f.compare("skinning"+suffix,true);
            f.clear();f.add("static-bone-position",{{0}},f.lit(),M4::translation(F3{.125f,.0625f,0}));
            f.compare_static_reference("skinning-static-reference"+suffix,skinning);
            f.clear();f.lit()->setCullingMode(filament::backend::CullingMode::NONE);f.add("mirrored",{{0}},f.lit(),M4::scaling(F3{-1,1,1}));f.compare("mirrored-two-sided"+suffix,true);
            M4 constant_w{1};constant_w[3][3]=.75f;f.clear();f.add("constant-W",{{0}},f.lit(),constant_w);f.compare("constant-W"+suffix,true);
            f.clear();f.add("displaced",{{0}},f.lit());f.add("independent-blocker",{{.05f}},f.minimal());
            auto neutral=f.compare("height-zero"+suffix,true);f.displacement(true);auto displaced=f.compare("height-displacement"+suffix,true);
            auto const before=f.center_rgb(neutral.pixels),after=f.center_rgb(displaced.pixels);
            require(std::abs(before[0]-after[0])+std::abs(before[1]-after[1])>.01f,"height displacement did not change blocker visibility");
            f.clear();f.add("prepared-carrier",{{0}},f.lit(),constant_w,false,false,true);f.add("carrier-blocker",{{.05f}},f.minimal());
            auto carrier_zero=f.compare("prepared-carrier-disabled"+suffix,true);
            f.displacement(true,true);auto carrier=f.compare("prepared-carrier-constant-W"+suffix,true);
            auto const carrier_before=f.center_rgb(carrier_zero.pixels),carrier_after=f.center_rgb(carrier.pixels);
            require(std::abs(carrier_before[0]-carrier_after[0])+std::abs(carrier_before[1]-carrier_after[1])>.01f,
                "frozen parent-height carrier did not change blocker visibility with near-neutral bound texture");
            f.clear();f.add("guardband",{{0}},f.lit());f.guardband(true);f.compare("guardband"+suffix,true);
            // Defined ordering witness: an earlier far color writer does not
            // write depth, a later near writer writes only depth, and the
            // eligible intermediate companion is then occluded. Seeding that
            // companion's depth would incorrectly erase the surviving far color.
            f.clear();f.lit()->setDepthWrite(false);
            auto earlier=f.add("earlier-far-red-no-depth",{{-.7f,.25f}},f.lit());f.set_priority(earlier,3);
            f.other()->setColorWrite(false);f.other()->setDepthWrite(true);
            f.add("near-colorless-depth-writer",{{.3f}},f.other());
            f.add("eligible-intermediate-companion",{{0}},f.minimal());
            auto const colorless=f.compare("fallback-colorless-depth-writer"+suffix,false,false,false,1);
            // The same colorless object without a depth write must remain
            // eligible as a non-occluder, proving this companion is real work.
            f.other()->setDepthWrite(false);
            auto const no_depth_writer=f.compare("colorless-nondepth-positive"+suffix,true);
            auto const colorless_center=f.center_rgb(colorless.pixels),visible_center=f.center_rgb(no_depth_writer.pixels);
            require(std::abs(colorless_center[0]-visible_center[0])+std::abs(colorless_center[1]-visible_center[1])>.01f,
                "colorless depth-writer witness did not change the independent visible winner");
            f.other()->setColorWrite(true);f.other()->setDepthWrite(true);
            // OneSide forces a depth-only first command despite raw MI
            // depthWrite=false. An earlier channel makes that command precede
            // the eligible ordinary companion independently of material sorting.
            f.clear();f.lit()->setDepthWrite(false);
            auto early_channel_red=f.add("early-channel-red-no-depth",{{-.7f,.25f}},f.lit());
            f.set_channel(early_channel_red,1);f.set_priority(early_channel_red,3);
            auto* forced_shadow=f.shadow_only();
            forced_shadow->setTransparencyMode(filament::TransparencyMode::TWO_PASSES_ONE_SIDE);
            auto forced_writer=f.add("early-channel-forced-depth",{{.3f}},forced_shadow);
            f.set_channel(forced_writer,1);
            f.add("default-channel-eligible-companion",{{0}},f.minimal());
            auto const forced=f.compare("fallback-forced-depth-OneSide"+suffix,false,false,false,1);
            forced_shadow->setTransparencyMode(filament::TransparencyMode::DEFAULT);
            auto const ordinary_shadow=f.compare("shadow-default-nondepth-positive"+suffix,true);
            auto const forced_center=f.center_rgb(forced.pixels),ordinary_center=f.center_rgb(ordinary_shadow.pixels);
            require(std::abs(forced_center[0]-ordinary_center[0])+std::abs(forced_center[1]-ordinary_center[1])>.01f,
                "forced depth mode did not change the independent visible winner");
            for(auto const mode:{VariableRateShadingMode::Rate2x1,VariableRateShadingMode::Rate2x2}) {
                f.clear();f.add("coarse-eligible-companion",{{0}},f.lit());
                if(f.coarse_rate(mode)) {
                    auto const coarse=f.compare("fallback-vrs-"+std::string(variable_rate_shading_mode_id(mode))+suffix,false);
                    require(coarse.native_rates && coarse.coarse_draws>0,
                        "supported coarse VRS fallback did not bind an actual coarse color draw");
                }
            }
            for(auto const state:{"channel","priority","offset","no-depth-test","stencil"}) {
                f.clear();f.add("eligible-companion",{{-.25f}},f.other());auto bad=f.add("unsupported-companion",{{0}},f.lit());
                if(state==std::string_view("channel"))f.set_channel(bad,1);
                if(state==std::string_view("priority"))f.set_priority(bad,3);
                if(state==std::string_view("offset"))f.lit()->setPolygonOffset(1,1);
                if(state==std::string_view("no-depth-test"))f.lit()->setDepthCulling(false);
                if(state==std::string_view("stencil"))f.lit()->setStencilWrite(true);
                f.compare(std::string("fallback-")+state+suffix,false);
            }
            f.clear();f.add("opaque-background",{{-.5f}},f.lit());f.add("opaque-blocker",{{.1f}},f.other(),M4::scaling(F3{.4f,.4f,1}));
            f.add("refractive-glass",{{0}},f.glass());f.compare("refraction-handoff"+suffix,true,false,true);
            f.clear();f.add("before-resize",{{0}},f.lit());f.compare("reload-before"+suffix,true);f.resize(160);f.compare("resized"+suffix,true);f.resize(kExtent);
        }
        return CaseResult::Pass;
    } catch(std::exception const& e) {std::fprintf(stderr,"%s FAIL: %s\n",kTag,e.what());return CaseResult::Fail;}
}
} // namespace heaven::smoke
