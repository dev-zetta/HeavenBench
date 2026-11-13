#!/usr/bin/env python3
"""Compile complete GLX / OpenGL retirement methods against fake native state.

This host-only contract checks drawable ownership, call order and cached-pointer
retirement. Native driver behavior and crash causation need separate GPU tests.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from dlss_lifecycle_contract_test import raw_body  # noqa: E402

GLX_SOURCE = ROOT / "lib/filament/filament/backend/src/opengl/platforms/PlatformGLX.cpp"
DRIVER_SOURCE = ROOT / "lib/filament/filament/backend/src/opengl/OpenGLDriver.cpp"

# Exact original methods from Google Filament, Copyright 2015/2017 The Android
# Open Source Project; retained under Apache-2.0 for executable negative controls.
ORIGINAL_GLX_SHA256 = "cef7c63fdb30d141646926b91118b34feabab2ff9545f95ca07792f9b458e083"
ORIGINAL_GLX = r'''
    auto it = std::find(mPBuffers.begin(), mPBuffers.end(), (GLXPbuffer)swapChain);
    if (it != mPBuffers.end()) {
        g_glx.destroyPbuffer(mGLXDisplay, (GLXPbuffer)swapChain);
        mPBuffers.erase(it);
    }
'''
ORIGINAL_DRIVER_SHA256 = "9d61033b4621108d14c0aa5265282291b5ee13d0b52a21437c4953f737c91991"
ORIGINAL_DRIVER = r'''
    DEBUG_MARKER()

    if (sch) {
        GLSwapChain const* sc = handle_cast<GLSwapChain*>(sch);
        mPlatform.destroySwapChain(sc->swapChain);
        destruct(sch, sc);
    }
'''

PREFIX = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// The production condition remains active. A failure exits before any native
// retirement, rather than linking the complete Filament logging subsystem.
struct FatalPostcondition {
    explicit FatalPostcondition(bool success) {
        if (!success) {
            std::fputs("Active GLX retirement postcondition\n", stderr);
            std::fflush(stderr); std::_Exit(86);
        }
    }
    template<class T> FatalPostcondition& operator<<(T const&) { return *this; }
};
#define FILAMENT_CHECK_POSTCONDITION(condition) FatalPostcondition(bool(condition))
#define DEBUG_MARKER()
using GLXDrawable = std::uintptr_t;
using GLXPbuffer = GLXDrawable;
using GLXContext = void*;
using Bool = int;
struct Display {} display;
constexpr GLXDrawable None = 0;
constexpr unsigned GL_FRAMEBUFFER = 0x8D40;
struct NativeState {
    GLXDrawable draw = 11, read = 12;
    GLXContext context = reinterpret_cast<GLXContext>(std::uintptr_t(27));
    std::vector<GLXDrawable> live{11, 12, 41, 42, 91};
    std::vector<std::string> calls;
    unsigned invalid_destroy = 0, invalid_current = 0, destroys = 0, binds = 0;
    bool fail_bind = false;
    bool is_live(GLXDrawable id) const {
        return std::find(live.begin(), live.end(), id) != live.end();
    }
} native;
Bool set_current(Display* dpy, GLXDrawable draw, GLXDrawable read, GLXContext context) {
    ++native.binds; native.calls.emplace_back("bind");
    if (dpy != &display || !native.is_live(draw) || !native.is_live(read) ||
            context != reinterpret_cast<GLXContext>(std::uintptr_t(27))) ++native.invalid_current;
    if (native.fail_bind) return 0;
    native.draw = draw; native.read = read; native.context = context;
    return 1;
}
void destroy_pbuffer(Display* dpy, GLXPbuffer id) {
    ++native.destroys; native.calls.emplace_back("destroy");
    if (dpy != &display || !native.is_live(id) || id == native.draw || id == native.read)
        ++native.invalid_destroy;
    std::erase(native.live, id);
}
GLXDrawable get_draw() { return native.draw; }
GLXDrawable get_read() { return native.read; }
struct GLXFunctions {
    decltype(&set_current) setCurrentContext = set_current;
    decltype(&destroy_pbuffer) destroyPbuffer = destroy_pbuffer;
    decltype(&get_draw) getCurrentDrawable = get_draw;
    decltype(&get_read) getCurrentReadDrawable = get_read;
} g_glx;
namespace filament::backend {
struct Platform { struct SwapChain {}; };
class PlatformGLX : public Platform {
public:
    enum class ContextType { UNPROTECTED };
    Display* mGLXDisplay = &display;
    GLXContext mGLXContext = reinterpret_cast<GLXContext>(std::uintptr_t(27));
    GLXPbuffer mDummySurface = 91;
    std::vector<GLXPbuffer> mPBuffers{41, 42};
    void destroySwapChain(Platform::SwapChain* swapChain) noexcept;
    bool makeCurrent(ContextType type, SwapChain* drawSwapChain, SwapChain* readSwapChain);
};
struct HwSwapChain {};
struct GLSwapChain { Platform::SwapChain* swapChain; };
template<class T> struct Handle {
    GLSwapChain* object = nullptr;
    explicit operator bool() const { return object != nullptr; }
};
template<class T> T handle_cast(Handle<HwSwapChain> handle) {
    return static_cast<T>(handle.object);
}
class OpenGLDriver;
struct DriverPlatform {
    OpenGLDriver* owner;
    unsigned destroys = 0, stale_at_platform_destroy = 0;
    Platform::SwapChain* last = nullptr;
    void destroySwapChain(Platform::SwapChain* swapChain);
};
class OpenGLDriver {
public:
    GLSwapChain* mCurrentDrawSwapChain = nullptr;
    DriverPlatform mPlatform{this};
    std::vector<std::string> retirement_calls;
    struct BackendState {
        std::vector<std::string>* calls;
        unsigned invalid_target = 0;
        void unbindFramebuffer(unsigned target) {
            if (target != GL_FRAMEBUFFER) ++invalid_target;
            calls->emplace_back("unbind");
        }
    } backend_state{&retirement_calls};
    BackendState& getBackendState() { return backend_state; }
    unsigned destructs = 0, stale_at_destruct = 0;
    GLSwapChain const* destructed = nullptr;
    void destroySwapChain(Handle<HwSwapChain> sch);
    void destruct(Handle<HwSwapChain>, GLSwapChain const* object) {
        retirement_calls.emplace_back("destruct");
        ++destructs;
        if (mCurrentDrawSwapChain == object) ++stale_at_destruct;
        destructed = object;
    }
};
void DriverPlatform::destroySwapChain(Platform::SwapChain* swapChain) {
    owner->retirement_calls.emplace_back("platform");
    ++destroys; last = swapChain;
    if (owner->mCurrentDrawSwapChain && owner->mCurrentDrawSwapChain->swapChain == swapChain)
        ++stale_at_platform_destroy;
}
'''

ORACLES = r'''
} // namespace filament::backend
using namespace filament::backend;
Platform::SwapChain* chain(GLXDrawable id) { return reinterpret_cast<Platform::SwapChain*>(id); }
unsigned failures = 0;
void check(bool passed, const char* name, const char* reason) {
    if (!passed) { ++failures; std::fprintf(stderr, "FAIL %s: %s\n", name, reason); }
}
void platform_case(const char* name, GLXDrawable draw, GLXDrawable read,
        GLXDrawable retired, bool owned) {
    native = NativeState{}; native.draw = draw; native.read = read;
    PlatformGLX platform;
    platform.destroySwapChain(chain(retired));
    const auto expected_draw = owned && draw == retired ? platform.mDummySurface : draw;
    const auto expected_read = owned && read == retired ? platform.mDummySurface : read;
    const bool needs_bind = expected_draw != draw || expected_read != read;
    check(native.draw == expected_draw && native.read == expected_read, name,
            "retired endpoints not replaced or unrelated endpoints changed");
    check(native.binds == unsigned(needs_bind), name, "unexpected native context switch count");
    check(native.destroys == unsigned(owned), name, "native ownership or exactly-once deletion failed");
    check(native.invalid_destroy == 0, name, "owned pbuffer destroyed while still current");
    check(native.invalid_current == 0 && native.context == platform.mGLXContext, name,
            "rebind lost valid context or drawable");
    check(platform.mPBuffers == (owned ? std::vector<GLXPbuffer>{42} :
            std::vector<GLXPbuffer>{41, 42}), name, "owned pbuffer bookkeeping changed");
    const std::vector<std::string> expected_calls = !owned ? std::vector<std::string>{} :
            needs_bind ? std::vector<std::string>{"bind", "destroy"} :
            std::vector<std::string>{"destroy"};
    check(native.calls == expected_calls, name, "retirement native-call order changed");
    check(native.is_live(native.draw) && native.is_live(native.read), name,
            "GL work after retirement has a deleted current drawable");
    const auto calls_after_first = native.calls;
    platform.destroySwapChain(chain(retired));
    check(native.calls == calls_after_first, name, "duplicate retirement reached native dispatch");
    const bool made_current = platform.makeCurrent(PlatformGLX::ContextType::UNPROTECTED,
            chain(11), chain(12));
    check(made_current && native.draw == 11 && native.read == 12 && !native.invalid_current,
            name, "valid onscreen makeCurrent after retirement failed");
}
void driver_cases() {
    GLSwapChain a{chain(41)}, b{chain(42)};
    {
        OpenGLDriver driver; driver.mCurrentDrawSwapChain = &a;
        driver.destroySwapChain(Handle<HwSwapChain>{&a});
        check(!driver.mCurrentDrawSwapChain && !driver.stale_at_destruct &&
                !driver.mPlatform.stale_at_platform_destroy, "driver-current",
                "draw swapchain cache survives retirement boundary");
        check(driver.mPlatform.destroys == 1 && driver.mPlatform.last == a.swapChain &&
                driver.destructs == 1 && driver.destructed == &a, "driver-current",
                "platform/arena retirement identity or count changed");
        check(driver.retirement_calls == std::vector<std::string>{"unbind", "platform", "destruct"} &&
                !driver.backend_state.invalid_target, "driver-current",
                "framebuffer unbind must precede platform and arena retirement");
    }
    {
        OpenGLDriver driver; driver.mCurrentDrawSwapChain = &b;
        driver.destroySwapChain(Handle<HwSwapChain>{&a});
        check(driver.mCurrentDrawSwapChain == &b && !driver.stale_at_destruct &&
                !driver.mPlatform.stale_at_platform_destroy && driver.destructed == &a,
                "driver-noncurrent", "unrelated draw swapchain cache changed");
        const auto calls_before_null = driver.retirement_calls;
        const unsigned destroys = driver.mPlatform.destroys, destructs = driver.destructs;
        driver.destroySwapChain(Handle<HwSwapChain>{});
        check(driver.mCurrentDrawSwapChain == &b && driver.mPlatform.destroys == destroys &&
                driver.destructs == destructs && driver.retirement_calls == calls_before_null,
                "driver-null", "null retirement mutated state");
    }
    {
        OpenGLDriver driver; driver.destroySwapChain(Handle<HwSwapChain>{&a});
        check(!driver.mCurrentDrawSwapChain && driver.destructed == &a && driver.mPlatform.destroys == 1,
                "driver-unbound", "unbound retirement changed");
    }
}
int main(int argc, char**) {
    if (argc == 2) {
        native = NativeState{}; native.draw = native.read = 41; native.fail_bind = true;
        PlatformGLX platform; platform.destroySwapChain(chain(41));
        std::fputs("FAIL: failed GLX handoff reached native destruction\n", stderr); return 1;
    }
    platform_case("both-current", 41, 41, 41, true);
    platform_case("draw-current", 41, 12, 41, true);
    platform_case("read-current", 11, 41, 41, true);
    platform_case("owned-noncurrent", 11, 12, 41, true);
    platform_case("external-current", 11, 12, 11, false);
    platform_case("external-read-current", 11, 12, 12, false);
    platform_case("unknown", 11, 12, 73, false);
    platform_case("null", 11, 12, 0, false);
    driver_cases();
    if (failures) return 1;
    std::puts("PASS: actual GLX drawable ownership/call order and driver cached draw-pointer retirement");
    return 0;
}
'''


def compiled_source(glx_body, make_current_body, driver_body):
    return (PREFIX + "\nvoid PlatformGLX::destroySwapChain(Platform::SwapChain* swapChain) noexcept {\n" +
            glx_body + "\n}\nbool PlatformGLX::makeCurrent(ContextType type, SwapChain* drawSwapChain, "
            "SwapChain* readSwapChain) {\n" + make_current_body +
            "\n}\nvoid OpenGLDriver::destroySwapChain(Handle<HwSwapChain> sch) {\n" +
            driver_body + "\n}\n" + ORACLES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    for source, digest in ((ORIGINAL_GLX, ORIGINAL_GLX_SHA256), (ORIGINAL_DRIVER, ORIGINAL_DRIVER_SHA256)):
        if hashlib.sha256(source.encode()).hexdigest() != digest:
            raise RuntimeError("Immutable original-source control changed")
    glx, driver = GLX_SOURCE.read_text(), DRIVER_SOURCE.read_text()
    destroy_glx = raw_body(glx, "void PlatformGLX::destroySwapChain(")
    make_current = raw_body(glx, "bool PlatformGLX::makeCurrent(")
    destroy_driver = raw_body(driver, "void OpenGLDriver::destroySwapChain(")
    unbind = "getBackendState().unbindFramebuffer(GL_FRAMEBUFFER);"
    if destroy_driver.count(unbind) != 1:
        raise RuntimeError("Expected one actual framebuffer retirement unbind")
    variants = (
        ("positive", destroy_glx, destroy_driver, ()),
        ("original-glx", ORIGINAL_GLX, destroy_driver,
         ("FAIL both-current:", "FAIL draw-current:", "FAIL read-current:")),
        ("original-driver", destroy_glx, ORIGINAL_DRIVER, ("FAIL driver-current:",)),
        ("missing-framebuffer-unbind", destroy_glx, destroy_driver.replace(unbind, ""),
         ("framebuffer unbind must precede platform and arena retirement",)),
    )
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    work, observations, passed = args.work_dir.resolve(), [], True
    for name, platform_body, driver_body, expected_failures in variants:
        directory = work / name; directory.mkdir(parents=True, exist_ok=True)
        source = directory / "fixture.cpp"
        source.write_text(compiled_source(platform_body, make_current, driver_body))
        binary = directory / ("fixture.exe" if msvc else "fixture")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", str(source), "/Fe:" + str(binary)]
                   if msvc else [args.cxx, "-std=c++20", str(source), "-o", str(binary)])
        compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError(f"{name} did not compile; invalid control: {compiled.stdout}{compiled.stderr}")
        executed = subprocess.run([str(binary)], cwd=directory, capture_output=True, text=True, timeout=10)
        (directory / "run.log").write_text(executed.stdout + executed.stderr)
        valid = (executed.returncode == (1 if expected_failures else 0) and
                 all(reason in executed.stderr for reason in expected_failures))
        assertion_exit = None
        if name == "positive":
            assertion = subprocess.run([str(binary), "failed-rebind"], cwd=directory,
                                       capture_output=True, text=True, timeout=10)
            (directory / "failed-rebind.log").write_text(assertion.stdout + assertion.stderr)
            assertion_exit = assertion.returncode
            valid = valid and assertion_exit == 86 and "Active GLX retirement postcondition" in assertion.stderr
        passed = passed and valid
        observations.append({"variant": name, "compile_argv": command,
                             "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                             "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                             "exit_code": executed.returncode, "expected_failures": expected_failures,
                             "failed_rebind_exit": assertion_exit, "oracle_passed": valid})
        print(f"{name}: {'PASS' if valid else 'FAIL'} (exit {executed.returncode})")
        if not valid: print(executed.stdout + executed.stderr, file=sys.stderr)
    receipt = {
        "passed": passed, "gpu_execution": False,
        "scope": "Complete actual GLX destroy/makeCurrent and OpenGLDriver destroy method bodies; fake native current draw/read, ownership, arena retirement and fatal postcondition adapter. Native driver behavior and crash causation are separate.",
        "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (GLX_SOURCE, DRIVER_SOURCE, Path(__file__).resolve())},
        "extracted_bodies_sha256": {"glx_destroy": hashlib.sha256(destroy_glx.encode()).hexdigest(),
                                    "glx_make_current": hashlib.sha256(make_current.encode()).hexdigest(),
                                    "driver_destroy": hashlib.sha256(destroy_driver.encode()).hexdigest()},
        "controls": observations,
    }
    (work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if not passed: raise SystemExit(1)


if __name__ == "__main__":
    main()
