#!/usr/bin/env python3
"""Execute the actual directional-contact helper policy and call site on CPU.

The extracted GLSL predicate and directional evaluator are compiled as C++ with
small lighting/map/contact stubs. An independent state oracle checks visible
outputs, map work and requested contact work. Source-order controls keep the
predicate after derivative/material/map work. This bounded host contract does
not prove shader compiler derivative behavior, helper execution masks, native
texture transactions, rendered pixels or a performance improvement.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


NAME = "shouldTraceDirectionalContactShadow"
FILES = (
    "surface_light_directional.fs", "surface_shadowing.fs", "surface_light_punctual.fs",
    "surface_main.fs", "surface_shading_parameters.fs", "surface_shading_lit.fs",
)
MACROS = ("TARGET_VULKAN_ENVIRONMENT", "TARGET_MOBILE",
          "MATERIAL_HAS_CUSTOM_SURFACE_SHADING", "HAS_CUSTOM_OUTPUT")


def require(value, message):
    if not value:
        raise ValueError(message)


def strip_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def paired(source, start, opening="(", closing=")"):
    require(source[start] == opening, "missing opening delimiter")
    depth = 0
    for end in range(start, len(source)):
        if source[end] == opening:
            depth += 1
        elif source[end] == closing:
            depth -= 1
            if depth == 0:
                return end + 1
    raise ValueError("unclosed source delimiter")


def function(source, name):
    matches = list(re.finditer(r"\b(?:bool|void|float|vec4)\s+" + re.escape(name) + r"\s*\(", source))
    require(len(matches) == 1, "missing or duplicate actual function: " + name)
    start = matches[0].start()
    arguments = paired(source, source.index("(", matches[0].start()))
    brace = source.index("{", arguments)
    end = paired(source, brace, "{", "}")
    return source[start:end]


def compact(source):
    return re.sub(r"\s+", "", source.replace("\\\n", ""))


def source_contract(units):
    directional = strip_comments(units[FILES[0]])
    predicate = function(directional, NAME)
    evaluator = function(directional, "evaluateDirectionalLight")
    require(directional.index(predicate) < directional.index(evaluator), "helper must precede directional evaluator")
    require(len(re.findall(r"\b" + NAME + r"\s*\(", directional)) == 2,
            "helper must have exactly one definition and one live call site")
    require(not re.search(r"\b(?:discard|atomic\w*|imageStore|dFdx|dFdy|fwidth)\b", predicate),
            "policy must be a side-effect-free predicate")
    require("gl_HelperInvocation" not in directional.replace(predicate, "", 1),
            "helper built-in escaped its bounded predicate")

    # Extract the entire actual if around the call; compiling only the helper
    # would miss a dead policy result or a receiver-flag bypass.
    call = evaluator.index(NAME + "(") if NAME + "(" in evaluator else evaluator.index(NAME + " (")
    candidates = []
    for match in re.finditer(r"\bif\s*\(", evaluator):
        begin = evaluator.index("(", match.start())
        end = paired(evaluator, begin)
        if begin < call < end:
            candidates.append((match.start(), evaluator[begin + 1:end - 1], end))
    require(len(candidates) == 1, "helper must be used directly in the receiver conditional")
    guard_start, condition, condition_end = candidates[0]
    require("object_uniforms_flagsChannels" in condition
            and "FILAMENT_OBJECT_CONTACT_SHADOWS_BIT" in condition,
            "helper guard must retain the actual contact-receiver flag")
    body_start = evaluator.index("{", condition_end)
    body_end = paired(evaluator, body_start, "{", "}")
    contact_body = evaluator[body_start:body_end]
    for ray_function in ("screenSpaceSunContactShadow", "screenSpaceContactShadow"):
        require(len(re.findall(r"\b" + ray_function + r"\s*\(\s*light\.l\s*\)", contact_body)) == 1,
                "original Sun/fallback contact call must remain in the guarded body")
        require(len(re.findall(r"\b" + ray_function + r"\s*\(", evaluator)) == 1,
                "contact work escaped its guarded body")
    map_sample = evaluator.index("visibility = shadow(")
    require(map_sample < guard_start < evaluator.index("visibility *= 1.0 - ssContactShadowOcclusion"),
            "helper may suppress contact only after original map sampling")
    require(evaluator.index("if (light.NoL > 0.0)") < map_sample,
            "NoL gate must retain the original map/contact scope")
    require("directionalShadows & 0x2" in evaluator[:guard_start]
            and "visibility > 0.0" in evaluator[:guard_start],
            "contact enable/positive map-visibility gates must precede helper policy")
    for other in FILES[1:]:
        require(NAME not in strip_comments(units[other]), "helper gate escaped directional contact into " + other)
        require("gl_HelperInvocation" not in strip_comments(units[other]),
                "helper built-in escaped directional policy into " + other)
    require("gl_HelperInvocation" not in function(directional, "screenSpaceSunContactShadow"),
            "per-ray/Sun algorithms must not gain a second helper policy")
    require("gl_HelperInvocation" not in strip_comments(units["surface_shadowing.fs"]),
            "fine marcher must retain its helper-independent implementation")
    main = function(strip_comments(units["surface_main.fs"]), "main")
    require(main.index("computeShadingParams();") < main.index("material(inputs);")
            < main.index("applyAlphaMask(inputs.baseColor);") < main.index("evaluateMaterial(inputs)"),
            "geometry/material/alpha derivatives must remain before lighting")
    shading = strip_comments(units["surface_shading_parameters.fs"])
    require("cross(dFdx(shading_position), dFdy(shading_position))" in shading,
            "actual receiver-plane derivatives must be preserved")
    lights = function(strip_comments(units["surface_shading_lit.fs"]), "evaluateLights")
    require(lights.index("getPixelParams(material, pixel);")
            < lights.index("evaluateIBL(material, pixel, color);")
            < lights.index("evaluateDirectionalLight(material, pixel, color);")
            < lights.index("evaluatePunctualLights(material, pixel, color);"),
            "pixel-parameter/specular-AA/IBL work must precede directional contact")
    return predicate, evaluator


SHIM = r"""
struct vec3 {
    float x, y, z;
    vec3& operator+=(vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
};
struct vec4 { float x=0, y=0, z=0, w=0; };
struct MaterialInputs {};
struct PixelParams {};
struct Light { float NoL; int channels; float l; };
struct FrameUniforms { int directionalShadows; int cascades; } frameUniforms;
bool gl_HelperInvocation;
int object_uniforms_flagsChannels;
constexpr int FILAMENT_OBJECT_CONTACT_SHADOWS_BIT = 1 << 8;
constexpr int sampler0_shadowMap = 0;
Light currentLight;
float shadowVisibility, contactOcclusion;
int mapCalls, contactCalls, sunCalls, rayCalls, stepCalls;
Light getDirectionalLight() { return currentLight; }
int getShadowCascade() { return 0; }
vec4 getShadowPosition(int) { return {}; }
float shadow(bool, int, int, vec4, float) { ++mapCalls; return shadowVisibility; }
float screenSpaceSunContactShadow(float) {
    ++contactCalls; ++sunCalls; rayCalls += 16; stepCalls += 16 * 32;
    return contactOcclusion;
}
float screenSpaceContactShadow(float) {
    ++contactCalls; ++rayCalls; stepCalls += 32;
    return contactOcclusion;
}
vec3 radiance(Light light, float visibility, bool custom) {
    const float n = light.NoL > 0 ? light.NoL : 0;
    const float scale = custom ? .75f : 1.f;
    return {n * visibility * scale, n * visibility * .5f * scale,
            n * visibility * .25f * scale};
}
vec3 surfaceShading(PixelParams, Light light, float visibility) {
    return radiance(light, visibility, false);
}
vec3 customSurfaceShading(MaterialInputs, PixelParams, Light light, float visibility) {
    return radiance(light, visibility, true);
}
"""


def check_program(predicate, evaluator):
    evaluator = evaluator.replace("inout vec3 color", "vec3& color").replace("color.rgb", "color")
    evaluator = re.sub(r"\bhighp\s+", "", evaluator)
    pieces = ["#include <cstdint>\n#include <cstring>\n#include <initializer_list>\n#include <iostream>\n"]
    invocations = []
    for profile in range(16):
        for sun in (False, True):
            name = f"p{profile}_sun{int(sun)}"
            define = [f"#define {macro} 1\n" for index, macro in enumerate(MACROS) if profile & (1 << index)]
            define += ["#define VARIANT_HAS_SHADOWING 1\n#define MATERIAL_CAN_SKIP_LIGHTING 1\n#define MATERIAL_FEATURE_LEVEL 1\n"]
            if sun:
                define += ["#define SUN_AS_AREA_LIGHT 1\n#define HAS_ATTRIBUTE_TANGENTS 1\n"]
            pieces += define
            pieces += [f"namespace {name} {{\n", SHIM, predicate, "\n", evaluator, "\n"]
            eligible = (profile & 1) != 0 and (profile & 14) == 0
            custom = (profile & 4) != 0
            pieces += [f"constexpr bool eligible = {'true' if eligible else 'false'};\n",
                       f"constexpr bool custom = {'true' if custom else 'false'};\n",
                       f"constexpr bool sunRoute = {'true' if sun else 'false'};\n", r"""
int test(std::uint64_t& checks) {
    const float normals[] = {-.25f, 0.f, .25f, 1.f};
    const float shadows[] = {0.f, .5f, 1.f};
    const float contacts[] = {0.f, .25f, .5f, .75f, 1.f};
    for (bool helper : {false,true}) for (bool receiver : {false,true})
    for (bool enabled : {false,true}) for (bool mapped : {false,true})
    for (bool cascadeVisible : {false,true}) for (bool channel : {false,true})
    for (float nol : normals) for (float mapVisibility : shadows)
    for (float occlusion : contacts) {
        ++checks;
        gl_HelperInvocation = helper;
        if (shouldTraceDirectionalContactShadow() != (!eligible || !helper)) return 1;
        currentLight = {nol, channel ? 1 : 2, 17.f};
        object_uniforms_flagsChannels = 1 | (receiver ? FILAMENT_OBJECT_CONTACT_SHADOWS_BIT : 0);
        frameUniforms = {(32 << 8) | (mapped ? 1 : 0) | (enabled ? 2 : 0),
                         cascadeVisible ? (1 << 8) : 0};
        shadowVisibility = mapVisibility;
        contactOcclusion = occlusion;
        mapCalls = contactCalls = sunCalls = rayCalls = stepCalls = 0;
        vec3 actual{.125f, .25f, .5f};
        evaluateDirectionalLight({}, {}, actual);
        const bool lit = channel && nol > 0.f;
        const bool expectedMap = lit && mapped && cascadeVisible;
        float visibility = expectedMap ? mapVisibility : 1.f;
        const bool expectedContact = lit && enabled && receiver && visibility > 0.f && (!eligible || !helper);
        if (expectedContact) visibility *= 1.f - occlusion;
        const int expectedRays = expectedContact ? (sunRoute ? 16 : 1) : 0;
        if (mapCalls != int(expectedMap) || contactCalls != int(expectedContact)
                || sunCalls != int(expectedContact && sunRoute)
                || rayCalls != expectedRays || stepCalls != expectedRays * 32) return 2;
        // Eligible helpers have no framebuffer result; this host test does
        // not claim that C++ stubs establish actual GPU helper semantics.
        if (!helper || !eligible) {
            const float scale = custom ? .75f : 1.f;
            const float strength = lit ? nol * visibility : 0.f;
            const vec3 expected{.125f + strength * scale,
                                .25f + strength * .5f * scale,
                                .5f + strength * .25f * scale};
            if (std::memcmp(&actual, &expected, sizeof(vec3)) != 0) return 3;
        }
    }
    return 0;
}
}
"""]
            pieces += [f"#undef {macro}\n" for macro in MACROS]
            pieces += ["#undef VARIANT_HAS_SHADOWING\n#undef MATERIAL_CAN_SKIP_LIGHTING\n#undef MATERIAL_FEATURE_LEVEL\n#undef SUN_AS_AREA_LIGHT\n#undef HAS_ATTRIBUTE_TANGENTS\n"]
            invocations.append(f'if (int error = {name}::test(checks)) {{ std::cerr << "{name} failure " << error << " at " << checks << "\\n"; return error; }}')
    pieces += ["int main() { std::uint64_t checks=0;\n", "\n".join(invocations),
               '\nstd::cout << checks << " policy/callsite cases passed\\n"; return 0; }\n']
    return "".join(pieces)


def compile_command(cxx, source, executable):
    compiler = Path(str(cxx).replace("\\", "/")).stem.lower()
    if compiler in ("cl", "clang-cl"):
        return [cxx, "/nologo", "/std:c++17", "/EHsc", "/Od", "/W4", "/WX", "/wd4100", str(source),
                "/Fe:" + str(executable), "/Fo:" + str(executable.with_suffix(".obj"))]
    return [cxx, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            str(source), "-o", str(executable)]


def command_construction_contract(directory):
    """Exact argv fixtures; these deliberately do not execute native MSVC."""
    source = directory / "private source.cpp"
    executable = directory / "private binary.exe"
    checks = []
    for compiler in ("C:/Program Files/MSVC/cl.exe", r"C:\MSVC\bin\cl.exe",
                     "C:/Program Files/LLVM/clang-cl.exe", "/opt/llvm/bin/clang++", "/usr/bin/g++"):
        command = compile_command(compiler, source, executable)
        msvc = Path(compiler.replace("\\", "/")).stem.lower() in ("cl", "clang-cl")
        require(command[0] == compiler and command.count(str(source)) == 1,
                "compiler/source paths must stay separate exact argv elements")
        if msvc:
            require(command == [compiler, "/nologo", "/std:c++17", "/EHsc", "/Od", "/W4", "/WX", "/wd4100",
                                str(source), "/Fe:" + str(executable),
                                "/Fo:" + str(directory / "private binary.obj")],
                    "MSVC dialect or private object-output location differs")
        else:
            require("-Werror" in command and command[-2:] == ["-o", str(executable)]
                    and not any(arg.startswith("/Fo") for arg in command),
                    "GNU/Clang compiler dialect differs")
        checks.append({"compiler": compiler, "msvc_dialect": msvc, "argv": command, "passed": True})
    return checks


def run_program(cxx, units, directory, label):
    predicate, evaluator = source_contract(units)
    source = directory / f"{label}.cpp"
    executable = directory / f"{label}.exe"
    source.write_text(check_program(predicate, evaluator))
    argv = compile_command(cxx, source, executable)
    compiled = subprocess.run(argv, cwd=directory, capture_output=True, text=True, timeout=60)
    (directory / f"{label}.compile.json").write_text(json.dumps({"argv": argv, "returncode": compiled.returncode,
        "stdout": compiled.stdout, "stderr": compiled.stderr}, indent=2) + "\n")
    require(compiled.returncode == 0 and executable.is_file(),
            label + " compile failed: " + compiled.stdout[:1000] + compiled.stderr[:1000])
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
    (directory / f"{label}.run.json").write_text(json.dumps({"returncode": result.returncode,
        "stdout": result.stdout, "stderr": result.stderr}, indent=2) + "\n")
    return result


def replace_once(source, before, after):
    require(source.count(before) == 1, "negative-control anchor changed: " + before)
    return source.replace(before, after)


def negative_variants(units):
    # Mutate comment-free tokens so explanatory comments inside the actual
    # evaluator cannot invalidate the deliberate mutation's exact anchor.
    source = strip_comments(units[FILES[0]])
    units = {**units, FILES[0]: source}
    helper = function(source, NAME)
    for macro, replacement in (("TARGET_VULKAN_ENVIRONMENT", "1"), ("TARGET_MOBILE", "0"),
                               ("MATERIAL_HAS_CUSTOM_SURFACE_SHADING", "0"), ("HAS_CUSTOM_OUTPUT", "0")):
        changed = re.sub(r"defined\s*\(\s*" + macro + r"\s*\)", replacement, helper)
        require(changed != helper, "negative-control gate absent: " + macro)
        yield "missing-" + macro.lower(), {**units, FILES[0]: replace_once(source, helper, changed)}
    for label, expression in (("missing-helper-rejection", "true"), ("wrong-real-return", "false"),
                              ("inverted-helper-return", "gl_HelperInvocation")):
        changed = replace_once(helper, "!gl_HelperInvocation", expression)
        yield label, {**units, FILES[0]: replace_once(source, helper, changed)}
    evaluator = function(source, "evaluateDirectionalLight")
    changed = replace_once(evaluator, NAME + "()", "(" + NAME + "() || true)")
    yield "dead-callsite-policy", {**units, FILES[0]: replace_once(source, evaluator, changed)}
    guard_call = evaluator.index(NAME + "()")
    guard_start = evaluator.rfind("if (", 0, guard_call)
    guard_open = evaluator.index("{", guard_call)
    guard_end = paired(evaluator, guard_open, "{", "}")
    guard = evaluator[guard_start:guard_end]
    moved = replace_once(evaluator, guard, "")
    changed = replace_once(moved, "float ssContactShadowOcclusion = 0.0;",
                           "float ssContactShadowOcclusion = 0.0;\n" + guard)
    yield "helper-before-map", {**units, FILES[0]: replace_once(source, evaluator, changed)}
    main = units["surface_main.fs"]
    changed = replace_once(main, "computeShadingParams();", "if (gl_HelperInvocation) return;\n    computeShadingParams();")
    yield "helper-before-derivatives", {**units, "surface_main.fs": changed}
    changed = replace_once(evaluator, "screenSpaceSunContactShadow(light.l)", "screenSpaceContactShadow(light.l)")
    yield "sun-budget-reduced", {**units, FILES[0]: replace_once(source, evaluator, changed)}


def execute(args, directory):
    root = args.filament.resolve() / "shaders/src"
    units = {name: (root / name).read_text() for name in FILES}
    receipt = {"schema": 1, "passed": False, "scope": __doc__.strip(),
               "source_inputs": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in units.items()},
               "compiler": args.cxx, "negative_controls": []}
    try:
        receipt["compiler_command_cases"] = command_construction_contract(directory)
        actual = run_program(args.cxx, units, directory, "actual")
        require(actual.returncode == 0, "actual source fails: " + actual.stdout + actual.stderr)
        receipt["actual"] = {"returncode": actual.returncode, "stdout": actual.stdout, "stderr": actual.stderr}
        for label, variant in negative_variants(units):
            try:
                result = run_program(args.cxx, variant, directory, label)
            except ValueError as exc:
                # Structural policy mutations fail before C++ generation. A
                # compiler error is not a valid negative semantic control.
                require("compile failed:" not in str(exc), "invalid negative control: " + str(exc))
                receipt["negative_controls"].append({"name": label, "rejected": True, "reason": str(exc)})
            else:
                require(result.returncode != 0, "negative control survived: " + label)
                receipt["negative_controls"].append({"name": label, "rejected": True,
                                                       "returncode": result.returncode,
                                                       "stdout": result.stdout, "stderr": result.stderr})
        receipt["passed"] = True
    except Exception as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        (directory / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filament", type=Path, default=Path(__file__).resolve().parents[2] / "lib/filament")
    parser.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++") or shutil.which("cl"))
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a C++ compiler is required")
    args.cxx = shutil.which(args.cxx) or args.cxx
    if args.work_dir:
        directory = args.work_dir.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        execute(args, directory)
    else:
        with tempfile.TemporaryDirectory(prefix="heaven-contact-helper-contract-") as temporary:
            execute(args, Path(temporary))


if __name__ == "__main__":
    main()
