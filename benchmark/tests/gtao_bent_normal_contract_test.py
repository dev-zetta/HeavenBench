#!/usr/bin/env python3
"""Compile real GTAO materials and check their source against geometric invariants.

This is a host contract, not GPU output or appearance acceptance. The CPU check
evaluates a small, restricted set of the producer's actual GLSL expressions;
the oracle constructs perspective plane intersections independently. No copied
estimator or expected PLUS-token assertion stands in for those expressions.
"""

import argparse
import ast
from functools import lru_cache
import math
import operator
from pathlib import Path
import re
import subprocess
import tempfile


class Vec(tuple):
    def __add__(self, other):
        return Vec(a + b for a, b in zip(self, other))

    def __sub__(self, other):
        return Vec(a - b for a, b in zip(self, other))

    def __mul__(self, scalar):
        return Vec(a * scalar for a in self)

    __rmul__ = __mul__

    def __truediv__(self, scalar):
        return self * (1 / scalar)

    def __neg__(self):
        return self * -1


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return Vec((a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0]))


def normalized(a):
    return a * (1 / math.sqrt(dot(a, a)))


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


def angular_error(a, b):
    return math.degrees(math.acos(clamp(dot(normalized(a), normalized(b)), -1, 1)))


@lru_cache(maxsize=64)
def expression_node(text):
    return ast.parse(text, mode="eval").body


def expression(text, values):
    """Interpret only scalar/vector arithmetic used by this contract, without eval."""
    if "?" in text:
        condition, alternatives = text.split("?", 1)
        yes, no = alternatives.split(":", 1)
        return expression(yes if expression(condition, values) else no, values)
    node = expression_node(" ".join(text.split()))
    binary = {ast.Add: operator.add, ast.Sub: operator.sub,
              ast.Mult: operator.mul, ast.Div: operator.truediv}
    functions = {"sin": math.sin, "cos": math.cos, "sqrt": math.sqrt,
                 "abs": abs, "dot": dot, "clamp": clamp,
                 "vec2": lambda *x: Vec(x if len(x) == 2 else (x[0], x[0])),
                 "vec3": lambda *x: Vec(x if len(x) == 3 else (x[0], x[0], x[0])),
                 "sign": lambda x: 1 if x > 0 else -1 if x < 0 else 0,
                 "acosFast": values.get("acosFast", math.acos)}
    if "integrateBentNormalArc" in values:
        functions["integrateBentNormalArc"] = values["integrateBentNormalArc"]

    def interpret(item):
        if isinstance(item, ast.Constant) and isinstance(item.value, (int, float, bool)):
            return item.value
        if isinstance(item, ast.Name):
            return values[item.id]
        if isinstance(item, ast.Attribute):
            value = interpret(item.value)
            if isinstance(value, dict):
                return value[item.attr]
            if isinstance(value, Vec) and item.attr in ("x", "y", "z", "w"):
                return value[("x", "y", "z", "w").index(item.attr)]
            raise ValueError("Only contract data and vector-component access are supported")
        if isinstance(item, ast.UnaryOp) and isinstance(item.op, ast.USub):
            return -interpret(item.operand)
        if isinstance(item, ast.BinOp) and type(item.op) in binary:
            return binary[type(item.op)](interpret(item.left), interpret(item.right))
        if isinstance(item, ast.Call) and isinstance(item.func, ast.Name):
            return functions[item.func.id](*(interpret(arg) for arg in item.args))
        raise ValueError(f"Unsupported contract expression: {text}")

    return interpret(node)


def strip_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def statement(source, lhs):
    matches = re.findall(r"\b" + re.escape(lhs) + r"\s*=\s*([^;]+);", source)
    if len(matches) != 1:
        raise ValueError(f"Expected one producer assignment to {lhs}; found {len(matches)}")
    return matches[0]


