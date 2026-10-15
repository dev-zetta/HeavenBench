// Native representative-surface input oracle; no SDK, assets or midpoint image.
#include "rhi/frame_generation/temporal_scene.h"
#include "rhi/frame_generation/temporal_camera.h"
#include "rhi/frame_generation/frame_generation_output.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "rhi/mesh_store.h"
#include "rhi/texture_store.h"
#include "engine/camera.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/half.h>
#include <utils/EntityManager.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace heaven {
namespace {
constexpr uint32_t kExtent = 192;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Mesh plane(const char* name, float distance, float width, float height) {
    Mesh mesh; mesh.name = mesh.path = name;
    Surface surface; surface.name = name;
    constexpr std::array corners{glm::vec2{-1,-1},glm::vec2{1,-1},
        glm::vec2{1,1},glm::vec2{-1,1}};
    for (const auto corner : corners) {
        Vertex vertex{};
        vertex.px=corner.x*width; vertex.py=distance; vertex.pz=corner.y*height;
        vertex.ny=-1; vertex.tx=1; vertex.tw=1;
        vertex.u=(corner.x+1)*.5f; vertex.v=(corner.y+1)*.5f;
        surface.vertices.push_back(vertex);
    }
    surface.indices={0,1,2,0,2,3};
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

std::array<size_t,18> owners(filament::Engine& engine) {
    return {engine.getBufferObjectCount(),engine.getViewCount(),engine.getSceneCount(),
        engine.getSwapChainCount(),engine.getIndexBufferCount(),engine.getSkinningBufferCount(),
        engine.getMorphTargetBufferCount(),engine.getInstanceBufferCount(),engine.getVertexBufferCount(),
        engine.getIndirectLightCount(),engine.getMaterialCount(),engine.getTextureCount(),
        engine.getSkyboxeCount(),engine.getColorGradingCount(),engine.getRenderTargetCount(),
        engine.getRenderableManager().getComponentCount(),engine.getTransformManager().getComponentCount(),
        engine.getLightManager().getComponentCount()};
}

struct Capture {
    std::vector<glm::vec4> packed = std::vector<glm::vec4>(size_t(kExtent)*kExtent);
    std::vector<glm::vec2> motion = std::vector<glm::vec2>(size_t(kExtent)*kExtent);
    std::vector<float> depth = std::vector<float>(size_t(kExtent)*kExtent);
    std::vector<uint8_t> color = std::vector<uint8_t>(size_t(kExtent)*kExtent*4);
};

class RefractionFixture {
public:
    explicit RefractionFixture(filament::Engine& engine) : engine_(engine) {}
    ~RefractionFixture() {
        engine_.flushAndWait();
        if (view_) { view_->setRenderTarget(nullptr); view_->setScene(nullptr); view_->setCamera(nullptr); }
        for (auto* target : read_targets_) if (target) engine_.destroy(target);
        output_.reset(); temporal_.reset();
        for (auto entity : entities_) {
            if (scene_) scene_->remove(entity);
            engine_.destroy(entity); utils::EntityManager::get().destroy(entity);
        }
        for (auto* instance : instances_material_) engine_.destroy(instance);
        meshes_.reset(); textures_.reset();
        for (auto* material : {glass_,marker_,motion_,masked_}) if (material) engine_.destroy(material);
        if (normal_) engine_.destroy(normal_);
        if (view_) engine_.destroy(view_);
        if (scene_) { scene_->setSkybox(nullptr); engine_.destroy(scene_); }
        if (sky_) engine_.destroy(sky_);
        if (camera_entity_) {
            engine_.destroyCameraComponent(camera_entity_);
            utils::EntityManager::get().destroy(camera_entity_);
        }
        if (renderer_) engine_.destroy(renderer_);
        if (swapchain_) engine_.destroy(swapchain_);
        engine_.flushAndWait();
    }
    RefractionFixture(const RefractionFixture&)=delete;
    RefractionFixture& operator=(const RefractionFixture&)=delete;

    void init() {
        renderer_=engine_.createRenderer(); scene_=engine_.createScene(); view_=engine_.createView();
        camera_entity_=utils::EntityManager::get().create(); camera_=engine_.createCamera(camera_entity_);
        swapchain_=engine_.createSwapChain(kExtent,kExtent,filament::SwapChain::CONFIG_READABLE);
        require(renderer_ && scene_ && view_ && camera_ && swapchain_,"Refraction fixture core unavailable");
        renderer_->setDisplayInfo({.refreshRate=0}); renderer_->setFrameRateOptions({.interval=0});
        camera.position={0,-6,0}; camera.yaw=90; camera.pitch=0; camera.fov=60;
        update_camera(); camera_->setExposure(1);
        sky_=filament::Skybox::Builder().color({0,0,0,1}).build(engine_);
        require(sky_,"Refraction fixture sky unavailable"); scene_->setSkybox(sky_);
        view_->setScene(scene_); view_->setCamera(camera_); view_->setViewport({0,0,kExtent,kExtent});
        view_->setPostProcessingEnabled(true); view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled=false});
        view_->setTemporalAntiAliasingOptions({.enabled=false});
        view_->setAmbientOcclusionOptions({.enabled=false});
        view_->setBloomOptions({.enabled=false}); view_->setDithering(filament::Dithering::NONE);
        view_->setScreenSpaceReflectionsOptions({.enabled=false});
        view_->setScreenSpaceRefractionEnabled(true);
        meshes_=std::make_unique<MeshStore>(engine_); textures_=std::make_unique<TextureStore>(engine_);
        textures_->create_placeholders();
        glass_=package(k_heaven_glass_filamat,k_heaven_glass_filamat_size);
        marker_=package(k_heaven_debug_arrow_filamat,k_heaven_debug_arrow_filamat_size);
        motion_=package(k_heaven_motion_filamat,k_heaven_motion_filamat_size);
        masked_=package(k_heaven_motion_masked_filamat,k_heaven_motion_masked_filamat_size);
        normal_=filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).build(engine_);
        require(normal_,"Controlled glass normal unavailable");
        constexpr static std::array<float,4> normal_pixel{.8f,.5f,.9f,1};
        normal_->setImage(engine_,0,{normal_pixel.data(),sizeof(normal_pixel),
            filament::Texture::Format::RGBA,filament::Texture::Type::FLOAT});
        auto* glass_instance=glass_->createInstance();
        require(glass_instance,"Glass instance unavailable");
        instances_material_.push_back(glass_instance); glass_instance_=glass_instance;
        material_tuning::bind_glass(*glass_instance);
        glass_instance->setParameter("hasBaseColor",false); glass_instance->setParameter("hasNormal",true);
        glass_instance->setParameter("hasEmissive",false);
        glass_instance->setParameter("baseTransform",filament::math::float4{1,1,0,0});
        glass_instance->setParameter("glassTint",filament::math::float3{1,1,1});
        glass_instance->setParameter("transmission",1.0f); glass_instance->setParameter("absorption",filament::math::float3{0,0,0});
        glass_instance->setParameter("roughness",.02f); glass_instance->setParameter("thickness",.8f);
        glass_instance->setParameter("normalStrength",1.0f);
        const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        glass_instance->setParameter("normalMap",normal_,sampler);
        glass_instance->setParameter("baseColorMap",textures_->placeholder(),sampler);
        glass_instance->setParameter("emissiveMap",textures_->placeholder(),sampler);
        add(plane("glass",0,1.25f,1.3f),glass_instance);
        add_marker(plane("backdrop",3,5,4),{.03f,.06f,.3f});
        add_marker(plane("vertical-red-marker",2.99f,.25f,3.5f),{1,.025f,.01f});
        add_marker(plane("horizontal-green-marker",2.98f,4.5f,.15f),{.01f,1,.025f});
        const BackendCore core{&engine_,scene_,view_,renderer_,camera_};
        temporal_=std::make_unique<TemporalScene>(core);
        output_=std::make_unique<FrameGenerationOutput>(core);
        require(temporal_->ensure(kExtent,kExtent) && output_->ensure(kExtent,kExtent),"Refraction temporal targets unavailable");
        AppConfig config; config.tessellation.enabled=false;
        require(temporal_->build(*meshes_,*textures_,instances_,gpu_meshes_,
            [](const std::string& name, [[maybe_unused]] const std::string& surface) {
                GpuMaterial material; material.glass=name=="glass"; return material;
            },config,motion_,masked_),"Refraction temporal proxies unavailable");
        require(temporal_->coverage().glass_primitives==1 && temporal_->coverage().proxy_primitives==4,
            "Refraction fixture did not exercise actual glass plus background proxies");
        read_targets_={read_target(temporal_->packed_texture()),read_target(output_->motion_texture()),read_target(output_->depth_texture())};
        for (auto* target : read_targets_) require(target,"Refraction readback target unavailable");
    }
    glm::mat4 clip() const { return temporal_gl_projection(camera_->getProjectionMatrix())*camera.view_matrix(); }
    void update_camera() {
        camera_->setProjection(camera.fov,1,.5,2000,filament::Camera::Fov::VERTICAL);
        camera_->setModelMatrix(glm_to_filament(glm::inverse(camera.view_matrix())));
    }
    void placement(glm::vec3 glass,glm::vec3 background) {
        for (size_t i=0;i<instances_.size();++i) {
            instances_[i].transform=glm::translate(glm::mat4(1),i==0?glass:background);
            auto& transforms=engine_.getTransformManager();
            transforms.setTransform(transforms.getInstance(entities_[i]),glm_to_filament(instances_[i].transform));
        }
    }
    void accept() { temporal_->commit_accepted(); }
    Capture capture(float ior,const char* label,bool refraction_enabled=true) {
        update_camera(); glass_instance_->setParameter("ior",ior);
        view_->setScreenSpaceRefractionEnabled(refraction_enabled);
        require(temporal_->prepare(camera,instances_,clip()),"Refraction temporal preparation failed");
        require(temporal_->coverage().missing_current_bones==0 && temporal_->coverage().missing_current_positions==0,
            "Refraction geometry readiness is incomplete");
        output_->prepare(temporal_->packed_texture(),kExtent,kExtent,false,
            {temporal_->current_clip(),temporal_->previous_clip(),temporal_->history_valid()});
        require(!output_->distortion_texture(),"Surface refraction was incorrectly tagged as a global post warp");
        view_->setRenderTarget(output_->scene_rt());
        Capture result;
        // Only this test readback representation changes: preserve signed
        // RG16F values instead of generic HALF-to-FLOAT's negative clamp.
        // Decode native halves independently; production output is untouched.
        std::vector<std::array<uint16_t,2>> half_motion(size_t(kExtent)*kExtent);
        for (int settle=0;settle<3;++settle) {
            bool accepted=false;
            for (int attempt=0;attempt<32 && !accepted;++attempt) {
                accepted=renderer_->beginFrame(swapchain_);
                if (!accepted) engine_.flushAndWait();
            }
            require(accepted,"Refraction capture rejected all bounded attempts");
            renderer_->render(temporal_->view()); output_->render_extract(); renderer_->render(view_);
            output_->render_composite();
            std::array<std::atomic_bool,4> done{};
            if (settle==2) {
                read(read_targets_[0],result.packed.data(),result.packed.size()*sizeof(glm::vec4),
                    filament::backend::PixelDataFormat::RGBA,filament::backend::PixelDataType::FLOAT,done[0]);
                read(read_targets_[1],half_motion.data(),half_motion.size()*sizeof(half_motion.front()),
                    filament::backend::PixelDataFormat::RG,filament::backend::PixelDataType::HALF,done[1]);
                read(read_targets_[2],result.depth.data(),result.depth.size()*sizeof(float),
                    filament::backend::PixelDataFormat::R,filament::backend::PixelDataType::FLOAT,done[2]);
                read(output_->scene_rt(),result.color.data(),result.color.size(),
                    filament::backend::PixelDataFormat::RGBA,filament::backend::PixelDataType::UBYTE,done[3]);
            }
            renderer_->endFrame(); engine_.flushAndWait();
            if (settle==2) for (const auto& completed : done) require(completed.load(),"Refraction readback incomplete");
        }
        for (size_t i=0;i<half_motion.size();++i)
            result.motion[i]={float(filament::math::makeHalf(half_motion[i][0])),
                float(filament::math::makeHalf(half_motion[i][1]))};
        if (const char* directory=std::getenv("HEAVEN_FG_REFRACTION_TEST_DIR"); directory && *directory) {
            const auto path=std::filesystem::path(directory)/(std::string(label)+".ppm");
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream(path,std::ios::binary);
            require(bool(stream),"Cannot open refraction pixel evidence");
            stream<<"P6\n"<<kExtent<<' '<<kExtent<<"\n255\n";
            for (size_t i=0;i<result.color.size();i+=4)
                stream.write(reinterpret_cast<const char*>(result.color.data()+i),3);
            require(bool(stream),"Cannot complete refraction pixel evidence");
        }
        return result;
    }
    Camera camera;
