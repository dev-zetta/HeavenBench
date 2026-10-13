#!/usr/bin/env python3
"""Run actual temporal GLSL fragment math against independent perspective cases.

The shim translates only the GLSL .xy swizzle syntax into a C++ accessor.
It is source/math evidence, not rasterization, skinning or GPU acceptance.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

SHIM = r"""
#include <algorithm>
#include <cmath>
#include <iostream>
struct float2 {
    float x,y;
    float2(float a):x(a),y(a){}
    float2(float a,float b):x(a),y(b){}
};
float2 operator/(float2 a,float b){return {a.x/b,a.y/b};}
float2 operator*(float2 a,float2 b){return {a.x*b.x,a.y*b.y};}
float2 operator+(float2 a,float b){return {a.x+b,a.y+b};}
float2 operator-(float2 a,float2 b){return {a.x-b.x,a.y-b.y};}
struct float4 {
    float x,y,z,w;
    float4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}
    float4(float2 a,float c,float d):x(a.x),y(a.y),z(c),w(d){}
    float2 xy()const{return {x,y};}
};
float max(float a,float b){return std::max(a,b);}
float saturate(float a){return std::clamp(a,0.0f,1.0f);}
float4 variable_currentClip(0,0,0,1),variable_previousClip(0,0,0,1);
struct {bool historyValid=true;} materialParams;
"""
ORACLES = r"""
int failures=0,checks=0;
void check(const char* name,float4 current,float4 previous,bool history,float4 expected){
    variable_currentClip=current;variable_previousClip=previous;materialParams.historyValid=history;
    const auto actual=temporalMotionFragment();++checks;
    const float a[]={actual.x,actual.y,actual.z,actual.w};
    const float e[]={expected.x,expected.y,expected.z,expected.w};
    for(int i=0;i<4;++i)if(!std::isfinite(a[i])||std::abs(a[i]-e[i])>2e-6f){
        ++failures;std::cerr<<"FAIL "<<name<<" component "<<i<<" actual "<<a[i]<<" expected "<<e[i]<<'\n';break;
    }
}
int main(){
    check("stationary",{.2f,-.3f,.5f,2},{.2f,-.3f,.5f,2},true,{0,0,.375f,1});
    // Horizontal translation of +8 source pixels at width 256 must address
    // the prior image eight pixels to the LEFT: -8/256 normalized units.
    check("right-eight-pixels",{-.75f,0,1,2},{-.875f,0,1,2},true,{-1.0f/32,0,.25f,1});
    // A downwards eight-pixel screen translation at height 192 has the same
    // negative lookup sign in top-left UV, despite positive-up GL clip space.
    check("down-eight-pixels",{0,-1.0f/6,1,2},{0,0,1,2},true,{0,-1.0f/24,.25f,1});
    check("homogeneous-invariance",{.5f,-.25f,0,2},{1,-.5f,0,4},true,{0,0,.5f,1});
    // Camera moves right by one world metre; an object two metres forward
    // moves left half an NDC unit with unit focal length. Prior lookup +.25.
    check("camera-translation",{-1,0,1,2},{0,0,1,2},true,{.25f,0,.25f,1});
    check("near-plane",{0,0,-.5f,.5f},{0,0,-.5f,.5f},true,{0,0,1,1});
    check("infinite-far-limit",{0,0,1,1},{0,0,1,1},true,{0,0,0,1});
    // Infinite reversed perspective must return near/distance, independently
    // derived from the geometric near-plane ratio rather than shader algebra.
    for(float distance:{.5f,1.0f,2.0f,1000.0f}){
        float4 clip(0,0,distance-1.0f,distance);
        check("depth-near-over-distance",clip,clip,true,{0,0,.5f/distance,1});
    }
    check("history-reset",{.5f,0,0,1},{-.5f,0,0,1},false,{0,0,.5f,0});
    check("prior-behind-camera",{0,0,0,1},{.5f,0,0,-1},true,{0,0,.5f,0});
    check("current-behind-camera",{.5f,0,0,-1},{0,0,0,1},true,{0,0,.5f,0});
    check("zero-projective-w",{0,0,0,0},{0,0,0,1},true,{0,0,.5f,0});
    std::cout<<checks<<" cases, "<<failures<<" failures\n";return failures?1:0;
}
"""


def compile_run(cxx, source, directory, name):
    cpp = directory / f"{name}.cpp"
    exe = directory / f"{name}.exe"
    cpp.write_text(SHIM + source.replace(".xy", ".xy()") + ORACLES)
    if Path(cxx).stem.lower() in {"cl", "clang-cl"}:
        cmd = [cxx, "/nologo", "/std:c++17", "/EHsc", str(cpp), "/Fe:" + str(exe)]
    else:
        cmd = [cxx, "-std=c++17", "-O0", str(cpp), "-o", str(exe)]
    result = subprocess.run(cmd, cwd=directory, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"Actual temporal helper host compilation failed:\n{result.stdout}{result.stderr}")
    return subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True, timeout=10)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--materials", type=Path, default=Path(__file__).resolve().parents[3] / "materials")
    p.add_argument("--cxx")
    a = p.parse_args()
    cxx = a.cxx or next((v for n in ("c++", "clang++", "g++", "cl") if (v := shutil.which(n))), None)
    if not cxx:
        raise SystemExit("No host C++ compiler available")
    source = (a.materials / "includes/temporal_motion_fragment.glsl").read_text()
    for name in ("heaven_motion", "heaven_motion_masked", "heaven_motion_grass"):
        text = (a.materials / f"{name}.mat").read_text()
        direct = "material.baseColor = temporalMotionFragment();" in text
        masked = "float4 payload = temporalMotionFragment();" in text and "material.baseColor = payload;" in text
        if '#include "includes/temporal_motion_fragment.glsl"' not in text or not (direct or masked):
            raise AssertionError(f"{name}: actual fragment helper not wired")
    mutants = [
        ("motion-sign", source.replace("previousUv - currentUv", "currentUv - previousUv"), "FAIL right-eight-pixels"),
        ("uv-y-axis", source.replace("float2(0.5, -0.5)", "float2(0.5, 0.5)"), "FAIL down-eight-pixels"),
        ("depth-convention", source.replace("0.5 - 0.5 * current.z", "0.5 + 0.5 * current.z"), "FAIL near-plane"),
        ("missing-reset", source.replace("materialParams.historyValid &&", "true &&"), "FAIL history-reset"),
    ]
    with tempfile.TemporaryDirectory(prefix="heaven-temporal-contract-") as temporary:
        directory = Path(temporary)
        actual = compile_run(cxx, source, directory, "actual")
        if actual.returncode:
            raise AssertionError(actual.stdout + actual.stderr)
        for name, mutant, expected in mutants:
            if mutant == source:
                raise AssertionError(f"Negative control did not change actual source: {name}")
            result = compile_run(cxx, mutant, directory, name)
            if result.returncode == 0 or expected not in result.stderr:
                raise AssertionError(f"Negative control failed: {name}\n{result.stdout}{result.stderr}")
    print(f"PASS: actual temporal fragment GLSL, {actual.stdout.strip()}; sign/Y/depth/reset negative controls rejected. Host source/math evidence only.")


if __name__ == "__main__":
    main()
