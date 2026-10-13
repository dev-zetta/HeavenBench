#!/usr/bin/env python3
"""Opt-in native generated/real UI pixel oracle; no cadence or scanout claim."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_ppm(path):
    content = path.read_bytes()
    header = re.match(rb"P6\n(\d+) (\d+)\n255\n", content)
    require(header is not None, f"Invalid canonical PPM header: {path}")
    width, height = map(int, header.groups())
    pixels = content[header.end():]
    require(len(pixels) == width * height * 3, f"Wrong PPM payload length: {path}")
    return width, height, pixels


def read_rgba_pam(path):
    content = path.read_bytes()
    header, marker, pixels = content.partition(b"ENDHDR\n")
    require(marker and header.startswith(b"P7\n"), f"Invalid PAM header: {path}")
    fields = {}
    for line in header.decode("ascii").splitlines()[1:]:
        key, value = line.split(" ", 1)
        require(key not in fields, f"Duplicate PAM field: {key}")
        fields[key] = value
    require(set(fields) == {"WIDTH", "HEIGHT", "DEPTH", "MAXVAL", "TUPLTYPE"},
            "Unexpected PAM fields")
    require((fields["DEPTH"], fields["MAXVAL"], fields["TUPLTYPE"]) == ("4", "255", "RGB_ALPHA"),
            "Expected raw RGBA8 PAM")
    width, height = int(fields["WIDTH"]), int(fields["HEIGHT"])
    require(len(pixels) == width * height * 4, "Wrong PAM payload length")
    return width, height, pixels


def fractional_check(width, height, before, ui, generated, source, control=None):
    current_x = 20 if (source - 1) % 2 == 0 else 40
    old_x = 40 if current_x == 20 else 20
    # Independently authored RGB/coverage, before rasterization or the UI
    # compute. The captured layer is checked against these values and is never
    # used to select expected alpha or expected premultiplied color.
    probes = [(22, 6, (220, 75, 29, 96)),
              (38, 6, (32, 160, 245, 192)),
              (current_x + 2, 38, (164, 43, 211, 128)),
              (old_x + 2, 38, (0, 0, 0, 0)),
              (22, 52, (7, 249, 131, 0))]
    maximum_layer_error = maximum_blend_error = 0.0
    partial = transparent = 0
    for left, top, paint in probes:
        alpha = paint[3] / 255.0
        for y in range(top, top + 8):
            for x in range(left, left + 8):
                rgb_offset, rgba_offset = (y * width + x) * 3, (y * width + x) * 4
                expected_ui = tuple(color * alpha for color in paint[:3]) + (paint[3],)
                maximum_layer_error = max(maximum_layer_error,
                    *(abs(ui[rgba_offset + channel] - expected_ui[channel]) for channel in range(4)))
                for channel, color in enumerate(paint[:3]):
                    scene = before[rgb_offset + channel]
                    expected = color * alpha + scene * (1 - alpha)
                    if control == "straight_alpha":
                        expected = min(255, color + scene * (1 - alpha)) if alpha else scene
                    elif control == "squared_alpha":
                        expected = color * alpha * alpha + scene * (1 - alpha)
                    elif control == "no_blend":
                        expected = scene
                    maximum_blend_error = max(maximum_blend_error,
                        abs(generated[rgb_offset + channel] - expected))
                if alpha:
                    partial += 1
                else:
                    transparent += 1
    require(maximum_layer_error <= 1.01,
            f"Source {source}: UI premultiplication/clear error {maximum_layer_error}")
    # The UI target and final generated image each round once to RGBA8.
    require(maximum_blend_error <= 1.51,
            f"Source {source}: generated fractional-UI blend error {maximum_blend_error}")
    return {"partial_alpha_pixels": partial, "transparent_pixels": transparent,
            "maximum_layer_byte_error": maximum_layer_error,
            "maximum_blend_byte_error": maximum_blend_error}


def pixel_check(width, height, pixels, source):
    require(width >= 64 and height >= 64, "Native UI oracle needs at least 64x64")
    # These are independently specified display-domain input bytes, not shader
    # output samples or a comparison to another image rendered by the same code.
    current_x = 20 if (source - 1) % 2 == 0 else 40
    old_x = 40 if current_x == 20 else 20
    probes = [(6, 6, (232, 36, 188)),
              (width - 14, height - 14, (29, 173, 241)),
              (current_x + 2, 22, (17, 231, 69)),
              (6, 38, (16 + 16 * source, 73, 201))]
    maximum_error = 0
    for left, top, expected in probes:
        for y in range(top, top + 8):
            for x in range(left, left + 8):
                offset = (y * width + x) * 3
                error = max(abs(pixels[offset + c] - expected[c]) for c in range(3))
                maximum_error = max(maximum_error, error)
    require(maximum_error <= 1, f"Source {source}: opaque UI byte error {maximum_error}")
    removed = 0
    for y in range(22, 30):
        for x in range(old_x + 2, old_x + 10):
            offset = (y * width + x) * 3
            removed += max(abs(pixels[offset + c] - (17, 231, 69)[c]) for c in range(3)) > 8
    require(removed >= 61, f"Source {source}: previous widget remains ({64 - removed}/64 pixels)")
    return {"opaque_probe_pixels": 256, "maximum_byte_error": maximum_error,
            "removed_widget_pixels": removed, "removed_widget_probe_pixels": 64}


def inspect(directory):
    records = [json.loads(path.read_text()) | {"metadata": path.name}
               for path in directory.glob("native-*.json")]
    records.sort(key=lambda record: record["source_frame"])
    require(len(records) == 4, f"Expected four native pairs, got {len(records)}")
    previous = 0
    results = []
    negative_controls = {"wrong_source_phase": 0, "wrong_source_same_parity": 0,
                         "flipped_ui_orientation": 0, "straight_alpha": 0,
                         "squared_alpha": 0, "no_blend": 0}
    for record in records:
        source = record["source_frame"]
        require(previous < source <= 12, "Pairs must come from distinct initial accepted sources")
        previous = source
        require(record["diagnostic_gpu_wait_enabled"] is True, "Expected fenced diagnostic readback")
        require(record["display_scanout_verified"] is False, "Native readback must not claim scanout")
        require(record["vk_format"] in (37, 44), "Pixel oracle currently requires UNORM SDR presentation")
        require(record["ui_inputs_source_frame"] == source, "UI input source differs from native pair")
        require(record["generated_before_ui_vk_format"] == 37 and record["ui_layer_vk_format"] == 37,
                "Generated/UI inputs must preserve actual RGBA8_UNORM bytes")
        times = [record[key] for key in ("generated_present_return_cpu_monotonic_ns",
                                        "real_present_return_cpu_monotonic_ns",
                                        "readback_fence_completed_cpu_monotonic_ns")]
        require(times == sorted(times), "Native pair return/readback timestamps are out of order")
        inputs, input_hashes = {}, {}
        for kind, reader in (("generated_before_ui", read_ppm), ("ui_layer", read_rgba_pam)):
            filename = record[kind]
            require(Path(filename).name == filename, "Input receipt image must be a local basename")
            width, height, pixels = reader(directory / filename)
            require((width, height) == (record["width"], record["height"]), "UI input extent mismatch")
            inputs[kind] = pixels
            input_hashes[kind] = hashlib.sha256((directory / filename).read_bytes()).hexdigest()
        for kind in ("generated", "real"):
            filename = record[kind]
            require(Path(filename).name == filename, "Receipt image must be a local basename")
            path = directory / filename
            width, height, pixels = read_ppm(path)
            require((width, height) == (record["width"], record["height"]), "Extent disagrees with receipt")
            result = pixel_check(width, height, pixels, source)
            if kind == "generated":
                result["fractional_alpha"] = fractional_check(width, height,
                    inputs["generated_before_ui"], inputs["ui_layer"], pixels, source)
                result["input_sha256"] = input_hashes
                for control in ("straight_alpha", "squared_alpha", "no_blend"):
                    try:
                        fractional_check(width, height, inputs["generated_before_ui"],
                                         inputs["ui_layer"], pixels, source, control)
                    except AssertionError:
                        negative_controls[control] += 1
                    else:
                        raise AssertionError(f"Fractional negative unexpectedly passed: {control}")
            result.update(kind=kind, source_frame=source, image=filename,
                          sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            results.append(result)
            wrong_phase = (pixels, source + 1, "wrong_source_phase")
            wrong_source = (pixels, source + 2, "wrong_source_same_parity")
            rows = [pixels[y * width * 3:(y + 1) * width * 3] for y in range(height)]
            flipped = (b"".join(reversed(rows)), source, "flipped_ui_orientation")
            for control_pixels, control_source, label in (wrong_phase, wrong_source, flipped):
                try:
                    pixel_check(width, height, control_pixels, control_source)
                except AssertionError:
                    negative_controls[label] += 1
                else:
                    raise AssertionError(f"Negative control unexpectedly passed: {label}")
    return results, negative_controls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    pairs = work / "native-pairs"
    pairs.mkdir(exist_ok=False)
    env = os.environ | {"HEAVEN_SMOKE": "1", "HEAVEN_SMOKE_CASE": "frame_generation",
                        "HEAVEN_SMOKE_FRAME_GENERATION": "1", "HEAVEN_SMOKE_FG_UI_ORACLE": "1",
                        "HEAVEN_FG_CAPTURE_DIR": str(pairs), "HEAVEN_FG_CAPTURE_UI_INPUTS": "1",
                        "HEAVEN_FG_CAPTURE_START_SOURCE": "1", "HEAVEN_DATA": str(args.data.resolve())}
    log = work / "native.log"
    receipt = {"schema": 1, "scope": "Actual FSR generated and real native UI pixels; diagnostic GPU waits",
               "binary": str(args.binary.resolve()), "data": str(args.data.resolve()),
               "cadence_latency_scanout_verified": False, "passed": False}
    try:
        with log.open("w") as stream:
            completed = subprocess.run([str(args.binary.resolve())], env=env, stdout=stream,
                                       stderr=subprocess.STDOUT, timeout=180, check=False)
        receipt["native_exit"] = completed.returncode
        output = log.read_text(errors="replace")
        receipt["validation_errors"] = output.count("Validation Error")
        receipt["validation_warnings"] = output.count("Validation Warning")
        require(completed.returncode == 0, f"Native fixture exited {completed.returncode}; see {log}")
        require("native UI oracle:" in output and "case 'frame_generation': PASS" in output,
                "Native fixture did not execute the expected UI/lifecycle stimuli")
        require(not receipt["validation_errors"] and not receipt["validation_warnings"],
                "Native fixture reported Vulkan validation errors/warnings")
        receipt["images"], receipt["negative_controls_rejected"] = inspect(pairs)
        receipt["passed"] = True
    except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        receipt["error"] = str(error)
    finally:
        if log.exists():
            receipt["log_sha256"] = hashlib.sha256(log.read_bytes()).hexdigest()
        (work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