private:
    filament::Material* package(const uint8_t* data,size_t size) {
        auto* material=filament::Material::Builder().package(data,size).build(engine_);
        require(material,"Refraction source package unavailable"); return material;
    }
    void add_marker(const Mesh& mesh,filament::math::float3 color) {
        auto* instance=marker_->createInstance(); require(instance,"Opaque marker instance unavailable");
        instances_material_.push_back(instance); instance->setParameter("color",color); add(mesh,instance);
    }
    void add(const Mesh& mesh,filament::MaterialInstance* material) {
        auto uploaded=meshes_->upload(mesh); require(uploaded.surfaces.size()==1,"Refraction mesh upload failed");
        const auto buffers=meshes_->find(uploaded.surfaces.front().handle);
        require(buffers.vb && buffers.ib,"Refraction source buffers unavailable");
        const auto entity=utils::EntityManager::get().create();
        entities_.push_back(entity);
        const auto extent=(uploaded.bbox_max-uploaded.bbox_min)*.5f;
        const auto center=(uploaded.bbox_max+uploaded.bbox_min)*.5f;
        require(filament::RenderableManager::Builder(1)
            .geometry(0,filament::RenderableManager::PrimitiveType::TRIANGLES,buffers.vb,buffers.ib)
            .material(0,material).boundingBox({{center.x,center.y,center.z},{extent.x,.01f,extent.z}})
            .culling(false).castShadows(false).receiveShadows(false).build(engine_,entity)==filament::RenderableManager::Builder::Success,
            "Refraction visible renderable unavailable");
        engine_.getTransformManager().create(entity); scene_->addEntity(entity);
        instances_.push_back({static_cast<int>(gpu_meshes_.size()),glm::mat4(1)});
        gpu_meshes_.push_back(std::move(uploaded));
    }
    filament::RenderTarget* read_target(filament::Texture* texture) {
        return filament::RenderTarget::Builder().texture(filament::RenderTarget::AttachmentPoint::COLOR0,texture).build(engine_);
    }
    void read(filament::RenderTarget* target,void* data,size_t bytes,filament::backend::PixelDataFormat format,
            filament::backend::PixelDataType type,std::atomic_bool& done) {
        renderer_->readPixels(target,0,0,kExtent,kExtent,filament::backend::PixelBufferDescriptor(data,bytes,format,type,
            [](void*,size_t,void* user) { static_cast<std::atomic_bool*>(user)->store(true); },&done));
    }
    filament::Engine& engine_;
    filament::Renderer* renderer_=nullptr; filament::Scene* scene_=nullptr; filament::View* view_=nullptr;
    filament::Camera* camera_=nullptr; filament::SwapChain* swapchain_=nullptr; filament::Skybox* sky_=nullptr;
    utils::Entity camera_entity_;
    filament::Material *glass_=nullptr,*marker_=nullptr,*motion_=nullptr,*masked_=nullptr;
    filament::MaterialInstance* glass_instance_=nullptr; filament::Texture* normal_=nullptr;
    std::vector<filament::MaterialInstance*> instances_material_;
    std::vector<utils::Entity> entities_; std::vector<GpuMesh> gpu_meshes_; std::vector<Instance> instances_;
    std::unique_ptr<MeshStore> meshes_; std::unique_ptr<TextureStore> textures_;
    std::unique_ptr<TemporalScene> temporal_; std::unique_ptr<FrameGenerationOutput> output_;
    std::array<filament::RenderTarget*,3> read_targets_{};
};

