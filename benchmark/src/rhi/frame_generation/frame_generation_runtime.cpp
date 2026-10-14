#include "rhi/frame_generation/frame_generation_runtime.h"
#include "rhi/frame_generation/frame_generation_output.h"
#include "rhi/frame_generation/frame_generation_history.h"
#include "rhi/frame_generation/temporal_scene.h"
#include "rhi/frame_generation/temporal_camera.h"
#include "rhi/materials_data.h"
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/Renderer.h>
#include <filament/View.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>
#include <atomic>
#include <limits>
#include "rhi/frame_generation/dlss_runtime_bridge.h"
#include "rhi/frame_generation/dlss_status.h"
#if HEAVEN_HAS_DLSS
#include "rhi/frame_generation/dlss_platform.h"
#include <backend/VulkanCommandContext.h>
#endif

#if HEAVEN_HAS_FSR3
#include "rhi/frame_generation/frame_generation_platform.h"
#include <backend/VulkanCommandContext.h>
#include "rhi/frame_generation/fsr31_vulkan.h"
#include "fg_ui_spirv.h"
#include <stdexcept>
#endif

namespace heaven {
#if HEAVEN_HAS_FSR3
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties{};
    bluevk::vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i=0;i<properties.memoryTypeCount;++i)
        if ((bits & (1u<<i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))) return i;
    throw std::runtime_error("No device-local interpolation memory");
}
}
struct FrameGenerationRuntime::Native {
    explicit Native(FrameGenerationRuntime& runtime) : owner(runtime) {}
    FrameGenerationRuntime& owner;
    std::unique_ptr<Fsr31Vulkan> provider;
    VkDevice device = VK_NULL_HANDLE;
    VkImage generated = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView output_view = VK_NULL_HANDLE, ui_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkExtent2D size{};
    bool initialized_layout = false;
    uint64_t frame_id = 0;
    std::chrono::steady_clock::time_point previous_callback;
    uint64_t previous_pacing_wait = 0;
    double unpaced_interval_ns = 16'666'667.0;
    std::atomic<bool> failed{false};
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
    std::atomic<uint64_t> provider_record_attempts{0};
#endif
    FrameGenerationHistoryLatch history;
    ~Native() { destroy(); }
    void destroy() {
        provider.reset();
        if (!device) return;
        if (pipeline) bluevk::vkDestroyPipeline(device,pipeline,nullptr);
        if (pipeline_layout) bluevk::vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
        if (descriptor_pool) bluevk::vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
        if (descriptor_layout) bluevk::vkDestroyDescriptorSetLayout(device,descriptor_layout,nullptr);
        if (sampler) bluevk::vkDestroySampler(device,sampler,nullptr);
        if (ui_view) bluevk::vkDestroyImageView(device,ui_view,nullptr);
        if (output_view) bluevk::vkDestroyImageView(device,output_view,nullptr);
        if (generated) bluevk::vkDestroyImage(device,generated,nullptr);
        if (memory) bluevk::vkFreeMemory(device,memory,nullptr);
        device=VK_NULL_HANDLE; generated=VK_NULL_HANDLE; memory=VK_NULL_HANDLE;
        output_view=ui_view=VK_NULL_HANDLE; sampler=VK_NULL_HANDLE;
        descriptor_layout=VK_NULL_HANDLE; pipeline_layout=VK_NULL_HANDLE;
        pipeline=VK_NULL_HANDLE; descriptor_pool=VK_NULL_HANDLE;
        initialized_layout=false;
    }
    void initialize(const filament::backend::VulkanCommandContext& context) {
        device=context.device;
        size=context.images[0].extent;
        Fsr31VulkanConfig config;
        config.physical_device=context.physicalDevice; config.device=device;
        config.get_device_proc_addr=bluevk::vkGetDeviceProcAddr;
        config.max_render_size=size; config.display_size=size;
        config.color_format=context.images[0].format;
        config.inverted_depth=true; config.infinite_depth=true;
        VkPhysicalDeviceFeatures features{};
        bluevk::vkGetPhysicalDeviceFeatures(context.physicalDevice,&features);
        config.storage_image_extended_formats_enabled=features.shaderStorageImageExtendedFormats;
        config.storage_image_read_without_format_enabled=features.shaderStorageImageReadWithoutFormat;
        config.storage_image_write_without_format_enabled=features.shaderStorageImageWriteWithoutFormat;
        auto made=Fsr31Vulkan::create(config);
        if (!made) throw std::runtime_error(made.error());
        provider=std::move(*made);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R8G8B8A8_UNORM;
        image.extent={size.width,size.height,1}; image.mipLevels=1; image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT; image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        check(bluevk::vkCreateImage(device,&image,nullptr,&generated),"Create generated image");
        VkMemoryRequirements requirements{};
        bluevk::vkGetImageMemoryRequirements(device,generated,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memory_type(context.physicalDevice,requirements.memoryTypeBits);
        check(bluevk::vkAllocateMemory(device,&allocation,nullptr,&memory),"Allocate generated image");
        check(bluevk::vkBindImageMemory(device,generated,memory,0),"Bind generated image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=generated; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=image.format;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(bluevk::vkCreateImageView(device,&view,nullptr,&output_view),"View generated image");
        view.image=context.images[3].image; view.format=context.images[3].format;
        check(bluevk::vkCreateImageView(device,&view,nullptr,&ui_view),"View UI layer");
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sample.magFilter=sample.minFilter=VK_FILTER_NEAREST;
        sample.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sample.addressModeU=sample.addressModeV=sample.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(bluevk::vkCreateSampler(device,&sample,nullptr,&sampler),"Create UI sampler");
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0]={0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[1]={1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount=2; layout.pBindings=bindings;
        check(bluevk::vkCreateDescriptorSetLayout(device,&layout,nullptr,&descriptor_layout),"Create UI descriptor layout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1; pl.pSetLayouts=&descriptor_layout;
        check(bluevk::vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout),"Create UI pipeline layout");
        VkShaderModule module{};
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize=sizeof(kFgUiSpirv); sm.pCode=kFgUiSpirv;
        check(bluevk::vkCreateShaderModule(device,&sm,nullptr,&module),"Create UI shader");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module=module; compute.stage.pName="main";
        compute.layout=pipeline_layout;
        const auto result=bluevk::vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline);
        bluevk::vkDestroyShaderModule(device,module,nullptr);
        check(result,"Create UI pipeline");
        VkDescriptorPoolSize pools[2]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets=1; pool.poolSizeCount=2; pool.pPoolSizes=pools;
        check(bluevk::vkCreateDescriptorPool(device,&pool,nullptr,&descriptor_pool),"Create UI descriptor pool");
        VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ds.descriptorPool=descriptor_pool; ds.descriptorSetCount=1; ds.pSetLayouts=&descriptor_layout;
        check(bluevk::vkAllocateDescriptorSets(device,&ds,&descriptor_set),"Allocate UI descriptors");
        VkDescriptorImageInfo images[2]={{VK_NULL_HANDLE,output_view,VK_IMAGE_LAYOUT_GENERAL},{sampler,ui_view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for(uint32_t i=0;i<2;++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=descriptor_set;
            writes[i].dstBinding=i; writes[i].descriptorCount=1;
            writes[i].descriptorType=bindings[i].descriptorType; writes[i].pImageInfo=&images[i];
        }
        bluevk::vkUpdateDescriptorSets(device,2,writes,0,nullptr);
        std::scoped_lock lock(owner.mutex_);
        owner.state_.set_capability({FrameGenerationMode::Fsr3,true,"FSR 3.1 Vulkan (experimental)"});
    }
    struct Packet { Native* native; Camera camera; bool reset; float dt_ms; float near_plane; };
    static void callback(void* data, const void* native_context) noexcept {
        std::unique_ptr<Packet> packet(static_cast<Packet*>(data));
        auto& self=*packet->native;
        auto& owner=self.owner;
        const auto callback_time=std::chrono::steady_clock::now();
        // A prepared packet can remain queued across process suspension. The
        // frontend's gap check cannot see that delay, so invalidate here too.
        const bool callback_gap=frame_generation_callback_gap(self.previous_callback,callback_time);
        const bool reset=self.history.should_reset(packet->reset,owner.platform_->history_epoch()) || callback_gap;
        const auto total_pacing=owner.platform_->presentation_status().pacing_wait_ns;
        if(self.previous_callback.time_since_epoch().count()) {
            const auto interval=std::chrono::duration<double,std::nano>(callback_time-self.previous_callback).count();
            const auto waits=total_pacing-self.previous_pacing_wait;
            const auto unpaced=std::clamp(interval-static_cast<double>(waits),1'000'000.0,100'000'000.0);
            self.unpaced_interval_ns=0.8*self.unpaced_interval_ns+0.2*unpaced;
        }
        self.previous_callback=callback_time; self.previous_pacing_wait=total_pacing;
        uint64_t source;
        { std::scoped_lock lock(owner.mutex_);
          if(reset) owner.state_.invalidate_history();
          source=owner.state_.record_real_frame(true); }
        owner.platform_->arm_real(source);
        try {
            // A frontend packet may already be queued when native presentation
            // breaks. Keep its real submission while refusing further SDK work.
            if(owner.platform_->generation_blocked()) {
                const auto presentation=owner.platform_->presentation_status();
                std::scoped_lock lock(owner.mutex_);
                owner.state_.set_capability({FrameGenerationMode::Fsr3,false,presentation.reason});
                return;
            }
            if(self.failed.load()) return;
            if (!native_context) throw std::runtime_error("Native Vulkan command resources unavailable");
            const auto& context=*static_cast<const filament::backend::VulkanCommandContext*>(native_context);
            if (!self.provider) self.initialize(context);
            if (self.size.width!=context.images[0].extent.width || self.size.height!=context.images[0].extent.height)
                throw std::runtime_error("Interpolation size changed without draining resources");
            const auto cb=context.commandBuffer;
            if (!self.initialized_layout) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                b.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                b.image=self.generated; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                bluevk::vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
                self.initialized_layout=true;
            }
            Fsr31VulkanFrame frame;
            frame.commands=cb; frame.frame_id=++self.frame_id;
            frame.reset=reset; frame.frame_time_ms=packet->dt_ms;
            frame.camera_near=packet->near_plane;
            frame.camera_far=std::numeric_limits<float>::max();
            frame.vertical_fov_radians=glm::radians(packet->camera.fov);
            frame.motion_vector_scale={float(self.size.width),float(self.size.height)};
            const auto model=glm::inverse(packet->camera.view_matrix());
            for (int i=0;i<3;++i) {
                frame.camera_position[i]=packet->camera.position[i];
                frame.camera_right[i]=model[0][i]; frame.camera_up[i]=model[1][i]; frame.camera_forward[i]=-model[2][i];
            }
            const auto input=[&](int i) {
                const auto& image=context.images[i];
                return Fsr31VulkanImage{image.image,image.format,image.extent,image.usage,image.layout};
            };
            frame.hudless_color=input(0); frame.depth=input(1); frame.motion_vectors=input(2);
            frame.generated_output={self.generated,VK_FORMAT_R8G8B8A8_UNORM,self.size,
                VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_LAYOUT_GENERAL};
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
            self.provider_record_attempts.fetch_add(1, std::memory_order_release);
#endif
            auto produced=self.provider->record(frame);
            if (!produced) throw std::runtime_error(produced.error());
            { std::scoped_lock lock(owner.mutex_);
              owner.state_.set_capability({FrameGenerationMode::Fsr3,true,"FSR 3.1 Vulkan (experimental)"}); }
            if (*produced) {
                bool captured_ui_inputs = false;
                if (owner.platform_->wants_ui_capture(source)) {
                    const auto& output = frame.generated_output;
                    const auto& ui = context.images[3];
                    captured_ui_inputs = owner.platform_->record_ui_inputs(cb,
                        {output.image, output.format, output.extent, output.usage, output.layout},
                        {ui.image, ui.format, ui.extent, ui.usage, ui.layout}, source);
                }
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
                bluevk::vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                bluevk::vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,self.pipeline);
                bluevk::vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,self.pipeline_layout,0,1,&self.descriptor_set,0,nullptr);
                bluevk::vkCmdDispatch(cb,(self.size.width+7)/8,(self.size.height+7)/8,1);
                barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                bluevk::vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                uint64_t generated_id;
                { std::scoped_lock lock(owner.mutex_); generated_id=owner.state_.record_generated_frame(source); }
                const bool armed = generated_id && owner.platform_->arm_generated(self.generated,
                    VK_FORMAT_R8G8B8A8_UNORM,self.size,source,static_cast<uint64_t>(self.unpaced_interval_ns));
                if (armed && captured_ui_inputs) owner.platform_->commit_ui_inputs(source);
                else owner.platform_->abandon_ui_inputs(source);
            }
        } catch (const std::exception& error) {
            owner.platform_->abandon_ui_inputs(source);
            if(native_context) {
                const auto& context=*static_cast<const filament::backend::VulkanCommandContext*>(native_context);
                if(context.discard) *context.discard=true;
            }
            std::fprintf(stderr,"Frame generation unavailable: %s\n",error.what());
            std::scoped_lock lock(owner.mutex_);
            owner.state_.set_capability({FrameGenerationMode::Fsr3,false,error.what()});
            self.failed.store(true);
            if(owner.platform_) owner.platform_->arm_real(source);
        }
    }
};
#else
struct FrameGenerationRuntime::Native {};
#endif

FrameGenerationRuntime::FrameGenerationRuntime(const BackendCore& core, FrameGenerationPlatform* platform,
        DlssPlatform* dlss_platform)
    : core_(core), platform_(platform), dlss_platform_(dlss_platform),
      temporal_(std::make_unique<TemporalScene>(core)),
      output_(std::make_shared<FrameGenerationOutput>(core)) {
#if HEAVEN_HAS_FSR3
    native_=std::make_unique<Native>(*this);
#endif
#if HEAVEN_HAS_DLSS
    if(dlss_platform_) dlss_=std::make_unique<DlssRuntimeBridge>(*dlss_platform_,output_);
#endif
}
FrameGenerationRuntime::~FrameGenerationRuntime() {
    // Callers drain queued native callbacks and GPU consumers first.
    drain();
    // Snapshot and formatting can allocate. A diagnostic failure must not
    // prevent resource retirement after the required drain has completed.
    try {
        const auto final=status();
        const auto count=[](uint64_t value,bool available) {
            return available?std::to_string(value):std::string("unavailable");
        };
        std::printf("Frame generation: requested=%s effective=%s real=%llu generated=%s native-real=%s native-generated=%s failed=%s reason=%s\n",
            std::string(frame_generation_mode_id(final.requested)).c_str(),std::string(frame_generation_mode_id(final.effective)).c_str(),
            static_cast<unsigned long long>(final.counters.real_frames),
            count(final.counters.generated_frames,final.counters.generated_frames_available).c_str(),
            count(final.counters.real_presented_frames,final.counters.presentation_submissions_available).c_str(),
            count(final.counters.generated_presented_frames,final.counters.presentation_submissions_available).c_str(),
            count(final.counters.failed_presentations,final.counters.presentation_submissions_available).c_str(),final.reason.c_str());
        if(final.sdk_presented_frames) std::printf("Frame generation SDK combined native submissions: %llu\n",
            static_cast<unsigned long long>(*final.sdk_presented_frames));
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Frame generation final diagnostics unavailable: %s\n",error.what());
    } catch (...) {
        std::fputs("Frame generation final diagnostics unavailable\n",stderr);
    }
    native_.reset(); dlss_.reset(); output_.reset(); grass_.reset(); temporal_.reset();
    if(motion_) core_.engine->destroy(motion_);
    if(masked_) core_.engine->destroy(masked_);
    if(grass_material_) core_.engine->destroy(grass_material_);
}
void FrameGenerationRuntime::drain() {
#if HEAVEN_HAS_FSR3
    if(platform_) {
        core_.renderer->nativeCommand(nullptr,nullptr,nullptr,nullptr,
            [](void* user,const void*) noexcept { static_cast<FrameGenerationPlatform*>(user)->drain_generated(); },platform_);
    }
#endif
#if HEAVEN_HAS_DLSS
    if(dlss_) core_.renderer->nativeCommand(nullptr,nullptr,nullptr,nullptr,
        [](void* user,const void*) noexcept {
            const auto result=static_cast<DlssRuntimeBridge*>(user)->drain_generation();
            if(result!=VK_SUCCESS) {
                // Retiring tagged images after a failed SDK drain is unsafe.
                std::fprintf(stderr,"DLSS device/presentation drain failed: %d\n",static_cast<int>(result));
                std::terminate();
            }
        },dlss_.get());
#endif
    core_.engine->flushAndWait();
    // Prospective tickets may have sampled input without an accepted render.
    // Retire them only after native callbacks and SDK presentation are drained.
    dlss_frame_.cancel();
}
void FrameGenerationRuntime::request(FrameGenerationMode mode) {
    drain();
    active_=false;
    temporal_->invalidate_history();
    if(grass_) grass_->invalidate_history();
    std::scoped_lock lock(mutex_);
    state_.request(mode);
    std::string reason;
    if(mode==FrameGenerationMode::Dlss) {
#if HEAVEN_HAS_DLSS
        if(!dlss_platform_) reason="Start the application with --frame-generation dlss to create the Streamline Vulkan device";
        else {
            const auto provider=dlss_platform_->provider_status();
            state_.set_capability({mode,provider.available,provider.reason});
        }
#else
        reason="This build does not include the optional Windows Streamline provider";
#endif
    }
#if !HEAVEN_HAS_FSR3
    if(mode==FrameGenerationMode::Fsr3) reason="This build does not include the optional FSR 3.1 Vulkan provider";
#endif
#if HEAVEN_HAS_FSR3
    if(mode==FrameGenerationMode::Fsr3 && !platform_) reason=dlss_platform_ ?
        "Restart with --frame-generation fsr3 to leave the Streamline presentation device" :
        "FSR frame generation requires the Vulkan presentation backend";
#endif
    if(!reason.empty()) state_.set_capability({mode,false,std::move(reason)});
    reset_=true; previous_source_={}; previous_camera_.reset();
}
void FrameGenerationRuntime::begin_application_frame([[maybe_unused]] uint64_t application_frame) {
#if HEAVEN_HAS_DLSS
    if(dlss_ && status().requested==FrameGenerationMode::Dlss)
        dlss_frame_.begin([this] { return dlss_->begin_frame(); });
#endif
}
void FrameGenerationRuntime::end_simulation() {
    // A rejected beginFrame retries the same logical source. Close simulation
    // and begin submission only when render_temporal enters an accepted frame.
}
void FrameGenerationRuntime::native_window_message([[maybe_unused]] uint32_t message) {
#if HEAVEN_HAS_DLSS
    if(dlss_platform_ && dlss_ && dlss_frame_.current().native_token &&
       message==dlss_platform_->latency_ping_message())
        dlss_->mark(dlss_frame_.current(),DlssLatencyMarker::LatencyPing);
#endif
}
FrameGenerationStatus FrameGenerationRuntime::status() {
    std::scoped_lock lock(mutex_);
#if HEAVEN_HAS_FSR3
    if(platform_) for(const auto& event:platform_->take_presentation_events()) {
        switch(event.kind) {
        case FrameGenerationPresentEventKind::RealSubmitted: state_.record_presented_frame(false,true); break;
        case FrameGenerationPresentEventKind::GeneratedSubmitted: state_.record_presented_frame(true,true); break;
        case FrameGenerationPresentEventKind::SubmissionFailed:
            state_.record_presented_frame(false,false);
            state_.set_capability({FrameGenerationMode::Fsr3,false,event.reason});
            reset_=true;
            break;
        case FrameGenerationPresentEventKind::HistoryInvalidated: state_.invalidate_history(); reset_=true; break;
        case FrameGenerationPresentEventKind::GenerationSkipped:
            if(!event.reason.empty()) state_.set_capability({FrameGenerationMode::Fsr3,false,event.reason});
            break;
        default: break;
        }
    }
#endif
    auto snapshot=state_.status();
#if HEAVEN_HAS_FSR3
    const bool presentation_blocked=platform_ && platform_->generation_blocked();
    if(platform_ && (snapshot.capability.mode==FrameGenerationMode::Fsr3 || presentation_blocked)) {
        const auto presentation=platform_->presentation_status();
        if(snapshot.requested==FrameGenerationMode::Fsr3 && presentation_blocked) {
            state_.set_capability({FrameGenerationMode::Fsr3,false,presentation.reason});
            snapshot=state_.status();
        }
        if(snapshot.capability.mode==FrameGenerationMode::Fsr3) {
            snapshot.counters.generated_frames_available=snapshot.capability.available;
            snapshot.counters.presentation_submissions_available=
                native_tracking_complete_ && presentation.dropped_events==0;
        }
    }
#endif
#if HEAVEN_HAS_DLSS
    if(dlss_platform_ && snapshot.requested==FrameGenerationMode::Dlss) {
        const auto provider=dlss_platform_->provider_status();
        apply_dlss_provider_status(state_,snapshot,provider);
    }
#endif
    return snapshot;
}
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
uint64_t FrameGenerationRuntime::debug_provider_record_attempts() const noexcept {
#if HEAVEN_HAS_FSR3
    if (native_) return native_->provider_record_attempts.load(std::memory_order_acquire);
#endif
    return 0;
}
#endif
bool FrameGenerationRuntime::prepare([[maybe_unused]] MeshStore& meshes, [[maybe_unused]] TextureStore& textures,
    [[maybe_unused]] std::span<const Instance> instances, [[maybe_unused]] std::span<const GpuMesh> gpu_meshes,
    [[maybe_unused]] const MaterialResolver& resolver, [[maybe_unused]] const AppConfig& config,
    [[maybe_unused]] const Camera& camera, [[maybe_unused]] int width, [[maybe_unused]] int height, [[maybe_unused]] bool has_ui) {
#if HEAVEN_HAS_FSR3 || HEAVEN_HAS_DLSS
    const bool was_active=active_;
#endif
    active_=false;
#if HEAVEN_HAS_FSR3 || HEAVEN_HAS_DLSS
    const auto requested=status().requested;
    bool usable=false;
#if HEAVEN_HAS_FSR3
    if(requested==FrameGenerationMode::Fsr3 && platform_ && platform_->generation_blocked())
        return unavailable(platform_->presentation_status().reason);
    usable=requested==FrameGenerationMode::Fsr3 && platform_ && !native_->failed.load();
#endif
#if HEAVEN_HAS_DLSS
    usable=usable || (requested==FrameGenerationMode::Dlss && dlss_ &&
        dlss_frame_.current().native_token && dlss_platform_->provider_status().available);
#endif
    if(!usable) return false;
    if(previous_camera_) {
        const auto old_model=glm::inverse(previous_camera_->view_matrix());
        const auto new_model=glm::inverse(camera.view_matrix());
        const auto angular=glm::dot(glm::vec3(old_model[2]),glm::vec3(new_model[2]));
        const auto gap=std::chrono::steady_clock::now()-previous_source_;
        if(glm::distance(previous_camera_->position,camera.position)>5.0f || angular<0.5f ||
           std::abs(previous_camera_->fov-camera.fov)>0.01f || gap>std::chrono::milliseconds(250))
            invalidate_history();
    }
    if(config.hdr || config.features.dynamic_resolution) {
        if(was_active) drain();
        temporal_->invalidate_history();
        if(grass_) grass_->invalidate_history();
        reset_=true;
        std::scoped_lock lock(mutex_);
        state_.set_capability({requested,false,"Frame generation currently requires SDR output and fixed resolution"});
        return false;
    }
#if HEAVEN_HAS_DLSS
    // Those effects do not yet provide a matching temporal participant. Keep
    // the strict SDK integration scoped to the measured opaque/masked path.
    // FSR optical flow remains an experimental option for the complete scene.
    if(requested==FrameGenerationMode::Dlss &&
       (config.features.particles || config.features.heat_haze ||
        config.features.clouds || config.features.volumetric_clouds ||
        config.features.god_rays || config.features.volumetric_fog ||
        config.features.lens_flares || config.features.refraction ||
        (config.features.rain && config.weather.rain_intensity>0.0f))) {
        if(was_active) drain();
        std::scoped_lock lock(mutex_);
        state_.set_capability({requested,false,
            "DLSS temporal coverage currently requires particles, heat haze, cloud layers, god rays, volumetric fog, lens flares, refraction and active rain disabled"});
        reset_=true;
        return false;
    }
#endif
    if(width_!=width || height_!=height) {
        drain();
#if HEAVEN_HAS_FSR3
        native_=std::make_unique<Native>(*this);
#endif
        invalidate_history(); width_=width; height_=height;
#if HEAVEN_HAS_DLSS
        // drain canceled the input-time ticket. Acquire and sleep for the next
        // logical source before its input; never replace that ticket here.
        if(requested==FrameGenerationMode::Dlss) return false;
#endif
    }
    if(!motion_) motion_=filament::Material::Builder().package(k_heaven_motion_filamat,k_heaven_motion_filamat_size).build(*core_.engine);
    if(!masked_) masked_=filament::Material::Builder().package(k_heaven_motion_masked_filamat,k_heaven_motion_masked_filamat_size).build(*core_.engine);
    if(!motion_ || !masked_ || !output_->ensure(width,height) || !temporal_->ensure(width,height) ||
       !temporal_->build(meshes,textures,instances,gpu_meshes,resolver,config,motion_,masked_))
        return unavailable("Frame-generation temporal targets or mesh proxies could not be created");
    const auto scaling=core_.camera->getScaling();
    const auto shift=core_.camera->getShift();
    projection_=temporal_gl_projection(core_.camera->getProjectionMatrix(),
        {float(scaling.x),float(scaling.y)},{float(shift.x),float(shift.y)});
    current_clip_=projection_*camera.view_matrix();
    if(!temporal_->prepare(camera,instances,current_clip_))
        return unavailable("Frame-generation camera or mesh history is invalid");
    const auto& coverage=temporal_->coverage();
    if(coverage.missing_current_bones || coverage.missing_current_positions)
        return unavailable("Frame generation is waiting for current skinned or deformed geometry");
    temporal_coverage_complete_=coverage.missing_current_bones==0 &&
        coverage.missing_current_positions==0 &&
        (reset_ || coverage.incomplete_history_primitives==0);
    has_ui_=has_ui;
    output_->prepare(temporal_->packed_texture(),width,height,has_ui);
    active_=true;
#endif
    return active_;
}
void FrameGenerationRuntime::bind_ui(filament::View& view) { if(active_) output_->bind_ui(view); }
void FrameGenerationRuntime::unbind_ui() { output_->unbind_ui(); }
void FrameGenerationRuntime::prepare_grass(std::span<const TemporalGrassPatch> patches,
        std::span<const Instance> instances,const Camera& camera,float time) {
    if(!active_) return;
    if(!grass_material_) grass_material_=filament::Material::Builder().package(
        k_heaven_motion_grass_filamat,k_heaven_motion_grass_filamat_size).build(*core_.engine);
    if(!grass_material_) { unavailable("Frame-generation foliage material is unavailable"); return; }
    if(!grass_) grass_=std::make_unique<TemporalGrass>(core_,temporal_->scene(),grass_material_);
    const auto scaling=core_.camera->getScaling();
    const auto shift=core_.camera->getShift();
    const auto projection=temporal_gl_projection(core_.camera->getProjectionMatrix(),
        {float(scaling.x),float(scaling.y)},{float(shift.x),float(shift.y)});
    if(!grass_->prepare(patches,instances,time,projection*camera.view_matrix()))
        unavailable("Frame-generation foliage ranges or transforms are invalid");
    temporal_coverage_complete_ &= grass_->coverage().unavailable_ranges==0 &&
        (reset_ || grass_->coverage().incomplete_history_ranges==0);
}
bool FrameGenerationRuntime::unavailable(std::string reason) {
    active_=false; reset_=true;
    std::scoped_lock lock(mutex_);
    state_.set_capability({state_.status().requested,false,std::move(reason)});
    return false;
}
filament::RenderTarget* FrameGenerationRuntime::scene_target() const { return active_?output_->scene_rt():nullptr; }
void FrameGenerationRuntime::render_temporal() {
    if(!active_) return;
#if HEAVEN_HAS_DLSS
    if(dlss_ && status().requested==FrameGenerationMode::Dlss)
        dlss_frame_.begin_submission([this](const DlssFrameToken& token,DlssLatencyMarker marker) {
            dlss_->mark(token,marker);
        });
#endif
    core_.renderer->render(temporal_->view()); output_->render_extract();
}
void FrameGenerationRuntime::render_composite() { if(active_) output_->render_composite(); }
void FrameGenerationRuntime::submit(bool accepted,[[maybe_unused]] const Camera& camera) {
    if(!accepted) { std::scoped_lock lock(mutex_); state_.record_real_frame(false); return; }
#if HEAVEN_HAS_DLSS
    if(active_ && status().requested==FrameGenerationMode::Dlss && dlss_) {
        DlssRuntimeFrame frame;
        frame.token=dlss_frame_.current(); frame.projection=projection_;
        frame.current_view_projection=current_clip_;
        frame.previous_view_projection=reset_?current_clip_:previous_clip_;
        frame.near_plane=static_cast<float>(core_.camera->getNear());
        frame.far_plane=std::numeric_limits<float>::max();
        frame.reset=reset_; frame.temporal_coverage_complete=temporal_coverage_complete_;
        frame.has_ui=has_ui_;
        struct Packet { FrameGenerationRuntime* owner; Camera camera; DlssRuntimeFrame frame; };
        auto* packet=new Packet{this,camera,frame};
        core_.renderer->nativeCommand(output_->color_texture(),output_->depth_texture(),output_->motion_texture(),output_->ui_texture(),
            [](void* data,const void* context) noexcept {
                std::unique_ptr<Packet> packet(static_cast<Packet*>(data));
                auto& owner=*packet->owner;
                { std::scoped_lock lock(owner.mutex_);
                  if(packet->frame.reset) owner.state_.invalidate_history();
                  owner.state_.record_real_frame(true); }
                if(context) {
                    const auto result=owner.dlss_->prepare_native(
                        *static_cast<const filament::backend::VulkanCommandContext*>(context),packet->camera,packet->frame);
                    if(!result) { std::scoped_lock lock(owner.mutex_);
                        owner.state_.set_capability({FrameGenerationMode::Dlss,false,result.error()}); }
                    else { const auto provider=owner.dlss_platform_->provider_status();
                        std::scoped_lock lock(owner.mutex_);
                        owner.state_.set_capability({FrameGenerationMode::Dlss,
                            provider.available && provider.enabled,provider.reason}); }
                }
                else { owner.dlss_->suspend_generation();
                    std::scoped_lock lock(owner.mutex_);
                    owner.state_.set_capability({FrameGenerationMode::Dlss,false,
                        "Native Vulkan temporal resources unavailable"}); }
            },packet);
        // The packet owns the reservation until the platform's pending/tag
        // owners take over; rejected submissions never release this ticket.
        dlss_frame_.accept();
        previous_clip_=current_clip_; previous_camera_=camera;
        previous_source_=std::chrono::steady_clock::now();
        temporal_->commit_accepted(); if(grass_) grass_->commit_accepted();
        reset_=false;
        return;
    }
#endif
    // An accepted ordinary fallback has no DLSS native packet. Cancel its
    // prospective simulation interval instead of reusing it for another source.
    dlss_frame_.cancel();
#if HEAVEN_HAS_FSR3
    if(active_) {
        const auto now=std::chrono::steady_clock::now();
        const auto dt=previous_source_.time_since_epoch().count()?std::chrono::duration<float,std::milli>(now-previous_source_).count():16.667f;
        previous_source_=now;
        previous_camera_=camera;
        previous_clip_=current_clip_;
        auto* packet=new Native::Packet{native_.get(),camera,reset_,std::clamp(dt,0.01f,1000.0f),
            static_cast<float>(core_.camera->getNear())};
        core_.renderer->nativeCommand(output_->color_texture(),output_->depth_texture(),output_->motion_texture(),output_->ui_texture(),Native::callback,packet);
        temporal_->commit_accepted(); reset_=false;
        if(grass_) grass_->commit_accepted();
        return;
    }
#endif
#if HEAVEN_HAS_FSR3
    if(platform_) {
        // Keep the native real-frame accounting complete while temporal
        // geometry warms up, generation is switched off, or the provider is
        // unavailable. A Runtime exists only after an explicit request. This callback
        // records no GPU work and does not allocate presentation resources.
        core_.renderer->nativeCommand(nullptr,nullptr,nullptr,nullptr,
            [](void* data,const void*) noexcept {
                auto& owner=*static_cast<FrameGenerationRuntime*>(data);
                uint64_t source;
                { std::scoped_lock lock(owner.mutex_);
                  source=owner.state_.record_real_frame(true); }
                owner.platform_->arm_real(source);
            },this);
        return;
    }
#endif
    std::scoped_lock lock(mutex_);
    native_tracking_complete_=false;
    state_.record_real_frame(true);
}
void FrameGenerationRuntime::set_bones(int mesh_index,std::span<const glm::mat4> matrices) {
    if(status().requested!=FrameGenerationMode::Off) temporal_->set_bones(mesh_index,matrices);
}
void FrameGenerationRuntime::set_positions(uint64_t handle,std::span<const Vertex> vertices) {
    if(status().requested!=FrameGenerationMode::Off) temporal_->set_positions(handle,vertices);
}
void FrameGenerationRuntime::invalidate_history() {
    temporal_->invalidate_history(); reset_=true; previous_source_={}; previous_camera_.reset();
    if(grass_) grass_->invalidate_history();
    std::scoped_lock lock(mutex_); state_.invalidate_history();
}
}
