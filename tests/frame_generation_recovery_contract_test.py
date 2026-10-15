#!/usr/bin/env python3
"""Execute actual FG replacement recovery methods against native-bundle failures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from dlss_lifecycle_contract_test import raw_body

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "benchmark/src/rhi/frame_generation/frame_generation_platform.cpp"

PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
using VkImage = uintptr_t;
constexpr VkImage VK_NULL_HANDLE = 0;
constexpr int VK_FORMAT_UNDEFINED = 0;
struct VkExtent2D { uint32_t width=0,height=0; };
using SwapChainPtr = const void*;
void* window() { static int identity; return &identity; }
SwapChainPtr surface() { static int identity; return &identity; }
SwapChainPtr other_surface() { static int identity; return &identity; }
struct Base {
    struct SwapChainBundle {
        std::vector<VkImage> colors;
        int colorFormat=VK_FORMAT_UNDEFINED;
        VkExtent2D extent;
    };
    static SwapChainBundle next_bundle;
    static bool returns_handle;
    static SwapChainPtr createSwapChain(void*,uint64_t,VkExtent2D) {
        return returns_handle?surface():nullptr;
    }
    static SwapChainBundle getSwapChainBundle(SwapChainPtr) { return next_bundle; }
};
Base::SwapChainBundle Base::next_bundle;
bool Base::returns_handle=true;
bool native_swapchain_images_ready(const Base::SwapChainBundle& bundle) {
'''
MIDDLE = r'''
}
struct Impl {
    struct Chain { Base::SwapChainBundle bundle; bool surface=false,broken=false; uint64_t trace_identity=0; };
    std::unordered_map<SwapChainPtr,Chain> chains;
    uint64_t trace_chain_sequence=0;
    std::atomic<bool> generation_blocked{true};
    struct History { unsigned epoch=0; void invalidate() { ++epoch; } } history;
    std::mutex status_mutex;
    struct Status { bool available=false; std::string reason="original native failure"; } status;
    void unblock_after_recreation() {
'''
CLASS = r'''
    }
};
struct FrameGenerationPlatform {
    using SwapChainPtr=::SwapChainPtr;
    std::unique_ptr<Impl> impl_=std::make_unique<Impl>();
    SwapChainPtr createSwapChain(void* nativeWindow,uint64_t flags,VkExtent2D extent);
};
FrameGenerationPlatform::SwapChainPtr FrameGenerationPlatform::createSwapChain(
        void* nativeWindow,uint64_t flags,VkExtent2D extent) {
'''
TEST = r'''
}
bool check(bool condition,const char* message) {
    if(!condition) std::fprintf(stderr,"FG replacement recovery: FAIL: %s\n",message);
    return condition;
}
Base::SwapChainBundle usable() { return {{1,2},37,{64,64}}; }
bool replacement(const Base::SwapChainBundle& bundle,bool should_unblock,bool headless=false,bool other_broken=false) {
    Base::next_bundle=bundle;
    FrameGenerationPlatform platform;
    if(other_broken) platform.impl_->chains[other_surface()].broken=true;
    const auto handle=platform.createSwapChain(headless?nullptr:window(),0,headless?VkExtent2D{64,64}:VkExtent2D{});
    bool ok=check(handle!=nullptr,"fixture did not reproduce nonnull upstream object");
    ok=check(platform.impl_->generation_blocked.load()!=should_unblock,
             "Replacement without usable native images cleared broken state")&&ok;
    if(should_unblock) {
        ok=check(platform.impl_->history.epoch==1&&!platform.impl_->status.available,
                 "usable replacement failed fresh-history/lazy-initialization contract")&&ok;
    } else {
        ok=check(platform.impl_->history.epoch==0&&platform.impl_->status.reason=="original native failure",
                 "failed replacement discarded original reason or history")&&ok;
    }
    return ok;
}
int main() try {
    bool ok=true;
    auto bundle=usable();bundle.colors.clear();ok=replacement(bundle,false)&&ok;
    bundle=usable();bundle.extent.width=0;ok=replacement(bundle,false)&&ok;
    bundle=usable();bundle.extent.height=0;ok=replacement(bundle,false)&&ok;
    bundle=usable();bundle.colorFormat=VK_FORMAT_UNDEFINED;ok=replacement(bundle,false)&&ok;
    bundle=usable();bundle.colors={VK_NULL_HANDLE};ok=replacement(bundle,false)&&ok;
    bundle=usable();bundle.colors={1,VK_NULL_HANDLE};ok=replacement(bundle,false)&&ok;
    ok=replacement(usable(),true)&&ok;
    ok=replacement(usable(),false,true)&&ok;
    ok=replacement(usable(),false,false,true)&&ok;
    Base::returns_handle=false;
    FrameGenerationPlatform failed;
    ok=check(!failed.createSwapChain(window(),0,{})&&failed.impl_->generation_blocked.load()&&
             failed.impl_->status.reason=="original native failure"&&failed.impl_->history.epoch==0,
             "null replacement cleared broken state")&&ok;
    if(!ok)return 1;
    std::puts("FG replacement recovery: PASS (actual methods, unusable bundles, lazy initialization, headless/other broken chains)");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FG replacement recovery: ERROR: %s\n",error.what());return 2;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    args.work_dir = args.work_dir.resolve()
    text = SOURCE.read_text()
    ready = raw_body(text, "bool native_swapchain_images_ready(")
    unblock = raw_body(text, "void unblock_after_recreation()")
    create = raw_body(text, "FrameGenerationPlatform::SwapChainPtr FrameGenerationPlatform::createSwapChain(")
    guard = "chain.surface && native_swapchain_images_ready(chain.bundle)"
    if create.count(guard) != 1:
        raise RuntimeError("Actual replacement guard changed; fixture needs explicit review")
    observations = []
    for name, body, expected in (
        ("positive", create, None),
        ("unguarded-nonnull-object", create.replace(guard, "chain.surface"),
         "Replacement without usable native images cleared broken state"),
    ):
        work = args.work_dir / name
        work.mkdir(parents=True, exist_ok=True)
        source = work / "fixture.cpp"
        source.write_text(PREFIX + ready + MIDDLE + unblock + CLASS + body + TEST)
        msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
        output = work / ("fixture.exe" if msvc else "fixture")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", str(source), f"/Fe:{output}"]
                   if msvc else [args.cxx, "-std=c++20", "-pthread", str(source), "-o", str(output)])
        compiled = subprocess.run(command, cwd=work, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=60)
        (work / "compile.log").write_text(compiled.stdout)
        if compiled.returncode:
            raise RuntimeError(f"{name} failed compilation; not a valid control")
        executed = subprocess.run([str(output)], cwd=work, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, timeout=15)
        (work / "run.log").write_text(executed.stdout)
        passed = executed.returncode == 0 if expected is None else executed.returncode == 1 and expected in executed.stdout
        if not passed:
            raise RuntimeError(f"{name} failed its specific expected result: {executed.stdout}")
        observations.append({"variant": name, "argv": command, "exit_code": executed.returncode,
                             "expected_failure": expected, "passed": True})
    receipt = {"passed": True, "native_gpu_execution": False, "production_source": str(SOURCE),
               "production_source_sha256": hashlib.sha256(text.encode()).hexdigest(),
               "extracted_body_sha256": {name: hashlib.sha256(body.encode()).hexdigest()
                                         for name, body in (("ready", ready), ("unblock", unblock), ("create", create))},
               "variants": observations,
               "scope": "Actual replacement coordinator/helper bodies against native bundle states; no Vulkan device execution"}
    (args.work_dir / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