void verify_inputs(const char* label,const Capture& capture,const Camera& camera,const glm::mat4& current_clip,
        const glm::mat4& previous_clip,glm::vec3 glass,glm::vec3 previous_glass,
        glm::vec3 background,glm::vec3 previous_background,bool history,bool wrong_surface) {
    const auto inverse=glm::inverse(glm::dmat4(current_clip));
    size_t glass_pixels=0,background_pixels=0,worst_index=0; double maximum=0;
    double glass_error=0,background_error=0,packed_motion_error=0,output_motion_error=0;
    glm::dvec3 worst_expected{},worst_world{},worst_previous{}; bool worst_glass=false;
    for (size_t index=0;index<capture.packed.size();++index) {
        const glm::dvec2 uv{(double(index%kExtent)+.5)/kExtent,(double(index/kExtent)+.5)/kExtent};
        const auto far=inverse*glm::dvec4(2*uv.x-1,1-2*uv.y,.5,1);
        const auto ray=glm::dvec3(far)/far.w-glm::dvec3(camera.position);
        if (ray.y<=0) continue;
        const auto at=[&](double y) { return glm::dvec3(camera.position)+ray*((y-camera.position.y)/ray.y); };
        const auto surface=at(glass.y);
        const bool on_glass=std::abs(surface.x-glass.x)<1.0 && std::abs(surface.z-glass.z)<1.05;
        const auto rear=at(3+background.y);
        const bool on_background=std::abs(surface.x-glass.x)>1.5 && std::abs(rear.x-background.x)<4.4 &&
            std::abs(rear.x-background.x)>.4 && std::abs(rear.z-background.z)<3.3 && std::abs(rear.z-background.z)>.35;
        if (!on_glass && !on_background) continue;
        const auto world=on_glass?surface:rear;
        auto previous=world-glm::dvec3(on_glass?glass:background)+glm::dvec3(on_glass?previous_glass:previous_background);
        if (wrong_surface && on_glass) previous.x+=.5;
        const auto cc=glm::dmat4(current_clip)*glm::dvec4(world,1);
        const auto pc=glm::dmat4(previous_clip)*glm::dvec4(previous,1);
        const glm::dvec2 motion=history?glm::dvec2(pc)/pc.w*glm::dvec2(.5,-.5)+.5-uv:glm::dvec2(0);
        const double depth=.5-.5*cc.z/cc.w;
        const auto& actual=capture.packed[index];
        require(history?actual.a>.9f:actual.a<.1f,"Refraction representative history validity mismatch");
        const double packed_error=glm::length(glm::dvec2(actual)-motion);
        const double extracted_error=glm::length(glm::dvec2(capture.motion[index])-motion);
        const double error=std::max({packed_error,std::abs(double(actual.b)-depth),
            extracted_error,std::abs(double(capture.depth[index])-depth)});
        packed_motion_error=std::max(packed_motion_error,packed_error);
        output_motion_error=std::max(output_motion_error,extracted_error);
        if (on_glass) glass_error=std::max(glass_error,error); else background_error=std::max(background_error,error);
        if (error>maximum) {
            maximum=error; worst_index=index; worst_expected=glm::dvec3(motion,depth);
            worst_world=world; worst_previous=previous; worst_glass=on_glass;
        }
        if (on_glass) ++glass_pixels; else ++background_pixels;
    }
    std::printf("[temporal_refraction_test] %s glass=%zu exterior_background=%zu max_input_error=%.9g\n",
        label,glass_pixels,background_pixels,maximum);
    const auto& actual=capture.packed[worst_index];
    const auto& output=capture.motion[worst_index];
    std::printf("[temporal_refraction_test] %s class_errors glass=%.9g background=%.9g packedMotion=%.9g outputMotion=%.9g worst_pixel=(%zu,%zu) type=%s packed=(%.9g,%.9g,%.9g,%.9g) output=(%.9g,%.9g,%.9g) expected=(%.9g,%.9g,%.9g) currentWorld=(%.9g,%.9g,%.9g) previousWorld=(%.9g,%.9g,%.9g)\n",
        label,glass_error,background_error,packed_motion_error,output_motion_error,worst_index%kExtent,worst_index/kExtent,
        worst_glass?"glass":"background",double(actual.x),double(actual.y),double(actual.z),double(actual.w),
        double(output.x),double(output.y),double(capture.depth[worst_index]),worst_expected.x,worst_expected.y,worst_expected.z,
        worst_world.x,worst_world.y,worst_world.z,worst_previous.x,worst_previous.y,worst_previous.z);
    require(glass_pixels>=512 && background_pixels>=256,"Refraction independent input oracle lacks geometric coverage");
    require(maximum<.00005,wrong_surface?"Injected wrong glass-surface motion rejected":"Refraction changed undistorted representative depth/motion");
}

