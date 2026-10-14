#!/usr/bin/env python3
"""GPU-free DLSS prospective-ticket regressions and isolated source negatives."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


HERE = Path(__file__).resolve().parent


def run(command, log):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    log.write_text(result.stdout)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--sdk-source", type=Path)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    header = (HERE / "dlss_frame_lifecycle.h").read_text()
    fixture = (HERE / "dlss_frame_lifecycle_tests.cpp").read_text()
    variants = {
        "positive": (header, None),
        "per-attempt-acquisition": (header.replace("if (token_.native_token) return token_;", ""),
                                    "rejected attempts rotated SDK tokens"),
        "missing-reservation-guard": (header.replace("if (!slots_[next_slot_].expired()) return {};", ""),
                                      "presentation owner did not pin the SDK slot"),
        "repeated-markers": (header.replace(" || submission_started_", ""),
                             "accepted submission repeated Reflex boundaries"),
        "dropped-packet-lease": (header.replace("auto accepted = token_;", "auto accepted = token_; accepted.reservation.reset();"),
                                 "presentation owner did not pin the SDK slot"),
        "failed-acquire-retry": (header.replace("blocked_ = true;", "blocked_ = false;"),
                                  "failed SDK acquisition retried an uncertain ring position"),
    }
    observations = []
    for name, (variant, expected) in variants.items():
        directory = args.work_dir / name
        directory.mkdir(exist_ok=True)
        (directory / "dlss_frame_lifecycle.h").write_text(variant)
        source = directory / "dlss_frame_lifecycle_tests.cpp"
        source.write_text(fixture)
        output = directory / "test"
        msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", str(source), f"/Fe:{output}.exe", f"/Fo:{output}.obj"]
                   if msvc else [args.cxx, "-std=c++20", "-pthread", str(source), "-o", str(output)])
        compiled = run(command, directory / "compile.log")
        if compiled.returncode:
            raise RuntimeError(f"{name} did not compile; this cannot satisfy a negative control")
        executed = run([str(output) + (".exe" if msvc else "")], directory / "run.log")
        passed = (executed.returncode == 0 if expected is None else
                  executed.returncode == 1 and expected in executed.stdout)
        if not passed:
            raise RuntimeError(f"{name} did not satisfy its specific result: {executed.stdout}")
        observations.append({"name": name, "exit_code": executed.returncode,
                             "specific_failure": expected, "passed": passed})

    runtime = (HERE / "frame_generation_runtime.cpp").read_text()
    first = runtime.index("    if(width_!=width || height_!=height) {")
    last = runtime.index("    if(!motion_)", first)
    resize = runtime[first:last]
    policy_prefix = r'''
#include "dlss_frame_lifecycle.h"
#include <cstdio>
using namespace heaven;
enum class FrameGenerationMode { Dlss };
class Policy {
public:
    explicit Policy(DlssTokenReservations& provider) : provider_(provider) {}
    int width_=0,height_=0,drains=0,markers=0,empty_native=0,accepted=0,acquisitions=0;
    uint64_t last_source=0;
    FrameGenerationMode requested=FrameGenerationMode::Dlss;
    DlssProspectiveFrame prospective;
    int native_identity=1;
    void begin() { prospective.begin([this] {
        auto lease=provider_.reserve();
        if(!lease) return DlssFrameToken{};
        ++acquisitions; last_source=lease->logical_frame;
        return DlssFrameToken{&native_identity,last_source,static_cast<uint32_t>(last_source),lease};
    }); }
    void drain() { ++drains; prospective.cancel(); }
    static void invalidate_history() {}
    bool prepare(int width,int height) {
'''
    policy_suffix = r'''
        if(!prospective.current().native_token) ++empty_native;
        prospective.begin_submission([this](const auto& token,auto) {
            if(!token.native_token) ++empty_native;
            ++markers;
        });
        prospective.accept(); ++accepted;
        return true;
    }
private:
    DlssTokenReservations& provider_;
};
int main() {
    DlssTokenReservations provider;
    Policy first(provider);
    first.begin();
    if(first.prepare(64,64) || first.empty_native || first.markers || first.acquisitions!=1 || first.drains!=1) {
        std::puts("FAIL: initial dimensions submitted or reacquired a canceled ticket"); return 1;
    }
    first.begin();
    if(!first.prepare(64,64) || first.empty_native || first.markers!=2 || first.acquisitions!=2 || first.drains!=1) {
        std::puts("FAIL: initial dimensions did not recover on the next input boundary"); return 1;
    }
    first.begin();
    if(first.prepare(96,96) || first.empty_native || first.acquisitions!=3 || first.drains!=2) {
        std::puts("FAIL: resize submitted or reacquired a canceled ticket"); return 1;
    }
    first.begin();
    if(!first.prepare(96,96) || first.empty_native || first.drains!=2 || first.last_source!=4) {
        std::puts("FAIL: resize did not recover on the next input boundary"); return 1;
    }
    Policy reload(provider);
    reload.begin();
    if(reload.prepare(64,64) || reload.empty_native || reload.last_source!=5) {
        std::puts("FAIL: reload dimensions submitted a canceled ticket"); return 1;
    }
    reload.begin();
    if(!reload.prepare(64,64) || reload.empty_native || reload.last_source!=6) {
        std::puts("FAIL: reload did not preserve provider identity and recover"); return 1;
    }
    std::puts("PASS: actual resize/init/reload policy");
}
'''
    provider = (HERE / "dlss_provider.cpp").read_text()
    first = provider.index("    const auto frame_result", provider.index("bool DlssProvider::collect_state()"))
    last = provider.index("    const auto api_error", first)
    publication = provider[first:last]
    publication_prefix = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
namespace sl { enum class Result : int32_t { eOk, eErrorInvalidState }; }
struct Impl {
    std::atomic<int32_t> threaded_result{0};
    std::atomic<bool> token_index_exhausted{false};
    bool available=true;
    std::string operation;
    bool accept(sl::Result result,const char* name) {
        if(result==sl::Result::eOk) return true;
        available=false; operation=name; return false;
    }
};
struct Provider {
    Impl storage;
    Impl* impl_=&storage;
    int suspended=0,queries=0;
    void suspend() { ++suspended; }
    bool collect() {
'''
    publication_suffix = r'''
        ++queries; return true;
    }
};
int main() {
    for(bool exhausted : {false,true}) {
        Provider provider;
        provider.storage.threaded_result.store(static_cast<int32_t>(sl::Result::eErrorInvalidState));
        provider.storage.token_index_exhausted.store(exhausted);
        if(provider.collect() || provider.storage.available || provider.suspended!=1 || provider.queries) {
            std::puts("FAIL: no-token ordinary present did not publish acquisition failure"); return 1;
        }
        if(exhausted && provider.storage.operation.find("exhausted")==std::string::npos) {
            std::puts("FAIL: terminal identity exhaustion remained unavailable without its reason"); return 1;
        }
    }
    Provider success;
    if(!success.collect() || !success.storage.available || success.suspended || success.queries!=1) return 1;
    std::puts("PASS: actual ordinary-present acquisition-failure publication");
}
'''
    policies = [
        ("resize-policy", policy_prefix + resize + policy_suffix, None),
        ("resize-empty-token-negative", policy_prefix + resize.replace(
            "if(requested==FrameGenerationMode::Dlss) return false;", "") + policy_suffix,
         "initial dimensions submitted or reacquired a canceled ticket"),
        ("ordinary-present-failure", publication_prefix + publication + publication_suffix, None),
        ("ordinary-present-failure-negative", publication_prefix + publication_suffix,
         "no-token ordinary present did not publish acquisition failure"),
    ]
    for name, text, expected in policies:
        directory = args.work_dir / name
        directory.mkdir(exist_ok=True)
        (directory / "dlss_frame_lifecycle.h").write_text(header)
        source = directory / "test.cpp"
        source.write_text(text)
        output = directory / "test"
        definitions = ["/DHEAVEN_HAS_FSR3=0", "/DHEAVEN_HAS_DLSS=1"] if msvc else ["-DHEAVEN_HAS_FSR3=0", "-DHEAVEN_HAS_DLSS=1"]
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", str(source), f"/Fe:{output}.exe", f"/Fo:{output}.obj"]
                   if msvc else [args.cxx, "-std=c++20", "-pthread", str(source), "-o", str(output)])
        compiled = run(command + definitions, directory / "compile.log")
        if compiled.returncode:
            raise RuntimeError(f"{name} did not compile; this cannot satisfy a negative control")
        executed = run([str(output) + (".exe" if msvc else "")], directory / "run.log")
        passed = executed.returncode == 0 if expected is None else executed.returncode == 1 and expected in executed.stdout
        if not passed:
            raise RuntimeError(f"{name} failed its specific source-policy oracle: {executed.stdout}")
        observations.append({"name": name, "exit_code": executed.returncode,
                             "specific_failure": expected, "passed": passed})

    source_proof = None
    if args.sdk_source:
        original = args.sdk_source / "source/core/sl.api/sl.cpp"
        data = original.read_bytes()
        if hashlib.sha256(data).hexdigest() != "e01e90f4278eacc86d58e11545243f62a8be141cbfcb1a0bb0c28e637724e507":
            raise RuntimeError("SDK token source differs from the reviewed Streamline2.14.1 input")
        text = data.decode()
        start = text.index("    auto getFrame =", text.index("Result slGetNewFrameToken("))
        end = text.index("\n    SL_EXCEPTION_HANDLE_START;", start)
        body = text[start:end]
        if not re.search(r"MAX_FRAMES_IN_FLIGHT\s*=\s*6\s*;",
                         (args.sdk_source / "include/sl_consts.h").read_text()):
            raise RuntimeError("Pinned SDK ring capacity changed")
        replacement = """enum class Result { eOk, eErrorInvalidState };
