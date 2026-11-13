#!/usr/bin/env python3
"""Run actual IBL preparation and shader LOD math against independent numeric/sampler oracles.

The C++ fixture supplies only the surrounding Engine/IBL/UBO types. Production function bodies
are copied unchanged. The historical negative restores exactly the unsigned subtraction.
No Filament engine, shader compiler, Vulkan loader, texture or GPU is created.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "lib/filament/filament/src/ds/ColorPassDescriptorSet.cpp"
SHADER = ROOT / "lib/filament/shaders/src/surface_light_indirect.fs"
PREFIX = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
using std::size_t;
struct float3 { float x,y,z; };
struct float4 {
    float x=0,y=0,z=0,w=0;
    float4()=default;
    float4(float3 value,float alpha):x(value.x),y(value.y),z(value.z),w(alpha){}
};
template<class T> struct Handle {
    uint32_t value=0;
    explicit operator bool() const { return value!=0; }
};
struct HwTexture {};
enum class SamplerMagFilter { LINEAR };
enum class SamplerMinFilter { LINEAR_MIPMAP_LINEAR };
struct SamplerParams { SamplerMagFilter filterMag; SamplerMinFilter filterMin; };
enum class PerViewBindingPoints { IBL_SPECULAR=3 };
int operator+(PerViewBindingPoints value) { return static_cast<int>(value); }
struct FTexture { Handle<HwTexture> getHwHandle() const { return {123}; } };
struct FEngine { FTexture dummy; const FTexture* getDummyCubemap() const { return &dummy; } };
struct FIndirectLight {
    size_t levels=0;
    bool has_reflection=false;
    std::array<float3,9> sh{};
    size_t getLevelCount() const { return levels; }
    const float3* getSH() const { return sh.data(); }
    Handle<HwTexture> getReflectionHwHandle() const { return {has_reflection ? 456u : 0u}; }
};
struct Uniforms {
    float iblRoughnessOneLevel=-1, iblLuminance=-1;
    float4 iblSH[9]{};
};
struct UniformBuffer { Uniforms data; Uniforms& edit() { return data; } };
struct ColorPassDescriptorSet {
    UniformBuffer mUniforms;
    unsigned sampler_calls=0;
    uint32_t bound_texture=0;
    int bound_binding=-1;
    SamplerParams bound_sampler{};
    void setSampler(int binding,Handle<HwTexture> texture,SamplerParams params) {
        ++sampler_calls; bound_texture=texture.value; bound_binding=binding; bound_sampler=params;
    }
    void prepareAmbientLight(FEngine const& engine,FIndirectLight const& ibl,float intensity,float exposure) noexcept;
};
Uniforms frameUniforms;
'''
ORACLE = r'''
int main() {
    unsigned checks=0;
    // A zero reflection count represents the Engine's default/irradiance-only IBL, whose actual
    // sampler is the single-level dummy cubemap. Explicit reflected IBLs retain their authored mips.
    for(const auto levels:std::array<size_t,5>{0,1,2,7,255}) {
        const bool reflected=levels!=0;
        for(const float intensity:std::array<float,2>{0,200}) {
            constexpr float exposure=.125f;
            FEngine engine;
            FIndirectLight ibl; ibl.levels=levels; ibl.has_reflection=reflected;
            for(size_t i=0;i<ibl.sh.size();++i) ibl.sh[i]={float(i+1),float(i+2),float(i+3)};
            ColorPassDescriptorSet descriptor;
            descriptor.prepareAmbientLight(engine,ibl,intensity,exposure);
            const auto& uniforms=descriptor.mUniforms.data;
            const float expected_max_lod=levels==0 ? 0.f : static_cast<float>(levels-1);
            if(!std::isfinite(uniforms.iblRoughnessOneLevel) || uniforms.iblRoughnessOneLevel!=expected_max_lod) {
                std::fprintf(stderr,"FAIL: unsigned no-reflection LOD underflow: count=%zu actual=%.9g expected=%.9g\n",
                    levels,static_cast<double>(uniforms.iblRoughnessOneLevel),static_cast<double>(expected_max_lod)); return 1;
            }
            if(uniforms.iblLuminance!=intensity*exposure || descriptor.sampler_calls!=1 ||
                    descriptor.bound_texture!=(reflected ? 456u : 123u) ||
                    descriptor.bound_binding!=+PerViewBindingPoints::IBL_SPECULAR ||
                    descriptor.bound_sampler.filterMag!=SamplerMagFilter::LINEAR ||
                    descriptor.bound_sampler.filterMin!=SamplerMinFilter::LINEAR_MIPMAP_LINEAR) {
                std::fputs("FAIL: IBL intensity/exposure or reflection/dummy sampler changed\n",stderr); return 2;
            }
            for(size_t i=0;i<ibl.sh.size();++i) {
                const auto& input=ibl.sh[i]; const auto& output=uniforms.iblSH[i];
                if(input.x!=output.x || input.y!=output.y || input.z!=output.z || output.w!=0) {
                    std::fputs("FAIL: diffuse spherical harmonics changed\n",stderr); return 3;
                }
            }
            frameUniforms=uniforms;
            constexpr std::array<float,4> roughness{0,.25f,.5f,1};
            // Independent normalized quadratic weights are 0,7/16,3/4,1.
            constexpr std::array<float,4> expected_weights{0,7.f/16,3.f/4,1};
            for(size_t i=0;i<roughness.size();++i) {
                const float actual=perceptualRoughnessToLod(roughness[i]);
                const float expected=expected_max_lod*expected_weights[i];
                if(!std::isfinite(actual) || std::abs(actual-expected)>1e-6f || actual<0 || actual>expected_max_lod) {
                    std::fprintf(stderr,"FAIL: shader roughness LOD outside bound texture: count=%zu roughness=%.3g actual=%.9g expected=%.9g\n",
                        levels,static_cast<double>(roughness[i]),static_cast<double>(actual),static_cast<double>(expected)); return 4;
                }
                ++checks;
            }
        }
    }
    std::printf("PASS: %u actual shader LOD cases; zero/single/regular reflected mip counts, dummy sampler, unchanged diffuse SH and exposure/intensity; no GPU execution\n",checks);
}
'''


def raw_function(source, signature):
    start = source.index(signature)
    first = source.index("{", start)
    depth = 0
    for end in range(first, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError("Unbalanced actual source function")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    function = raw_function(SOURCE.read_text(), "void ColorPassDescriptorSet::prepareAmbientLight(")
    shader = raw_function(SHADER.read_text(), "float perceptualRoughnessToLod(")
    fixed_expression = "float(reflectionLevelCount - 1)"
    if function.count(fixed_expression) != 1:
        raise ValueError("Actual IBL LOD expression changed; explicit contract review required")
    historical = function.replace(fixed_expression, "float(ibl.getLevelCount() - 1)")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    records = []
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    for name, actual, expected, diagnostic in (
            ("production", function, 0, "PASS: 40 actual shader LOD cases"),
            ("historical-unsigned-underflow", historical, 1, "unsigned no-reflection LOD underflow: count=0")):
        directory = args.work_dir / name
        directory.mkdir(exist_ok=True)
        source = directory / "actual-ibl-lod.cpp"
        source.write_text(PREFIX + actual + "\n" + shader + ORACLE)
        executable = directory / ("contract.exe" if msvc else "contract")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", str(source),
                    "/Fe:" + str(executable), "/Fo:" + str(directory / "contract.obj")]
                   if msvc else [args.cxx, "-std=c++20", str(source), "-o", str(executable)])
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout)
        if compiled.returncode:
            raise ValueError(name + " did not compile; not a valid source control: " + compiled.stdout)
        result = subprocess.run([str(executable)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        (directory / "run.log").write_text(result.stdout)
        if result.returncode != expected or diagnostic not in result.stdout:
            raise ValueError(f"{name} missed its independent oracle: {result.returncode}\n{result.stdout}")
        records.append({"name": name, "exit_code": result.returncode, "expected_diagnostic": diagnostic, "passed": True})
    receipt = {"schema": 1, "sources_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in (SOURCE, SHADER, Path(__file__))},
               "actual_cpu_function_sha256": hashlib.sha256(function.encode()).hexdigest(),
               "actual_shader_function_sha256": hashlib.sha256(shader.encode()).hexdigest(),
               "compiler": args.cxx, "cases": records, "GPU_API_calls": 0,
               "limits": "Verbatim ambient-IBL preparation and shader LOD functions with fake surrounding types; numeric/sampler evidence only, not native reset/driver qualification."}
    (args.work_dir / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("IBL fallback LOD contract PASS: actual CPU/shader functions + specific historical unsigned-underflow negative; no GPU execution")


if __name__ == "__main__":
    main()
