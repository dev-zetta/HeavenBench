#!/usr/bin/env python3
"""Execute actual Vulkan linear-image upload code against padded mapped memory."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from dlss_lifecycle_contract_test import raw_body

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "lib/filament/filament/backend/src/vulkan/VulkanTexture.cpp"

PREFIX = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <backend/PixelBufferDescriptor.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
namespace utils {
UTILS_NORETURN
void panic(const char* function, const char* file, int line, const char* assertion) noexcept {
    std::fprintf(stderr,"Fixture assertion failed: %s | %s | %s:%d\n",function,assertion,file,line);
    std::fflush(stderr);std::_Exit(86);
}
}
using namespace filament::backend;
'''

UPLOAD_PREFIX = r'''
namespace fvkmemory { template<class T> using resource_ptr=std::shared_ptr<T>; }
template<class H> H token() {
    alignas(16) static unsigned char value;
    if constexpr(std::is_pointer_v<H>) return reinterpret_cast<H>(&value);
    else return static_cast<H>(reinterpret_cast<uintptr_t>(&value));
}
struct Allocation {} allocation;
std::vector<uint8_t> destination;
VkSubresourceLayout supplied_layout{};
VkDeviceSize flushed_offset=0,flushed_size=0;
unsigned queries=0,maps=0,unmaps=0,flushes=0;
uint32_t expected_width=0,expected_height=0;
bool calls_valid=true;
std::vector<std::string> operations;
struct VulkanStageImage {
    struct Resource {
        VkImage image() const { return token<VkImage>(); }
        Allocation* memory() const { return &allocation; }
    };
};
struct StagePool {
    fvkmemory::resource_ptr<VulkanStageImage::Resource> acquireImage(
            PixelDataFormat,PixelDataType,uint32_t width,uint32_t height) {
        calls_valid=calls_valid&&width==expected_width&&height==expected_height;
        return std::make_shared<VulkanStageImage::Resource>();
    }
};
struct State { StagePool mStagePool; int mAllocator=19; VkDevice mDevice=token<VkDevice>(); };
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device,VkImage image,
        const VkImageSubresource* subresource,VkSubresourceLayout* layout) {
    ++queries;operations.emplace_back("query");
    calls_valid=calls_valid&&device==token<VkDevice>()&&image==token<VkImage>()&&
        subresource->aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&subresource->mipLevel==0&&
        subresource->arrayLayer==0;
    *layout=supplied_layout;
}
VkResult vmaMapMemory(int allocator,Allocation* memory,void** mapped) {
    ++maps;operations.emplace_back("map");
    calls_valid=calls_valid&&allocator==19&&memory==&allocation;
    *mapped=destination.data();return VK_SUCCESS;
}
void vmaUnmapMemory(int allocator,Allocation* memory) {
    ++unmaps;operations.emplace_back("unmap");
    calls_valid=calls_valid&&allocator==19&&memory==&allocation;
}
VkResult vmaFlushAllocation(int allocator,Allocation* memory,VkDeviceSize offset,VkDeviceSize size) {
    ++flushes;operations.emplace_back("flush");
    calls_valid=calls_valid&&allocator==19&&memory==&allocation;
    flushed_offset=offset;flushed_size=size;return VK_SUCCESS;
}
void upload(PixelBufferDescriptor const& data,uint32_t width,uint32_t height,uint32_t depth,
        SamplerType target,uint32_t zoffset=0) {
    State state;auto* mState=&state;
'''

TEST = r'''
}
struct Case {
    const char* name;
    PixelDataFormat format;
    PixelDataType type;
    unsigned pixel_size,width,height,depth,stride,left,top,source_height,alignment;
    VkDeviceSize pitch,offset;
    SamplerType target;
};
uint8_t pixel(unsigned z,unsigned y,unsigned x,unsigned channel) {
    return static_cast<uint8_t>((z*61+y*13+x*5+channel)%251);
}
bool run(const Case& c) {
    const size_t source_row=((c.stride*c.pixel_size+c.alignment-1)/c.alignment)*c.alignment;
    std::vector<uint8_t> source(source_row*c.source_height*c.depth,0xD3);
    for(unsigned z=0;z<c.depth;++z) for(unsigned y=0;y<c.source_height;++y)
        for(unsigned x=0;x<c.stride;++x) for(unsigned channel=0;channel<c.pixel_size;++channel)
            source[z*source_row*c.source_height+y*source_row+x*c.pixel_size+channel]=pixel(z,y,x,channel);
    const auto preserved_source=source;
    PixelBufferDescriptor data(source.data(),source.size(),c.format,c.type,
        static_cast<uint8_t>(c.alignment),c.left,c.top,c.stride);
    if(PixelBufferDescriptor::computeDataSize(c.format,c.type,1,1,1)!=c.pixel_size) return false;
    supplied_layout={c.offset,c.pitch*c.height*c.depth,c.pitch,0x123400,0x567800};
    destination.assign(static_cast<size_t>(c.offset+supplied_layout.size+47),0xA5);
    auto expected=destination;
    for(unsigned z=0;z<c.depth;++z) for(unsigned y=0;y<c.height;++y)
        for(unsigned x=0;x<c.width;++x) for(unsigned channel=0;channel<c.pixel_size;++channel)
            expected[static_cast<size_t>(c.offset+(z*c.height+y)*c.pitch+x*c.pixel_size+channel)]=
                pixel(z,y+c.top,x+c.left,channel);
    queries=maps=unmaps=flushes=0;calls_valid=true;operations.clear();
    expected_width=c.width;expected_height=c.height*c.depth;
    flushed_offset=flushed_size=0;
    upload(data,c.width,c.height,c.depth,c.target);
    if(destination!=expected) {
        std::fprintf(stderr,"FAIL: linear staging pixels or padding differ: %s\n",c.name);return false;
    }
    const VkDeviceSize expected_flush=(VkDeviceSize(c.height)*c.depth-1)*c.pitch+
        VkDeviceSize(c.width)*c.pixel_size;
    if(flushed_offset!=c.offset||flushed_size!=expected_flush) {
        std::fprintf(stderr,"FAIL: linear staging flush excludes written rows: %s\n",c.name);return false;
    }
    if(!calls_valid||queries!=1||maps!=1||unmaps!=1||flushes!=1||
            operations!=std::vector<std::string>{"query","map","flush","unmap"}) {
        std::fprintf(stderr,"FAIL: linear staging layout query/map/flush contract: %s\n",c.name);return false;
    }
    // The existing buffer-upload helper still packs the same source into tight rows.
    std::vector<uint8_t> tight(c.width*c.height*c.depth*c.pixel_size+31,0xB7),tight_expected=tight;
    for(unsigned z=0;z<c.depth;++z) for(unsigned y=0;y<c.height;++y)
        for(unsigned x=0;x<c.width;++x) for(unsigned channel=0;channel<c.pixel_size;++channel)
            tight_expected[((z*c.height+y)*c.width+x)*c.pixel_size+channel]=
                pixel(z,y+c.top,x+c.left,channel);
    adjustedMemcpy(tight.data(),data,c.width,c.height,c.depth);
    if(tight!=tight_expected||source!=preserved_source) {
        std::fprintf(stderr,"FAIL: existing tight buffer or source bytes changed: %s\n",c.name);return false;
    }
    return true;
}
int main(int argc,char**) try {
    if(argc==2) {
        assert_invariant(false);
        std::fputs("FAIL: actual Filament assertion disabled\n",stderr);return 2;
    }
    const Case cases[]={
        {"padded-float",PixelDataFormat::RGBA,PixelDataType::FLOAT,16,8,8,1,8,0,0,8,1,256,0,SamplerType::SAMPLER_2D},
        {"tight-float",PixelDataFormat::RGBA,PixelDataType::FLOAT,16,8,8,1,8,0,0,8,1,128,0,SamplerType::SAMPLER_2D},
        {"offset-padded-float",PixelDataFormat::RGBA,PixelDataType::FLOAT,16,8,8,1,8,0,0,8,1,256,96,SamplerType::SAMPLER_2D},
        {"cropped-aligned-volume",PixelDataFormat::RGB,PixelDataType::UBYTE,3,3,2,3,7,2,1,5,8,64,31,SamplerType::SAMPLER_3D},
        {"cropped-aligned-array",PixelDataFormat::RG,PixelDataType::HALF,4,5,3,4,9,1,2,6,8,128,64,SamplerType::SAMPLER_2D_ARRAY},
        {"cropped-cubemap",PixelDataFormat::RGBA,PixelDataType::FLOAT,16,2,2,6,4,1,1,4,4,64,17,SamplerType::SAMPLER_CUBEMAP},
        {"single-row-offset",PixelDataFormat::R,PixelDataType::UBYTE,1,1,1,1,1,0,0,1,1,256,257,SamplerType::SAMPLER_2D},
        {"tight-volume",PixelDataFormat::RGBA,PixelDataType::FLOAT,16,4,3,2,4,0,0,3,1,64,0,SamplerType::SAMPLER_3D},
    };
    for(const auto& c:cases) if(!run(c)) return 1;
    std::puts("PASS: eight actual staging uploads preserve pixels, offset/padding, source crops/slices, flush span and tight buffers");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"Fixture exception: %s\n",error.what());return 3;
} catch(...) { std::fputs("Fixture exception\n",stderr);return 3; }
'''


def upload_body(text):
    body = raw_body(text, "void VulkanTexture::updateImageWithBlit(")
    boundary = "    VulkanCommandBuffer& commands = mState->mCommands->get();"
    if body.count(boundary) != 1:
        raise RuntimeError("Actual staging boundary changed; explicit fixture review required")
    return body.split(boundary)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--vulkan-include", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--original-source", type=Path,
                        help="Optional preserved pre-fix source for the exact old-source negative")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    text = SOURCE.read_text()
    packed = "void adjustedMemcpy(void* mapped, PixelBufferDescriptor const& p, size_t width, size_t height, size_t depth) {\n" + raw_body(text, "void adjustedMemcpy(") + "\n}\n"
    linear = "VkDeviceSize copyToLinearImage(void* mapped, PixelBufferDescriptor const& p, size_t width, size_t height, size_t depth, VkSubresourceLayout const& layout) {\n" + raw_body(text, "VkDeviceSize copyToLinearImage(") + "\n}\n"
    upload = upload_body(text)
    copy_call = "VkDeviceSize const writeSize = copyToLinearImage(mapped, data, width, height, depth, layout);"
    flush_call = "vmaFlushAllocation(mState->mAllocator, stage->memory(), layout.offset, writeSize);"
    if upload.count(copy_call) != 1 or upload.count(flush_call) != 1:
        raise RuntimeError("Actual linear staging bindings changed; explicit fixture review required")
    tight_size = "PixelBufferDescriptor::computeDataSize(data.format, data.type, 1, 1, 1) * width * height * depth"
    old_copy = upload.replace(copy_call, "size_t const writeSize = " + tight_size + ";\n    adjustedMemcpy(mapped, data, width, height, depth);")
    variants = [("positive", linear, upload, None),
                ("old-tight-copy", linear, old_copy, "linear staging pixels or padding differ"),
                ("missing-offset", linear.replace("static_cast<uint8_t*>(mapped) + layout.offset", "static_cast<uint8_t*>(mapped)"), upload, "linear staging pixels or padding differ"),
                ("tight-flush", linear, upload.replace(flush_call, "vmaFlushAllocation(mState->mAllocator, stage->memory(), layout.offset, " + tight_size + ");"), "linear staging flush excludes written rows")]
    if args.original_source:
        variants.append(("actual-original-source", linear, upload_body(args.original_source.read_text()), "linear staging pixels or padding differ"))
    observations = []
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    includes = [args.vulkan_include.resolve()] + [ROOT / "lib/filament" / p for p in
        ("filament/backend/include", "libs/utils/include", "libs/filabridge/include", "libs/math/include")]
    for name, helper, staging, failure in variants:
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "fixture.cpp"
        source.write_text(PREFIX + packed + helper + UPLOAD_PREFIX + staging + TEST)
        binary = directory / ("fixture.exe" if msvc else "fixture")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc"] if msvc else
                   [args.cxx, "-std=c++20"])
        command += [("/I" if msvc else "-I") + str(p) for p in includes]
        command += [str(source)] + (["/Fe:" + str(binary)] if msvc else ["-o", str(binary)])
        compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError(f"{name} did not compile; not a valid control: {compiled.stdout}{compiled.stderr}")
        assertion = subprocess.run([str(binary), "assertion-probe"], cwd=directory,
                                   capture_output=True, text=True, timeout=10)
        (directory / "assertion.log").write_text(assertion.stdout + assertion.stderr)
        if assertion.returncode != 86 or "Fixture assertion failed:" not in assertion.stderr:
            raise RuntimeError(f"{name} lost actual active assertions")
        executed = subprocess.run([str(binary)], cwd=directory, capture_output=True, text=True, timeout=10)
        (directory / "run.log").write_text(executed.stdout + executed.stderr)
        if executed.returncode != (1 if failure else 0) or (failure and failure not in executed.stderr):
            raise RuntimeError(f"{name} failed its specific oracle: {executed.stdout}{executed.stderr}")
        observations.append({"variant": name, "compile_argv": command,
                             "exit_code": executed.returncode, "expected_failure": failure,
                             "assertion_probe_exit": assertion.returncode})
    sources = [SOURCE, ROOT / "tests/filament_linear_staging_test.py",
               ROOT / "lib/filament/filament/backend/include/backend/PixelBufferDescriptor.h"]
    if args.original_source:
        sources.append(args.original_source.resolve())
    receipt = {"passed": True, "gpu_execution": False,
               "scope": "Actual PixelBufferDescriptor, existing packed copy, linear copy and blit staging body; fake layout/map/flush only. Native driver row pitch and corrected readback remain separate.",
               "sources": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
               "controls": observations}
    (work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
