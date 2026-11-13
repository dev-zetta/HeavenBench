#!/usr/bin/env python3
"""Exercise the actual Intel driver gate, cached getter and image-creation policy on the host.

Only Vulkan format-property queries and the surrounding Context storage/logging are faked.
Production policy/getter/texture block/aspect classifier are copied unchanged or included directly.
Independent controls verify exact identity, once-only logging, preserved flags and fail-clear shape
checks. No native Vulkan call, allocation, driver initialization or GPU workload is executed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

try:
    import resource
except ImportError:  # Windows has no POSIX resource module or preexec_fn support.
    resource = None

ROOT = Path(__file__).resolve().parents[1]
VULKAN = ROOT / 'lib/filament/filament/backend/src/vulkan'
POLICY = VULKAN / 'utils/DriverWorkarounds.h'
CONTEXT = VULKAN / 'VulkanContext.cpp'
TEXTURE = VULKAN / 'VulkanTexture.cpp'
IMAGE = VULKAN / 'utils/Image.cpp'

PREFIX = r'''
#include "vulkan/utils/DriverWorkarounds.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace filament::backend {
std::atomic<unsigned> warnings{0};
struct Log { Log() { ++warnings; } template<class T> Log& operator<<(T const&) { return *this; } };
#define FVK_LOGW Log{}
#define FVK_LOGE std::cerr
struct VulkanContext {
    bool mDriverPropertiesSupported=true;
    VkPhysicalDeviceProperties2 mPhysicalDeviceProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceDriverProperties mDriverProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    mutable std::once_flag mColorImageAliasPolicyOnce;
    mutable bool mRequiresColorImageAliasWorkaround=false;
    bool requiresColorImageAliasWorkaround() const noexcept;
};
void require(bool value, char const* diagnostic) {
    if (!value) { std::cerr << "FAIL: " << diagnostic << '\n'; std::exit(42); }
}
std::unique_ptr<VulkanContext> context() {
    auto ctx=std::make_unique<VulkanContext>();
    auto& p=ctx->mPhysicalDeviceProperties.properties;
    p.vendorID=0x8086; p.deviceID=0xe223; p.driverVersion=VK_MAKE_VERSION(26,0,8);
    ctx->mDriverProperties.driverID=VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA;
    return ctx;
}
unsigned queries=0;
VkImageCreateFlags expectedFlags=0;
VkResult queryResult=VK_SUCCESS;
VkImageFormatProperties limits{{4096,4096,2048},16,2048,VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,1ull<<32};
VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physical, VkFormat format,
        VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties* properties) {
    require(physical==VK_NULL_HANDLE && (format==VK_FORMAT_R8G8B8A8_UNORM || format==VK_FORMAT_R16G16B16A16_SFLOAT), "exact format query input");
    require(type==VK_IMAGE_TYPE_2D && tiling==VK_IMAGE_TILING_OPTIMAL &&
            usage==(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), "unchanged type/tiling/usage");
    require(flags==expectedFlags, "preserved original flags in capability query");
    ++queries; *properties=limits; return queryResult;
}
namespace fvkutils {
'''
MIDDLE = r'''
} // fvkutils
void apply(VulkanContext const& context, VkImageCreateInfo& imageInfo) {
    VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
    VkFormat vkFormat=imageInfo.format;
'''
ORACLE = r'''
}
VkImageCreateInfo image(VkImageCreateFlags flags=0) {
    return {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,nullptr,flags,VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM,{64,32,1},6,6,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE,0,nullptr,VK_IMAGE_LAYOUT_UNDEFINED};
}
} // filament::backend
int main(int argc,char** argv) {
    using namespace filament::backend;
    if(argc==2) {
        auto ctx=context(); auto info=image(); expectedFlags=VK_IMAGE_CREATE_ALIAS_BIT;
        std::string mode=argv[1];
        if(mode=="unsupported") queryResult=VK_ERROR_FORMAT_NOT_SUPPORTED;
        else if(mode=="extent") limits.maxExtent.width=63;
        else if(mode=="mips") limits.maxMipLevels=5;
        else if(mode=="layers") limits.maxArrayLayers=5;
        else if(mode=="samples") limits.sampleCounts=VK_SAMPLE_COUNT_4_BIT;
        else return 44;
        apply(*ctx,info);
        std::cerr << "FAIL: unsupported alias image accepted\n"; return 42;
    }
    auto exact=context();
    auto const device=exact->mPhysicalDeviceProperties.properties;
    auto const driver=exact->mDriverProperties;
    require(fvkutils::requiresIntelMesaColorImageAliasWorkaround(true,device,driver), "exact driver identity enabled");
    require(!fvkutils::requiresIntelMesaColorImageAliasWorkaround(false,device,driver), "missing driver properties excluded");
    for(auto id:std::array<VkDriverId,3>{VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,VK_DRIVER_ID_MESA_RADV,VK_DRIVER_ID_NVIDIA_PROPRIETARY}) {
        auto p=driver; p.driverID=id;
        require(!fvkutils::requiresIntelMesaColorImageAliasWorkaround(true,device,p), "other driver excluded");
    }
    for(auto vendor:std::array<uint32_t,3>{0,0x1002,0x10de}) {
        auto p=device; p.vendorID=vendor;
        require(!fvkutils::requiresIntelMesaColorImageAliasWorkaround(true,p,driver), "other vendor excluded");
    }
    for(auto id:std::array<uint32_t,4>{0,0xe222,0xe224,UINT32_MAX}) {
        auto p=device; p.deviceID=id;
        require(!fvkutils::requiresIntelMesaColorImageAliasWorkaround(true,p,driver), "other device excluded");
    }
    for(auto version:std::array<uint32_t,7>{0,VK_MAKE_VERSION(25,3,8),VK_MAKE_VERSION(26,0,7),
            VK_MAKE_VERSION(26,0,9),VK_MAKE_VERSION(26,1,0),VK_MAKE_VERSION(27,0,8),UINT32_MAX}) {
        auto p=device; p.driverVersion=version;
        require(!fvkutils::requiresIntelMesaColorImageAliasWorkaround(true,p,driver), "other version excluded");
    }
    warnings=0;
    std::vector<std::thread> readers;
    for(unsigned i=0;i<8;++i) readers.emplace_back([&] {
        for(unsigned n=0;n<64;++n) require(exact->requiresColorImageAliasWorkaround(), "cached concurrent policy enabled");
    });
    for(auto& reader:readers) reader.join();
    require(warnings==1, "active policy logs exactly once per context");
    auto excluded=context(); excluded->mDriverPropertiesSupported=false;
    for(unsigned i=0;i<8;++i) require(!excluded->requiresColorImageAliasWorkaround(), "cached unavailable policy disabled");
    require(warnings==1, "excluded policy does not warn");
    queries=0;
    for(auto flags:std::array<VkImageCreateFlags,5>{0,VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            VK_IMAGE_CREATE_PROTECTED_BIT,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_PROTECTED_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT}) {
        auto info=image(flags); auto const original=info;
        expectedFlags=flags | VK_IMAGE_CREATE_ALIAS_BIT;
        apply(*exact,info);
        require(info.flags==expectedFlags, "original image flags retained");
        require(info.sType==original.sType && info.pNext==original.pNext &&
                info.imageType==original.imageType && info.format==original.format &&
                info.extent.width==original.extent.width && info.extent.height==original.extent.height &&
                info.extent.depth==original.extent.depth && info.mipLevels==original.mipLevels &&
                info.arrayLayers==original.arrayLayers && info.samples==original.samples &&
                info.tiling==original.tiling && info.usage==original.usage &&
                info.sharingMode==original.sharingMode &&
                info.queueFamilyIndexCount==original.queueFamilyIndexCount &&
                info.pQueueFamilyIndices==original.pQueueFamilyIndices &&
                info.initialLayout==original.initialLayout, "image shape/usage/sample parameters retained");
    }
    require(queries==5, "one exact capability query per affected allocation");
    // HDR / FLOAT destination retains format. No quality, mips or sample substitution.
    auto fp16=image(); fp16.format=VK_FORMAT_R16G16B16A16_SFLOAT; expectedFlags=VK_IMAGE_CREATE_ALIAS_BIT;
    apply(*exact,fp16); require(fp16.format==VK_FORMAT_R16G16B16A16_SFLOAT, "HDR format preserved");
    auto previousQueries=queries;
    for(auto format:std::array<VkFormat,4>{VK_FORMAT_D32_SFLOAT,VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM,VK_FORMAT_S8_UINT}) {
        auto info=image(VK_IMAGE_CREATE_PROTECTED_BIT); info.format=format;
        apply(*exact,info); require(info.flags==VK_IMAGE_CREATE_PROTECTED_BIT, "depth/stencil unchanged");
    }
    auto msaa=image(); msaa.samples=VK_SAMPLE_COUNT_4_BIT; apply(*exact,msaa);
    require(msaa.flags==0 && msaa.samples==VK_SAMPLE_COUNT_4_BIT, "MSAA unchanged");
    auto other=image(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT); apply(*excluded,other);
    require(other.flags==VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT && queries==previousQueries, "unaffected allocations not queried or modified");
    require(warnings==1, "allocation loop does not repeat warning");
    std::cout << "PASS: exact driver gate, cached concurrent warning, original flags/format/shape and unaffected allocations\n";
}
'''


def balanced_block(source, start):
    first = source.index('{', start)
    depth = 0
    for end in range(first, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError('Unbalanced production source')


def disable_core_files():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(args, directory, name, header, getter, aspect, block, modes, expected_diagnostic):
    out = directory / name
    out.mkdir(exist_ok=True)
    # A shadow include tree changes exactly the gate for historical/specific negatives.
    shadow = out / 'include/vulkan/utils'
    shadow.mkdir(parents=True, exist_ok=True)
    (shadow / 'DriverWorkarounds.h').write_text(header)
    source = out / 'actual-intel-alias.cpp'
    source.write_text(PREFIX + aspect + '\n}\n' + getter + '\nnamespace fvkutils {\n' + MIDDLE + block + ORACLE)
    exe = out / ('contract.exe' if args.msvc else 'contract')
    includes = [out / 'include', VULKAN.parent, ROOT / 'lib/filament/libs/bluevk/include', ROOT / 'lib/filament/libs/utils/include']
    command = ([args.cxx, '/nologo', '/std:c++20', '/EHsc', str(source), '/Fe:' + str(exe), '/Fo:' + str(out / 'contract.obj')]
               + ['/I' + str(path) for path in includes] if args.msvc else
               [args.cxx, '-std=c++20', '-pthread', str(source), '-o', str(exe)] + ['-I' + str(path) for path in includes])
    compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (out / 'compile.log').write_text(compiled.stdout)
    if compiled.returncode:
        raise ValueError(name + ' invalid compile control: ' + compiled.stdout)
    receipts = []
    for mode in modes:
        result = subprocess.run([str(exe)] + ([mode] if mode else []), text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15,
                preexec_fn=disable_core_files if os.name == 'posix' and resource is not None else None)
        (out / ((mode or 'main') + '.log')).write_text(result.stdout)
        success = result.returncode == 0 if name == 'production' and not mode else result.returncode != 0
        if not success or expected_diagnostic not in result.stdout:
            raise ValueError(f'{name}/{mode} missed independent oracle: {result.returncode}\n{result.stdout}')
        receipts.append({'mode': mode or 'main', 'exit_code': result.returncode, 'expected_diagnostic': expected_diagnostic, 'passed': True})
    return {'name': name, 'runs': receipts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', required=True)
    parser.add_argument('--work-dir', required=True, type=Path)
    args = parser.parse_args()
    args.msvc = Path(args.cxx).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    args.work_dir.mkdir(parents=True, exist_ok=True)
    header = POLICY.read_text()
    getter = balanced_block(CONTEXT.read_text(), CONTEXT.read_text().index('bool VulkanContext::requiresColorImageAliasWorkaround()'))
    aspect = balanced_block(IMAGE.read_text(), IMAGE.read_text().index('VkImageAspectFlags getImageAspect('))
    texture = TEXTURE.read_text()
    block = balanced_block(texture, texture.index('if (imageInfo.samples == VK_SAMPLE_COUNT_1_BIT &&'))
    version = 'device.driverVersion == VK_MAKE_VERSION(26, 0, 8)'
    flags = 'imageInfo.flags | VK_IMAGE_CREATE_ALIAS_BIT'
    if header.count(version) != 1 or block.count(flags) != 1:
        raise ValueError('Production policy changed; explicit source-contract review required')
    records = [run(args, args.work_dir, 'production', header, getter, aspect, block, [''], 'PASS: exact driver gate')]
    records.append(run(args, args.work_dir, 'unsupported-image', header, getter, aspect, block,
            ['unsupported', 'extent', 'mips', 'layers', 'samples'], 'Intel Mesa CCS workaround cannot support the required image'))
    records.append(run(args, args.work_dir, 'negative-unbounded-version', header.replace(version, 'true'), getter, aspect, block,
            [''], 'FAIL: other version excluded'))
    records.append(run(args, args.work_dir, 'negative-replaced-flags', header, getter, aspect, block.replace(flags, 'VK_IMAGE_CREATE_ALIAS_BIT'),
            [''], 'FAIL: preserved original flags in capability query'))
    receipt = {'schema': 1, 'sources_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (POLICY, CONTEXT, TEXTURE, IMAGE, Path(__file__))},
               'compiler': args.cxx, 'cases': records, 'native_Vulkan_calls': 0,
               'limits': 'Actual source host policy and error-path contract; native alias/CCS efficacy remains separately measured on exact hardware/driver.'}
    (args.work_dir / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Intel color-alias contract PASS: actual policy/getter/image block, independent shape guards and specific negative controls; no GPU execution')


if __name__ == '__main__':
    main()
