#!/usr/bin/env python3
"""Preserve clang-tidy output; recognize only pinned Filament false positives.

Filament's quaternion union stores four floats and operator[] asserts i < 4.
Clang 21.1.1's ArrayBound checker follows (&x)[i] as a single scalar in the grass
packTangentFrame path. These two findings use indices 1 or 2, not invalid
quaternion indices. Do not broaden this exception when the dependency changes:
review the new source and diagnostic trace instead. ArrayBound stays enabled.

Clang 21.1.8 also rejects valid combinations of the fixed-underlying uint16_t
TextureUsage bitmask. Only three reviewed callers and their entire diagnostic
traces are recognized below; EnumCastOutOfRange remains enabled elsewhere.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
QUAT_PATH = Path("lib/filament/libs/math/include/math/quat.h")
QUAT_SHA256 = "d50bd4346a4815e87b65b6479147e37b75de965d2d5ccfa32f1c7bc8ca990fc5"
GRASS_PATH = Path("benchmark/src/rhi/features/grass_feature.cpp")
HELPER_PATH = Path("benchmark/src/rhi/grass_tangent_upload.h")
# Byte offsets and line numbers are checked in addition to the whole-file hash.
BASELINE = {
    1930: (74, "q...x", (1,)),
    2034: (79, "quat...x", (1, 2)),
}
BITMASK_PATH = Path("lib/filament/libs/utils/include/utils/BitmaskEnum.h")
DRIVER_ENUMS_PATH = Path("lib/filament/filament/backend/include/backend/DriverEnums.h")
TEXTURE_USAGE_SOURCES = {
    BITMASK_PATH: "ced2077e04ba983bd8c775ed30d085a20b66a2818f99bdb9161db44a87f59acf",
    DRIVER_ENUMS_PATH: "06cd2d117dff36dc9eb0d806f657168b9e626a9a3c38fceb95da7e8400ff4023",
}
# Full caller hashes, expression offsets and trace hashes intentionally require
# renewed review after source or analyzer changes. Fixtures retain all notes.
# The October 14 review retains the unchanged enum and bitmask expressions;
# NativeCommand.h adds 35 bytes before TextureUsage, shifting its trace offsets.
TEXTURE_USAGE_CALLERS = {
    Path("benchmark/src/rhi/height_sample_batch.cpp"): {
        "source_sha256": "090a76c61532083225d20b8ad0187a197e55987372eda9162920d7dd314ce976",
        "trace_sha256": "1c60df57202ec190bd7f0e5eadfa15bc61bb0b10e785616f9912413a72b6d1f5",
        "value": 65, "call_offset": 5094, "call_line": 122,
        "expression": "filament::Texture::Usage::COLOR_ATTACHMENT | filament::Texture::Usage::BLIT_SRC",
    },
    Path("benchmark/smoke/smoke_displacement_surface.cpp"): {
        "source_sha256": "022e5f28ff6d6d7ce9c2ac0991b87978ad8436d73395a14278f015e633aa8b7a",
        "trace_sha256": "73e62decb5d06db7a0ea9d73a34aae8fd86655c420d26a0d17e834882d648e83",
        "value": 65, "call_offset": 55152, "call_line": 889,
        "expression": "filament::Texture::Usage::COLOR_ATTACHMENT|filament::Texture::Usage::BLIT_SRC",
    },
    Path("benchmark/src/rhi/render_test_scene_assets.cpp"): {
        "source_sha256": "d72e9c502f8d7192a8c00b3acde6950ce3d29d802254040dbfa01640b8a0930a",
        "trace_sha256": "5a097ab28216dcf5ac5ebe18ab0f74071e18f879a321aea93b7c487c3bc1de1c",
        "value": 536, "call_offset": 1226, "call_line": 41,
        "expression": "Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE",
    },
}


def same_path(value, expected):
    return (isinstance(value, str) and Path(value).is_absolute()
            and Path(value).resolve() == expected.resolve())


def permitted_report(report):
    """Fail closed unless every exported diagnostic matches the reviewed trace."""
    if not isinstance(report, dict) or not same_path(
            report.get("MainSourceFile"), REPO_ROOT / GRASS_PATH):
        return False
    diagnostics = report.get("Diagnostics")
    if not isinstance(diagnostics, list) or not diagnostics:
        return False
    quat = REPO_ROOT / QUAT_PATH
    source = quat.read_bytes()
    if hashlib.sha256(source).hexdigest() != QUAT_SHA256:
        return False
    for diagnostic in diagnostics:
        if not isinstance(diagnostic, dict) or diagnostic.get("DiagnosticName") != (
                "clang-analyzer-security.ArrayBound") or diagnostic.get("Level") != "Error":
            return False
        message = diagnostic.get("DiagnosticMessage")
        if not isinstance(message, dict):
            return False
        offset = message.get("FileOffset")
        if type(offset) is not int or offset not in BASELINE:
            return False
        line, variable, indices = BASELINE[offset]
        if (not same_path(message.get("FilePath"), quat)
                or message.get("Message") != (
                    f"Out of bound access to memory after the end of '{variable}'")
                or message.get("Replacements") != []
                or source[:offset].count(b"\n") + 1 != line):
            return False
        notes = diagnostic.get("Notes")
        if not isinstance(notes, list) or not all(
                isinstance(note, dict) and note.get("Replacements") == [] for note in notes):
            return False
        if not any(same_path(note.get("FilePath"), REPO_ROOT / HELPER_PATH)
                   and note.get("Message") == "Calling 'TMat33::packTangentFrame'"
                   for note in notes):
            return False
        expected_notes = {
            f"Access of '{variable}' at index {index}, while it holds only a single 'float' element"
            for index in indices
        }
        bound_notes = [note for note in notes
                       if same_path(note.get("FilePath"), quat)
                       and isinstance(note.get("Message"), str)
                       and note["Message"].startswith("Access of ")]
        if (len(bound_notes) != 1 or bound_notes[0].get("FileOffset") != offset
                or bound_notes[0].get("Message") not in expected_notes):
            return False
    return True


def normalized_texture_trace(value, allowed_paths):
    """Normalize only repo location; preserve every diagnostic/trace field."""
    if isinstance(value, dict):
        result = {}
        for key, child in value.items():
            if key == "FilePath":
                if not isinstance(child, str) or not Path(child).is_absolute():
                    raise ValueError("Diagnostic source path must be absolute")
                path = Path(child).resolve().relative_to(REPO_ROOT)
                if path not in allowed_paths:
                    raise ValueError("Unreviewed diagnostic source path")
                result[key] = path.as_posix()
            else:
                result[key] = normalized_texture_trace(child, allowed_paths)
        return result
    if isinstance(value, list):
        return [normalized_texture_trace(child, allowed_paths) for child in value]
    return value


def permitted_texture_usage_report(report):
    """Accept one exact valid bitmask finding, never other enum diagnostics."""
    if not isinstance(report, dict):
        return False
    caller = next((path for path in TEXTURE_USAGE_CALLERS
                   if same_path(report.get("MainSourceFile"), REPO_ROOT / path)), None)
    if caller is None:
        return False
    diagnostics = report.get("Diagnostics")
    if not isinstance(diagnostics, list) or len(diagnostics) != 1:
        return False
    expected = TEXTURE_USAGE_CALLERS[caller]
    sources = TEXTURE_USAGE_SOURCES.copy()
    sources[caller] = expected["source_sha256"]
    for path, sha256 in sources.items():
        if hashlib.sha256((REPO_ROOT / path).read_bytes()).hexdigest() != sha256:
            return False
    source = (REPO_ROOT / caller).read_bytes()
    offset = expected["call_offset"]
    if (not source[offset:].startswith(expected["expression"].encode("utf-8"))
            or source[:offset].count(b"\n") + 1 != expected["call_line"]):
        return False
    diagnostic = diagnostics[0]
    if (not isinstance(diagnostic, dict)
            or diagnostic.get("DiagnosticName") != "clang-analyzer-optin.core.EnumCastOutOfRange"
            or diagnostic.get("Level") != "Error"):
        return False
    message = diagnostic.get("DiagnosticMessage")
    if (not isinstance(message, dict)
            or message.get("Message") != (
                f"The value '{expected['value']}' provided to the cast expression "
                "is not in the valid range of values for 'TextureUsage'")
            or not same_path(message.get("FilePath"), REPO_ROOT / BITMASK_PATH)
            or message.get("FileOffset") != 3295 or message.get("Replacements") != []):
        return False
    trace_diagnostic = diagnostic.copy()
    # The compile-database location is environment metadata, not a trace field.
    trace_diagnostic.pop("BuildDirectory", None)
    try:
        normalized = normalized_texture_trace(trace_diagnostic, set(sources))
    except (ValueError, TypeError):
        return False
    trace = json.dumps(normalized, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(trace).hexdigest() == expected["trace_sha256"]


def main(args):
    real_tidy = os.environ.get("HEAVEN_CLANG_TIDY_REAL")
    if not real_tidy or Path(real_tidy).resolve() == Path(__file__).resolve():
        print("error: HEAVEN_CLANG_TIDY_REAL must select the real clang-tidy", file=sys.stderr)
        return 2
    if any(arg.lstrip("-").split("=", 1)[0] == "export-fixes" for arg in args):
        print("error: the project clang-tidy gate owns --export-fixes", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="heaven-clang-tidy-") as temporary:
        export_path = Path(temporary) / "diagnostics.yaml"
        args = list(args)
        position = args.index("--") if "--" in args else len(args)
        args.insert(position, f"--export-fixes={export_path}")
        try:
            # Inherit both output streams so diagnostics and traces remain visible.
            status = subprocess.run([real_tidy, *args], check=False).returncode
        except OSError as error:
            print(f"error: cannot execute clang-tidy: {error}", file=sys.stderr)
            return 2
        if status != 1:
            return 128 - status if status < 0 else status
        try:
            report = yaml.safe_load(export_path.read_text(encoding="utf-8"))
            permitted = permitted_report(report) or permitted_texture_usage_report(report)
        except (OSError, UnicodeError, ValueError, TypeError, yaml.YAMLError):
            permitted = False
        if not permitted:
            return status
        print("clang-tidy gate: accepted only a pinned Filament diagnostic "
              "baseline (diagnostics retained above)", file=sys.stderr)
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
