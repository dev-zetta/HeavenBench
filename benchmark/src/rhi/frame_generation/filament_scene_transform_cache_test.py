#!/usr/bin/env python3
"""Execute actual Filament scene-cache matrix blocks against a scalar 4x4 oracle.

The real producer declarations, cached matrix writes and per-frame recenter loop
are extracted verbatim and compiled using Filament's actual math headers. The
oracle independently multiplies every element of complete double 4x4 matrices.
This covers constant positive homogeneous W; spatially projective model rows
remain outside the scene cache's affine bounds/prepared-displacement contract.
No GPU, renderer build, or Filament source mutation is performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


PREFIX = r'''
#include <math/mat4.h>
#include <utils/compiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
using namespace filament::math;
struct FScene { enum { WORLD_TRANSFORM }; };
struct Soa {
    std::vector<mat4f> matrices;
    template<unsigned Column> mat4f& elementAt(size_t index) {
        static_assert(Column == FScene::WORLD_TRANSFORM);
        return matrices.at(index);
    }
};
struct TransformManager {
    mat4 source;
    mat4 getWorldTransformAccurate(unsigned) const { return source; }
};
double3 produce(Soa& soa, size_t index, mat4 const& source, mat3 const& worldRotation) {
    TransformManager const tcm{source};
    unsigned const ti = 1;
    __ACTUAL_PRODUCER_DECLARATIONS__
    __ACTUAL_CACHED_MATRIX_WRITES__
    return rotatedTranslation;
}
void recenter(Soa& soa, std::vector<double3> const& translations,
        std::vector<float3> const& offsets, double3 const& worldTranslation,
        std::vector<float3>& centers, std::array<std::vector<float>, 3>& components) {
    size_t const renderableCount = soa.matrices.size();
    double3 const* const rTranslations = translations.data();
    float3 const* const rAabbOffsets = offsets.data();
    mat4f* const outTransforms = soa.matrices.data();
    float3* const outCenters = centers.data();
    float* const outCenterX = components[0].data();
    float* const outCenterY = components[1].data();
    float* const outCenterZ = components[2].data();
    __ACTUAL_RECENTER_LOOP__
}
'''

SUFFIX = r'''
// Row-major scalar arrays deliberately avoid the tested math matrix product.
using Matrix = std::array<std::array<double, 4>, 4>;
Matrix identity() {
    Matrix result{};
    for (unsigned i = 0; i < 4; ++i) result[i][i] = 1;
    return result;
}
Matrix multiply(Matrix const& left, Matrix const& right) {
    Matrix result{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned k = 0; k < 4; ++k) result[r][c] += left[r][k] * right[k][c];
    return result;
}
Matrix rotation(unsigned axis, double angle) {
    Matrix result = identity();
    unsigned const a = (axis + 1) % 3, b = (axis + 2) % 3;
    result[a][a] = result[b][b] = std::cos(angle);
    result[a][b] = -std::sin(angle);
    result[b][a] = std::sin(angle);
    return result;
}
mat4 to_actual(Matrix const& source) {
    mat4 result;
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) result[c][r] = source[r][c];
    return result;
}
bool close(double actual, double expected) {
    double const tolerance = 8 * std::numeric_limits<float>::epsilon() *
            std::max(1.0, std::abs(expected));
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}
int main() {
    unsigned matrices = 0, points = 0, unit_w_exact = 0, mixed_rows = 0;
    // Duplicate source shapes with different W share one cache population;
    // changed origins exercise reuse without re-running the producer.
    std::array<float, 7> const weights{1.0f, .8f, 1.75f, 1.0f, .8f, 2.5f, 1.25f};
    std::array<double3, 5> const origins{double3{0}, double3{.12, -.09, .06},
            double3{-3.75, 2.5, -1.8}, double3{119.125, -83.25, 7.5},
            double3{-1012.125, 809.5, -300.75}};
    std::array<std::array<double, 4>, 4> const probes{{
            {{0, 0, 0, 1}}, {{.25, -.5, .125, 1}},
            {{-3.5, .125, 2.75, 1}}, {{.015, .02, -.01, 0}}}};
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (double angle : {0.0, .31, -1.2, 2.7}) {
            Matrix const local_rotation = multiply(rotation(axis, angle), rotation((axis + 1) % 3, -.47));
            for (unsigned world_axis = 0; world_axis < 3; ++world_axis) {
                for (double world_angle : {0.0, .62, -2.1}) {
                    Matrix const world_rotation = rotation(world_axis, world_angle);
                    mat3 const actual_rotation = to_actual(world_rotation).upperLeft();
                    std::vector<Matrix> source_rows;
                    std::vector<double3> translations;
                    std::vector<float3> offsets(weights.size(), float3{.2f, -.1f, .3f});
                    std::vector<float3> centers(weights.size());
                    std::array<std::vector<float>, 3> components;
                    for (auto& values : components) values.resize(weights.size());
                    Soa soa{std::vector<mat4f>(weights.size())};
                    for (size_t i = 0; i < weights.size(); ++i) {
                        auto model = local_rotation;
                        double const scale = i % 2 ? .75 : 1.5;
                        for (unsigned r = 0; r < 3; ++r)
                            for (unsigned c = 0; c < 3; ++c) model[r][c] *= scale;
                        model[0][3] = .12; model[1][3] = -.09; model[2][3] = .06;
                        model[3][3] = weights[i];
                        source_rows.push_back(model);
                        translations.push_back(produce(soa, i, to_actual(model), actual_rotation));
                    }
                    for (auto const origin : origins) {
                        recenter(soa, translations, offsets, origin, centers, components);
                        ++mixed_rows;
                        auto world = world_rotation;
                        for (unsigned r = 0; r < 3; ++r) world[r][3] = origin[r];
                        for (size_t i = 0; i < weights.size(); ++i) {
                            ++matrices;
                            auto const expected = multiply(world, source_rows[i]);
                            auto const& actual = soa.matrices[i];
                            for (unsigned r = 0; r < 4; ++r) {
                                for (unsigned c = 0; c < 4; ++c) {
                                    if (!close(actual[c][r], expected[r][c])) {
                                        std::printf("FAIL: complete 4x4 W %.9g row %u col %u actual %.17g expected %.17g\n",
                                                weights[i], r, c, double(actual[c][r]), expected[r][c]);
                                        return 1;
                                    }
                                }
                            }
                            if (weights[i] == 1) {
                                float3 const common = float3(translations[i] + origin);
                                if (std::memcmp(&common, &actual[3].xyz, sizeof(common))) {
                                    std::puts("FAIL: W1 common-path translation bytes changed"); return 2;
                                }
                                ++unit_w_exact;
                            }
                            for (unsigned r = 0; r < 3; ++r) {
                                float const expected_center = offsets[i][r] + actual[3][r];
                                if (centers[i][r] != expected_center || components[r][i] != expected_center) {
                                    std::puts("FAIL: culling cache center/SoA components disagree"); return 3;
                                }
                            }
                            for (auto const& probe : probes) {
                                std::array<double, 4> actual_point{}, expected_point{};
                                for (unsigned r = 0; r < 4; ++r) {
                                    for (unsigned c = 0; c < 4; ++c) {
                                        actual_point[r] += double(actual[c][r]) * probe[c];
                                        expected_point[r] += expected[r][c] * probe[c];
                                    }
                                }
                                // Both raw homogeneous and post-divide physical
                                // coordinates must retain the same 4x4 mapping.
                                for (unsigned r = 0; r < 4; ++r) {
                                    if (!close(actual_point[r], expected_point[r])) {
                                        std::puts("FAIL: raw homogeneous point mapping"); return 4;
                                    }
                                }
                                if (probe[3] != 0) {
                                    for (unsigned r = 0; r < 3; ++r)
                                        if (!close(actual_point[r] / actual_point[3],
                                                expected_point[r] / expected_point[3])) {
                                            std::puts("FAIL: physical post-divide mapping"); return 5;
                                        }
                                }
                                ++points;
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("PASS: actual scene producer/recenter, %u complete matrices, %u raw/physical points, %u mixed cache populations, %u byte-exact W1 translations; no GPU\n",
            matrices, points, mixed_rows, unit_w_exact);
}
'''


def extract_between(source, begin, end):
    if source.count(begin) != 1:
        raise RuntimeError("Expected one actual-source anchor: " + begin)
    start = source.index(begin)
    finish = source.index(end, start)
    return source[start:finish]


def extract_loop(source, marker):
    if source.count(marker) != 1:
        raise RuntimeError("Expected one actual recenter loop")
    start = source.index(marker)
    position = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[position] == "{") - (source[position] == "}")
        position += 1
    return source[start:position]


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--filament-root", type=Path, help="Explicit vendor/staging root; defaults to SOURCE_ROOT/lib/filament")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--stdlib", choices=("libc++", "default"), default="default")
    args = parser.parse_args()
    vendor = args.filament_root.resolve() if args.filament_root else args.source_root.resolve() / "lib/filament"
    path = vendor / "filament/src/details/Scene.cpp"
    source = path.read_text()
    producer = extract_between(source, "static void updateRenderableCache(",
                               "\nUTILS_NOINLINE\nstatic void updateLightCache(")
    declarations = extract_between(producer, "mat4 const model = tcm.getWorldTransformAccurate(ti);\n    mat3f const shaderWorldRotation",
                                   "    bool const reversedWindingOrder")
    writes = extract_between(producer, "mat4f& shaderWorldTransform = soa.elementAt<FScene::WORLD_TRANSFORM>(index);",
                             "    soa.elementAt<FScene::VISIBILITY_STATE>(index)")
    loop = extract_loop(source, "for (size_t i = 0; i < renderableCount; ++i) {")
    preserve = "shaderWorldTransform[3].w = float(model[3].w);"
    scaled = "rTranslations[i] + worldTranslation * w"
    if writes.count(preserve) != 1 or loop.count(scaled) != 1:
        raise RuntimeError("Expected actual homogeneous W preservation and recenter weighting")
    variants = (("production", writes, loop),
                ("lost-homogeneous-w-negative", writes.replace(preserve, ""), loop),
                ("unscaled-recenter-negative", writes, loop.replace(scaled, "rTranslations[i] + worldTranslation")))
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    command = [args.cxx, "/nologo", "/std:c++20", "/O1", "/EHsc"] if msvc else [args.cxx, "-std=c++20", "-O1"]
    if args.stdlib == "libc++" and not msvc:
        command.append("-stdlib=libc++")
    command += [("/I" if msvc else "-I") + str(vendor / relative)
                for relative in ("libs/math/include", "libs/utils/include")]
    results = []
    for label, cached, shifted in variants:
        cpp = work / (label + ".cpp")
        exe = work / (label + (".exe" if msvc else ""))
        cpp.write_text(PREFIX.replace("__ACTUAL_PRODUCER_DECLARATIONS__", declarations)
                       .replace("__ACTUAL_CACHED_MATRIX_WRITES__", cached)
                       .replace("__ACTUAL_RECENTER_LOOP__", shifted) + SUFFIX)
        argv = command + [str(cpp)]
        argv += ["/Fe:" + str(exe), "/Fo" + str(work) + "/"] if msvc else ["-o", str(exe)]
        built = subprocess.run(argv, cwd=work, capture_output=True, text=True, timeout=60)
        (work / (label + "-compile.log")).write_text(built.stdout + built.stderr)
        row = {"case": label, "command": argv, "compile_exit": built.returncode}
        if built.returncode == 0:
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            (work / (label + "-run.log")).write_text(run.stdout + run.stderr)
            print(run.stdout + run.stderr, end="")
            row["run_exit"] = run.returncode
            row["expected_run_exit"] = 0 if label == "production" else 1
            row["passed"] = run.returncode == row["expected_run_exit"]
        else:
            row["passed"] = False
            print(built.stdout + built.stderr, end="")
        results.append(row)
    receipt = {"schema": 1, "passed": all(row["passed"] for row in results),
               "source": str(path), "source_sha256": digest(source),
               "blocks": {"producer_sha256": digest(declarations), "cached_matrix_sha256": digest(writes),
                          "recenter_sha256": digest(loop)}, "results": results,
               "scope": "Actual extracted CPU matrix blocks and real math headers, independent scalar full4x4 oracle. Constant positive W only; no spatial-projective, GPU/native or rendering acceptance claim."}
    (work / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
