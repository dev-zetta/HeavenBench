#!/usr/bin/env python3
"""Execute the actual surface-color GLSL helper with independent numeric cases.

This host-only contract compiles the unchanged helper as C++ through a small
float3 arithmetic shim. It checks composition order and material wiring, not
GPU execution, texture binding, HDR output, or visual parity with Unigine.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


MATERIALS = (
    "heaven_lit", "heaven_lit_colored_penumbra", "heaven_lit_twosided",
    "heaven_lit_doublesided", "heaven_lit_clearcoat", "heaven_lit_aniso",
    "heaven_subsurface", "heaven_cloth", "heaven_window", "heaven_glass",
    "heaven_foliage_shadow",
)


SHIM = r"""
#include <algorithm>
#include <cmath>
#include <iostream>
struct float3 {
    float x, y, z;
    float3(float v) : x(v), y(v), z(v) {}
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
    float3& operator*=(float3 b) { x *= b.x; y *= b.y; z *= b.z; return *this; }
};
float3 operator+(float3 a, float3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
float3 operator-(float3 a, float3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
float3 operator*(float3 a, float3 b) { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
float3 max(float3 a, float3 b) {
    return {std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z)};
}
float3 sqrt(float3 a) { return {std::sqrt(a.x), std::sqrt(a.y), std::sqrt(a.z)}; }
float3 clamp(float3 a, float lo, float hi) {
    return {std::clamp(a.x,lo,hi), std::clamp(a.y,lo,hi), std::clamp(a.z,lo,hi)};
}
// Used only by the negative control that simulates decoding a legacy detail
// binding before its encoded-domain overlay has been evaluated.
float hardwareSrgbDecode(float x) {
    return x <= 0.04045f ? x/12.92f : std::pow((x+0.055f)/1.055f,2.4f);
}
float3 hardwareSrgbDecode(float3 x) {
    return {hardwareSrgbDecode(x.x), hardwareSrgbDecode(x.y), hardwareSrgbDecode(x.z)};
}
"""


ORACLES = r"""
int checks = 0;
int failures = 0;
void check(const char* name, float3 actual, float3 expected) {
    ++checks;
    if (!std::isfinite(actual.x) || !std::isfinite(actual.y) || !std::isfinite(actual.z)
            || std::fabs(actual.x-expected.x) > 0.000002f
            || std::fabs(actual.y-expected.y) > 0.000002f
            || std::fabs(actual.z-expected.z) > 0.000002f) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << actual.x << ',' << actual.y << ',' << actual.z
                  << " expected " << expected.x << ',' << expected.y << ',' << expected.z << '\n';
    }
}
int main() {
    // Fixed hand-calculated targets, independent of the helper implementation:
    // overlay (.5 + (.6*2-1)*.5)^2=.36, neutral .5^2=.25,
    // multiply (.5*(.3+1-.5))^2=.16. All RGB channels participate.
    check("legacy base", composeSurfaceColor(.5f,.6f,.5f,false,1,1), .25f);
    check("legacy overlay", composeSurfaceColor(.5f,.6f,.5f,true,1,3), .36f);
    check("legacy overlay UV1", composeSurfaceColor(.5f,.6f,.5f,true,2,3), .36f);
    check("legacy neutral detail", composeSurfaceColor(.5f,.5f,.5f,true,1,3), .25f);
    check("legacy multiply", composeSurfaceColor(.5f,.3f,.5f,true,3,3), .16f);
    check("legacy multiply UV1", composeSurfaceColor(.5f,.3f,.5f,true,4,3), .16f);
    check("channel independence", composeSurfaceColor({.2f,.5f,.8f},{.7f,.3f,.5f},.25f,true,1,3),
          {.09f,.16f,.64f});
    // Linear/procedural and already hardware-decoded input stays linear when
    // no legacy binding participates. No additional square is allowed.
    check("procedural no decode", composeSurfaceColor({.1f,.25f,.7f},.5f,.5f,false,0,0),
          {.1f,.25f,.7f});
    check("sRGB no double decode", composeSurfaceColor(.21404114f,.5f,.5f,false,0,0),
          .21404114f);
    check("linear overlay", composeSurfaceColor(.25f,.6f,.5f,true,1,0), .35f);
    check("linear multiply", composeSurfaceColor(.25f,.3f,.5f,true,3,0), .20f);
    check("sRGB detail without legacy", composeSurfaceColor(.25f,.5f,.5f,true,1,4), .25f);
    // Mixed gamma-2 domains use known square/square-root pairs. These do not
    // assert that an sRGB transfer curve equals the historical gamma-2 curve.
    check("linear base legacy detail", composeSurfaceColor(.25f,.6f,.5f,true,1,2), .36f);
    check("legacy base linear color detail", composeSurfaceColor(.5f,.36f,.5f,true,1,5), .36f);
    check("legacy base decoded neutral", composeSurfaceColor(.5f,.25f,.5f,true,1,5), .25f);
    check("legacy base raw signed detail", composeSurfaceColor(.5f,.6f,.5f,true,1,1), .36f);
    check("unused legacy detail flag", composeSurfaceColor(.25f,.6f,.5f,false,1,2), .25f);
    check("unused mixed detail flags", composeSurfaceColor(.25f,.6f,.5f,false,1,6), .25f);
    check("disabled detail preserves legacy base", composeSurfaceColor(.5f,.6f,.5f,false,1,3), .25f);
    // Overlay/factor clamps and the mixed-domain square-root guard must stay
    // finite. Clamp placement is evaluated before the final legacy square.
    check("overlay upper clamp", composeSurfaceColor(.9f,.9f,.9f,true,1,3), 1.0f);
    check("overlay lower clamp", composeSurfaceColor(.1f,0.0f,1.0f,true,1,3), 0.0f);
    check("multiply factor upper clamp", composeSurfaceColor(.5f,1.0f,0.0f,true,3,3), .25f);
    check("multiply factor lower clamp", composeSurfaceColor(.5f,0.0f,2.0f,true,3,3), 0.0f);
    check("mixed negative base guard", composeSurfaceColor(-.5f,.5f,.5f,true,1,2), 0.0f);
    check("mixed negative detail guard", composeSurfaceColor(.5f,-.5f,.5f,true,1,5), 0.0f);
    std::cout << checks << " numeric cases, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
"""


def strip_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def check_material_wiring(materials):
    expected = {name + ".mat" for name in MATERIALS}
    actual = set()
    for path in materials.glob("*.mat"):
        source = strip_comments(path.read_text())
        if '"includes/surface_color.glsl"' not in source:
            continue
        actual.add(path.name)
        declarations = re.findall(r"type\s*:\s*int\s*,\s*name\s*:\s*legacyColorFlags\b", source)
        calls = re.findall(r"\b\w+\.rgb\s*=\s*composeSurfaceColor\s*\((.*?)\)\s*;", source, re.S)
        if len(declarations) != 1 or len(calls) != 1:
            raise AssertionError(f"{path.name}: expected one integer flag declaration and RGB-only helper call")
        if "materialParams.legacyColorFlags" not in calls[0]:
            raise AssertionError(f"{path.name}: helper does not consume bound source-color flags")
    if actual != expected:
        raise AssertionError(f"Material color contract inventory differs: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}")


def inject(source, statement):
    pattern = r"(\bfloat3\s+composeSurfaceColor\s*\([^)]*\)\s*\{)"
    changed, count = re.subn(pattern, lambda match: match[0] + "\n" + statement, source, count=1)
    if count != 1:
        raise AssertionError("Missing actual composeSurfaceColor function")
    return changed


def compile_and_run(cxx, source, temporary, name):
    cpp = temporary / (name + ".cpp")
    executable = temporary / (name + ".exe")
    cpp.write_text(SHIM + "\n" + source + "\n" + ORACLES)
    if Path(cxx).stem.lower() in ("cl", "clang-cl"):
        command = [cxx, "/nologo", "/std:c++17", "/EHsc", str(cpp), "/Fe:" + str(executable)]
    else:
        command = [cxx, "-std=c++17", "-O0", str(cpp), "-o", str(executable)]
    compiled = subprocess.run(command, cwd=temporary, capture_output=True, text=True, timeout=30)
    if compiled.returncode or not executable.is_file():
        raise RuntimeError(f"Host compilation of actual helper failed ({name}):\n{compiled.stdout}{compiled.stderr}")
    return subprocess.run([str(executable)], cwd=temporary, capture_output=True, text=True, timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--materials", type=Path, default=Path(__file__).resolve().parents[1] / "materials")
    parser.add_argument("--cxx", default=None, help="Host C++ compiler; never an Android target compiler")
    args = parser.parse_args()
    cxx = args.cxx or next((path for name in ("c++", "clang++", "g++", "cl") if (path := shutil.which(name))), None)
    if not cxx:
        raise RuntimeError("No host C++ compiler found for actual-source surface-color contract")
    check_material_wiring(args.materials)
    source = (args.materials / "includes/surface_color.glsl").read_text()
    with tempfile.TemporaryDirectory(prefix="heaven-surface-color-contract-") as directory:
        temporary = Path(directory)
        actual = compile_and_run(cxx, source, temporary, "actual")
        if actual.returncode:
            raise AssertionError(f"Actual helper violates independent color oracles:\n{actual.stdout}{actual.stderr}")
        early_decode = inject(source, "if ((flags & 1) != 0) base = base * base;\n"
                              "if ((flags & 2) != 0) detail = detail * detail;\nflags &= ~3;")
        hardware_neutral = inject(source, "if (hasDetail && (flags & 2) != 0) detail = hardwareSrgbDecode(detail);")
        for name, mutant, expected_failure in (
                ("early-decode", early_decode, "FAIL legacy overlay"),
                ("hardware-neutral", hardware_neutral, "FAIL legacy neutral detail")):
            result = compile_and_run(cxx, mutant, temporary, name)
            if result.returncode == 0 or expected_failure not in result.stderr:
                raise AssertionError(f"Negative control did not expose the intended regression ({name}):\n{result.stdout}{result.stderr}")
    print(f"PASS: actual GLSL helper: {actual.stdout.strip()}; all {len(MATERIALS)} material flag/RGB routes; "
          "early-decode and hardware-sRGB-neutral negative controls rejected. Host math/source evidence only; no GPU or appearance claim.")


if __name__ == "__main__":
    main()
