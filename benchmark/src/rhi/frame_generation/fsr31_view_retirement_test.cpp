// Host-only regression against the actual generated SDK backend. Fake Vulkan
// callbacks own synthetic handles; no device, queue, GPU work or wait exists.
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include HEAVEN_FSR31_TEST_BACKEND_COPY
#include "fsr31_prepare_scope.h"

namespace {
template<class T> T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
template<class T> uintptr_t identity(T object) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(object);
    else return static_cast<uintptr_t>(object);
}
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct HostState {
    std::unordered_map<uintptr_t, unsigned> live;
    unsigned current_record = 0;
    unsigned completed_record = 0;
    unsigned created = 0;
    unsigned deleted = 0;
    unsigned restored = 0;
    uintptr_t next_handle = 100;
};
HostState* current = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice, const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks*, VkImageView* output) {
    require(current && info && output, "Invalid fake image-view allocation");
    require(info->format == VK_FORMAT_R8G8B8A8_UNORM && info->subresourceRange.levelCount == 1,
        "Unexpected actual SDK single-level image-view description");
    *output = handle<VkImageView>(++current->next_handle);
    require(current->live.emplace(identity(*output), current->current_record).second,
        "SDK overwrote a live image-view handle");
    ++current->created;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice, VkImageView view,
        const VkAllocationCallbacks*) {
    const auto found = current->live.find(identity(view));
    require(found != current->live.end(), "Image view retired twice or without ownership");
    if (found->second > current->completed_record) {
        throw std::runtime_error("Retired unfinished GPU view: owner record=" +
            std::to_string(found->second) + " current=" + std::to_string(current->current_record) +
            " completed=" + std::to_string(current->completed_record));
    }
    current->live.erase(found);
    ++current->deleted;
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
        VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
        const VkBufferMemoryBarrier*, uint32_t count, const VkImageMemoryBarrier* images) {
    require(count > 0 && images, "SDK unregister omitted the real input barriers");
    for (uint32_t i = 0; i != count; ++i) {
        require(images[i].oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            images[i].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            images[i].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
            images[i].dstAccessMask == VK_ACCESS_SHADER_READ_BIT,
            "Deferred view retirement broke input-layout restoration");
        ++current->restored;
    }
}

struct Harness {
    HostState host;
    BackendContext_VK backend{};
    BackendContext_VK::EffectContext effects[2]{};
    std::vector<BackendContext_VK::VkResourceView> views{
        2 * FFX_MAX_QUEUED_FRAMES * FFX_MAX_RESOURCE_COUNT * 2};
    std::vector<BackendContext_VK::Resource> resources{2 * FFX_MAX_RESOURCE_COUNT};
    FfxInterface interface{};

    Harness() {
        require(current == nullptr, "Fixture harnesses must not overlap");
        current = &host;
        backend.pEffectContexts = effects;
        backend.pResourceViews = views.data();
        backend.pResources = resources.data();
        backend.device = handle<VkDevice>(1);
        backend.vkFunctionTable.vkCreateImageView = create_view;
        backend.vkFunctionTable.vkDestroyImageView = destroy_view;
        backend.vkFunctionTable.vkCmdPipelineBarrier = barrier;
        interface.scratchBuffer = &backend;
        for (unsigned c = 0; c != 2; ++c) {
            effects[c].nextStaticResource = c * FFX_MAX_RESOURCE_COUNT + 1;
            effects[c].nextDynamicResource = getDynamicResourcesStartIndex(c);
            effects[c].nextStaticResourceView = c * FFX_MAX_QUEUED_FRAMES * FFX_MAX_RESOURCE_COUNT * 2;
            for (unsigned slot = 0; slot != FFX_MAX_QUEUED_FRAMES; ++slot)
                effects[c].nextDynamicResourceView[slot] = getDynamicResourceViewsStartIndex(c, slot);
        }
    }
    ~Harness() { current = nullptr; }

    // Counts model this provider's current single-level imported resources.
    // Use the actual SDK registration allocator and SRV/UAV creation paths.
    void stage(unsigned context, unsigned count, unsigned uav_count) {
        std::vector<FfxResourceInternal> imported;
        const auto first = getDynamicResourcesStartIndex(context);
        for (unsigned i = 0; i != count; ++i) {
            FfxResource resource{};
            resource.resource = reinterpret_cast<void*>(uintptr_t(1000 + i));
            resource.state = FFX_RESOURCE_STATE_COMPUTE_READ;
            resource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
            resource.description.format = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
            resource.description.width = resource.description.height = 64;
            resource.description.depth = resource.description.mipCount = 1;
            resource.description.usage = i < count - uav_count ? FFX_RESOURCE_USAGE_READ_ONLY : FFX_RESOURCE_USAGE_UAV;
            FfxResourceInternal output{};
            require(RegisterResourceVK(&interface, &resource, context, &output) == FFX_OK,
                "Actual SDK registration failed");
            imported.push_back(output);
            backend.pResources[output.internalIndex].currentState = FFX_RESOURCE_STATE_COPY_DEST;
        }
        require(UnregisterResourcesVK(&interface,
            reinterpret_cast<FfxCommandList>(uintptr_t(1)), context) == FFX_OK,
            "Actual SDK unregister failed");
        for (auto imported_resource : imported) {
            const auto& resource = backend.pResources[imported_resource.internalIndex];
            require(resource.currentState == resource.initialState &&
                resource.srvViewIndex == -1 && resource.uavViewIndex == -1,
                "SDK unregister did not restore resource states/view indices");
        }
        require(effects[context].nextDynamicResource == first &&
            backend.scheduledImageBarrierCount == 0,
            "SDK unregister did not reset resource/barrier bookkeeping");
    }
    void drain() {
        // Model an explicit successful owner drain, never reset or discard.
        host.completed_record = host.current_record;
        for (unsigned c = 0; c != 2; ++c)
            for (unsigned slot = 0; slot != FFX_MAX_QUEUED_FRAMES; ++slot)
                destroyDynamicViews(&backend, c, slot);
        require(host.live.empty() && host.created == host.deleted,
            "Explicit drain did not balance actual SDK view ownership");
    }
};