def producer_expressions(source):
    source = strip_comments(source)
    # Limit inspection to the real producer, not another helper or caller.
    producer = source.split("void groundTruthAmbientOcclusion(", 1)[1]
    prefix = producer.split("float visibility", 1)[0]
    initializer = statement(prefix, "bentNormal")
    direction = re.findall(r"\bbentNormal\s*\+=\s*([^;]+);", producer)
    if len(direction) != 1:
        raise ValueError("Expected one bent-normal slice contribution")
    result = {"initializer": initializer, "direction": direction[0]}
    for name in ("signNorm", "n", "normalView", "normalTangent", "moment"):
        result[name] = statement(producer, name)
    for name in ("slice", "phi", "cosPhi", "sinPhi"):
        result[name] = statement(producer, name)
    result["screen_direction"] = statement(producer, "direction")
    for name in ("h0", "h1"):
        assignments = re.findall(r"\b" + name + r"\s*=\s*([^;]+);", producer)
        if len(assignments) != 2:
            raise ValueError(f"Expected horizon initialization and clamp for {name}")
        result[name], result[name + "_clamp"] = assignments
    return result


def arc_primitive(source):
    """Evaluate the actual helper; the numerical oracle does not use its formula."""
    source = strip_comments(source)
    body = re.search(r"(?:highp\s+)?vec2\s+integrateBentNormalArc\([^)]*\)\s*\{([^}]+)\}",
                     source, re.S)
    if body is None:
        raise ValueError("Missing real bent-normal arc primitive")
    assignments = re.findall(r"(?:highp\s+)?float\s+(\w+)\s*=\s*([^;]+);", body[1])
    result = re.findall(r"return\s+([^;]+);", body[1])
    if not assignments or len(result) != 1:
        raise ValueError("Arc primitive needs an updated interpreter")

    def primitive(theta, normal_view, normal_tangent):
        values = {"theta": theta, "normalView": normal_view, "normalTangent": normal_tangent}
        for name, value in assignments:
            values[name] = expression(value, values)
        return expression(result[0], values)

    return primitive


def acos_approximation(source):
    source = strip_comments(source)
    body = re.search(r"float acosFast\(float x\)\s*\{([^}]+)\}", source)
    if body is None:
        raise ValueError("Missing real acosFast definition")
    y = statement(body[1], "y")
    p = statement(body[1], "p")
    scale = re.findall(r"\bp\s*\*=\s*([^;]+);", body[1])
    result = re.findall(r"return\s+([^;]+);", body[1])
    if len(scale) != 1 or len(result) != 1:
        raise ValueError("acosFast contract needs an updated interpreter")

    def approximate(x):
        values = {"x": x, "PI": math.pi}
        values["y"] = expression(y, values)
        values["p"] = expression(p, values) * expression(scale[0], values)
        # This condition is scalar and deliberately handled without eval.
        condition, alternatives = result[0].split("?", 1)
        if condition.strip() != "x >= 0.0":
            raise ValueError("acosFast condition needs an updated interpreter")
        yes, no = alternatives.split(":", 1)
        return expression(yes if x >= 0 else no, values)

    return approximate


def plane_point(uv, normal, plane_constant, scales=(2, 2)):
    ray = Vec(((0.5 - uv[0]) * scales[0], (0.5 - uv[1]) * scales[1], 1))
    depth = plane_constant / dot(normal, ray)
    if depth >= 0:
        raise ValueError("Plane sample is behind the camera")
    return ray * depth


def numerical_moment(lo, hi, normal_view, normal_tangent, samples=8192):
    """Integrate directions and cosine visibility by midpoint quadrature.

    This uses spherical direction/Jacobian definitions, not the GLSL primitive.
    It intentionally splits at zero because abs(sin(theta)) has a cusp there.
    """
    result = [0.0, 0.0]
    boundaries = [lo] + ([0.0] if lo < 0 < hi else []) + [hi]
    for start, end in zip(boundaries, boundaries[1:]):
        step = (end - start) / samples
        for index in range(samples):
            theta = start + (index + 0.5) * step
            cosine, sine = math.cos(theta), math.sin(theta)
            weight = max(0.0, normal_view * cosine + normal_tangent * sine) * abs(sine)
            result[0] += cosine * weight * step
            result[1] += sine * weight * step
    return Vec(result)


