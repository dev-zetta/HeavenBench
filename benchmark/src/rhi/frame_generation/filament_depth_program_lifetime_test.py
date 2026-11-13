#!/usr/bin/env python3
"""Execute actual Filament depth-release and ownership methods with a fake driver."""
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
#include <cstdlib>
#include <map>
#include <vector>
namespace utils {
UTILS_NORETURN
void panic(const char* function, const char* file, int line, const char* assertion) noexcept {
    std::fprintf(stderr, "Fixture assertion failed: %s | %s | %s:%d\n", function, assertion, file, line);
    std::fflush(stderr);
    std::_Exit(86);
}
}
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
struct FEngine;
struct MaterialCache {
    struct ProgramCacheEntry { Handle<HwProgram> program; };
    std::map<unsigned, ProgramCacheEntry> entries;
    unsigned released = 0;
    void releaseProgram(FEngine& engine, ProgramSpecialization const& key);
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
    struct Features {
        struct Engine { bool enable_program_cache = false; } engine;
    } features;
    Driver driver;
    MaterialCache cache;
    MaterialCache& getMaterialCache() { return cache; }
    Driver& getDriverApi() { return driver; }
    ShaderModel getShaderModel() const { return ShaderModel::DESKTOP; }
};
void MaterialCache::releaseProgram(FEngine& engine, ProgramSpecialization const& key) {
    auto it = entries.find(key.variant.key);
    if (it == entries.end()) { std::puts("missing cache reference"); std::abort(); }
    ++released;
    engine.getDriverApi().destroyProgram(it->second.program);
    entries.erase(it);
}
struct MaterialParser {
    unsigned identity = 1;
    unsigned getCrc32() const { return identity; }
    bool operator==(MaterialParser const&) const = default;
};
struct MaterialDefinition {
    bool hasCustomDepthShader = false;
    bool isVariantLit = true;
    MaterialDomain materialDomain = MaterialDomain::SURFACE;
    bool omit_geometry = false;
    MaterialParser parser;
    MaterialParser const* mMaterialParser = &parser;
    MaterialParser const& getMaterialParser() const { return parser; }
    Slice<const Variant> getVariants() const { return {}; }
    Slice<const Variant> getDepthVariants() const { return VariantUtils::getDepthVariants(); }
    bool isValidProgram(Variant v, DynamicSpecConstKey, ShaderModel, bool) const {
        return !omit_geometry || !(v.key & (Variant::SKN | Variant::STE));
    }
    void releasePrograms(FEngine&, Slice<Handle<HwProgram>>, MaterialParser const&,
            Slice<const Program::SpecializationConstant>, bool) const;
    // These ownership accessors are extracted unchanged from MaterialDefinition.h.
    __ACTUAL_OWNERSHIP_METHODS__
};
struct LocalProgramCache {
    using CacheKey = unsigned;
    static CacheKey mapCacheEntryKey(Variant v, DynamicSpecConstKey, size_t) { return v.key; }
};
'''

SUFFIX = r'''
} // namespace filament
int main(int argc, char**) {
    if (argc == 2) {
        assert_invariant(false);
        std::fputs("FAIL: actual Filament assertion was disabled\n", stderr);
        return 2;
    }
    using namespace filament;
    unsigned cases = 0;
    for (bool cache_enabled : {false, true}) {
        for (bool parser_mismatch : {false, true}) {
            for (bool default_material : {false, true}) {
                for (bool custom : {false, true}) {
                    for (bool omit_geometry : {false, true}) {
                        for (unsigned presence : {0u, 1u, 2u}) {
                            ++cases;
                            FEngine engine;
                            engine.features.engine.enable_program_cache = cache_enabled;
                            MaterialDefinition def;
                            def.hasCustomDepthShader = custom;
                            def.omit_geometry = omit_geometry;
                            MaterialParser runtime_parser{parser_mismatch ? 2u : 1u};
                            const bool use_cache = cache_enabled && !parser_mismatch;
                            std::array<Handle<HwProgram>, 64> programs{};
                            std::vector<unsigned> expected;
                            unsigned references = 0;
                            for (auto v : def.getDepthVariants()) {
                                if (!def.isValidProgram(v, {}, ShaderModel::DESKTOP, true)) continue;
                                ++references;
                                // Independent ownership oracle: ordinary/picking/motion depth
                                // borrows unless default/custom; MNT without PCK is owned here.
                                const bool moment = (v.key & 0x30u) == 0x20u;
                                const bool owned = default_material || custom || moment;
                                const bool present = presence == 2 ||
                                        (presence == 1 && (v.key & 0x04u));
                                if (present) {
                                    programs[v.key] = {1000u + v.key};
                                    if (use_cache || owned) expected.push_back(1000u + v.key);
                                }
                                if (use_cache) engine.cache.entries.emplace(v.key,
                                        MaterialCache::ProgramCacheEntry{programs[v.key]});
                            }
                            Slice<Handle<HwProgram>> slice(programs.data(), programs.size());
                            def.releasePrograms(engine, slice, runtime_parser, {}, default_material);
                            std::sort(expected.begin(), expected.end());
                            std::sort(engine.driver.retired.begin(), engine.driver.retired.end());
                            if (expected != engine.driver.retired) {
                                std::fprintf(stderr, "FAIL: depth ownership cache=%d mismatch=%d default=%d custom=%d omit=%d presence=%u expected=%zu retired=%zu\n",
                                        cache_enabled, parser_mismatch, default_material, custom,
                                        omit_geometry, presence, expected.size(), engine.driver.retired.size());
                                return 1;
                            }
                            for (const auto p : programs) if (p) {
                                std::puts("FAIL: retired or borrowed local program slot remains"); return 1;
                            }
                            if (use_cache && (!engine.cache.entries.empty() ||
                                    engine.cache.released != references)) {
                                std::puts("FAIL: cache references are unbalanced"); return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("PASS: actual releasePrograms/ownership, %u owned/borrowed/cache/parser/empty/filtered cases; no GPU\n", cases);
}
'''


def extract_function(source, marker):
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
    vendor = root / "lib/filament"
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = vendor / "filament/src/MaterialDefinition.cpp"
    header = vendor / "filament/src/MaterialDefinition.h"
    function = extract_function(source.read_text(), "void MaterialDefinition::releasePrograms(")
    ownership = "\n".join(extract_function(header.read_text(), marker) for marker in (
            "bool hasSurfaceVariants()", "bool isValidDepthVariant(",
            "bool isDepthMomentsVariant(", "bool isSharedVariant("))
    moment_condition = "!isDepthMomentsVariant(variant)"
    release_condition = "!isSharedVariant(variant, isDefaultMaterial) && program"
    if ownership.count(moment_condition) != 1 or function.count(release_condition) != 1:
        raise RuntimeError("Expected exact upstream depth ownership and release conditions")
    negatives = (
            ("old-moment-leak-negative", function, ownership.replace(moment_condition, "true")),
            ("destroy-borrowed-negative", function.replace(release_condition, "program"), ownership),
            ("omit-motion-negative", function.replace(release_condition,
                    "!isSharedVariant(variant, isDefaultMaterial) && "
                    "!Variant::isMotionVectorsVariant(variant) && program"), ownership))
    variants = (("production", function, ownership),) + negatives
    includes = ["filament/src", "filament/include", "filament/backend/include",
                "libs/filabridge/include", "libs/utils/include", "libs/math/include"]
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    # Actual DriverEnums.h uses C++20 designated initializers on every compiler.
    command = [args.cxx, "/nologo", "/std:c++20", "/O1", "/EHsc"] if msvc else [
            args.cxx, "-std=c++20", "-O1"]
    if args.stdlib == "libc++" and not msvc:
        command.append("-stdlib=libc++")
    command += [("/I" if msvc else "-I") + str(vendor / p) for p in includes]
    results = []
    for label, body, methods in variants:
        cpp = work / (label + ".cpp")
        exe = work / (label + (".exe" if msvc else ""))
        cpp.write_text(PREFIX.replace("__ACTUAL_OWNERSHIP_METHODS__", methods) + body + SUFFIX)
        cmd = command + [str(cpp), str(vendor / "libs/filabridge/src/Variant.cpp")]
        cmd += ["/Fe:" + str(exe), "/Fo" + str(work) + "/"] if msvc else ["-o", str(exe)]
        compiled = subprocess.run(cmd, capture_output=True, text=True, cwd=work)
        (work / (label + "-compile.log")).write_text(compiled.stdout + compiled.stderr)
        result = {"case": label, "command": cmd, "compile_exit": compiled.returncode}
        if not compiled.returncode:
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            (work / (label + "-run.log")).write_text(run.stdout + run.stderr)
            result["run_exit"] = run.returncode
            print(run.stdout + run.stderr, end="")
            probe = subprocess.run([str(exe), "--assertion-probe"], capture_output=True, text=True)
            (work / (label + "-assertion-probe.log")).write_text(probe.stdout + probe.stderr)
            result["assertion_probe_exit"] = probe.returncode
            result["assertion_probe_valid"] = probe.returncode == 86 and \
                "Fixture assertion failed: main | false |" in probe.stderr
            print(probe.stdout + probe.stderr, end="")
        else:
            print(compiled.stdout + compiled.stderr, end="")
        results.append(result)
    passed = results[0].get("run_exit") == 0 and \
        all(result.get("run_exit") == 1 for result in results[1:]) and \
        all(result.get("assertion_probe_valid", False) for result in results)
    (work / "receipt.json").write_text(json.dumps({"passed": passed, "gpu_executed": False,
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
            "function_sha256": hashlib.sha256(function.encode()).hexdigest(),
            "ownership_methods_sha256": hashlib.sha256(ownership.encode()).hexdigest(),
            "runs": results}, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
