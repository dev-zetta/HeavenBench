#!/usr/bin/env python3
"""Execute the actual Filament present metadata path and guard observer retirement."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from dlss_lifecycle_contract_test import raw_body

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / "benchmark/src/rhi/frame_generation/frame_generation_platform.cpp"
SURFACE = ROOT / "lib/filament/filament/backend/src/vulkan/platform/VulkanPlatformSwapChainImpl.cpp"


def lifecycle_contract(text):
    for signature, retirement in (("VkResult FrameGenerationPlatform::recreate(", "Base::recreate(handle)"),
                                  ("void FrameGenerationPlatform::destroy(", "Base::destroy(handle)"),
                                  ("void FrameGenerationPlatform::terminate()", "Base::terminate()")):
        body = raw_body(text, signature)
        if not 0 <= body.find("impl_->present_trace.finish(") < body.find(retirement):
            raise AssertionError("Presentation observer was not joined before native retirement")
    recreate = raw_body(text, "VkResult FrameGenerationPlatform::recreate(")
    for clear in ("bundle.nativeSwapchain = VK_NULL_HANDLE;", "bundle.presentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;"):
        if not 0 <= recreate.find(clear) < recreate.find("Base::recreate(handle)"):
            raise AssertionError("Failed recreation can retain retired native timing metadata")
    acquire = raw_body(SURFACE.read_text(), "VkResult VulkanPlatformSurfaceSwapChain::acquire(")
    if "recreate(" in acquire or "destroy(" in acquire:
        raise AssertionError("Internal acquire retirement bypasses the platform lifecycle hook")
    driver = (ROOT / "lib/filament/filament/backend/src/vulkan/VulkanSwapChain.cpp").read_text()
    if "mPlatform->recreate(swapChain)" not in driver:
        raise AssertionError("Automatic recreation no longer uses the virtual platform hook")


PREFIX = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <cstdio>
#include <iostream>
#define UTILS_VERY_UNLIKELY(value) (value)
#define FVK_LOGW std::cerr
bool google=false, valid=true;
unsigned calls=0;
VkResult next_result=VK_SUCCESS;
const VkPresentIdKHR* expected_id=nullptr;
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue,const VkPresentInfoKHR* info) {
    ++calls;
    const auto* node=static_cast<const VkBaseInStructure*>(info->pNext);
    if(google) {
        valid=valid&&node&&node->sType==VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
        if(node&&node->sType==VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) {
            const auto* timing=reinterpret_cast<const VkPresentTimesInfoGOOGLE*>(node);
            valid=valid&&timing->swapchainCount==1&&timing->pTimes->desiredPresentTime==1234;
            node=node->pNext;
        }
    }
    valid=valid&&node==reinterpret_cast<const VkBaseInStructure*>(expected_id);
    if(expected_id) valid=valid&&expected_id->swapchainCount==1&&*expected_id->pPresentIds==71;
    return next_result;
}
struct VulkanPlatformSurfaceSwapChain {
    struct Context { bool isGoogleDisplayTimingEnabled() const { return google; } } mContext;
    bool mSurfaceLost=false,mSuboptimal=false;
    uint32_t mArbitraryFrameId=0;
    int64_t mPresentationTime=1234;
    VkSwapchainKHR mSwapchain=VK_NULL_HANDLE;
    VkQueue mQueue=VK_NULL_HANDLE;
    VkResult presentWithNext(uint32_t index,VkSemaphore finished,void const* next);
};
VkResult VulkanPlatformSurfaceSwapChain::presentWithNext(uint32_t index,VkSemaphore finished,void const* next) {
'''
TEST = r'''
}
int main() {
    const uint64_t id=71;
    const VkPresentIdKHR metadata{VK_STRUCTURE_TYPE_PRESENT_ID_KHR,nullptr,1,&id};
    for(const bool google_timing : {false,true}) {
        google=google_timing;
        VulkanPlatformSurfaceSwapChain chain;
        expected_id=&metadata;
        if(chain.presentWithNext(0,VK_NULL_HANDLE,&metadata)!=VK_SUCCESS||!valid) {
            std::fputs("FAIL: borrowed present ID or Google metadata lost\n",stderr);return 1;
        }
        expected_id=nullptr;
        if(chain.presentWithNext(0,VK_NULL_HANDLE,nullptr)!=VK_SUCCESS||!valid) {
            std::fputs("FAIL: ordinary present metadata changed\n",stderr);return 1;
        }
    }
    google=false;
    VulkanPlatformSurfaceSwapChain chain;
    expected_id=nullptr;next_result=VK_SUBOPTIMAL_KHR;
    if(chain.presentWithNext(0,VK_NULL_HANDLE,nullptr)!=VK_SUBOPTIMAL_KHR||!chain.mSuboptimal) return 1;
    next_result=VK_ERROR_SURFACE_LOST_KHR;
    if(chain.presentWithNext(0,VK_NULL_HANDLE,nullptr)!=VK_ERROR_SURFACE_LOST_KHR||!chain.mSurfaceLost) return 1;
    const auto before=calls;next_result=VK_SUCCESS;
    if(chain.presentWithNext(0,VK_NULL_HANDLE,nullptr)!=VK_ERROR_SURFACE_LOST_KHR||calls!=before) return 1;
    std::puts("Actual surface present path passed metadata, ordinary, Google, suboptimal and surface-loss controls");
    return 0;
}
'''

DEVICE_PREFIX = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
bool extensions_supported=true,features_supported=true,loader_supported=true;
bool expect_enabled=false,valid=true,fail_create=false,looked_up_null=false;
unsigned enumerated=0,queried=0,created=0,looked_up=0;
template<class H> H identity() { static int token;return reinterpret_cast<H>(&token); }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice,const char*,uint32_t* count,VkExtensionProperties* data) {
    ++enumerated;
    if(!data) { *count=extensions_supported?2u:0u;return VK_SUCCESS; }
    if(extensions_supported) {
        std::strcpy(data[0].extensionName,VK_KHR_PRESENT_ID_EXTENSION_NAME);
        std::strcpy(data[1].extensionName,VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice,VkPhysicalDeviceFeatures2* features) {
    ++queried;
    auto* id=static_cast<VkPhysicalDevicePresentIdFeaturesKHR*>(features->pNext);
    auto* wait=static_cast<VkPhysicalDevicePresentWaitFeaturesKHR*>(id->pNext);
    valid=valid&&id->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR&&
        wait->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    id->presentId=wait->presentWait=features_supported?VK_TRUE:VK_FALSE;
}
VKAPI_ATTR VkResult VKAPI_CALL wait_present(VkDevice,VkSwapchainKHR,uint64_t,uint64_t) { return VK_SUCCESS; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char* name) {
    ++looked_up;looked_up_null=looked_up_null||device==VK_NULL_HANDLE;
    valid=valid&&std::strcmp(name,"vkWaitForPresentKHR")==0;
    return loader_supported?reinterpret_cast<PFN_vkVoidFunction>(wait_present):nullptr;
}
struct Base {
    static VkDevice createVkDevice(const VkDeviceCreateInfo& info) {
        ++created;
        bool id=false,wait=false,old=false;
        unsigned id_ext=0,wait_ext=0;
        for(uint32_t i=0;i<info.enabledExtensionCount;++i) {
            id_ext+=std::strcmp(info.ppEnabledExtensionNames[i],VK_KHR_PRESENT_ID_EXTENSION_NAME)==0;
            wait_ext+=std::strcmp(info.ppEnabledExtensionNames[i],VK_KHR_PRESENT_WAIT_EXTENSION_NAME)==0;
        }
        for(auto* node=static_cast<const VkBaseInStructure*>(info.pNext);node;node=node->pNext) {
            if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)
                id=reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(node)->presentId==VK_TRUE;
            if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR)
                wait=reinterpret_cast<const VkPhysicalDevicePresentWaitFeaturesKHR*>(node)->presentWait==VK_TRUE;
            old=old||node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        }
        valid=valid&&old&&id==expect_enabled&&wait==expect_enabled&&
            id_ext==(expect_enabled?1u:0u)&&wait_ext==(expect_enabled?1u:0u);
        return fail_create?VK_NULL_HANDLE:identity<VkDevice>();
    }
};
struct MockTrace {
    using Wait=int32_t(*)(void*,uint64_t,uint64_t,uint64_t);
    bool request=true,available=false;
    unsigned throw_configuration=0;
    std::string reason;
    bool requested() const { return request; }
    void configure(bool enabled,std::string why,Wait callback=nullptr,void* =nullptr) {
        if(throw_configuration==1) throw std::runtime_error("injected observer configuration failure");
        if(throw_configuration==2) throw 17;
        available=enabled&&callback;reason=std::move(why);
    }
};
struct FrameGenerationPlatform {
    struct Impl {
        MockTrace present_trace;
        PFN_vkWaitForPresentKHR wait_for_present=nullptr;
        VkDevice trace_device=VK_NULL_HANDLE;
        static int32_t wait_present(void*,uint64_t,uint64_t,uint64_t) { return 0; }
    };
    std::unique_ptr<Impl> impl_=std::make_unique<Impl>();
    VkPhysicalDevice getPhysicalDevice() { return identity<VkPhysicalDevice>(); }
    VkDevice createVkDevice(const VkDeviceCreateInfo&) noexcept;
    bool presentation_trace_requested() const noexcept;
};
VkDevice FrameGenerationPlatform::createVkDevice(const VkDeviceCreateInfo& createInfo) noexcept {
'''
DEVICE_TEST = r'''
}
int main() {
    const char* extensions[]={"VK_KHR_swapchain"};
    VkPhysicalDeviceFeatures2 original{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext=&original;info.enabledExtensionCount=1;info.ppEnabledExtensionNames=extensions;
    for(unsigned mode=0;mode<9;++mode) {
        extensions_supported=mode!=1&&mode!=6;features_supported=mode!=2;loader_supported=mode!=3;
        expect_enabled=mode>=3&&mode!=6;valid=true;enumerated=queried=created=looked_up=0;
        fail_create=mode==7;looked_up_null=false;
        FrameGenerationPlatform platform;platform.impl_->present_trace.request=mode!=0;
        platform.impl_->present_trace.throw_configuration=mode==8?2u:(mode==5||mode==6?1u:0u);
        if(platform.presentation_trace_requested()!=(mode!=0)) {
            std::fputs("FAIL: public trace request getter changed default-off identity\n",stderr);return 1;
        }
        const auto device=platform.createVkDevice(info);
        const bool expected_available=mode==4;
        if(created!=1) {
            std::fputs("FAIL: device created more than once after observer setup failure\n",stderr);return 1;
        }
        if(looked_up_null||(mode==7&&looked_up!=0)) {
            std::fputs("FAIL: looked up null device after failed creation\n",stderr);return 1;
        }
        if(device!=(mode==7?VK_NULL_HANDLE:identity<VkDevice>())||!valid||
            platform.impl_->present_trace.available!=expected_available||
            (mode==0&&(enumerated!=0||queried!=0))||
            (mode==1&&queried!=0)||
            original.pNext||info.pNext!=&original||info.enabledExtensionCount!=1) {
            std::fprintf(stderr,"FAIL: trace feature/default-off guard or borrowed create-info changed, case%u\n",mode);
            return 1;
        }
        if(mode>0&&mode<4&&platform.impl_->present_trace.reason.empty()) return 1;
    }
    std::puts("Actual device hook passed default-off, capabilities, null creation and pre/post-create exception controls");
    return 0;
}
'''

HANDLE_PREFIX = r'''
#include <bit>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <type_traits>
'''
HANDLE_TEST = r'''
int main() {
    alignas(16) int token=0;
    for(auto* pointer : {static_cast<int*>(nullptr),&token}) {
        if(handle_from_bits<int*>(handle_bits(pointer))!=pointer) return 1;
    }
    for(const uint64_t value : {uint64_t{0},uint64_t{1},uint64_t{1}<<63,UINT64_MAX}) {
        if(handle_from_bits<uint64_t>(handle_bits(value))!=value) return 1;
    }
    std::puts("Actual handle helpers preserve null, pointer and integer handle bits");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--vulkan-include", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    work = args.work_dir.resolve()
    text = PLATFORM.read_text()
    lifecycle_contract(text)
    altered = text.replace("impl_->present_trace.finish(", "ignored_trace_finish(")
    try:
        lifecycle_contract(altered)
        raise RuntimeError("Missing lifecycle join negative was accepted")
    except AssertionError:
        pass
    altered = text.replace("found->second.bundle.nativeSwapchain = VK_NULL_HANDLE;", "")
    try:
        lifecycle_contract(altered)
        raise RuntimeError("Stale borrowed handle negative was accepted")
    except AssertionError:
        pass
    surface = SURFACE.read_text()
    body = raw_body(surface, "VkResult VulkanPlatformSurfaceSwapChain::presentWithNext(")
    assert body.count(".pNext = next,") == 2
    observations = []
    device = raw_body(text, "VkDevice FrameGenerationPlatform::createVkDevice(")
    device += ("\n}\nbool FrameGenerationPlatform::presentation_trace_requested() const noexcept {\n" +
               raw_body(text, "bool FrameGenerationPlatform::presentation_trace_requested()"))
    feature_guard = "if (!present_id.presentId || !present_wait.presentWait)"
    assert feature_guard in device
    null_guard = "if (!device) return device;"
    assert null_guard in device
    exception_report = 'std::fprintf(stderr, "Presentation trace observer setup failed: %s\\n", error.what());'
    assert exception_report in device
    handles = ("template<class Handle> uint64_t handle_bits(Handle handle) noexcept {\n" +
               raw_body(text, "uint64_t handle_bits(") + "\n}\n" +
               "template<class Handle> Handle handle_from_bits(uint64_t bits) noexcept {\n" +
               raw_body(text, "Handle handle_from_bits(") + "\n}\n")
    for name, candidate, expected, prefix, suffix, failure in (
            ("positive", body, 0, PREFIX, TEST, None),
            ("lost-borrowed-metadata", body.replace(".pNext = next,", ".pNext = nullptr,"), 1,
             PREFIX, TEST, "borrowed present ID or Google metadata lost"),
            ("device-positive", device, 0, DEVICE_PREFIX, DEVICE_TEST, None),
            ("missing-feature-guard", device.replace(feature_guard, "if (false)"), 1,
             DEVICE_PREFIX, DEVICE_TEST, "trace feature/default-off guard"),
            ("post-create-fallback", device.replace(exception_report, exception_report +
             "\n            return Base::createVkDevice(createInfo);"), 1,
             DEVICE_PREFIX, DEVICE_TEST, "device created more than once after observer setup failure"),
            ("missing-null-device-guard", device.replace(null_guard, ""), 1,
             DEVICE_PREFIX, DEVICE_TEST, "looked up null device after failed creation"),
            ("invented-trace-request", device.replace("return impl_->present_trace.requested();", "return true;"), 1,
             DEVICE_PREFIX, DEVICE_TEST, "public trace request getter changed default-off identity"),
            ("handle-roundtrip", handles, 0, HANDLE_PREFIX, HANDLE_TEST, None)):
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "fixture.cpp"
        source.write_text(prefix + candidate + suffix)
        msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
        binary = directory / ("fixture.exe" if msvc else "fixture")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", "/I" + str(args.vulkan_include),
                    str(source), "/Fe:" + str(binary)] if msvc else
                   [args.cxx, "-std=c++20", "-I" + str(args.vulkan_include), str(source), "-o", str(binary)])
        compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError("Actual present-body fixture did not compile: " + compiled.stdout + compiled.stderr)
        executed = subprocess.run([str(binary)], cwd=directory, capture_output=True, text=True, timeout=10)
        (directory / "run.log").write_text(executed.stdout + executed.stderr)
        if executed.returncode != expected or (failure and failure not in executed.stderr):
            raise RuntimeError("Present metadata control had the wrong result: " + executed.stdout + executed.stderr)
        observations.append({"name": name, "command": command, "exit_code": executed.returncode})
    receipt = {"passed": True, "gpu_execution": False, "scope": "Actual device creation, handle helpers and surface present bodies with actual Vulkan structs/fake calls; pre/post-create failures, null device and metadata controls. Lifecycle join source contract backed by the observer CPU fixture.",
               "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (PLATFORM, SURFACE)},
               "controls": observations}
    (work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("Presentation device/metadata/lifecycle source contracts passed, including duplicate-device, null-lookup and retirement negatives")


if __name__ == "__main__":
    main()