def slice_values(code, primitive, acos_fast, origin, normal, phi):
    view = normalized(-origin)
    uv = tuple(0.5 - origin[i] / (2 * origin[2]) for i in (0, 1))
    screen_values = {"phi": phi}
    for name in ("cosPhi", "sinPhi"):
        screen_values[name] = expression(code[name], screen_values)
    screen_direction = expression(code["screen_direction"], screen_values)
    ortho = normalized(screen_direction - view * dot(screen_direction, view))
    axis = cross(ortho, view)
    projected = normal - axis * dot(normal, axis)
    plane_constant = dot(normal, origin)
    delta_uv = (0.0005 * math.cos(phi), 0.0005 * math.sin(phi))
    plus = plane_point(tuple(uv[i] + delta_uv[i] for i in (0, 1)),
                       normal, plane_constant) - origin
    minus = plane_point(tuple(uv[i] - delta_uv[i] for i in (0, 1)),
                        normal, plane_constant) - origin
    values = {"viewDir": view, "orthoDirection": ortho, "normal": normal,
              "projNormal": projected, "HALF_PI": math.pi / 2,
              "cosNorm": clamp(dot(projected, view) / math.sqrt(dot(projected, projected)), 0, 1),
              "horizonCos0": clamp(dot(normalized(plus), view), -1, 1),
              "horizonCos1": clamp(dot(normalized(minus), view), -1, 1),
              "acosFast": acos_fast, "integrateBentNormalArc": primitive}
    for name in ("signNorm", "n", "h0", "h1"):
        values[name] = expression(code[name], values)
    for name in ("h0", "h1"):
        values[name] = expression(code[name + "_clamp"], values)
    for name in ("normalView", "normalTangent", "moment"):
        values[name] = expression(code[name], values)
    return values


def check_primitive(primitive):
    worst = 0.0
    count = 0
    for tilt in (-1.2, -0.65, 0, 0.3, 1.1):
        a, b = math.cos(tilt), math.sin(tilt)
        for shrink_left, shrink_right in ((0, 0), (0.25, 0.4), (0.65, 0.12)):
            lo = (tilt - math.pi / 2) * (1 - shrink_left)
            hi = (tilt + math.pi / 2) * (1 - shrink_right)
            actual = primitive(hi, a, b) - primitive(lo, a, b)
            expected = numerical_moment(lo, hi, a, b)
            absolute = max(abs(x - y) for x, y in zip(actual, expected))
            # 8192 steps on each smooth side: measured quadrature error <5e-9;
            # 5e-8 allows its O(step^2) error, not a direction-estimator tolerance.
            if not math.isfinite(absolute) or absolute > 5e-8:
                raise AssertionError(f"Arc primitive differs from vector quadrature by {absolute:.9g}")
            worst = max(worst, absolute)
            count += 1
    if primitive(0, 0.8, 0.6) != Vec((0.0, 0.0)):
        raise AssertionError("Arc primitive is not continuous at the signed-angle origin")
    return count, worst


