#!/usr/bin/env python3
"""Host oracle for the actual linear-stage upload and host-write barrier code."""
import hashlib
import re
from pathlib import Path
import subprocess

from dlss_lifecycle_contract_test import raw_body

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include "vulkan/utils/LinearStageImage.h"
#include <backend/PixelBufferDescriptor.h>
#include <utils/FixedCapacityVector.h>
#include <utils/Panic.h>
#include <algorithm>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
namespace utils {
UTILS_NORETURN void panic(const char*,const char*,int,const char*) noexcept { std::_Exit(86); }
}
using namespace filament::backend;
#define FVK_LOGE std::cerr
template<class H> H token(unsigned index=0) {
    alignas(16) static unsigned char values[32]{};
    if constexpr(std::is_pointer_v<H>) return reinterpret_cast<H>(&values[index]);
    else return static_cast<H>(reinterpret_cast<uintptr_t>(&values[index]));
}
void require(bool valid,const char* reason) { if(!valid) throw std::runtime_error(reason); }
std::vector<std::string> operations;
unsigned barriers=0,blits=0,stage_transitions=0;
VkImageTiling active_tiling=VK_IMAGE_TILING_LINEAR;
std::vector<uint8_t> buffer_bytes(1024,0xAB),optimal_bytes;
std::string failure;
bool initially_undefined=false;
unsigned transitions=0;
VkImageLayout expected_old=VK_IMAGE_LAYOUT_PREINITIALIZED;
std::vector<uint8_t> mapped_bytes(128*12,0);
struct Allocation {} allocation;
namespace filament::backend {
namespace fvkmemory {
template<class T> struct resource_ptr {
    T* value;
    T* operator->() const { return value; }
    static resource_ptr cast(T* pointer) { return {pointer}; }
};
}
class VulkanStage {
public:
    struct Segment {
        Allocation* memory() const { return &allocation; }
        uint32_t offset() const { return 64; }
        VkBuffer buffer() const { return token<VkBuffer>(2); }
        void* mapping() const { return buffer_bytes.data()+offset(); }
    };
};
uint8_t getAlignmentForBufferToImageCopy(VkFormat) { return 4; }
class VulkanStageImage {
public:
    VkImage image() const { return token<VkImage>(); }
    fvkutils::LinearStageImageState mHostLayout;
    VkImageTiling tiling() const { return active_tiling; }
    struct Resource {
        VulkanStageImage* mImage;
        VkImage image() const { return mImage->image(); }
        Allocation* memory() const { return &allocation; }
        VkImageTiling tiling() const { return mImage->tiling(); }
        VkFormat format() const { return VK_FORMAT_R32G32B32A32_SFLOAT; }
        void prepareForRead(VkCommandBuffer commands) const noexcept;
    };
};
struct StagePool {
    VulkanStageImage image;
    VulkanStageImage::Resource resource{&image};
    VulkanStage::Segment segment;
    fvkmemory::resource_ptr<VulkanStage::Segment> acquireStage(size_t bytes,uint32_t alignment) {
        require(bytes==2*2*6*4*sizeof(float)&&alignment==4,"optimal staging extent/alignment differ");
        operations.emplace_back("acquire-buffer");return {&segment};
    }
    fvkmemory::resource_ptr<VulkanStageImage::Resource> acquireImage(
            PixelDataFormat,PixelDataType,uint32_t width,uint32_t height) {
        require(width==2&&height==12,"actual upload lost tall cubemap staging extent");
        return {&resource};
    }
};
struct VulkanCommandBuffer {
    VkCommandBuffer buffer() { return token<VkCommandBuffer>(); }
    template<class T> void acquire(fvkmemory::resource_ptr<T>) {
        if constexpr(std::is_same_v<T,VulkanStageImage::Resource>) operations.emplace_back("retain-stage");
        else if constexpr(std::is_same_v<T,VulkanStage::Segment>) operations.emplace_back("retain-buffer");
        else operations.emplace_back("retain-texture");
    }
};
struct Commands { VulkanCommandBuffer command; VulkanCommandBuffer& get() { return command; } };
struct State { StagePool mStagePool; Commands commands; Commands* mCommands=&commands;
    int mAllocator=19; VkDevice mDevice=token<VkDevice>();
    VkImage mTextureImage=token<VkImage>(1); };
enum class VulkanLayout { UNDEFINED,SAMPLE,TRANSFER_DST,TRANSFER_SRC };
namespace fvkutils {
VkImageAspectFlags getImageAspect(VkFormat) { return VK_IMAGE_ASPECT_COLOR_BIT; }
VkImageLayout getVkLayout(VulkanLayout layout) { return layout==VulkanLayout::TRANSFER_DST?
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:(layout==VulkanLayout::TRANSFER_SRC?
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
struct Transition { VkImage image;VulkanLayout oldLayout,newLayout;VkImageSubresourceRange subresources; };
void transitionLayout(VkCommandBuffer,Transition const& t) {
    require(active_tiling==VK_IMAGE_TILING_OPTIMAL&&t.image==token<VkImage>()&&
        t.oldLayout==(stage_transitions?VulkanLayout::TRANSFER_DST:VulkanLayout::TRANSFER_SRC)&&
        t.newLayout==(stage_transitions?VulkanLayout::TRANSFER_SRC:VulkanLayout::TRANSFER_DST)&&
        t.subresources.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&t.subresources.levelCount==1&&
        t.subresources.layerCount==1,"optimal staging transition lost layout/dependency");
    ++stage_transitions;operations.emplace_back("optimal-transition");
}
}
class VulkanTexture {
public:
    State state;
    State* mState=&state;
    SamplerType target=SamplerType::SAMPLER_CUBEMAP;
    VkImageAspectFlags getImageAspect() { return VK_IMAGE_ASPECT_COLOR_BIT; }
    VulkanLayout getLayout(uint32_t,uint32_t) { return initially_undefined?VulkanLayout::UNDEFINED:VulkanLayout::SAMPLE; }
    VulkanLayout getDefaultLayout() { return VulkanLayout::SAMPLE; }
    void transitionLayout(VulkanCommandBuffer*,VkImageSubresourceRange const&,VulkanLayout layout) {
        require(layout==(transitions++?VulkanLayout::SAMPLE:VulkanLayout::TRANSFER_DST),
            "actual upload did not restore defined prior/default texture layout");
    }
    void updateImageWithBlit(PixelBufferDescriptor const&,uint32_t,uint32_t,uint32_t,
        uint32_t,uint32_t,uint32_t,uint32_t);
};
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer command,VkPipelineStageFlags src,
        VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t memory_count,
        VkMemoryBarrier const*,uint32_t buffer_count,VkBufferMemoryBarrier const*,
        uint32_t count,VkImageMemoryBarrier const* image) {
    require(active_tiling==VK_IMAGE_TILING_LINEAR,"optimal image used host-write barrier");
    ++barriers;operations.emplace_back("host-barrier");
    require(command==token<VkCommandBuffer>()&&src==VK_PIPELINE_STAGE_HOST_BIT&&
        dst==VK_PIPELINE_STAGE_TRANSFER_BIT&&!flags&&!memory_count&&!buffer_count&&count==1,
        "host-write barrier stages or command scope differ");
    require(image->sType==VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER&&
        image->srcAccessMask==VK_ACCESS_HOST_WRITE_BIT&&image->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT&&
        image->oldLayout==expected_old&&image->newLayout==VK_IMAGE_LAYOUT_GENERAL&&
        image->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&
        image->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&image->image==token<VkImage>()&&
        image->subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&
        image->subresourceRange.baseMipLevel==0&&image->subresourceRange.levelCount==1&&
        image->subresourceRange.baseArrayLayer==0&&image->subresourceRange.layerCount==1,
        "host-write barrier lost fresh/reused layout, access or subresource identity");
}
namespace bluevk { PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier=&::barrier; }
void vkGetImageSubresourceLayout(VkDevice,VkImage,VkImageSubresource const*,VkSubresourceLayout* layout) {
    require(active_tiling==VK_IMAGE_TILING_LINEAR,"optimal image queried as linear");
    operations.emplace_back("query");*layout={0,128*12,128,0,0};
}
VkResult vmaMapMemory(int,Allocation*,void** mapped) {
    require(active_tiling==VK_IMAGE_TILING_LINEAR,"optimal image host-mapped");
    operations.emplace_back("map");
    if(failure=="map") return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *mapped=mapped_bytes.data();return VK_SUCCESS;
}
VkResult vmaFlushAllocation(int,Allocation*,VkDeviceSize offset,VkDeviceSize size) {
    if(active_tiling==VK_IMAGE_TILING_OPTIMAL) require(offset==64&&size==2*2*6*4*sizeof(float),
        "optimal buffer flush misses its segment");
    operations.emplace_back("flush");
    return failure=="flush"||(active_tiling==VK_IMAGE_TILING_OPTIMAL&&failure=="flush-optimal")?
        VK_ERROR_MEMORY_MAP_FAILED:VK_SUCCESS;
}
void vmaUnmapMemory(int,Allocation*) { operations.emplace_back("unmap"); }
void vkCmdBlitImage(VkCommandBuffer,VkImage,VkImageLayout source,VkImage,
        VkImageLayout destination,uint32_t count,VkImageBlit const* regions,VkFilter filter) {
    ++blits;operations.emplace_back("blit");
    if(active_tiling==VK_IMAGE_TILING_LINEAR) require(barriers==1,"host-write barrier missing before actual blit");
    else require(barriers==0&&stage_transitions==2,"optimal image received host barrier or lost transfer dependency");
    require(source==(active_tiling==VK_IMAGE_TILING_LINEAR?VK_IMAGE_LAYOUT_GENERAL:
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)&&destination==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&
        count==6&&filter==VK_FILTER_NEAREST,"blit layout or six cubemap slices differ");
    for(unsigned face=0;face<6;++face) require(regions[face].srcOffsets[0].y==int(face*2)&&
        regions[face].srcOffsets[1].y==int((face+1)*2)&&regions[face].dstSubresource.baseArrayLayer==face,
        "actual upload lost tall-image cubemap slicing");
}
void vkCmdCopyBufferToImage(VkCommandBuffer,VkBuffer buffer,VkImage image,VkImageLayout layout,
        uint32_t count,VkBufferImageCopy const* copy) {
    require(active_tiling==VK_IMAGE_TILING_OPTIMAL&&stage_transitions==1&&
        buffer==token<VkBuffer>(2)&&image==token<VkImage>()&&
        layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&count==1&&copy->bufferOffset==64&&
        copy->bufferRowLength==0&&copy->bufferImageHeight==0&&copy->imageExtent.width==2&&
        copy->imageExtent.height==12&&copy->imageExtent.depth==1&&
        copy->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&
        copy->imageSubresource.mipLevel==0&&copy->imageSubresource.baseArrayLayer==0&&
        copy->imageSubresource.layerCount==1,"optimal buffer copy lost packed slices or subresources");
    optimal_bytes.assign(buffer_bytes.begin()+64,buffer_bytes.begin()+64+2*2*6*4*sizeof(float));
    operations.emplace_back("copy-buffer");
}
VkResult vmaCreateImage(int,VkImageCreateInfo const*,int const*,VkImage*,int*,void*) {
    return failure=="create"?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;
}
'''

MAIN = r'''
int main(int argc,char** argv) try {
    std::signal(SIGABRT,[](int) { std::_Exit(87); });
    if(argc==2) failure=argv[1];
    check_allocation();
    require(actual_creation().initialLayout==VK_IMAGE_LAYOUT_PREINITIALIZED,
        "fresh linear image does not preserve initial host writes");
    VulkanTexture texture;
    std::vector<float> pixels(2*2*6*4,0.25f);
    for(unsigned upload=0;upload<5;++upload) {
        operations.clear();barriers=blits=transitions=stage_transitions=0;
        initially_undefined=upload==0;
        expected_old=upload&&upload!=4?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_PREINITIALIZED;
        PixelBufferDescriptor data(pixels.data(),pixels.size()*sizeof(float),
            PixelDataFormat::RGBA,PixelDataType::FLOAT);
        // A separate image must retain fresh state even after repeated uploads
        // to the first image; these resources may coexist in an unretired batch.
        VulkanTexture independent;
        auto& selected=upload==4?independent:texture;
        selected.updateImageWithBlit(data,2,2,6,0,0,0,0);
        require(barriers==1&&blits==1,"actual upload omitted one host barrier or blit");
        require(operations==std::vector<std::string>{"retain-stage","retain-texture",
            "query","map","flush","unmap","host-barrier","blit"},
            "actual upload did not retain resources and publish host writes after flushing");
    }
    auto const optimal_creation=actual_creation(VK_IMAGE_TILING_OPTIMAL);
    require(optimal_creation.initialLayout==VK_IMAGE_LAYOUT_UNDEFINED&&
        optimal_creation.tiling==VK_IMAGE_TILING_OPTIMAL,
        "optimal image lost tiling or incorrectly inherits host-write initial layout");
    active_tiling=VK_IMAGE_TILING_OPTIMAL;
    for(unsigned upload=0;upload<3;++upload) {
        operations.clear();barriers=blits=transitions=stage_transitions=0;
        initially_undefined=upload==0;
        std::fill(buffer_bytes.begin(),buffer_bytes.end(),0xAB);optimal_bytes.clear();
        for(size_t i=0;i<pixels.size();++i) pixels[i]=float(i+upload)/256;
        PixelBufferDescriptor data(pixels.data(),pixels.size()*sizeof(float),
            PixelDataFormat::RGBA,PixelDataType::FLOAT);
        texture.updateImageWithBlit(data,2,2,6,0,0,0,0);
        auto const* expected=reinterpret_cast<uint8_t const*>(pixels.data());
        require(optimal_bytes==std::vector<uint8_t>(expected,expected+pixels.size()*sizeof(float)),
            "optimal upload payload differs from independently authored source");
        require(std::all_of(buffer_bytes.begin(),buffer_bytes.begin()+64,[](uint8_t b){return b==0xAB;})&&
            std::all_of(buffer_bytes.begin()+64+pixels.size()*sizeof(float),buffer_bytes.end(),
                [](uint8_t b){return b==0xAB;}),"optimal upload overwrote adjacent segments");
        require(operations==std::vector<std::string>{"retain-stage","retain-texture","acquire-buffer",
            "flush","retain-buffer","optimal-transition","copy-buffer","optimal-transition","blit"},
            "optimal upload failed retention, flush or dependency ordering");
    }
    std::puts("PASS: actual fresh/reused linear and optimal uploads preserve layouts, bytes, dependencies and retained identities");
    return 0;
} catch(std::exception const& error) { std::fprintf(stderr,"FAIL: %s\n",error.what());return 1; }
'''


def run_layout_contract(cxx, vulkan_include, work, filament_source=None):
    filament = (filament_source or ROOT / "lib/filament").resolve()
    vulkan = filament / "filament/backend/src/vulkan"
    texture = (vulkan / "VulkanTexture.cpp").read_text()
    pool = (vulkan / "VulkanStagePool.cpp").read_text()
    body = raw_body(texture, "void VulkanTexture::updateImageWithBlit(")
    helper = "VkDeviceSize copyToLinearImage(void* mapped, PixelBufferDescriptor const& p, size_t width, size_t height, size_t depth, VkSubresourceLayout const& layout) {\n" + raw_body(texture, "VkDeviceSize copyToLinearImage(") + "\n}\n"
    prepare = "void VulkanStageImage::Resource::prepareForRead(VkCommandBuffer const commands) const noexcept {\n" + raw_body(pool, "void VulkanStageImage::Resource::prepareForRead(") + "\n}\n"
    allocation = pool.split("    VkResult const result = vmaCreateImage(", 1)[1].split("\n\n    VulkanStageImage*", 1)[0]
    allocation = "void check_allocation() { int mAllocator=0,allocInfo=0,memory=0; VkImage image{}; VkImageCreateInfo imageInfo{}; VkResult const result = vmaCreateImage(" + allocation + "\n}\n"
    match = re.search(r"VkImageCreateInfo\s+const\s+imageInfo\s*=\s*\{(.*?)\n    \};", pool, re.S)
    if not match:
        raise RuntimeError("Actual image creation initializer changed; explicit fixture review required")
    creation = match[1]
    creation = "VkImageCreateInfo actual_creation(VkImageTiling tiling=VK_IMAGE_TILING_LINEAR) { uint32_t width=2,height=12; bool const isLinear=tiling==VK_IMAGE_TILING_LINEAR; VkFormat vkformat=VK_FORMAT_R32G32B32A32_SFLOAT; const VkImageCreateInfo imageInfo={" + creation + "};return imageInfo;}\n"
    upload = "void VulkanTexture::updateImageWithBlit(PixelBufferDescriptor const& data,uint32_t width,uint32_t height,uint32_t depth,uint32_t xoffset,uint32_t yoffset,uint32_t zoffset,uint32_t miplevel) {\n" + body + "\n}\n"
    variants = [("layout-positive", creation, upload, None),
                ("layout-initial-undefined", creation.replace("fvkutils::LinearStageImageState::INITIAL_LAYOUT", "VK_IMAGE_LAYOUT_UNDEFINED"), upload, "fresh linear image does not preserve initial host writes"),
                ("layout-missing-host-barrier", creation, upload.replace("stage->prepareForRead(cmdbuf);", ""), "host-write barrier missing"),
                ("layout-blit-transfer-src", creation, upload.replace("? fvkutils::LinearStageImageState::READ_LAYOUT", "? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL"), "blit layout")]
    variants += [
        ("optimal-initial-host-layout", creation.replace(": VK_IMAGE_LAYOUT_UNDEFINED",
            ": VK_IMAGE_LAYOUT_PREINITIALIZED"), upload, "optimal image lost tiling"),
        ("optimal-uses-host-barrier", creation, upload.replace(
            "    } else {\n        // The device does not support", "    } else {\n        stage->prepareForRead(cmdbuf);\n        // The device does not support"), "optimal image used host-write barrier"),
        ("optimal-wrong-blit-layout", creation, upload.replace(
            ": fvkutils::getVkLayout(VulkanLayout::TRANSFER_SRC)", ": VK_IMAGE_LAYOUT_GENERAL"), "blit layout"),
        ("optimal-missing-copy", creation, upload.replace(
            "        vkCmdCopyBufferToImage(cmdbuf, stageSegment->buffer(), stage->image(),\n                fvkutils::getVkLayout(VulkanLayout::TRANSFER_DST), 1, &copyRegion);", ""), "optimal upload payload"),
    ]
    packed = "void adjustedMemcpy(void* mapped, PixelBufferDescriptor const& p, size_t width, size_t height, size_t depth) {\n" + raw_body(texture, "void adjustedMemcpy(") + "\n}\n"
    observations = []
    msvc = Path(cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    includes = [vulkan_include.resolve()] + [filament / path for path in
        ("filament/backend/src", "filament/backend/include", "libs/bluevk/include", "libs/utils/include", "libs/filabridge/include", "libs/math/include")]
    for name, create, actual_upload, failure in variants:
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "fixture.cpp"
        source.write_text(PREFIX + packed + helper + create + allocation + prepare + actual_upload + MAIN)
        binary = directory / ("fixture.exe" if msvc else "fixture")
        command = ([cxx, "/nologo", "/std:c++20", "/EHsc"] if msvc else [cxx, "-std=c++20"])
        command += [("/I" if msvc else "-I") + str(path) for path in includes]
        command += [str(source)] + (["/Fe:" + str(binary)] if msvc else ["-o", str(binary)])
        compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError(f"{name} did not compile; not a valid control: {compiled.stdout}{compiled.stderr}")
        executed = subprocess.run([str(binary)], cwd=directory, capture_output=True, text=True, timeout=10)
        (directory / "run.log").write_text(executed.stdout + executed.stderr)
        # The bad optimal host barrier reaches a noexcept production helper; the deliberate
        # fake-device rejection terminates there and the fixture's SIGABRT handler returns87.
        expected_exit = 87 if name == "optimal-uses-host-barrier" else (1 if failure else 0)
        if executed.returncode != expected_exit or (failure and failure not in executed.stderr):
            raise RuntimeError(f"{name} failed its specific oracle: {executed.stdout}{executed.stderr}")
        observations.append({"variant": name, "compile_argv": command,
                             "exit_code": executed.returncode, "expected_exit": expected_exit,
                             "expected_failure": failure})
        if not failure:
            for operation, reason in (("create", "vmaCreateImage failed. VkResult=-1"),
                                      ("map", "vmaMapMemory failed. VkResult=-2"),
                                      ("flush", "vmaFlushAllocation failed. VkResult=-5"),
                                      ("flush-optimal", "staging buffer vmaFlushAllocation failed. VkResult=-5")):
                failed = subprocess.run([str(binary), operation], cwd=directory,
                                        capture_output=True, text=True, timeout=10)
                (directory / (operation + "-failure.log")).write_text(failed.stdout + failed.stderr)
                if failed.returncode != 87 or reason not in failed.stderr:
                    raise RuntimeError(f"{operation} did not stop on its exact VMA error: {failed.stdout}{failed.stderr}")
                observations.append({"variant": "vma-" + operation + "-failure",
                                     "exit_code": failed.returncode, "expected_failure": reason})
    sources = [vulkan / "VulkanStagePool.cpp", vulkan / "VulkanStagePool.h",
               vulkan / "utils/LinearStageImage.h", Path(__file__).resolve()]
    return {"scope": "Complete actual upload and Resource::prepareForRead bodies, packed/padded copy helpers, actual layout state and tiling-dependent image initializer; fake commands/VMA only. Independent LINEAR/OPTIMAL bytes, adjacent segments, layouts, barriers, flush/retention ordering and isolated negatives; real fence and pool reuse require native evidence.",
            "sources": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
            "controls": observations}