struct SdkRing;
SdkRing* active_context = nullptr;
struct SdkRing {
    std::mutex mtxFrameHandle;
    uint32_t frameCounter = 0;
    uint32_t frameHandleIndex = 0;
    std::array<FrameToken, MAX_FRAMES_IN_FLIGHT> frameHandles;
    unsigned calls = 0;
    static Result slValidateState() { return Result::eOk; }
    FrameToken* acquire(uint32_t index) {
        ++calls;
        active_context = this;
#define s_ctx (*active_context)
#define SL_CHECK(expression) do { const auto status = (expression); if (status != Result::eOk) return status; } while (false)
""" + body + """
#undef SL_CHECK
#undef s_ctx
        FrameToken* result = nullptr;
        if (getFrame(result, &index) != Result::eOk) return nullptr;
        return result;
    }
};
"""
        first, last = fixture.index("struct SdkRing {"), fixture.index("struct Marker {")
        actual_fixture = fixture[:first] + replacement + fixture[last:]
        directory = args.work_dir / "pinned-sdk-source"
        directory.mkdir(exist_ok=True)
        (directory / "dlss_frame_lifecycle.h").write_text(header)
        source = directory / "test.cpp"
        source.write_text(actual_fixture)
        output = directory / "test"
        compiled = run([args.cxx, "-std=c++20", "-pthread", str(source), "-o", str(output)], directory / "compile.log")
        if compiled.returncode:
            raise RuntimeError("Actual pinned token allocator fixture did not compile")
        executed = run([str(output)], directory / "run.log")
        if executed.returncode:
            raise RuntimeError("Actual pinned token allocator rejected production lifecycle")
        source_proof = {"path": str(original), "sha256": hashlib.sha256(data).hexdigest(),
                        "exact_allocator_lambda_sha256": hashlib.sha256(body.encode()).hexdigest(),
                        "native_gpu_execution": False, "exit_code": executed.returncode}
    report = {"passed": True, "native_gpu_execution": False, "variants": observations,
              "pinned_source_proof": source_proof,
              "scope": "Mutable SDK token ownership, retry markers and failure handling; no NVIDIA runtime acceptance"}
    (args.work_dir / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