def check_slice_moments(code, primitive, acos_fast):
    worst = 0.0
    count = 0
    for origin in (Vec((0, 0, -3)), Vec((-0.7, 0.3, -4)), Vec((0.8, -0.5, -5))):
        view = normalized(-origin)
        tangent = normalized(Vec((1, 0, 0)) - view * view[0])
        uv = tuple(0.5 - origin[i] / (2 * origin[2]) for i in (0, 1))
        for tilt in (-1.25, -0.8, -0.35, 0.35, 0.8, 1.25):
            normal = view * math.cos(tilt) + tangent * math.sin(tilt)
            plane_constant = dot(normal, origin)
            for index in range(16):
                phi = (index + 0.25) * math.pi / 16
                # Reconstruct the geometric normal independently from depth rays.
                dx = plane_point((uv[0] + 0.0002, uv[1]), normal, plane_constant) - origin
                dy = plane_point((uv[0], uv[1] + 0.0002), normal, plane_constant) - origin
                if angular_error(cross(dx, dy), normal) > 0.00002:
                    raise AssertionError("Independent plane normal reconstruction failed")
                values = slice_values(code, primitive, acos_fast, origin, normal, phi)
                actual = expression(code["direction"], values)
                expected_moment = numerical_moment(values["h0"], values["h1"],
                                                   dot(normal, view),
                                                   dot(normal, values["orthoDirection"]), 2048)
                expected = view * expected_moment[0] + values["orthoDirection"] * expected_moment[1]
                error = angular_error(actual, expected)
                if not math.isfinite(error) or error > 0.00002:
                    raise AssertionError(f"Slice moment differs from independent quadrature by {error:.6f} degrees")
                worst = max(worst, error)
                count += 1
    # An asymmetric visible angular interval must contain its estimated axis.
    values = {"h0": -0.2, "h1": 0.8, "viewDir": Vec((0, 0, 1)),
              "orthoDirection": Vec((1, 0, 0)), "normalView": 1.0, "normalTangent": 0.0,
              "integrateBentNormalArc": primitive}
    values["moment"] = expression(code["moment"], values)
    actual = expression(code["direction"], values)
    actual_angle = math.atan2(actual[0], actual[2])
    if not values["h0"] < actual_angle < values["h1"]:
        raise AssertionError("Bent direction lies outside the open angular interval")
    return count, worst


def check_aggregates(code, primitive, acos_fast):
    centered_worst = 0.0
    off_axis_worst = 0.0
    count = 0
    for origin in (Vec((0, 0, -3)), Vec((-0.7, 0.3, -4)), Vec((0.8, -0.5, -5))):
        view = normalized(-origin)
        tangent = normalized(Vec((1, 0, 0)) - view * view[0])
        for tilt in (20, 40, 60, 80):
            angle = math.radians(tilt)
            normal = view * math.cos(angle) + tangent * math.sin(angle)
            # The actual spatialDirectionNoise formula spans these 16 phases.
            for phase in range(16):
                for slices in (4, 8, 16):
                    total = Vec((0.0, 0.0, 0.0))
                    for index in range(slices):
                        angle_values = {"i": index, "noiseDirection": phase / 16,
                                        "materialParams": {"sliceCount": Vec((slices, 1 / slices))},
                                        "PI": math.pi}
                        angle_values["slice"] = expression(code["slice"], angle_values)
                        phi = expression(code["phi"], angle_values)
                        values = slice_values(code, primitive, acos_fast, origin, normal, phi)
                        total += expression(code["direction"], values)
                    error = angular_error(total, normal)
                    if not math.isfinite(error):
                        raise AssertionError("Nonfinite plane aggregate")
                    if origin[0] == origin[1] == 0:
                        # The independent full-hemisphere cosine moment is (2*pi/3)*N.
                        # Exact angular integration returns N for >=2 centered slices;
                        # 0.02 degrees allows current fast-acos horizon/clamp error.
                        if error > 0.02:
                            raise AssertionError(f"Unoccluded plane aggregate is biased by {error:.6f} degrees")
                        centered_worst = max(centered_worst, error)
                    else:
                        # Uniform screen angles project nonuniformly around off-axis V.
                        # These angles exercise source paths but do NOT assert N equality.
                        off_axis_worst = max(off_axis_worst, error)
                    count += 1
    return count, centered_worst, off_axis_worst


