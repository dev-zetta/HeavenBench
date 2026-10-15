#!/usr/bin/env python3
"""Execute actual trace source under deterministic worker-bookkeeping contention."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "benchmark/src/rhi/frame_generation/frame_generation_present_trace.cpp"
UPDATE = "++sample.wait_calls;"
START = "try { worker = std::thread([this] { observe(); }); }"
DECLARATIONS = "#include <mutex>\nextern std::mutex fixture_admission_mutex;\nvoid fixture_worker_gate();\n"
HARNESS = r'''
#include "frame_generation_present_trace.h"
#include "json.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
using namespace heaven;
using Json = nlohmann::json;
std::mutex fixture_admission_mutex;
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned> entries{0};
    bool entered=false,release=false,timed_out=false;
    unsigned producers_done=0;
} gate;
void fixture_worker_gate() {
    if(gate.entries.fetch_add(1)!=0)return;
    std::unique_lock lock(gate.mutex);
    gate.entered=true;gate.changed.notify_all();
    if(!gate.changed.wait_for(lock,10s,[]{return gate.release;}))gate.timed_out=true;
}
void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
struct WaitState {
    std::atomic<uint64_t> calls{0},allowed_chain{1};
    std::array<std::atomic<unsigned>,64> seen{};
    std::atomic<bool> invalid{false};
    static int32_t wait(void* userdata,uint64_t chain,uint64_t id,uint64_t timeout) {
        auto& state=*static_cast<WaitState*>(userdata);
        if(chain!=state.allowed_chain||!id||id>=state.seen.size()||timeout!=1'000'000)
            state.invalid=true;
        else if(state.seen[static_cast<size_t>(id)].fetch_add(1)!=0)state.invalid=true;
        ++state.calls;return 0;
    }
};
PresentTraceSubmission submission(uint64_t id,uint64_t chain=1) {
    PresentTraceSubmission sample;
    sample.chain=sample.native_chain=chain;sample.present_id=id;sample.source_frame=id;
    sample.kind=PresentTraceKind::Real;sample.present_mode=2;
    sample.submit_begin_ns=sample.submit_end_ns=FrameGenerationPresentTrace::now_ns();
    return sample;
}
Json read(const std::filesystem::path& path) {
    std::ifstream stream(path);return Json::parse(stream);
}
int main(int argc,char** argv) try {
    require(argc==3,"pass output directory and worker mode");
    const std::filesystem::path directory(argv[1]);std::filesystem::create_directories(directory);
    const auto path=directory/"trace.json";WaitState state;
    FrameGenerationPresentTrace trace(path);
    trace.configure(true,"Actual-source fixture capability",WaitState::wait,&state);
    if(std::string(argv[2])=="worker-unavailable") {
        require(!trace.available(),"Unavailable observer worker advertised availability");
        for(uint64_t id=1;id<=4;++id)trace.submitted(submission(id));
        trace.finish("Worker creation failed");
        const auto document=read(path);
        require(document.at("available")==false&&document.at("dropped_samples")==0&&
                document.at("samples").size()==4&&
                document.at("reason").get<std::string>().find("observer worker unavailable")!=std::string::npos,
                "Worker failure lost records or reason/capability");
        for(const auto& sample:document.at("samples"))
            require(sample.at("status")=="wait_failed","Worker failure invented completion");
        require(state.calls==0,"Unavailable worker queried a borrowed handle");
        std::puts("Actual trace worker failure: PASS (all identities retained, capability unavailable)");
        return 0;
    }
    trace.submitted(submission(1));
    {
        std::unique_lock lock(gate.mutex);
        require(gate.changed.wait_for(lock,2s,[]{return gate.entered;}),"Worker gate did not run");
    }
    std::array<std::jthread,4> producers;
    for(size_t index=0;index<producers.size();++index) {
        producers[index]=std::jthread([&,index] {
            for(uint64_t offset=0;offset<8;++offset)
                trace.submitted(submission(2+static_cast<uint64_t>(index)*8+offset));
            {std::scoped_lock lock(gate.mutex);++gate.producers_done;}
            gate.changed.notify_all();
        });
    }
    bool returned_before_worker_release=false;
    {
        std::unique_lock lock(gate.mutex);
        returned_before_worker_release=gate.changed.wait_for(lock,2s,[]{return gate.producers_done==4;});
        gate.release=true;
    }
    gate.changed.notify_all();
    for(auto& producer:producers)producer.join();
    trace.finish("Worker contention completed");
    require(returned_before_worker_release&&!gate.timed_out,"Submission blocked on worker bookkeeping");
    auto document=read(path);
    require(document.at("dropped_samples")==0&&document.at("samples").size()==33,
            "Submission identity lost under worker-bookkeeping contention");
    std::set<uint64_t> ids;
    for(const auto& sample:document.at("samples")) {
        const auto id=sample.at("present_id").get<uint64_t>();
        require(ids.insert(id).second&&sample.at("source_frame")==id&&
                sample.at("chain")==1&&sample.at("kind")=="real"&&
                (sample.at("status")=="khr_complete"||sample.at("status")=="unresolved_retired"),
                "Published identity/status corrupted");
    }
    require(*ids.begin()==1&&*ids.rbegin()==33,"Published identity missing or duplicated");
    const auto calls_before=state.calls.load();state.allowed_chain=2;
    trace.submitted(submission(34,2));
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(state.calls==calls_before&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    trace.finish("Restart completed");document=read(path);
    require(state.calls==calls_before+1&&!state.invalid,"Restart accessed an old native handle");
    require(document.at("samples").size()==34&&document.at("samples").back().at("chain")==2&&
            document.at("samples").back().at("status")=="khr_complete",
            "Submission after finish did not restart or preserve cursor");
    std::puts("Actual trace handoff: PASS (worker gate, concurrent producers, no loss/block, restart cursor)");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"Actual trace concurrency: FAIL: %s\n",error.what());return 1;
}
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def instrument(text, variant):
    if text.count(UPDATE) != 1:
        raise ValueError("Actual observer update anchor changed; review fixture explicitly")
    hook = "fixture_worker_gate();\n            " + UPDATE
    if variant == "old-mutex-admission":
        hook = "std::unique_lock fixture_hold(fixture_admission_mutex);\n            " + hook
        anchor = "if (!impl_) return;\n    // No observer mutex"
        if text.count(anchor) != 1:
            raise ValueError("Actual submission anchor changed; review negative explicitly")
        text = text.replace(anchor, "if (!impl_) return;\n"
            "    std::unique_lock fixture_lock(fixture_admission_mutex, std::try_to_lock);\n"
            "    if (!fixture_lock.owns_lock()) { ++impl_->dropped; return; }\n"
            "    // No observer mutex", 1)
    if variant == "worker-unavailable":
        if text.count(START) != 1:
            raise ValueError("Actual worker registration anchor changed; review fixture explicitly")
        text = text.replace(START, 'try { throw std::runtime_error("fixture worker creation failure"); }', 1)
    if variant == "restart-cursor":
        anchor = "void start() noexcept {"
        if text.count(anchor) != 1:
            raise ValueError("Actual registration anchor changed; review cursor negative explicitly")
        text = text.replace(anchor, anchor + "\n        imported_count = 0;", 1)
    return DECLARATIONS + text.replace(UPDATE, hook, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--compiler-id", default="not-supplied")
    parser.add_argument("--compiler-version", default="not-supplied")
    parser.add_argument("--cxx-flag", action="append", default=[])
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--old-source", type=Path)
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    current = SOURCE.read_text()
    variants = [("positive", current, None),
                ("old-mutex-admission", current,
                 "Submission identity lost under worker-bookkeeping contention"),
                ("worker-unavailable", current, None),
                ("restart-cursor", current, "Restart accessed an old native handle")]
    if args.old_source:
        variants.append(("actual-old-source", args.old_source.read_text(),
                         "Submission identity lost under worker-bookkeeping contention"))
    observations = []
    for name, text, expected_error in variants:
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "actual_trace.cpp"
        source.write_text(instrument(text, name), newline="\n")
        harness = directory / "fixture.cpp"
        harness.write_text(HARNESS, newline="\n")
        msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
        binary = directory / ("fixture.exe" if msvc else "fixture")
        includes = [ROOT / "benchmark/src/rhi/frame_generation", ROOT / "lib/nlohmann"]
        command = ([args.cxx, *args.cxx_flag, "/nologo", "/std:c++latest", "/EHsc", "/O2",
                    *["/I" + str(p) for p in includes], str(source),
                    str(harness), "/Fe:" + str(binary)] if msvc else
                   [args.cxx, *args.cxx_flag, "-std=c++23", "-O3", "-DNDEBUG", "-pthread",
                    *["-I" + str(p) for p in includes], str(source), str(harness), "-o", str(binary)])
        compiled = subprocess.run(command, cwd=directory, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        (directory / "compile.log").write_text(compiled.stdout)
        if compiled.returncode:
            raise RuntimeError("Actual trace fixture compile failed: " + compiled.stdout)
        result = subprocess.run([str(binary), str(directory / "data"),
                                 "worker-unavailable" if name == "worker-unavailable" else "contention"],
            cwd=directory, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (directory / "run.log").write_text(result.stdout)
        passed = ((result.returncode == 1 and expected_error in result.stdout) if expected_error else
                  result.returncode == 0)
        observations.append({"name": name, "command": command, "compile_exit": compiled.returncode,
            "run_exit": result.returncode, "expected_error": expected_error, "passed": passed,
            "actual_source_sha256": hashlib.sha256(text.encode()).hexdigest(),
            "instrumented_source_sha256": sha(source), "harness_sha256": sha(harness),
            "binary_sha256": sha(binary)})
        (work / "receipt.json").write_text(json.dumps({"source": str(SOURCE),
            "source_sha256": sha(SOURCE), "cases": observations,
            "configured_compiler": {"path": args.cxx, "id": args.compiler_id,
                "version": args.compiler_version, "native_flags": args.cxx_flag},
            "scope": "Actual copied CPU observer implementation; deterministic bookkeeping gate and source mutation negatives. No GPU, Vulkan device or physical timing acceptance."}, indent=2) + "\n")
        if not passed:
            raise RuntimeError(name + " oracle mismatch: " + result.stdout)
        print(name + ": PASS (compile0, run" + str(result.returncode) + ")")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