void records(bool old_policy, bool violate_completion = false) {
    Harness h;
    for (h.host.current_record = 1; h.host.current_record <= 32; ++h.host.current_record) {
        // A successful FrameSkipper latency2 admission permits previous and
        // current records unfinished, but requires R-2 complete before R.
        h.host.completed_record = violate_completion ? 0 :
            (h.host.current_record > 2 ? h.host.current_record - 2 : 0);
        const auto fi_slot = h.effects[0].frameIndex;
        {
            heaven::Fsr31PrepareViewScope prepare(h.interface);
            if (old_policy) heavenFsr31VkDeferViewRotation(&h.interface, false);
            h.stage(0, 5, 3); // Actual SDK FI Prepare's five imports.
            if (!old_policy) require(h.effects[0].frameIndex == fi_slot,
                "Prepare rotated the provider-owned image-view slot");
        }
        require(!h.backend.heavenDeferViewRotation, "Prepare left view rotation deferred");
        h.stage(1, 3, 2); // Optical flow rotates its separate context once.
        h.stage(0, 7, 6); // Final FI Dispatch rotates once, including reset records.
        if (!old_policy) require(h.effects[0].frameIndex == (fi_slot + 1) % FFX_MAX_QUEUED_FRAMES,
            "FI record did not advance its image-view slot exactly once");
    }
    h.host.current_record = 32;
    require(h.host.created == 32 * 26 && h.host.restored == 32 * 15,
        "Actual SDK registration/barrier coverage changed");
    h.drain();
    std::cout << "PASS: 32 overlapping provider records, " << h.host.deleted <<
        " actual SDK view retirements, " << h.host.restored << " input-layout restorations\n";
}

enum class PrepareExit { Success, ErrorBefore, ErrorAfter, ThrowBefore, ThrowAfter };
void prepare_cleanup() {
    for (auto test : {PrepareExit::Success, PrepareExit::ErrorBefore, PrepareExit::ErrorAfter,
            PrepareExit::ThrowBefore, PrepareExit::ThrowAfter}) {
        Harness h;
        h.host.current_record = 1;
        bool threw = false;
        FfxErrorCode result = FFX_OK;
        try {
            result = [&] {
                heaven::Fsr31PrepareViewScope prepare(h.interface);
                require(h.backend.heavenDeferViewRotation, "Shared prepare scope did not defer rotation");
                if (test == PrepareExit::ErrorBefore) return FFX_ERROR_BACKEND_API_ERROR;
                if (test == PrepareExit::ThrowBefore) throw std::runtime_error("Injected prepare exception");
                h.stage(0, 5, 3);
                if (test == PrepareExit::ErrorAfter) return FFX_ERROR_BACKEND_API_ERROR;
                if (test == PrepareExit::ThrowAfter) throw std::runtime_error("Injected prepare exception");
                return FFX_OK;
            }();
        } catch (const std::runtime_error& error) {
            if (std::string(error.what()) != "Injected prepare exception") throw;
            threw = true;
        }
        const bool expected_throw = test == PrepareExit::ThrowBefore || test == PrepareExit::ThrowAfter;
        const bool expected_error = test == PrepareExit::ErrorBefore || test == PrepareExit::ErrorAfter;
        require(threw == expected_throw && (result != FFX_OK) == expected_error,
            "Prepare error/exception did not propagate");
        require(!h.backend.heavenDeferViewRotation && h.effects[0].frameIndex == 0,
            "Shared RAII prepare cleanup leaked the policy or advanced views");
        require(h.host.deleted == 0 && h.host.live.size() == h.host.created,
            "Prepare cleanup treated discard/failure as GPU completion");
        h.drain();
    }
    {
        Harness h;
        {
            heaven::Fsr31PrepareViewScope outer(h.interface);
            {
                heaven::Fsr31PrepareViewScope inner(h.interface);
                require(h.backend.heavenDeferViewRotation, "Nested prepare scope lost policy");
            }
            require(h.backend.heavenDeferViewRotation, "Nested prepare scope did not restore previous policy");
        }
        require(!h.backend.heavenDeferViewRotation, "Outer prepare scope did not restore policy");
    }
    std::cout << "PASS: shared RAII prepare success, early error, post-unregister error, "
        "early/post-unregister exception, nested cleanup; failure retains live views until explicit drain\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--negative-old-double-rotation") {
            records(true);
            std::cerr << "Negative control unexpectedly survived old double rotation\n";
            return 2;
        }
        if (argc == 2 && std::string(argv[1]) == "--negative-unbounded-inflight") {
            records(false, true);
            std::cerr << "Negative control unexpectedly survived caller completion violation\n";
            return 2;
        }
        require(argc == 1, "Unknown host fixture argument");
        prepare_cleanup();
        records(false);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