def check_source(source, common_math):
    code = producer_expressions(source)
    normal = normalized(Vec((0.6, 0, 0.8)))
    for bitmask, expected in ((False, Vec((0, 0, 0))), (True, normal)):
        actual = expression(code["initializer"],
                            {"materialConstants_useVisibilityBitmasks": bitmask, "normal": normal})
        if actual != expected:
            raise AssertionError("Bent-normal initializer biases the horizon sum or invalidates bitmask fallback")
    primitive = arc_primitive(source)
    acos_fast = acos_approximation(common_math)
    primitive_count, primitive_worst = check_primitive(primitive)
    slice_count, slice_worst = check_slice_moments(code, primitive, acos_fast)
    aggregate_count, aggregate_worst, off_axis_worst = check_aggregates(code, primitive, acos_fast)
    return {"primitive_cases": primitive_count, "primitive_worst": primitive_worst,
            "slice_count": slice_count, "slice_worst": slice_worst,
            "aggregate_count": aggregate_count, "aggregate_worst": aggregate_worst,
            "off_axis_worst": off_axis_worst}


def expect_failure(action, label):
    try:
        action()
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"Negative control unexpectedly passed: {label}")


def check_negative_controls(source, common_math):
    code = producer_expressions(source)
    primitive = arc_primitive(source)
    acos_fast = acos_approximation(common_math)
    missing = source.replace("bentNormal = " + code["initializer"] + ";", "", 1)
    if missing == source:
        raise ValueError("Initializer negative control needs an updated mutation")
    expect_failure(lambda: check_source(missing, common_math), "removed initializer")
    # Mutate only the actual vector's lateral contribution for the sign control.
    mutated = dict(code)
    mutated["direction"] = code["direction"].replace("+ orthoDirection", "- orthoDirection", 1)
    if mutated["direction"] == code["direction"]:
        raise ValueError("Direction negative control needs an updated expression mutation")
    expect_failure(lambda: check_slice_moments(mutated, primitive, acos_fast), "mirrored direction")
    # Restore the old unit-midpoint estimator while retaining its correct sign.
    # It has correct slice orientation but fails the independent hemisphere moment.
    mutated["direction"] = ("viewDir * cos(0.5 * (h0 + h1))"
                            " + orthoDirection * sin(0.5 * (h0 + h1))")
    expect_failure(lambda: check_aggregates(mutated, primitive, acos_fast), "unit-midpoint aggregation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filament", type=Path, required=True)
    parser.add_argument("--matc", type=Path, required=True)
    args = parser.parse_args()
    materials = args.filament / "filament/src/materials/ssao"
    source = (materials / "gtaoImpl.fs").read_text()
    common_math = (args.filament / "shaders/src/common_math.glsl").read_text()
    metrics = check_source(source, common_math)
    check_negative_controls(source, common_math)
    with tempfile.TemporaryDirectory(prefix="heaven-gtao-contract-") as temporary:
        for platform in ("desktop", "mobile"):
            for name in ("gtao", "gtaoBentNormals"):
                output = Path(temporary) / f"{platform}-{name}.filamat"
                command = [str(args.matc), "-a", "opengl", "-a", "vulkan", "-p", platform,
                           "-o", str(output), str(materials / (name + ".mat"))]
                completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
                if completed.returncode or not output.exists() or output.stat().st_size == 0:
                    raise RuntimeError(f"Actual {platform}/{name} compilation failed:\n{completed.stdout}{completed.stderr}")
    print("PASS: actual desktop/mobile scalar/bent materials compile; "
          f"{metrics['primitive_cases']} vector quadratures (worst {metrics['primitive_worst']:.3g}), "
          f"{metrics['slice_count']} plane slices, {metrics['aggregate_count']} aggregates "
          f"(centered worst {metrics['aggregate_worst']:.6f} degrees), and three negative controls pass. "
          f"Known off-axis screen-angle quadrature residual: {metrics['off_axis_worst']:.6f} degrees. "
          "Host source/math contract only; no GPU-output or appearance claim.")


if __name__ == "__main__":
    main()
