#!/usr/bin/env python3
"""Compile pinned upstream GLSL and emit the SDK's actual shader-blob contract."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

FI = [
    "reconstruct_and_dilate", "setup", "reconstruct_previous_depth",
    "game_motion_vector_field", "optical_flow_vector_field", "disocclusion_mask",
    "", "compute_inpainting_pyramid", "inpainting",
    "compute_game_vector_field_inpainting_pyramid", "debug_view",
]
OF = ["prepare_luma", "compute_luminance_pyramid", "generate_scd_histogram",
      "compute_scd_divergence", "compute_optical_flow_advanced_pass_v5",
      "filter_optical_flow_pass_v5", "scale_optical_flow_advanced_pass_v5"]
CATEGORIES = ["cbv", "srvTexture", "uavTexture", "srvBuffer", "uavBuffer", "sampler", "rtAccelStruct"]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--sdk", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--compiler", required=True)
    p.add_argument("--reflect", required=True)
    p.add_argument("--validator", default=shutil.which("spirv-val"))
    p.add_argument("--jobs", type=int, default=4)
    a = p.parse_args()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    work = a.output.parent / "spirv"
    work.mkdir(exist_ok=True)
    gpu = a.sdk / "sdk/include/FidelityFX/gpu"
    shaders = a.sdk / "sdk/src/backends/vk/shaders"

    def compile_one(item):
        effect, pass_id, name, permutation = item
        group = "frameinterpolation" if effect == "fi" else "opticalflow"
        stem = f"ffx_{group}_{name + '_' if name else ''}pass"
        if name.endswith("pass_v5"):
            stem = f"ffx_{group}_{name}"
        src = shaders / group / (stem + ".glsl")
        if not src.is_file():
            raise RuntimeError(f"Missing pinned shader: {src}")
        dest = work / f"{effect}_{pass_id}_{permutation}.spv"
        # Filament creates a Vulkan 1.1 instance. All required subgroup and
        # storage operations are available in its SPIR-V 1.3 environment.
        args = [a.compiler, "-V", "--target-env", "vulkan1.1", "-S", "comp", "-Os",
                "-DFFX_GLSL=1", "-DFFX_GPU=1", "-DFFX_HALF=0", f"-I{gpu}", f"-I{gpu / group}"]
        if effect == "fi":
            definitions = {"LOW_RES_MOTION_VECTORS": permutation & 1,
                           "JITTER_MOTION_VECTORS": (permutation >> 1) & 1,
                           "INVERTED_DEPTH": (permutation >> 2) & 1,
                           "UPSAMPLE_SAMPLERS_USE_DATA_HALF": 0,
                           "ACCUMULATE_SAMPLERS_USE_DATA_HALF": 0,
                           "REPROJECT_SAMPLERS_USE_DATA_HALF": 1,
                           "POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF": 0,
                           "UPSAMPLE_USE_LANCZOS_TYPE": 2}
            args += [f"-DFFX_FRAMEINTERPOLATION_OPTION_{key}={value}" for key, value in definitions.items()]
        else:
            args += [f"-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={permutation}"]
        result = subprocess.run(args + [str(src), "-o", str(dest)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"{src}:\n{result.stdout}\n{result.stderr}")
        data = dest.read_bytes()
        if len(data) < 20 or struct.unpack_from("<2I", data) != (0x07230203, 0x00010300):
            raise RuntimeError(f"{src}: expected a SPIR-V 1.3 module for Vulkan 1.1")
        if a.validator:
            result = subprocess.run([a.validator, "--target-env", "vulkan1.1", "--relax-block-layout", str(dest)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"{src}: SPIR-V validation failed:\n{result.stdout}\n{result.stderr}")
        reflection = json.loads(subprocess.check_output([a.reflect, str(dest)], text=True))
        for binding in reflection:
            if binding["set"] != 0 or binding["count"] < 1:
                raise RuntimeError(f"Unsupported descriptor in {src}: {binding}")
        return item, data, reflection

    items = [("fi", i, name, perm) for i, name in enumerate(FI) for perm in range(8)]
    items += [("of", i, name, perm) for i, name in enumerate(OF) for perm in range(2)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        results = list(pool.map(compile_one, items))
    output = ["// Generated from official FidelityFX SDK 1.1.4 GLSL; do not edit.",
              '#include "ffx_shader_blobs.h"', '#include <cstring>',
              '#include <FidelityFX/host/ffx_frameinterpolation.h>',
              '#include <FidelityFX/host/ffx_opticalflow.h>']
    names = []
    receipts = []
    for (effect, pass_id, _, perm), data, bindings in results:
        key = f"{effect}_{pass_id}_{perm}"
        names.append((effect, pass_id, perm, key))
        words = struct.unpack(f"<{len(data) // 4}I", data)
        output.append(f"alignas(4) static const uint32_t code_{key}[] = {{" + ','.join(hex(v) for v in words) + "};")
        fields = [f"reinterpret_cast<const uint8_t*>(code_{key})", str(len(data))]
        groups = [[b for b in bindings if b["category"] == c] for c in CATEGORIES]
        fields += [str(len(g)) for g in groups]
        for cat, group in zip(CATEGORIES, groups):
            if not group:
                fields += ["nullptr"] * 4
                continue
            prefix = f"{key}_{cat}"
            output.append(f"static const char* {prefix}_names[] = {{" + ','.join(json.dumps(b["name"]) for b in group) + "};")
            for field in ["binding", "count", "set"]:
                output.append(f"static const uint32_t {prefix}_{field}[] = {{" + ','.join(str(b[field]) for b in group) + "};")
            fields += [f"{prefix}_names", f"{prefix}_binding", f"{prefix}_count", f"{prefix}_set"]
        output.append(f"static const FfxShaderBlob blob_{key} = {{" + ','.join(fields) + "};")
        receipts.append({"effect": effect, "pass": pass_id, "permutation": perm,
                         "sha256": hashlib.sha256(data).hexdigest(), "descriptors": bindings})
    output += ['extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage stage, uint32_t options, FfxShaderBlob* out) {',
               'if (!out || stage != FFX_BIND_COMPUTE_SHADER_STAGE) return FFX_ERROR_INVALID_ARGUMENT;',
               'const FfxShaderBlob* blob = nullptr;']
    for effect, pass_id, perm, key in names:
        enum = "FFX_EFFECT_FRAMEINTERPOLATION" if effect == "fi" else "FFX_EFFECT_OPTICALFLOW"
        mask = "(options & 7u)" if effect == "fi" else "((options >> 2u) & 1u)"
        output.append(f"if (effect == {enum} && pass == {pass_id} && {mask} == {perm}) blob = &blob_{key};")
    output += ['if (!blob) return FFX_ERROR_INVALID_ARGUMENT;',
               'std::memcpy(out, blob, sizeof(*out)); return FFX_OK; }',
               'extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool& wave64) { wave64 = false; return FFX_OK; }']
    # Vulkan GLSL wave32/wave64 source arguments are identical upstream. Let
    # Vulkan use its native subgroup width; the portable build uses FP32 math.
    temporary = a.output.with_suffix(".tmp")
    temporary.write_text('\n'.join(output) + '\n')
    temporary.replace(a.output)
    a.output.with_suffix(".json").write_text(json.dumps({"schema": 2, "precision": "fp32", "target_environment": "vulkan1.1", "spirv_version": "1.3", "spirv_validated": bool(a.validator), "shaders": receipts}, indent=2) + '\n')
    print(f"Compiled {len(results)} real FSR 3.1 Vulkan shader permutations")


if __name__ == "__main__":
    main()
