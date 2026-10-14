#!/usr/bin/env python3
"""Execute the actual Filament release template with a host-only fake driver."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


PREFIX = r'''
#include "DynamicSpecConstKey.h"
#include <private/filament/Variant.h>
#include <utils/Slice.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <vector>
namespace filament {
using backend::ShaderModel;
using utils::Slice;
struct HwProgram {};
template<typename T> struct Handle {
    unsigned value = 0;
    explicit operator bool() const { return value != 0; }
    void clear() { value = 0; }
};
struct Program { using SpecializationConstant = unsigned; };
struct ProgramSpecialization {
    unsigned materialCrc32;
    Variant variant;
    DynamicSpecConstKey specKey;
    Slice<const Program::SpecializationConstant> specializationConstants;
};
struct MaterialCache {
    struct ProgramCacheEntry { Handle<HwProgram> program; };
    struct ProgramCache {
        std::map<unsigned, ProgramCacheEntry> entries;
        unsigned released = 0;
        template<typename F> void release(ProgramSpecialization const& key, F fn) {
            auto it = entries.find(key.variant.key);
            if (it == entries.end()) { std::puts("missing cache reference"); std::abort(); }
            ++released;
            fn(it->second);
            entries.erase(it);
        }
    } cache;
    ProgramCache& getProgramCache() { return cache; }
};
struct Driver {
    std::vector<unsigned> retired;
    bool isStereoSupported() const { return true; }
    void destroyProgram(Handle<HwProgram> p) {
        if (!p) return;
        if (std::find(retired.begin(), retired.end(), p.value) != retired.end()) {
            std::puts("double program retirement"); std::abort();
        }
        retired.push_back(p.value);
    }
};
struct FEngine {
    Driver driver;
    MaterialCache cache;
    MaterialCache& getMaterialCache() { return cache; }
    Driver& getDriverApi() { return driver; }
    ShaderModel getShaderModel() const { return ShaderModel::DESKTOP; }
};
struct Parser { unsigned getCrc32() const { return 1; } };
struct MaterialDefinition {
    bool hasCustomDepthShader = false;
    bool isVariantLit = true;
    MaterialDomain materialDomain = MaterialDomain::SURFACE;
    bool omit_geometry = false;
    Parser parser;
    Parser const& getMaterialParser() const { return parser; }
    Slice<const Variant> getVariants() const { return {}; }
    Slice<const Variant> getDepthVariants() const { return VariantUtils::getDepthVariants(); }
    bool isValidProgram(Variant v, DynamicSpecConstKey, ShaderModel, bool) const {
        return !omit_geometry || !(v.key & (Variant::SKN | Variant::STE));
    }
};
struct LocalProgramCache {
    using CacheKey = unsigned;
    static CacheKey mapCacheEntryKey(Variant v, DynamicSpecConstKey, size_t) { return v.key; }
};
'''

SUFFIX = r'''
} // namespace filament
int main() {
    using namespace filament;
    unsigned cases = 0;
    for (bool cache_enabled : {false, true}) {
        for (bool default_material : {false, true}) {
            for (bool custom : {false, true}) {
                for (bool omit_geometry : {false, true}) {
                    for (unsigned presence : {0u, 1u, 2u}) {
                        ++cases;
                        FEngine engine;
                        MaterialDefinition def;
                        def.hasCustomDepthShader = custom;
                        def.omit_geometry = omit_geometry;
                        std::array<Handle<HwProgram>, 128> programs{};
                        std::vector<unsigned> expected;
                        unsigned references = 0;
                        for (auto v : def.getDepthVariants()) {
                            if (!def.isValidProgram(v, {}, ShaderModel::DESKTOP, true)) continue;
                            ++references;
                            // The independent ownership oracle models FMaterial::isSharedVariant:
                            // DEP/PCK borrow unless default/custom; MNT programs always belong here.
                            const bool owned = default_material || custom || (v.key & 0x40u);
                            const bool present = presence == 2 || (presence == 1 && (v.key & 0x08));
                            if (present) {
                                programs[v.key] = {1000u + v.key};
                                if (cache_enabled || owned) expected.push_back(1000u + v.key);
                            }
                            if (cache_enabled) engine.cache.cache.entries.emplace(v.key,
                                    MaterialCache::ProgramCacheEntry{programs[v.key]});
                        }
                        Slice<Handle<HwProgram>> slice(programs.data(), programs.size());
                        if (cache_enabled) releaseProgramsImpl<true>(engine, slice, def, {}, default_material);
                        else releaseProgramsImpl<false>(engine, slice, def, {}, default_material);
                        std::sort(expected.begin(), expected.end());
                        std::sort(engine.driver.retired.begin(), engine.driver.retired.end());
                        if (expected != engine.driver.retired) {
                            std::fprintf(stderr, "FAIL: depth ownership cache=%d default=%d custom=%d omit=%d presence=%u expected=%zu retired=%zu\n",
                                    cache_enabled, default_material, custom, omit_geometry, presence,
                                    expected.size(), engine.driver.retired.size());
                            return 1;
                        }
                        for (const auto p : programs) if (p) {
                            std::puts("FAIL: retired or borrowed local program slot remains"); return 1;
                        }
                        if (cache_enabled && (!engine.cache.cache.entries.empty() ||
                                engine.cache.cache.released != references)) {
                            std::puts("FAIL: cache references are unbalanced"); return 1;
                        }
                    }
                }
            }
        }
    }
    std::printf("PASS: actual releaseProgramsImpl, %u owned/borrowed/cache/empty/filtered cases; no GPU\n", cases);
}
'''


def extract_release(source):
    marker = "template<bool useCache>\nvoid releaseProgramsImpl("
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--stdlib", choices=("libc++", "default"), default="default")
    args = parser.parse_args()
    root = args.source_root.resolve()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = root / "lib/filament/filament/src/MaterialDefinition.cpp"
    function = extract_release(source.read_text())
    fixed = "(destroySharedVariants || Variant::isDepthMomentsVariant(variant)) && program"
    if function.count(fixed) != 1:
        raise RuntimeError("Expected exactly one owned-moment depth release condition")
    negative = function.replace(fixed, "destroySharedVariants && program")
    includes = ["filament/src", "filament/include", "filament/backend/include",
                "libs/filabridge/include", "libs/utils/include", "libs/math/include"]
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    # Actual DriverEnums.h uses C++20 designated initializers on every compiler.
    command = [args.cxx, "/nologo", "/std:c++20", "/O1", "/EHsc"] if msvc else [
            args.cxx, "-std=c++20", "-O1"]
    if args.stdlib == "libc++" and not msvc:
        command.append("-stdlib=libc++")
    command += [("/I" if msvc else "-I") + str(root / "lib/filament" / p) for p in includes]
    results = []
    for label, body in (("production", function), ("old-condition-negative", negative)):
        cpp = work / (label + ".cpp")
        exe = work / (label + (".exe" if msvc else ""))
        cpp.write_text(PREFIX + body + SUFFIX)
        cmd = command + [str(cpp), str(root / "lib/filament/libs/filabridge/src/Variant.cpp")]
        cmd += ["/Fe:" + str(exe), "/Fo" + str(work) + "/"] if msvc else ["-o", str(exe)]
        compiled = subprocess.run(cmd, capture_output=True, text=True, cwd=work)
        (work / (label + "-compile.log")).write_text(compiled.stdout + compiled.stderr)
        result = {"case": label, "command": cmd, "compile_exit": compiled.returncode}
        if not compiled.returncode:
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            (work / (label + "-run.log")).write_text(run.stdout + run.stderr)
            result["run_exit"] = run.returncode
            print(run.stdout + run.stderr, end="")
        else:
            print(compiled.stdout + compiled.stderr, end="")
        results.append(result)
    passed = results[0].get("run_exit") == 0 and results[1].get("run_exit") == 1
    (work / "receipt.json").write_text(json.dumps({"passed": passed, "gpu_executed": False,
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "function_sha256": hashlib.sha256(function.encode()).hexdigest(),
            "runs": results}, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
