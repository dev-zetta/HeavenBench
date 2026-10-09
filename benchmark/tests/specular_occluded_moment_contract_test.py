#!/usr/bin/env python3
"""Actual-source uniform occluded-moment contract; no GPU appearance claim.

Independent polar quadrature and spherical-cap moments qualify the producer
and strength fit. A fixed world rectangle supplies independent inside/outside
visibility. RGBA8 and dispersed fits are reported as approximation limits.
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
    def binary(self, other, operation):
        rhs = other if isinstance(other, tuple) else (other,) * len(self)
        return Vec(operation(a, b) for a, b in zip(self, rhs))

    def __add__(self, other):
        return self.binary(other, operator.add)

    __radd__ = __add__

    def __sub__(self, other):
        return self.binary(other, operator.sub)

    def __mul__(self, other):
        return self.binary(other, operator.mul)

    __rmul__ = __mul__

    def __truediv__(self, other):
        return self.binary(other, operator.truediv)

    def __neg__(self):
        return self * -1


def vector(size, *values):
    flat = tuple(x for value in values for x in (value if isinstance(value, tuple) else (value,)))
    return Vec(flat * size if len(flat) == 1 else flat)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def length(v):
    return math.sqrt(dot(v, v))


def normalized(v):
    return v / length(v)


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


def smoothstep(lo, hi, x):
    t = clamp((x-lo)/(hi-lo), 0, 1)
    return t*t*(3-2*t)


def clean(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def matching(source, start, opening, closing):
    depth = 0
    for index in range(start, len(source)):
        depth += source[index] == opening
        depth -= source[index] == closing
        if depth == 0:
            return index
    raise ValueError("Unbalanced actual shader source")


def body(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", clean(source))
    if match is None:
        raise ValueError(f"Missing real function {name}")
    source = clean(source)
    start = source.index("{", match.start())
    return source[start+1:matching(source, start, "{", "}")]


@lru_cache(maxsize=512)
def node(text):
    return ast.parse(text.replace("||", " or ").replace("&&", " and "), mode="eval").body


def expression(text, values, functions):
    if "?" in text:
        condition, alternatives = text.split("?", 1)
        yes, no = alternatives.split(":", 1)
        return expression(yes if expression(condition, values, functions) else no, values, functions)
    operations = {ast.Add: operator.add, ast.Sub: operator.sub,
                  ast.Mult: operator.mul, ast.Div: operator.truediv}
    comparisons = {ast.Lt: operator.lt, ast.LtE: operator.le, ast.Gt: operator.gt,
                   ast.GtE: operator.ge, ast.Eq: operator.eq, ast.NotEq: operator.ne}

    def visit(item):
        if isinstance(item, ast.Constant) and isinstance(item.value, (float, int, bool)):
            return item.value
        if isinstance(item, ast.Name):
            return values[item.id]
        if isinstance(item, ast.Attribute):
            value = visit(item.value)
            if isinstance(value, dict):
                return value[item.attr]
            return value[("x", "y", "z", "w").index(item.attr)]
        if isinstance(item, ast.UnaryOp) and isinstance(item.op, ast.USub):
            return -visit(item.operand)
        if isinstance(item, ast.BinOp) and type(item.op) in operations:
            return operations[type(item.op)](visit(item.left), visit(item.right))
        if isinstance(item, ast.Compare) and len(item.ops) == 1:
            return comparisons[type(item.ops[0])](visit(item.left), visit(item.comparators[0]))
        if isinstance(item, ast.BoolOp):
            args = [bool(visit(x)) for x in item.values]
            return any(args) if isinstance(item.op, ast.Or) else all(args)
        if isinstance(item, ast.Call) and isinstance(item.func, ast.Name):
            return functions[item.func.id](*(visit(arg) for arg in item.args))
        raise ValueError(f"Unsupported actual-source expression: {text}")

    return visit(node(" ".join(text.split())))


def parse_if(source, start):
    paren = source.index("(", start)
    end = matching(source, paren, "(", ")")
    brace = source.index("{", end)
    close = matching(source, brace, "{", "}")
    yes = parse_block(source[brace+1:close])
    cursor = close+1
    while cursor < len(source) and source[cursor].isspace():
        cursor += 1
    no = []
    if source.startswith("else", cursor):
        cursor += 4
        while source[cursor].isspace():
            cursor += 1
        if source.startswith("if", cursor):
            nested, cursor = parse_if(source, cursor)
            no = [nested]
        else:
            close = matching(source, cursor, "{", "}")
            no = parse_block(source[cursor+1:close])
            cursor = close+1
    return ("if", source[paren+1:end], yes, no), cursor


def parse_block(source):
    result = []
    cursor = 0
    while cursor < len(source):
        if source[cursor].isspace():
            cursor += 1
        elif source.startswith("if", cursor):
            item, cursor = parse_if(source, cursor)
            result.append(item)
        else:
            end = source.index(";", cursor)
            text = source[cursor:end].strip()
            if text.startswith("return "):
                result.append(("return", text[7:]))
            else:
                match = re.fullmatch(r"(?:(?:highp|mediump|lowp)\s+)?(?:(?:float|vec[234])\s+)?"
                                     r"(\w+)\s*([+*\-/]?=)\s*(.+)", text, re.S)
                if match is None:
                    raise ValueError(f"Unsupported actual-source statement: {text}")
                result.append(("assign", *match.groups()))
            cursor = end+1
    return result


def execute(statements, values, functions):
    for item in statements:
        if item[0] == "return":
            return True, expression(item[1], values, functions)
        if item[0] == "if":
            branch = item[2] if expression(item[1], values, functions) else item[3]
            returned, value = execute(branch, values, functions)
            if returned:
                return returned, value
        else:
            _, name, operation, text = item
            value = expression(text, values, functions)
            if operation != "=":
                value = {"+=": operator.add, "*=": operator.mul,
                         "-=": operator.sub, "/=": operator.truediv}[operation](values[name], value)
            values[name] = value
    return False, None


def assignment(source, name, operation="="):
    matches = re.findall(r"\b" + re.escape(name) + r"\s*" + re.escape(operation) + r"\s*([^;]+);", source)
    if len(matches) != 1:
        raise ValueError(f"Expected one actual assignment {name}{operation}, found {len(matches)}")
    return matches[0]


class Model:
    def __init__(self, sources):
        self.sources = sources
        self.functions = {"sin": math.sin, "cos": math.cos, "sqrt": math.sqrt,
                          "abs": abs, "min": min, "max": max, "clamp": clamp,
                          "dot": dot, "length": length, "normalize": normalized,
                          "sign": lambda x: 1 if x > 0 else -1 if x < 0 else 0,
                          "sq": lambda x: x*x, "pow": pow, "exp2": lambda x: 2**x,
                          "saturate": lambda x: clamp(x, 0, 1), "smoothstep": smoothstep,
                          "mix": lambda a, b, t: a*(1-t)+b*t,
                          **{f"vec{n}": (lambda *x, n=n: vector(n, *x)) for n in (2, 3, 4)}}
        self.globals = {"PI": math.pi, "shading_reflected": Vec((0., 0., 1.))}
        for source, name, arguments in (("common", "acosFast", ("x",)),
                ("common", "acosFastPositive", ("x",)),
                ("producer", "integrateUniformArcArea", ("theta",)),
                ("producer", "integrateUniformArcMoment", ("theta",)),
                ("utils", "packOccludedMoment", ("moment",)),
                ("utils", "packBentNormal", ("bn",)),
                ("utils", "unpackBentNormal", ("bn",)),
                ("consumer", "SpecularAO_Lagarde", ("NoV", "visibility", "roughness")),
                ("consumer", "sphericalCapsIntersection", ("cosCap1", "cosCap2", "cosDistance")),
                ("consumer", "SpecularAO_OccludedMoments", ("encoded", "mass", "visibility", "NoV", "roughness"))):
            statements = parse_block(body(sources[source], name))
            def call(*args, statements=statements, arguments=arguments):
                returned, value = execute(statements, {**self.globals, **dict(zip(arguments, args))}, self.functions)
                if not returned:
                    raise ValueError("Actual function failed to return")
                return value
            self.functions[name] = call
        producer = body(sources["producer"], "groundTruthAmbientOcclusion")
        prefix = producer.split("float visibility", 1)[0]
        self.initializers = {name: assignment(prefix, name) for name in ("occludedFraction", "occludedMoment")}
        self.slice = {name: assignment(producer, name) for name in
                      ("fullLo", "fullHi", "fullArea", "visibleArea", "blocked")}
        self.slice["mass"] = assignment(producer, "occludedFraction", "+=")
        self.slice["moment"] = assignment(producer, "occludedMoment", "+=")
        self.mass_scale = assignment(producer.split("obscurance =", 1)[1], "occludedFraction")
        self.moment_scale = assignment(producer, "occludedMoment", "*=")
        material = clean(sources["material"])
        self.strength = assignment(material, "strength")
        self.mass_strength = assignment(material, "occludedFraction", "*=")
        self.moment_strength = assignment(material, "occludedMoment", "*=")

    def evaluate(self, text, values):
        return expression(text, values, self.functions)

    def arc(self, lo, hi):
        area = self.functions["integrateUniformArcArea"]
        moment = self.functions["integrateUniformArcMoment"]
        return area(hi)-area(lo), moment(hi)-moment(lo)

    def slice_value(self, n, lo, hi, view, tangent):
        values = {"n": n, "h0": lo, "h1": hi, "HALF_PI": math.pi/2,
                  "viewDir": view, "orthoDirection": tangent}
        for name in ("fullLo", "fullHi", "fullArea", "visibleArea", "blocked"):
            values[name] = self.evaluate(self.slice[name], values)
        return self.evaluate(self.slice["mass"], values), self.evaluate(self.slice["moment"], values)

    def finalize(self, mass, moment, slices, intensity):
        values = {"occludedFraction": mass, "occludedMoment": moment,
                  "materialParams": {"sliceCount": Vec((slices, 1/slices)), "intensity": intensity}}
        for name, text in self.initializers.items():
            initial = self.evaluate(text, {})
            if initial != (Vec((0., 0., 0.)) if name == "occludedMoment" else 0.):
                raise AssertionError("Nonzero actual blocked initializer")
        values["occludedFraction"] = self.evaluate(self.mass_scale, values)
        values["occludedMoment"] *= self.evaluate(self.moment_scale, values)
        values["strength"] = self.evaluate(self.strength, values)
        values["occludedFraction"] *= self.evaluate(self.mass_strength, values)
        values["occludedMoment"] *= self.evaluate(self.moment_strength, values)
        return values["occludedFraction"], values["occludedMoment"]

    def ao(self, moment, mass, reflected, intensity=1., quantized=False, visibility=.6, roughness=.35):
        encoded = self.functions["packOccludedMoment"](moment*intensity)
        mass *= intensity
        if quantized:
            encoded = Vec(round(clamp(x, 0, 1)*255)/255 for x in encoded)
            mass = round(clamp(mass, 0, 1)*255)/255
        self.globals["shading_reflected"] = reflected
        # Power-one scalar strength has the same intensity as the directional
        # field. This matters when quantization selects the scalar fallback.
        visibility = 1-intensity*(1-visibility)
        value = self.functions["SpecularAO_OccludedMoments"](encoded, mass, visibility, 0.8, roughness)
        if not math.isfinite(value) or not 0 <= value <= 1:
            raise AssertionError(f"Nonfinite/out-of-range actual cap result {value}")
        return value


def numeric_arc(lo, hi):
    result = [0., 0., 0.]
    boundaries = [lo] + ([0.] if lo < 0 < hi else []) + [hi]
    for a, b in zip(boundaries, boundaries[1:]):
        step = (b-a)/8192
        for index in range(8192):
            t = a+(index+.5)*step
            weight = abs(math.sin(t))*step
            for j, x in enumerate((1., math.cos(t), math.sin(t))):
                result[j] += weight*x
    return result[0], Vec(result[1:])


def check_producer(model):
    worst = 0.
    for lo, hi in ((-2.8, 2.7), (-2.3, -.05), (.01, 2.2), (-1.4, .8), (-.3, .2)):
        a, m = model.arc(lo, hi)
        b, n = numeric_arc(lo, hi)
        error = max(abs(a-b), *(abs(x-y) for x, y in zip(m, n)))
        if error > 8e-8:
            raise AssertionError(f"Actual uniform primitive fails independent quadrature: {error}")
        worst = max(worst, error)
    view = Vec((0., 0., 1.))
    for angle in (-1.3, -.4, 0., .6, 1.3):
        mass, moment = model.slice_value(angle, angle-math.pi/2, angle+math.pi/2, view, Vec((1., 0., 0.)))
        mass, moment = model.finalize(mass, moment, 1, .8)
        if mass != 0 or length(moment) > 1e-14 or model.ao(moment, mass, Vec((1., 0., 0.)), quantized=True) != 1:
            raise AssertionError("Actual unobstructed source hemisphere introduces extra masking")
    # Independent ray/rectangle intersections on equal solid-angle bins.
    mass = 0.
    moment = Vec((0., 0., 0.))
    ray_mass = 0.
    ray_moment = Vec((0., 0., 0.))
    reflections = []
    for theta, phi in ((65., 0.), (65., 180.), (36.87, 0.), (65., 49.4), (140., 0.)):
        theta, phi = math.radians(theta), math.radians(phi)
        reflections.append(Vec((math.sin(theta)*math.cos(phi), math.sin(theta)*math.sin(phi), math.cos(theta))))
    cap_cosine = math.cos(math.radians(40.))
    # This oracle is a uniform reflection cap, not a GGX/DFG certificate.
    cap_roughness = math.sqrt(-math.log2(cap_cosine)/3.321928)
    blocked_cap_counts = [0] * len(reflections)
    slices = 2048
    for index in range(slices):
        phi = (index+.5)*math.pi/slices
        u = Vec((math.cos(phi), math.sin(phi), 0.))
        lo, hi = -math.pi/2, math.pi/2
        if abs(u[0]) > 1e-15 and abs(.3*u[1]/u[0]) <= .35:
            horizon = math.atan(.3/(.4*abs(u[0])))
            if u[0] > 0:
                hi = horizon
            else:
                lo = -horizon
        a, m = model.slice_value(0., lo, hi, view, u)
        mass += a
        moment += m
    mass, moment = model.finalize(mass, moment, slices, 1.)
    bins = 256*512
    for iz in range(256):
        z = (iz+.5)/256
        radius = math.sqrt(1-z*z)
        for ip in range(512):
            phi = (ip+.5)*2*math.pi/512
            direction = Vec((radius*math.cos(phi), radius*math.sin(phi), z))
            if direction[0] > 0:
                t = .3/direction[0]
                if abs(t*direction[1]) <= .35 and t*z <= .4:
                    ray_mass += 1/bins
                    ray_moment += direction*(2/bins)
                    for index, reflected in enumerate(reflections):
                        blocked_cap_counts[index] += dot(direction, reflected) >= cap_cosine
    if max(abs(mass-ray_mass), *(abs(x-y) for x, y in zip(moment, ray_moment))) > .002:
        raise AssertionError("Actual missing-area/moment slice units fail world-ray oracle")
    inside = model.ao(moment, mass, normalized(Vec((.9063, 0., .4226))))
    outside = model.ao(moment, mass, normalized(Vec((-.9063, 0., .4226))))
    if not inside < .65 or not outside > .98:
        raise AssertionError(f"Independent fixed blocker disappears or masks out-of-lobe reflection: {inside}, {outside}")
    cap_errors = []
    for reflected, count in zip(reflections, blocked_cap_counts):
        reference = 1-count/(bins*(1-cap_cosine))
        approximation = model.ao(moment, mass, reflected, roughness=cap_roughness)
        cap_errors.append(abs(reference-approximation))
    return worst, mass, max(abs(x-y) for x, y in zip(moment, ray_moment)), max(cap_errors)


def check_fit(model):
    axis = normalized(Vec((.6, .3, .8)))
    quantization_worst = 0.
    low_mass_cases = 0
    resolved_cases = 0
    clamped_cases = 0
    max_cosine_error = 0.
    max_axis_angle = 0.
    max_opacity_error = 0.
    bounds_slack = 0.
    moment_error_bound = math.sqrt(3)/254
    mass_error_bound = 1/510
    for radius in (10., 25., 50.):
        c = math.cos(math.radians(radius))
        for opacity in (.2, .6, 1.):
            mass = opacity*(1-c)
            moment = axis*(mass*(1+c))
            full = model.ao(moment, mass, axis)
            for strength in (0., .25, .8, 1.):
                actual = model.ao(moment, mass, axis, strength)
                expected = 1-strength*(1-full)
                if abs(actual-expected) > 1e-12:
                    raise AssertionError("Actual strength changes fitted aperture or loses a blocker")
                quantized = model.ao(moment, mass, axis, strength, quantized=True)
                if mass*strength >= 8/255:
                    resolved_cases += 1
                    quantization_worst = max(quantization_worst, abs(actual-quantized))
                    exact_mass = mass*strength
                    exact_moment = moment*strength
                    encoded = model.functions["packOccludedMoment"](exact_moment)
                    encoded = Vec(round(clamp(x, 0, 1)*255)/255 for x in encoded)
                    observed = model.evaluate(assignment(body(model.sources["consumer"], "SpecularAO_OccludedMoments"), "moment"),
                                              {"encoded": encoded})
                    observed_mass = round(exact_mass*255)/255
                    magnitude = length(observed)
                    if length(observed-exact_moment) > moment_error_bound+1e-14 or abs(observed_mass-exact_mass) > mass_error_bound+1e-14:
                        raise AssertionError("RGBA8 error exceeds component-wise rounding bound")
                    if magnitude > observed_mass:
                        raw_cosine = magnitude/observed_mass-1
                        fitted_cosine = clamp(raw_cosine, 0, 1-observed_mass)
                        clamped_cases += raw_cosine > 1-observed_mass
                        # Valid exact caps have |M|/mass <= 2. The bound is
                        # deliberately loose and diverges as mass approaches 0.
                        ratio_bound = (moment_error_bound+2*mass_error_bound)/(exact_mass-mass_error_bound)
                        if abs(raw_cosine-c) > ratio_bound+1e-14:
                            raise AssertionError("RGBA8 cap ratio exceeds perturbation bound")
                        bounds_slack = max(bounds_slack, ratio_bound)
                        max_cosine_error = max(max_cosine_error, abs(fitted_cosine-c))
                        max_opacity_error = max(max_opacity_error, abs(observed_mass/(1-fitted_cosine)-opacity*strength))
                        max_axis_angle = max(max_axis_angle, math.degrees(math.acos(clamp(dot(normalized(observed), axis), -1, 1))))
                else:
                    low_mass_cases += 1
    encoded = model.functions["packOccludedMoment"](Vec((0., 0., 0.)))
    if tuple(round(x*255) for x in encoded) != (128, 128, 128):
        raise AssertionError("Actual moment packing lacks byte128 neutral")
    # Follow the real blur's legacy affine unpack/pack through a neutral field.
    blurred = model.functions["packBentNormal"](model.functions["unpackBentNormal"](encoded))
    if tuple(round(x*255) for x in blurred) != (128, 128, 128):
        raise AssertionError("Neutral field changes through actual blur affine packing")
    # The existing affine blur encoding must also preserve a weighted moment,
    # even though its intermediate legacy 'bn' is not a physical unit normal.
    a, b = Vec((.15, -.1, .07)), Vec((-.02, .2, .05))
    pa, pb = (model.functions["packOccludedMoment"](x) for x in (a, b))
    mixture = model.functions["packBentNormal"](
        model.functions["unpackBentNormal"](pa)*.3+model.functions["unpackBentNormal"](pb)*.7)
    expected = model.functions["packOccludedMoment"](a*.3+b*.7)
    if max(abs(x-y) for x, y in zip(mixture, expected)) > 1e-14:
        raise AssertionError("Actual affine blur changes weighted blocked moments")
    for mass in (1/255, .1, .8):
        model.ao(Vec((0., 0., 0.)), mass, axis, quantized=True)
    # Opaque caps around +Z deliberately exercise the rounding-triggered
    # opacity clamp. Their exact cosine-weighted diffuse visibility is c*c.
    # Quantization may move the fitted aperture; it is not an intensity policy.
    clamp_control_count = 0
    clamp_control_error = 0.
    for radius in (5., 25.):
        c = math.cos(math.radians(radius))
        mass = 1-c
        axis_z = Vec((0., 0., 1.))
        moment = axis_z*(mass*(1+c))
        for strength in (.125, .25, .8, 1.):
            encoded = model.functions["packOccludedMoment"](moment*strength)
            decoded = Vec((round(x*255)-128)/127 for x in encoded)
            observed_mass = round(mass*strength*255)/255
            if observed_mass > 0 and length(decoded)/observed_mass-1 > 1-observed_mass:
                clamp_control_count += 1
            unquantized = model.ao(moment, mass, axis_z, strength, visibility=c*c)
            quantized = model.ao(moment, mass, axis_z, strength, quantized=True, visibility=c*c)
            clamp_control_error = max(clamp_control_error, abs(unquantized-quantized))
    if clamp_control_count == 0:
        raise AssertionError("RGBA8 opacity-clamp control did not engage")
    # Cap shape errors are not hidden by a universal quality assertion. The
    # pure cap fit is qualified; dispersed/multiple blockers use scalar fallback.
    # This helper is an additional screen term. The full material also takes
    # the minimum with its unchanged static SIMPLE / authored cone term.
    return dict(resolved_RGBA8_AO_error=quantization_worst, low_mass_cases=low_mass_cases,
                resolved_cases=resolved_cases, quantization_clamped_cases=clamped_cases,
                max_quantized_cosine_error=max_cosine_error, max_quantized_axis_degrees=max_axis_angle,
                max_quantized_opacity_error=max_opacity_error, maximum_ratio_error_bound=bounds_slack,
                moment_L2_rounding_bound=moment_error_bound, mass_rounding_bound=mass_error_bound,
                active_opacity_clamp_controls=clamp_control_count,
                opacity_clamp_control_AO_error=clamp_control_error)


def expect_failure(action, label):
    try:
        action()
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"Negative control unexpectedly passed: {label}")


def check_negative_controls(sources):
    producer = clean(sources["producer"])
    initialized = assignment(body(producer, "groundTruthAmbientOcclusion").split("float visibility", 1)[0], "occludedMoment")
    missing = producer.replace("occludedMoment = " + initialized + ";", "", 1)
    if missing == producer:
        raise ValueError("Initializer mutation must affect actual producer")
    expect_failure(lambda: check_producer(Model({**sources, "producer": missing})), "missing moment initializer")
    neutral = sources["utils"].replace("(128.0 / 255.0)", "0.5")
    if neutral == sources["utils"]:
        raise ValueError("Neutral mutation must affect actual packing")
    expect_failure(lambda: check_fit(Model({**sources, "utils": neutral})), "half-byte neutral")
    consumer = clean(sources["consumer"])
    disappearing = consumer.replace(body(consumer, "SpecularAO_OccludedMoments"), "return 1.0;", 1)
    if disappearing == consumer:
        raise ValueError("Disappearing-blocker mutation must affect actual consumer")
    expect_failure(lambda: check_producer(Model({**sources, "consumer": disappearing})), "disappearing blocker")
    aperture = consumer.replace(assignment(body(consumer, "SpecularAO_OccludedMoments"), "cosOccluded"), "1.0 - mass", 1)
    if aperture == consumer:
        raise ValueError("Aperture mutation must affect actual consumer")
    expect_failure(lambda: check_fit(Model({**sources, "consumer": aperture})), "intensity aperture shrink")


def load_sources(filament):
    materials = filament/"filament/src/materials/ssao"
    return {"producer": (materials/"gtaoImpl.fs").read_text(),
            "material": (materials/"gtaoBentNormals.mat").read_text(),
            "utils": (materials/"ssaoUtils.fs").read_text(),
            "consumer": (filament/"shaders/src/surface_ambient_occlusion.fs").read_text(),
            "common": (filament/"shaders/src/common_math.glsl").read_text()}


def check_source(filament):
    sources = load_sources(filament)
    model = Model(sources)
    primitive, mass, moment_error, cap_error = check_producer(model)
    quantization = check_fit(model)
    check_negative_controls(sources)
    return dict(primitive_error=primitive, world_blocker_mass=mass, world_moment_error=moment_error,
                rectangle_cap_approximation_max_error=cap_error, **quantization)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filament", type=Path, required=True)
    parser.add_argument("--matc", type=Path, required=True)
    args = parser.parse_args()
    metrics = check_source(args.filament)
    materials = args.filament/"filament/src/materials/ssao"
    with tempfile.TemporaryDirectory(prefix="heaven-occluded-moment-contract-") as temporary:
        for platform in ("desktop", "mobile"):
            for name in ("gtao", "gtaoBentNormals"):
                output = Path(temporary)/f"{platform}-{name}.filamat"
                command = [str(args.matc), "-a", "opengl", "-a", "vulkan", "-p", platform,
                           "-o", str(output), str(materials/(name+".mat"))]
                completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
                if completed.returncode or not output.exists() or output.stat().st_size == 0:
                    raise RuntimeError(f"Actual {platform}/{name} compilation failed:\n{completed.stdout}{completed.stderr}")
    print(f"PASS: actual uniform missing-area/moment source, independent world blocker, strength fit, "
          f"neutral encoding and four negatives; desktop/mobile scalar/bent sources compile. {metrics}. "
          "RGBA8 errors are reported, not a global quality certificate; dispersed/clipped/multiple caps "
          "and low-mass fits remain approximation limits. No GPU appearance claim.")


if __name__ == "__main__":
    main()