void same_inputs(const Capture& a,const Capture& b) {
    require(a.packed==b.packed && a.motion==b.motion && a.depth==b.depth,
        "Changing only surface refraction changed the geometry temporal inputs");
}

void visual_difference(const char* label,const Capture& a,const Capture& b,bool require_both_markers=true) {
    size_t changed=0,red_a=0,red_b=0; double error=0;
    // Strict central glass interior, independent of the observed color mask.
    for (size_t y=70;y<122;++y) for (size_t x=70;x<122;++x) {
        const size_t index=(y*kExtent+x)*4;
        double difference=0;
        for (size_t channel=0;channel<3;++channel)
            difference+=std::abs(int(a.color[index+channel])-int(b.color[index+channel]));
        if (difference>12) ++changed;
        error+=difference;
        red_a+=a.color[index]>a.color[index+1]+20 && a.color[index]>a.color[index+2]+20;
        red_b+=b.color[index]>b.color[index+1]+20 && b.color[index]>b.color[index+2]+20;
    }
    std::printf("[temporal_refraction_test] %s changed_glass_pixels=%zu mean_RGB_L1_bytes=%.6g red_marker_pixels=%zu/%zu\n",
        label,changed,error/(52*52),red_a,red_b);
    require(changed>=64 && error/(52*52)>2 && (!require_both_markers || (red_a>=16 && red_b>=16)),
        "Actual refracted moving marker lacks visible on/off or motion coverage");
}

