#!/usr/bin/env python3
"""Run actual Filament froxel prepare/commit against a retiring fake command stream.

No graphics API, renderer build, or Filament source rewrite is performed. The
synthetic oracle supplies valid per-cell light lists independently of the upload
implementation. Both production methods are extracted verbatim for compilation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


# Exact pre-fix methods from Google Filament, Copyright 2015 The Android Open
# Source Project; retained under Apache-2.0. This immutable negative fixture
# executes complete methods rather than a hand-written old-path model.
ORIGINAL_SOURCE_SHA256 = "e319b51cb218095ebccb0a501cc83f18f3d9d18b154caf40645dd2222d4aabfc"
ORIGINAL_PREPARE_SHA256 = "a2ebc3d99a1f8eb0d46616adb8ffc67016c2897eed35bd549b8a399a9aefd8b8"
ORIGINAL_COMMIT_SHA256 = "d21b9914ca70f5b8933e6827a5d8ae12b0ea3a38f75591defa334512ad423a4b"
ORIGINAL_PREPARE = r'''
bool Froxelizer::prepare(
        FEngine::DriverApi& driverApi, LinearAllocatorArena& arena,
        filament::Viewport const& viewport,
        const mat4f& projection, float const projectionNear, float const projectionFar,
        float4 const& clipTransform) noexcept {
    assert_invariant(projectionFar > projectionNear);
    setViewport(viewport);
    setProjection(projection, projectionNear, projectionFar);

    // Only for debugging
    mClipTransform = clipTransform;

    bool uniformsNeedUpdating = false;
    if (UTILS_UNLIKELY(mDirtyFlags)) {
        uniformsNeedUpdating = update();
    }

    /*
     * Allocations that need to persist until the driver consumes them are done from
     * the command stream.
     */

    // froxel buffer (16 KiB with 4096 froxels)
    mFroxelBufferUser.set(
            driverApi.allocatePod<FroxelEntry>(mFroxelBufferEntryCount),
            mFroxelBufferEntryCount);

    // record buffer (64 KiB max)
    mRecordBufferUser.set(
            driverApi.allocatePod<RecordBufferType>(mFroxelRecordBufferEntryCount),
            mFroxelRecordBufferEntryCount);

    /*
     * Temporary allocations for processing all froxel data
     */

    // light records per froxel (~256 KiB with 4096 froxels)
    mLightRecords.set(
            arena.alloc<LightRecord>(getFroxelBufferEntryCount(), CACHELINE_SIZE),
            getFroxelBufferEntryCount());

    // froxel thread data (~256KiB with 8192 max froxels and 256 lights)
    mFroxelShardedData.set(
            arena.alloc<FroxelThreadData>(GROUP_COUNT, CACHELINE_SIZE),
            uint32_t(GROUP_COUNT));

    assert_invariant(mFroxelBufferUser.begin());
    assert_invariant(mRecordBufferUser.begin());
    assert_invariant(mLightRecords.begin());
    assert_invariant(mFroxelShardedData.begin());

    // initialize buffers that need to be
    memset(mLightRecords.data(), 0, mLightRecords.sizeInBytes());

    return uniformsNeedUpdating;
}
'''
ORIGINAL_COMMIT = r'''
void Froxelizer::commit(DriverApi& driverApi, LinearAllocatorArena& arena) {
    // send data to GPU
    driverApi.updateBufferObject(mFroxelsBuffer,
            { mFroxelBufferUser.data(), mFroxelBufferEntryCount * sizeof(FroxelEntry) }, 0);

    driverApi.updateBufferObject(mRecordsBuffer,
            { mRecordBufferUser.data(), mFroxelRecordBufferEntryCount }, 0);

    arena.free(mLightRecords.data(), mLightRecords.sizeInBytes());

    arena.free(mFroxelShardedData.data(), mFroxelShardedData.sizeInBytes());

    mFroxelBufferUser.clear();
    mRecordBufferUser.clear();
    mLightRecords.clear();
    mFroxelShardedData.clear();
}
'''


PREFIX = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>
#define UTILS_UNLIKELY(x) (x)
#define UTILS_UNUSED
#define assert_invariant(x) assert(x)
constexpr size_t CACHELINE_SIZE = 64;
namespace filament {
struct Viewport { uint32_t left, bottom, width, height; };
struct mat4f { float elements[16]{}; };
struct float4 { float elements[4]{}; };
template<class T> struct Slice {
    T* pointer{}; size_t count{};
    void set(T* p, size_t n) { pointer=p; count=n; }
    T* data() const { return pointer; }
    T* begin() const { return pointer; }
    size_t sizeInBytes() const { return count*sizeof(T); }
    void clear() { pointer=nullptr; count=0; }
};
struct LinearAllocatorArena {
    struct Block { void* allocation; unsigned char* pointer; size_t bytes; bool live; };
    std::vector<Block> blocks;
    size_t live_bytes=0, peak_bytes=0;
    ~LinearAllocatorArena() { for(auto const& b:blocks) ::operator delete(b.allocation); }
    template<class T> T* alloc(size_t n, size_t alignment) {
        const size_t bytes=n*sizeof(T);
        auto* allocation=static_cast<unsigned char*>(::operator new(bytes+alignment));
        const auto address=(reinterpret_cast<uintptr_t>(allocation)+alignment-1)&~uintptr_t(alignment-1);
        auto* pointer=reinterpret_cast<unsigned char*>(address);
        std::memset(pointer,0xC7,bytes);
        blocks.push_back({allocation,pointer,bytes,true});
        live_bytes+=bytes; peak_bytes=std::max(peak_bytes,live_bytes);
        return reinterpret_cast<T*>(pointer);
    }
    void free(void* p,size_t bytes) {
        auto found=std::find_if(blocks.begin(),blocks.end(),[&](auto const& b){return b.pointer==p&&b.live;});
        if(found==blocks.end()||found->bytes!=bytes) throw std::runtime_error("invalid arena release");
        // Logical release immediately destroys data, while physical allocation
        // remains safe for a delayed consumer to expose stale borrowing.
        std::memset(found->pointer,0xED,bytes); found->live=false; live_bytes-=bytes;
    }
};
struct BufferDescriptor { const void* buffer; size_t size; };
struct FakeDriver {
    static constexpr size_t slab=8192*4+65536;
    std::vector<unsigned char> stream=std::vector<unsigned char>(4*slab,0);
    struct Allocation { size_t begin,end; };
    std::vector<Allocation> live_allocations;
    std::vector<std::function<void()>> commands;
    std::array<std::vector<unsigned char>,2> uploaded;
    size_t cursor=0, wraps=0, flushes=0, consumers=0;
    template<class T> T* allocatePod(size_t n) {
        const size_t bytes=n*sizeof(T);
        if(bytes>stream.size()) throw std::runtime_error("oversized inline allocation");
        cursor=(cursor+alignof(T)-1)&~size_t(alignof(T)-1);
        if(cursor+bytes>stream.size()) { cursor=0; ++wraps; }
        for(auto const& a:live_allocations)
            if(cursor<a.end && a.begin<cursor+bytes) throw std::runtime_error("overwriting unretired stream payload");
        auto* pointer=stream.data()+cursor;
        live_allocations.push_back({cursor,cursor+bytes}); cursor+=bytes;
        // Actual CommandStream::allocate inserts a NoopCommand containing the
        // bytes. Processing that range does not retain them for a future range.
        commands.push_back([]{});
        return reinterpret_cast<T*>(pointer);
    }
    void updateBufferObject(unsigned handle,BufferDescriptor descriptor,unsigned offset) {
        if(handle<1||handle>2||offset!=0) throw std::runtime_error("unexpected upload destination");
        // Deliberately do not copy at enqueue time: VulkanDriver only reads
        // BufferDescriptor::buffer when the backend later executes this command.
        commands.push_back([this,handle,descriptor] {
            auto* p=static_cast<const unsigned char*>(descriptor.buffer);
            uploaded[handle-1].assign(p,p+descriptor.size); ++consumers;
        });
    }
    void flush_and_release() {
        for(auto& command:commands) command();
        commands.clear(); live_allocations.clear(); ++flushes;
    }
    void wrap_released_payloads(unsigned phase) {
        // Four independently flushed slabs cover every byte in the circular
        // store and necessarily reuse any prepare-time payload range.
        for(unsigned i=0;i<4;++i) {
            auto* p=allocatePod<unsigned char>(slab);
            std::memset(p,0x91+((phase+i)%29),slab);
            flush_and_release();
        }
    }
};
using DriverApi=FakeDriver;
struct FEngine { using DriverApi=FakeDriver; };
struct Froxelizer {
'''

MIDDLE = r'''
    using RecordBufferType=uint8_t;
    struct LightRecord { std::array<uint64_t,4> words; };
    struct FroxelThreadData { std::array<uint32_t,8192> words; };
    uint32_t mFroxelBufferEntryCount=8192;
    uint32_t mFroxelRecordBufferEntryCount=65536;
    Slice<FroxelEntry> mFroxelBufferUser;
    Slice<RecordBufferType> mRecordBufferUser;
    Slice<LightRecord> mLightRecords;
    Slice<FroxelThreadData> mFroxelShardedData;
    unsigned mFroxelsBuffer=1,mRecordsBuffer=2,mDirtyFlags=1;
    float4 mClipTransform;
    void setViewport(Viewport const&) {}
    void setProjection(mat4f const&,float,float) {}
    bool update() { mDirtyFlags=0; return true; }
    size_t getFroxelBufferEntryCount() const { return mFroxelBufferEntryCount; }
    bool prepare(FEngine::DriverApi&,LinearAllocatorArena&,Viewport const&,mat4f const&,float,float,float4 const&) noexcept;
    void commit(DriverApi&,LinearAllocatorArena&);
};
constexpr size_t GROUP_COUNT=8;
'''

SUFFIX = r'''
} // namespace filament
namespace {
struct Expected {
    std::vector<uint32_t> froxels=std::vector<uint32_t>(8192,0);
    std::vector<uint8_t> records=std::vector<uint8_t>(65536,0xD3);
};
Expected light_lists(unsigned phase) {
    Expected result;
    size_t cursor=0;
    for(size_t cell=0;cell<result.froxels.size();++cell) {
        const unsigned count=(cell%9==0)?0:1+(cell+phase)%4;
        // Independent shader-facing table oracle: each cell owns a list of
        // valid punctual indices, and its descriptor stores offset/count.
        result.froxels[cell]=(uint32_t(cursor)<<16)|count;
        for(unsigned j=0;j<count;++j)
            result.records[cursor++]=uint8_t((cell*17+j*53+phase*29)%255);
    }
    return result;
}
void completed_worker(filament::Froxelizer& f,Expected const& expected) {
    for(size_t i=0;i<expected.froxels.size();++i) f.mFroxelBufferUser.data()[i].u32=expected.froxels[i];
    std::memcpy(f.mRecordBufferUser.data(),expected.records.data(),expected.records.size());
}
bool check_bytes(std::vector<unsigned char> const& uploaded,const void* reference,size_t bytes,
                 const char* buffer,unsigned scenario,unsigned phase) {
    if(uploaded.size()!=bytes) {
        std::fprintf(stderr,"FAIL: %s size scenario=%u phase=%u got=%zu expected=%zu\n",buffer,scenario,phase,uploaded.size(),bytes);
        return false;
    }
    auto* expected=static_cast<const unsigned char*>(reference);
    auto mismatch=std::mismatch(uploaded.begin(),uploaded.end(),expected);
    if(mismatch.first!=uploaded.end()) {
        auto offset=size_t(mismatch.first-uploaded.begin());
        std::fprintf(stderr,"FAIL: %s bytes scenario=%u phase=%u offset=%zu got=%u expected=%u\n",
            buffer,scenario,phase,offset,unsigned(*mismatch.first),unsigned(expected[offset]));
        return false;
    }
    return true;
}
}
int main() {
    using namespace filament;
    unsigned failures=0,cases=0,control_failures=0;
    size_t peak=0,wraps=0,consumers=0;
    for(unsigned scenario=0;scenario<3;++scenario) {
        FakeDriver driver; Froxelizer froxelizer;
        for(unsigned phase=0;phase<3;++phase) {
            LinearAllocatorArena arena;
            auto expected=light_lists(phase);
            bool changed=froxelizer.prepare(driver,arena,{0,0,960,540},{},.5f,2000.f,{});
            if(changed!=(phase==0)) throw std::runtime_error("prepare dirty-state handling changed");
            // Scenario0 is a control with no intervening retirement. Scenario1
            // retires after worker completion; scenario2 retires while its
            // output is pending, joins it, then reuses storage again.
            if(scenario==2) { driver.flush_and_release(); driver.wrap_released_payloads(phase); }
            completed_worker(froxelizer,expected);
            if(scenario!=0) { driver.flush_and_release(); driver.wrap_released_payloads(phase+3); }
            const size_t consumed_before=driver.consumers;
            froxelizer.commit(driver,arena);
            if(driver.consumers!=consumed_before) throw std::runtime_error("fake upload was not delayed");
            if(arena.live_bytes) throw std::runtime_error("unreleased renderer arena scratch");
            if(froxelizer.mFroxelBufferUser.data()||froxelizer.mRecordBufferUser.data()||
               froxelizer.mLightRecords.data()||froxelizer.mFroxelShardedData.data())
                throw std::runtime_error("committed scratch slices not cleared");
            // Arena free poisoned every scratch block before these consumers;
            // successful bytes prove uploads own a live snapshot across reuse.
            driver.flush_and_release();
            if(driver.consumers!=consumed_before+2) throw std::runtime_error("missing or repeated delayed consumer");
            bool a=check_bytes(driver.uploaded[0],expected.froxels.data(),expected.froxels.size()*4,"froxel",scenario,phase);
            bool b=check_bytes(driver.uploaded[1],expected.records.data(),expected.records.size(),"record",scenario,phase);
            if(!a||!b) { ++failures; if(scenario==0) ++control_failures; }
            peak=std::max(peak,arena.peak_bytes); ++cases;
        }
        wraps+=driver.wraps; consumers+=driver.consumers;
    }
    std::printf("%s: actual Froxelizer prepare/commit cases=%u failures=%u control_failures=%u delayed_consumers=%zu wraps=%zu arena_peak=%zu output_scratch=%zu no_GPU\n",
        failures?"FAIL":"PASS",cases,failures,control_failures,consumers,wraps,peak,size_t(8192*4+65536));
    return failures?1:0;
}
'''


def extract_braced(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    # These production methods have no brace characters inside string literals.
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def negative_methods(prepare, commit, buffer):
    replacements = {
        "froxel": ("FroxelEntry", "mFroxelBufferEntryCount", "mFroxelBufferUser", "froxels", "mFroxelsBuffer"),
        "record": ("RecordBufferType", "mFroxelRecordBufferEntryCount", "mRecordBufferUser", "records", "mRecordsBuffer"),
    }
    for name in (["froxel", "record"] if buffer == "both" else [buffer]):
        kind, count, user, local, destination = replacements[name]
        allocation = f"arena.alloc<{kind}>({count}, CACHELINE_SIZE)"
        if prepare.count(allocation) != 1:
            raise RuntimeError("Unexpected production output allocation: " + name)
        prepare = prepare.replace(allocation, f"driverApi.allocatePod<{kind}>({count})")
        pattern = (rf"    auto\* {local} = driverApi\.allocatePod<{kind}>\({count}\);\n"
                   rf"    memcpy\({local}, {user}\.data\(\), {user}\.sizeInBytes\(\)\);\n")
        commit, n = re.subn(pattern, "", commit)
        if n != 1:
            raise RuntimeError("Unexpected production upload copy: " + name)
        commit = commit.replace(f"{{ {local}, {user}.sizeInBytes() }}", f"{{ {user}.data(), {user}.sizeInBytes() }}")
        release = f"    arena.free({user}.data(), {user}.sizeInBytes());\n"
        if commit.count(release) != 1:
            raise RuntimeError("Unexpected production scratch release: " + name)
        commit = commit.replace(release, "")
    return prepare, commit


def source_contracts(root):
    paths = {
        "renderer": "lib/filament/filament/src/details/Renderer.cpp",
        "render_pass": "lib/filament/filament/src/RenderPass.cpp",
        "stream": "lib/filament/filament/backend/include/private/backend/CommandStream.h",
        "engine": "lib/filament/filament/src/details/Engine.cpp",
        "queue": "lib/filament/filament/backend/src/CommandBufferQueue.cpp",
        "circular": "lib/filament/filament/backend/include/private/backend/CircularBuffer.h",
    }
    texts = {key: (root / path).read_text() for key, path in paths.items()}
    renderer = extract_braced(texts["renderer"], "void FRenderer::renderJob(")
    render_pass = extract_braced(texts["render_pass"], "void RenderPass::Executor::execute(FEngine const& engine, DriverApi& driver,\n        Command const*")
    stream = extract_braced(texts["stream"], "void* CommandStream::allocate(")
    engine = extract_braced(texts["engine"], "bool FEngine::execute()")
    queue = extract_braced(texts["queue"], "void CommandBufferQueue::releaseBuffer(")
    checks = {
        "prepare_precedes_shadow_render": renderer.index("view.prepare(engine") < renderer.index("view.renderShadowMaps("),
        "shadows_precede_join_and_commit": renderer.index("view.renderShadowMaps(") < renderer.index("view.commitFroxels(driver, arena)"),
        "join_precedes_commit": renderer.index("js.waitAndRelease(sync)") < renderer.index("view.commitFroxels(driver, arena)"),
        "capacity_and_half_full_flushes_exist": render_pass.count("const_cast<FEngine&>(engine).flush()") >= 2,
        "inline_payload_embedded_in_noop": "NoopCommand(p + s)" in stream and "allocateCommand(s)" in stream,
        "range_retired_after_execution": engine.index("driver.execute(item.begin)") < engine.index("mCommandBufferQueue.releaseBuffer(item)", engine.index("driver.execute(item.begin)")),
        "retirement_returns_range_space": "mFreeSpace += used" in queue,
        "circular_lifetime_contract": "make sure the returned range is no longer in use" in texts["circular"],
    }
    if not all(checks.values()):
        raise RuntimeError("Retiring-stream model source contract changed: " + repr(checks))
    return checks, {paths[key]: hashlib.sha256((root / paths[key]).read_bytes()).hexdigest() for key in paths}


def check_run(label, result, output):
    """Only the precise corruption signal is an acceptable negative result."""
    summary = re.fullmatch(
        r"(PASS|FAIL): actual Froxelizer prepare/commit cases=9 failures=(\d+) "
        r"control_failures=0 delayed_consumers=18 wraps=9 arena_peak=(\d+) "
        r"output_scratch=98304 no_GPU\n", output.stdout)
    expected_buffers = {
        "production": (),
        "old-inline-both-negative": ("froxel", "record"),
        "old-inline-froxel-negative": ("froxel",),
        "old-inline-record-negative": ("record",),
        "preserved-original-negative": ("froxel", "record"),
    }[label]
    expected_peak = 622592 - (32768 if "froxel" in expected_buffers else 0) - (65536 if "record" in expected_buffers else 0)
    expected_summary = ("FAIL", "6", str(expected_peak)) if expected_buffers else ("PASS", "0", str(expected_peak))
    expected_lines = set()
    for buffer in expected_buffers:
        for scenario in (1, 2):
            for phase in range(3):
                expected = 0 if buffer == "froxel" else 17 + 29 * phase
                expected_lines.add(f"FAIL: {buffer} bytes scenario={scenario} phase={phase} offset=0 got={151+phase} expected={expected}")
    actual_lines = output.stderr.splitlines()
    return (result == (1 if expected_buffers else 0) and summary is not None and
            summary.groups() == expected_summary and len(actual_lines) == len(expected_lines) and
            set(actual_lines) == expected_lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--stdlib", choices=("libc++", "default"), default="default")
    parser.add_argument("--original-source", type=Path, help="Optional preserved old source identity; no mutation")
    args = parser.parse_args()
    root, work = args.source_root.resolve(), args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = root / "lib/filament/filament/src/Froxelizer.cpp"
    header = root / "lib/filament/filament/src/Froxelizer.h"
    text, header_text = source.read_text(), header.read_text()
    prepare = extract_braced(text, "bool Froxelizer::prepare(")
    commit = extract_braced(text, "void Froxelizer::commit(")
    entry = extract_braced(header_text, "struct FroxelEntry {") + ";\n"
    checks, hashes = source_contracts(root)
    hashes[str(source.relative_to(root))] = hashlib.sha256(source.read_bytes()).hexdigest()
    hashes[str(header.relative_to(root))] = hashlib.sha256(header.read_bytes()).hexdigest()
    methods = [("production", prepare, commit)]
    for label, buffer in (("old-inline-both-negative", "both"), ("old-inline-froxel-negative", "froxel"), ("old-inline-record-negative", "record")):
        p, c = negative_methods(prepare, commit, buffer)
        methods.append((label, p, c))
    original_prepare, original_commit = ORIGINAL_PREPARE.strip(), ORIGINAL_COMMIT.strip()
    if (hashlib.sha256(original_prepare.encode()).hexdigest() != ORIGINAL_PREPARE_SHA256 or
            hashlib.sha256(original_commit.encode()).hexdigest() != ORIGINAL_COMMIT_SHA256):
        raise RuntimeError("Immutable pre-fix method fixture changed")
    original = {"path": "embedded complete pre-fix prepare/commit methods", "sha256": ORIGINAL_SOURCE_SHA256,
                "prepare_sha256": ORIGINAL_PREPARE_SHA256, "commit_sha256": ORIGINAL_COMMIT_SHA256}
    if args.original_source:
        old = args.original_source.read_text()
        old_prepare = extract_braced(old, "bool Froxelizer::prepare(")
        old_commit = extract_braced(old, "void Froxelizer::commit(")
        if old_prepare != original_prepare or old_commit != original_commit:
            raise RuntimeError("Supplied original methods differ from the immutable fixture")
        original["supplied_path"] = str(args.original_source.resolve())
        original["supplied_sha256"] = hashlib.sha256(args.original_source.read_bytes()).hexdigest()
    methods.append(("preserved-original-negative", original_prepare, original_commit))
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    compiler = [args.cxx, "/nologo", "/std:c++17", "/O1", "/EHsc"] if msvc else [args.cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    if args.stdlib == "libc++" and not msvc:
        compiler.append("-stdlib=libc++")
    results = []
    for label, p, c in methods:
        cpp, exe = work / (label + ".cpp"), work / (label + (".exe" if msvc else ""))
        cpp.write_text(PREFIX + entry + MIDDLE + p + "\n" + c + SUFFIX)
        command = compiler + [str(cpp)]
        command += ["/Fe:" + str(exe), "/Fo" + str(work) + "/"] if msvc else ["-o", str(exe)]
        compiled = subprocess.run(command, capture_output=True, text=True, cwd=work)
        (work / (label + "-compile.log")).write_text(compiled.stdout + compiled.stderr)
        result = {"case": label, "command": command, "compile_exit": compiled.returncode}
        if compiled.returncode == 0:
            run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=work)
            (work / (label + "-run.log")).write_text(run.stdout + run.stderr)
            result.update(run_exit=run.returncode, output=run.stdout + run.stderr,
                          expected_result=check_run(label, run.returncode, run))
            print(run.stdout + run.stderr, end="")
        else:
            print(compiled.stdout + compiled.stderr, end="")
        results.append(result)
    passed = all(r.get("compile_exit") == 0 and r.get("expected_result") is True for r in results)
    receipt = {"schema_version": 1, "passed": passed, "gpu_executed": False, "renderer_built": False,
               "source_sha256": hashes, "source_contracts": checks, "original_source": original,
               "extracted_prepare_sha256": hashlib.sha256(prepare.encode()).hexdigest(),
               "extracted_commit_sha256": hashlib.sha256(commit.encode()).hexdigest(),
               "oracle": "Independent valid punctual-light lists per cell; byte-exact descriptors and full record-buffer sentinel coverage",
               "lifetime_model": "Noop payload retirement, four-slab range reuse, joined worker, deferred upload consumer after poisoned arena free",
               "runs": results}
    (work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
