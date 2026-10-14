#!/usr/bin/env python3
"""Source-aware DLSS lifecycle contracts; no vendor DLL or graphics device."""
from pathlib import Path
import argparse
import json
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "benchmark/src/rhi/frame_generation"


def code(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", lambda match: " " * len(match.group()), text, flags=re.S)


def raw_body(text, signature):
    original = text
    text = code(text)
    start = text.find(signature)
    if start < 0:
        raise AssertionError(f"Missing production function: {signature}")
    opening = text.index("{", start)
    depth = 1
    for index in range(opening + 1, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return original[opening + 1:index]
    raise AssertionError(f"Unbalanced production function: {signature}")


def body(text, signature):
    return code(raw_body(text, signature))


def require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def validate(platform, bridge, runtime):
    helper = body(platform, "VkResult DlssPlatform::drain_generation()")
    require(helper.find("suspend_generation();") >= 0, "SDK generation is not suspended")
    wait = helper.find("return impl_->wait_device_idle(getDevice());")
    require(wait > helper.find("suspend_generation();"),
            "SDK proxied DeviceWaitIdle must follow suspension and return its result")
    device_wait = body(platform, "VkResult wait_device_idle(VkDevice device)")
    require("if (!device) return VK_SUCCESS;" in device_wait,
            "uninitialized native device must not be waited on")
    require("if (sdk_device_created)" in device_wait and
            "provider.device_hook(device, \"vkDeviceWaitIdle\")" in device_wait and
            "return sdk_device_idle(device);" in device_wait and
            "return bluevk::vkDeviceWaitIdle(device);" in device_wait,
            "SDK proxied DeviceWaitIdle must remain distinct from native fallback drainage")
    creation = body(platform, "VkDevice DlssPlatform::createVkDevice(")
    success = body(creation, "if (result == VK_SUCCESS && device)")
    require("impl_->sdk_device_created = true;" in success,
            "SDK device creation must establish the idle-dispatch ownership flag")
    failure = body(platform, "void fail_lifecycle_drain(")
    require("std::terminate();" in failure, "void lifecycle failure can permit resource retirement")
    for signature, retirement in (("void DlssPlatform::destroy(", "Base::destroy(handle);"),
                                  ("void DlssPlatform::terminate()", "impl_->provider.shutdown_after_drain();")):
        lifecycle = body(platform, signature)
        require(0 <= lifecycle.find("drain_generation();") < lifecycle.find(retirement) and
                lifecycle.find("if (drained != VK_SUCCESS) fail_lifecycle_drain(") < lifecycle.find(retirement) and
                "if (drained != VK_SUCCESS) fail_lifecycle_drain(" in lifecycle,
                "void lifecycle must fail-stop before resource retirement")
    recreate = body(platform, "VkResult DlssPlatform::recreate(")
    require(0 <= recreate.find("if (drained != VK_SUCCESS) return drained;") < recreate.find("Base::recreate(handle)"),
            "failed implicit recreation drain reaches Base retirement")
    fallback = body(platform, "DlssPlatform::createDriver(")
    require(0 <= fallback.find("drain_generation();") < fallback.find("fail_lifecycle_drain(") <
            fallback.find("impl_->provider.shutdown_after_drain();") < fallback.find("impl_->restore();"),
            "initialization fallback retires proxies or SDK before checked drainage")
    require(fallback.find("impl_->sdk_device_created = false;") > fallback.find("impl_->provider.shutdown_after_drain();"),
            "fallback native ownership changed before SDK shutdown")

    forward = body(bridge, "VkResult DlssRuntimeBridge::drain_generation()")
    require("platform_.drain_generation()" in forward,
            "bridge bypasses SDK presentation drain")

    drain = body(runtime, "void FrameGenerationRuntime::drain()")
    call = re.search(r"(?:const\s+)?auto\s+(\w+)\s*=\s*"
                     r"[^;]*->drain_generation\(\)\s*;", drain)
    require(call is not None, "runtime does not retain the SDK drain result")
    native = drain.rfind("nativeCommand(", 0, call.start())
    finish = drain.find("core_.engine->flushAndWait();", call.end())
    require(native >= 0 and finish >= 0,
            "SDK drain must execute in queued native callback before engine completion")
    require(drain.find("dlss_frame_.cancel();") > finish,
            "prospective ticket retired before SDK/backend completion")
    failed = re.search(rf"if\s*\(\s*{call.group(1)}\s*!=\s*VK_SUCCESS\s*\)",
                       drain[call.end():finish])
    require(failed is not None, "failed SDK drain is ignored")
    failure_body = body(drain[call.end():finish], failed.group(0))
    require("std::terminate();" in failure_body,
            "failed SDK drain can return and permit tagged-image retirement")

    destructor = body(runtime, "FrameGenerationRuntime::~FrameGenerationRuntime()")
    require(0 <= destructor.find("drain();") < destructor.find("output_.reset();"),
            "tagged output is destroyed before SDK drain")
    prepare = body(runtime, "bool FrameGenerationRuntime::prepare(")
    resize = body(prepare, "if(width_!=width || height_!=height)")
    require("drain();" in resize, "resizing output omits SDK drain")
    require(prepare.find("if(width_!=width || height_!=height)") <
            prepare.find("output_->ensure(width,height)"),
            "target resize can occur before the drain boundary")


class DlssLifecycleContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.platform = (SOURCES / "dlss_platform.cpp").read_text()
        cls.bridge = (SOURCES / "dlss_runtime_bridge.cpp").read_text()
        cls.runtime = (SOURCES / "frame_generation_runtime.cpp").read_text()

    def test_actual_production_chain(self):
        validate(self.platform, self.bridge, self.runtime)

    def test_native_queue_wait_cannot_replace_sdk_drain(self):
        altered = self.platform.replace("return sdk_device_idle(device);",
                                        "return bluevk::vkDeviceWaitIdle(device);")
        self.assertNotEqual(altered, self.platform)
        with self.assertRaisesRegex(AssertionError, "proxied DeviceWaitIdle"):
            validate(altered, self.bridge, self.runtime)

    def test_implicit_recreation_cannot_ignore_failed_drain(self):
        altered = self.platform.replace("if (drained != VK_SUCCESS) return drained;", "")
        with self.assertRaisesRegex(AssertionError, "implicit recreation"):
            validate(altered, self.bridge, self.runtime)

    def test_void_retirement_cannot_ignore_failed_drain(self):
        for operation in ("swapchain destruction", "device termination"):
            altered = self.platform.replace(f'if (drained != VK_SUCCESS) fail_lifecycle_drain("{operation}", drained);', "")
            with self.assertRaisesRegex(AssertionError, "void lifecycle"):
                validate(altered, self.bridge, self.runtime)

    def test_fallback_preserves_hooks_until_shutdown(self):
        function = raw_body(self.platform, "DlssPlatform::createDriver(")
        altered_function = function.replace("impl_->restore();", "")
        altered_function = altered_function.replace("const auto drained = drain_generation();",
                                                     "impl_->restore(); const auto drained = drain_generation();")
        with self.assertRaisesRegex(AssertionError, "fallback retires proxies"):
            validate(self.platform.replace(function, altered_function), self.bridge, self.runtime)

    def test_sdk_creation_flag_cannot_mark_native_fallback(self):
        creation = raw_body(self.platform, "VkDevice DlssPlatform::createVkDevice(")
        altered_creation = creation.replace("impl_->sdk_device_created = true;", "")
        with self.assertRaisesRegex(AssertionError, "ownership flag"):
            validate(self.platform.replace(creation, altered_creation), self.bridge, self.runtime)

    def test_missing_bridge_drain_is_rejected(self):
        altered = self.bridge.replace("platform_.drain_generation()",
                                      "platform_.suspend_generation()")
        self.assertNotEqual(altered, self.bridge)
        with self.assertRaisesRegex(AssertionError, "bridge bypasses"):
            validate(self.platform, altered, self.runtime)

    def test_failed_drain_cannot_permit_retirement(self):
        drain = raw_body(self.runtime, "void FrameGenerationRuntime::drain()")
        altered_drain = drain.replace("std::terminate();", "return;")
        self.assertNotEqual(altered_drain, drain)
        altered = self.runtime.replace(drain, altered_drain)
        with self.assertRaisesRegex(AssertionError, "failed SDK drain can return"):
            validate(self.platform, self.bridge, altered)

    def test_destroy_without_drain_is_rejected(self):
        destructor = raw_body(self.runtime, "FrameGenerationRuntime::~FrameGenerationRuntime()")
        altered = self.runtime.replace(destructor, destructor.replace("drain();", ""))
        with self.assertRaisesRegex(AssertionError, "output is destroyed"):
            validate(self.platform, self.bridge, altered)

    def test_early_ticket_retirement_is_rejected(self):
        drain = raw_body(self.runtime, "void FrameGenerationRuntime::drain()")
        altered_drain = drain.replace("dlss_frame_.cancel();", "")
        altered_drain = altered_drain.replace("core_.engine->flushAndWait();",
                                            "dlss_frame_.cancel(); core_.engine->flushAndWait();")
        altered = self.runtime.replace(drain, altered_drain)
        with self.assertRaisesRegex(AssertionError, "ticket retired before"):
            validate(self.platform, self.bridge, altered)

    def test_resize_without_drain_is_rejected(self):
        prepare = raw_body(self.runtime, "bool FrameGenerationRuntime::prepare(")
        resize = raw_body(prepare, "if(width_!=width || height_!=height)")
        altered = self.runtime.replace(resize, resize.replace("drain();", ""))
        with self.assertRaisesRegex(AssertionError, "resizing output omits"):
            validate(self.platform, self.bridge, altered)


def compiled_contracts(compiler, directory):
    platform = (SOURCES / "dlss_platform.cpp").read_text()
    helper = raw_body(platform, "VkResult wait_device_idle(VkDevice device)")
    suspend = raw_body(platform, "void DlssPlatform::suspend_generation()")
    drain = raw_body(platform, "VkResult DlssPlatform::drain_generation()")
    recreate = raw_body(platform, "VkResult DlssPlatform::recreate(")
    destroy = raw_body(platform, "void DlssPlatform::destroy(")
    terminate = raw_body(platform, "void DlssPlatform::terminate()")
    fallback = raw_body(platform, "DlssPlatform::createDriver(")
    fallback = fallback[fallback.index("    const auto drained = drain_generation();"):]
    fatal = raw_body(platform, "void fail_lifecycle_drain(").replace("std::terminate();", "throw Fatal{};")
    prefix = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#define _WIN32 1
using VkDevice=uintptr_t;
using VkResult=int;
using SwapChainPtr=const void*;
SwapChainPtr surface() { static int identity; return &identity; }
using PFN_vkDeviceWaitIdle=VkResult(*)(VkDevice);
constexpr VkResult VK_SUCCESS=0,VK_ERROR_DEVICE_LOST=-4,VK_ERROR_INITIALIZATION_FAILED=-3;
struct State {
    int native=0,sdk=0,resolved=0,recreated=0,destroyed=0,terminated=0,shutdown=0,restored=0;
    VkResult result=VK_SUCCESS;
    bool hook=true;
    std::vector<std::string> order;
} state;
VkResult sdk_idle(VkDevice) { ++state.sdk; state.order.push_back("sdk-idle"); return state.result; }
namespace bluevk {
VkResult vkDeviceWaitIdle(VkDevice) { ++state.native; state.order.push_back("native-idle"); return state.result; }
}
struct Provider {
    void suspend() { state.order.push_back("suspend"); }
    void shutdown_after_drain() { ++state.shutdown; state.order.push_back("shutdown"); }
    void* device_hook(VkDevice,const char*) { ++state.resolved; return state.hook?reinterpret_cast<void*>(sdk_idle):nullptr; }
};
struct Fatal {};
[[noreturn]] void fail_lifecycle_drain(const char* operation,VkResult result) {
'''
    middle = r'''
}
struct Impl {
    Provider provider;
    bool sdk_device_created=false;
    PFN_vkDeviceWaitIdle sdk_device_idle=nullptr;
    int pending=1;
    SwapChainPtr surface_chain=surface();
    uint64_t surface_flags=0;
    struct Swapchain { uint64_t epoch=1; } swapchain;
    std::mutex mutex;
    std::atomic<bool> bound{true};
    void publish(std::string={}) {}
    void publish_swapchain(int,bool,uint64_t) { ++swapchain.epoch; }
    void restore() { ++state.restored; state.order.push_back("restore"); }
    VkResult wait_device_idle(VkDevice device) {
'''
    base = r'''
    }
};
struct Base {
    static VkResult recreate(SwapChainPtr) { ++state.recreated; state.order.push_back("recreate"); return VK_SUCCESS; }
    static void destroy(SwapChainPtr) { ++state.destroyed; state.order.push_back("destroy"); }
    static void terminate() { ++state.terminated; state.order.push_back("terminate"); }
    static int getSwapChainBundle(SwapChainPtr) { return 1; }
};
struct Platform {
    Impl storage;
    Impl* impl_=&storage;
    VkDevice device=1;
    VkDevice getDevice() const { return device; }
    void suspend_generation() {
'''
    def fixture(changes):
        parts = dict(helper=helper, suspend=suspend, drain=drain, recreate=recreate,
                     destroy=destroy, terminate=terminate, fallback=fallback)
        parts.update(changes)
        return (prefix + fatal + middle + parts["helper"] + base + parts["suspend"] +
                "\n} VkResult drain_generation() {\n" + parts["drain"] +
                "\n} VkResult recreate(SwapChainPtr handle) {\n" + parts["recreate"] +
                "\n} void destroy(SwapChainPtr handle) {\n" + parts["destroy"] +
                "\n} void terminate() {\n" + parts["terminate"] +
                "\n} int fallback() { int driver=1; std::string reason;\n" + parts["fallback"] +
                "\n}\n};\n" + suffix)
    suffix = r'''
int main() {
    { state={}; Platform p; p.device=0; p.storage.sdk_device_created=true;
      if(p.drain_generation()!=VK_SUCCESS || state.native || state.sdk || state.resolved) {
          std::puts("FAIL: no-device drainage called a native API"); return 1; } }
    { state={}; Platform p; p.storage.sdk_device_created=false;
      if(p.drain_generation()!=VK_SUCCESS || state.native!=1 || state.sdk || state.resolved) {
          std::puts("FAIL: native fallback was routed through unregistered SDK device"); return 1; } }
    { state={}; Platform p; p.storage.sdk_device_created=true;
      if(p.drain_generation()!=VK_SUCCESS || state.native || state.sdk!=1 || state.resolved!=1) {
          std::puts("FAIL: SDK-created partial-install device bypassed SDK idle"); return 1; } }
    { state={}; state.hook=false; Platform p; p.storage.sdk_device_created=true;
      if(p.drain_generation()!=VK_ERROR_INITIALIZATION_FAILED || state.native || state.sdk) {
          std::puts("FAIL: unresolved SDK idle silently used native fallback"); return 1; } }
    { state={}; state.result=VK_ERROR_DEVICE_LOST; Platform p; p.storage.sdk_device_created=true;
      if(p.recreate(surface())!=VK_ERROR_DEVICE_LOST || state.recreated || p.storage.swapchain.epoch!=1) {
          std::puts("FAIL: failed recreation drain retired surface"); return 1; } }
    for(int operation=0;operation!=3;++operation) {
        state={}; state.result=VK_ERROR_DEVICE_LOST; Platform p; p.storage.sdk_device_created=true;
        bool failed=false;
        try { if(operation==0) p.destroy(surface()); else if(operation==1) p.terminate(); else p.fallback(); }
        catch(const Fatal&) { failed=true; }
        if(!failed || state.destroyed || state.terminated || state.shutdown || state.restored) {
            std::puts(operation==0?"FAIL: destruction ignored failed drain":
                      operation==1?"FAIL: termination ignored failed drain":"FAIL: fallback ignored failed drain"); return 1;
        }
    }
    { state={}; Platform p; p.storage.sdk_device_created=true;
      if(p.recreate(surface())!=VK_SUCCESS || state.sdk!=1 || state.recreated!=1) return 1; }
    { state={}; Platform p; p.storage.sdk_device_created=true;
      p.destroy(surface()); if(state.sdk!=1 || state.destroyed!=1) return 1; }
    { state={}; Platform p; p.storage.sdk_device_created=true;
      p.terminate();
      if(state.order!=std::vector<std::string>{"suspend","sdk-idle","shutdown","restore","terminate"}) {
          std::puts("FAIL: termination reordered drain/shutdown/proxy retirement"); return 1; } }
    { state={}; Platform p; p.storage.sdk_device_created=true;
      if(p.fallback()!=1 || state.order!=std::vector<std::string>{"suspend","sdk-idle","shutdown","restore"}) {
          std::puts("FAIL: fallback restored hooks before SDK idle/shutdown"); return 1; }
      state={}; p.drain_generation();
      if(state.native!=1 || state.sdk || state.resolved || p.storage.sdk_device_idle) {
          std::puts("FAIL: fallback reused SDK idle after shutdown"); return 1; } }
    std::puts("PASS: actual platform drain selection/recreation/destruction/termination/fallback");
}
'''
    variants = [
        ("positive", {}, None),
        ("unchecked-recreate", {"recreate": recreate.replace("if (drained != VK_SUCCESS) return drained;", "")},
         "failed recreation drain retired surface"),
        ("unchecked-destroy", {"destroy": destroy.replace('if (drained != VK_SUCCESS) fail_lifecycle_drain("swapchain destruction", drained);', "")},
         "destruction ignored failed drain"),
        ("unchecked-terminate", {"terminate": terminate.replace('if (drained != VK_SUCCESS) fail_lifecycle_drain("device termination", drained);', "")},
         "termination ignored failed drain"),
        ("sdk-idle-bypassed", {"helper": helper.replace("return sdk_device_idle(device);", "return bluevk::vkDeviceWaitIdle(device);")},
         "SDK-created partial-install device bypassed SDK idle"),
        ("native-routed-through-sdk", {"helper": helper.replace("if (sdk_device_created)", "if (true)")},
         "native fallback was routed through unregistered SDK device"),
        ("early-proxy-restore", {"fallback": fallback.replace("impl_->restore();", "").replace(
            "const auto drained = drain_generation();", "impl_->restore(); const auto drained = drain_generation();")},
         "fallback ignored failed drain"),
        ("stale-sdk-after-fallback", {"fallback": fallback.replace("impl_->sdk_device_created = false;", "")},
         "fallback reused SDK idle after shutdown"),
    ]
    observations = []
    directory.mkdir(parents=True, exist_ok=True)
    for name, changes, expected in variants:
        work = directory / name
        work.mkdir(exist_ok=True)
        source = work / "test.cpp"
        source.write_text(fixture(changes))
        output = work / "test"
        msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
        command = ([compiler, "/nologo", "/std:c++20", "/EHsc", str(source), f"/Fe:{output}.exe", f"/Fo:{output}.obj"]
                   if msvc else [compiler, "-std=c++20", "-pthread", str(source), "-o", str(output)])
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (work / "compile.log").write_text(compiled.stdout)
        require(compiled.returncode == 0, f"{name} failed compilation; not valid negative evidence")
        executed = subprocess.run([str(output) + (".exe" if msvc else "")], text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        (work / "run.log").write_text(executed.stdout)
        require(executed.returncode == 0 if expected is None else executed.returncode == 1 and expected in executed.stdout,
                f"{name} did not satisfy its precise result: {executed.stdout}")
        observations.append({"variant": name, "exit_code": executed.returncode, "specific_failure": expected})
    report = {"passed": True, "native_gpu_execution": False, "variants": observations,
              "scope": "Actual extracted platform methods; fatal branches use a test-only throwing sentinel; no NVIDIA runtime qualification"}
    (directory / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--cxx")
    parser.add_argument("--work-dir", type=Path)
    options, remaining = parser.parse_known_args()
    if options.cxx:
        require(options.work_dir is not None, "--work-dir is required with --cxx")
        compiled_contracts(options.cxx, options.work_dir)
    else:
        unittest.main(argv=[__file__, *remaining])
