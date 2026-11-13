#!/usr/bin/env python3
"""Compile actual Vulkan command wait/submit methods against independent native result controls.

Only surrounding driver objects are faked. Method bodies are copied verbatim from the production
source, with their identities retained. Historical ignored-error controls remove just the guard.
No Vulkan loader, native queue, GPU work, or device wait is executed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "lib/filament/filament/backend/src/vulkan/VulkanCommands.cpp"
SHIM = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#define UTILS_UNLIKELY(x) (x)
#define FVK_ENABLED(...) 0
#define FVK_LOGE std::cerr
namespace filament::backend {
namespace fvkmemory { template<class T> using resource_ptr = std::shared_ptr<T>; }
template<class T> T fake(uintptr_t value) { return reinterpret_cast<T>(value); }
VkResult waitStatus=VK_SUCCESS, submitStatus=VK_SUCCESS, endStatus=VK_SUCCESS;
unsigned waits=0, submits=0, ends=0, marks=0, updates=0;
void require(bool condition) { if(!condition) std::exit(41); }
VkResult vkWaitForFences(VkDevice, uint32_t count, VkFence const* fences, VkBool32 all, uint64_t timeout) {
    require(count==2 && fences[0]==fake<VkFence>(4) && fences[1]==fake<VkFence>(5) && all && timeout==UINT64_MAX);
    ++waits; return waitStatus;
}
VkResult vkEndCommandBuffer(VkCommandBuffer commands) { require(commands==fake<VkCommandBuffer>(1)); ++ends; return endStatus; }
VkResult vkQueueSubmit(VkQueue queue, uint32_t count, VkSubmitInfo const* info, VkFence fence) {
    require(queue==fake<VkQueue>(2) && count==1 && info->commandBufferCount==1 &&
        *info->pCommandBuffers==fake<VkCommandBuffer>(1) && fence==fake<VkFence>(4));
    require(marks==0); ++submits; return submitStatus;
}
struct VulkanSemaphore { VkSemaphore getVkSemaphore() const { return fake<VkSemaphore>(3); } };
struct FenceState { void markSubmitted() { ++marks; } };
template<class T> struct List : std::vector<T> {
    uint32_t size() const { return static_cast<uint32_t>(std::vector<T>::size()); }
};
struct VulkanCommandBuffer {
    unsigned mMarkerCount=0;
    bool mIsProtected=false;
    VkCommandBuffer mBuffer=fake<VkCommandBuffer>(1);
    VkQueue mQueue=fake<VkQueue>(2);
    std::shared_ptr<VulkanSemaphore> mSubmission=std::make_shared<VulkanSemaphore>();
    std::shared_ptr<FenceState> mFenceStatus=std::make_shared<FenceState>();
    List<VkSemaphore> mWaitSemaphores;
    List<VkPipelineStageFlags> mWaitSemaphoreStages;
    VkFence getVkFence() const { return fake<VkFence>(4); }
    void popMarker() { --mMarkerCount; }
    fvkmemory::resource_ptr<VulkanSemaphore> submit();
};
struct Submitted {
    bool active=true;
    template<class F> void forEachSetBit(F function) const { if(active) { function(0); function(1); } }
};
struct Buffer { VkFence fence; VkFence getVkFence() const { return fence; } };
struct CommandBufferPool {
    static constexpr unsigned CAPACITY=3;
    VkDevice mDevice=fake<VkDevice>(6);
    Submitted mSubmitted;
    std::array<std::unique_ptr<Buffer>,2> mBuffers{
        std::make_unique<Buffer>(Buffer{fake<VkFence>(4)}), std::make_unique<Buffer>(Buffer{fake<VkFence>(5)})};
    void update() { ++updates; }
    void wait();
};
'''
ORACLE = r'''
} // namespace filament::backend
int main(int argc,char** argv) {
    using namespace filament::backend;
    const std::string mode=argc==2 ? argv[1] : "success";
    if(mode.starts_with("wait-")) {
        if(mode=="wait-lost") waitStatus=VK_ERROR_DEVICE_LOST;
        else if(mode=="wait-oom") waitStatus=VK_ERROR_OUT_OF_HOST_MEMORY;
        else if(mode=="wait-timeout") waitStatus=VK_TIMEOUT;
        else if(mode=="wait-unknown") waitStatus=VK_ERROR_UNKNOWN;
        else return 42;
        CommandBufferPool pool; pool.wait();
        std::cerr<<"ERROR: command wait returned after failure; updates="<<updates<<"\n";
        return 23;
    }
    if(mode.starts_with("submit-") || mode=="end-failed") {
        if(mode=="submit-lost") submitStatus=VK_ERROR_DEVICE_LOST;
        else if(mode=="submit-oom") submitStatus=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        else if(mode=="submit-unknown") submitStatus=VK_ERROR_UNKNOWN;
        else if(mode=="end-failed") endStatus=VK_ERROR_OUT_OF_HOST_MEMORY;
        else return 42;
        VulkanCommandBuffer commands; commands.submit();
        std::cerr<<"ERROR: command submission returned after failure; marks="<<marks<<"\n";
        return 23;
    }
    require(mode=="success");
    CommandBufferPool pool; pool.wait(); require(waits==1 && updates==1);
    pool.mSubmitted.active=false; pool.wait(); require(waits==1 && updates==2);
    VulkanCommandBuffer commands; auto submission=commands.submit();
    require(submission && ends==1 && submits==1 && marks==1);
    std::cout<<"Command wait/submit success contract PASS; no GPU execution\n";
}
'''


def brace_end(text, start):
    depth = 0
    for index in range(start, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            return index + 1
    raise ValueError("Unbalanced production function")


def method(source, signature):
    start = source.index(signature)
    end = brace_end(source, source.index("{", start))
    return source[start:end]


def without_guard(source, condition):
    start = source.index(condition)
    end = brace_end(source, source.index("{", start))
    return source[:start] + source[end:]


def disable_core():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--vulkan-include", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    production = SOURCE.read_text()
    wait = method(production, "void CommandBufferPool::wait()")
    submit = method(production, "fvkmemory::resource_ptr<VulkanSemaphore> VulkanCommandBuffer::submit()")
    variants = {
        "production": (wait, submit),
        "historical-ignored-wait": (without_guard(wait, "if (UTILS_UNLIKELY(result != VK_SUCCESS))"), submit),
        "historical-ignored-submit": (wait, without_guard(submit, "if (UTILS_UNLIKELY(result != VK_SUCCESS))")),
    }
    args.work_dir.mkdir(parents=True, exist_ok=True)
    records = []
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe")
    for name, (wait_method, submit_method) in variants.items():
        directory = args.work_dir / name
        directory.mkdir(exist_ok=True)
        source = directory / "actual-method-contract.cpp"
        source.write_text(SHIM + wait_method + "\n" + submit_method + ORACLE)
        executable = directory / ("contract.exe" if msvc else "contract")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", "/I" + str(args.vulkan_include),
                    str(source), "/Fe:" + str(executable), "/Fo:" + str(directory / "contract.obj")]
                   if msvc else [args.cxx, "-std=c++20", "-I" + str(args.vulkan_include), str(source), "-o", str(executable)])
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout)
        if compiled.returncode:
            raise ValueError(name + " did not compile; not a valid source control")
        cases = (["success", "wait-lost", "wait-oom", "wait-timeout", "wait-unknown", "submit-lost", "submit-oom", "submit-unknown", "end-failed"]
                 if name == "production" else ["wait-lost"] if name.endswith("wait") else ["submit-lost"])
        for case in cases:
            result = subprocess.run([str(executable), case], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=15, preexec_fn=disable_core if os.name == "posix" else None)
            (directory / (case + ".log")).write_text(result.stdout)
            if name != "production":
                passed = result.returncode == 23 and "returned after failure" in result.stdout
            elif case == "success":
                passed = result.returncode == 0 and "success contract PASS" in result.stdout
            else:
                abort = result.returncode == -signal.SIGABRT if os.name == "posix" else result.returncode != 0
                operation = "vkWaitForFences" if case.startswith("wait-") else "vkEndCommandBuffer" if case == "end-failed" else "vkQueueSubmit"
                numeric = 2 if case.endswith("timeout") else -13 if case.endswith("unknown") else -4 if case.endswith("lost") else -2 if case == "submit-oom" else -1
                passed = abort and (operation + " failed. VkResult=" + str(numeric)) in result.stdout and "returned after failure" not in result.stdout
            if not passed:
                raise ValueError(f"{name}/{case} did not meet its independent result oracle: {result.returncode}\n{result.stdout}")
            records.append({"variant": name, "case": case, "returncode": result.returncode, "passed": passed})
    receipt = {"schema": 1, "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
               "wait_method_sha256": hashlib.sha256(wait.encode()).hexdigest(),
               "submit_method_sha256": hashlib.sha256(submit.encode()).hexdigest(),
               "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "compiler": args.cxx, "cases": records, "GPU_API_calls": 0,
               "limits": "Verbatim production wait/submit method bodies against fake surrounding driver objects; no GPU reset, loader or hardware qualification."}
    (args.work_dir / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("Vulkan command failure contract PASS: actual wait/submit methods and historical ignored-error negatives; no GPU execution")


if __name__ == "__main__":
    main()