void run(filament::Engine& engine,bool wrong_surface) {
    { RefractionFixture warmup(engine); warmup.init(); warmup.capture(1.5f,"warmup"); }
    const auto baseline=owners(engine);
    {
        RefractionFixture fixture(engine); fixture.init();
        const auto initial_clip=fixture.clip();
        const auto straight=fixture.capture(1,"straight-birth");
        const auto refracted=fixture.capture(1.5f,"refracted-birth");
        same_inputs(straight,refracted); visual_difference("IOR1_vs1.5",straight,refracted);
        const auto disabled=fixture.capture(1.5f,"refraction-disabled",false);
        same_inputs(refracted,disabled); visual_difference("refraction_enabled_vs_disabled",refracted,disabled,false);
        verify_inputs("birth",refracted,fixture.camera,initial_clip,initial_clip,{},{},{},{},false,false);
        fixture.accept();
        const glm::vec3 background{.3f,0,.04f}; fixture.placement({},background);
        const auto moved_background=fixture.capture(1.5f,"moving-background");
        verify_inputs("independent-background",moved_background,fixture.camera,fixture.clip(),initial_clip,{},{},background,{},true,wrong_surface);
        visual_difference("background_motion",refracted,moved_background);
        fixture.accept(); const auto previous_clip=fixture.clip();
        fixture.camera.position.x=.12f; fixture.camera.position.z=.04f;
        const glm::vec3 glass{.08f,0,.03f}; fixture.placement(glass,background);
        const auto camera_straight=fixture.capture(1,"camera-straight");
        const auto camera_refracted=fixture.capture(1.5f,"camera-refracted");
        same_inputs(camera_straight,camera_refracted);
        verify_inputs("camera_and_surface",camera_refracted,fixture.camera,fixture.clip(),previous_clip,
            glass,{},background,background,true,false);
        visual_difference("camera_IOR_control",camera_straight,camera_refracted);
        fixture.accept();
    }
    engine.flushAndWait(); require(owners(engine)==baseline,"Refraction borrowers/owners did not return to baseline");
    std::puts("[temporal_refraction_test] representative surface only; no exact transmitted optical-flow or generated-image claim");
}
} // namespace
} // namespace heaven

int main(int argc,char** argv) {
    const bool negative=argc==2 && std::string_view(argv[1])=="--negative-wrong-surface-motion";
    if (argc>1 && !negative) { std::fprintf(stderr,"Usage: %s [--negative-wrong-surface-motion]\n",argv[0]); return 2; }
    filament::Engine* engine=nullptr; int status=0;
    try {
        engine=filament::Engine::create(filament::Engine::Backend::VULKAN);
        heaven::require(engine,"Native refraction Vulkan engine unavailable");
        heaven::run(*engine,negative);
        std::puts(negative?"[temporal_refraction_test] NEGATIVE NOT DETECTED":"[temporal_refraction_test] PASS");
    } catch (const std::exception& error) {
        std::fprintf(stderr,"[temporal_refraction_test] FAIL: %s\n",error.what()); status=1;
    }
    if (engine) filament::Engine::destroy(&engine);
    return status;
}
