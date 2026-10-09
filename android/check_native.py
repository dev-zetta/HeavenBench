#!/usr/bin/env python3
"""Check the Android ELF ABI, page alignment, JNI entry, and packaged dependencies."""
import argparse
from pathlib import Path
import re
import struct
import subprocess


def check(directory: Path, abi: str, tools: Path) -> None:
    machines = {"arm64-v8a": 183, "x86_64": 62}
    system_libraries = {
        "libandroid.so", "libc.so", "libdl.so", "libEGL.so", "libGLESv1_CM.so",
        "libGLESv2.so", "libGLESv3.so", "liblog.so", "libm.so", "libOpenSLES.so",
        "libvulkan.so", "libz.so",
    }
    libraries = {path.name: path for path in directory.glob("*.so")}
    for required in ("libSDL2.so", "libheaven_benchmark.so"):
        if required not in libraries:
            raise ValueError(f"Missing Android library: {required}")
    for name, path in libraries.items():
        data = path.read_bytes()
        if data[:6] != b"\x7fELF\x02\x01" or len(data) < 64:
            raise ValueError(f"{name}: expected a little-endian ELF64 shared library")
        file_type, machine = struct.unpack_from("<HH", data, 16)
        if file_type != 3 or machine != machines[abi]:
            raise ValueError(f"{name}: incorrect Android shared-library ABI")
        offset = struct.unpack_from("<Q", data, 32)[0]
        entry_size, count = struct.unpack_from("<HH", data, 54)
        if entry_size < 56 or offset + count * entry_size > len(data):
            raise ValueError(f"{name}: invalid ELF program headers")
        load_segments = 0
        for index in range(count):
            header = offset + index * entry_size
            if struct.unpack_from("<I", data, header)[0] == 1:
                load_segments += 1
                alignment = struct.unpack_from("<Q", data, header + 48)[0]
                if alignment < 16384:
                    raise ValueError(f"{name}: LOAD segment is not aligned for 16 KiB pages")
        if load_segments == 0:
            raise ValueError(f"{name}: no ELF LOAD segments")
        dynamic = subprocess.check_output([tools / "llvm-readelf", "-d", path], text=True)
        dependencies = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
        missing = set(dependencies) - system_libraries - libraries.keys()
        if missing:
            raise ValueError(f"{name}: unpackaged dependencies: {sorted(missing)}")
    exports = subprocess.check_output(
        [tools / "llvm-nm", "-D", "--defined-only", libraries["libheaven_benchmark.so"]], text=True)
    for symbol in ("SDL_main", "JNI_OnLoad"):
        if not re.search(rf"\bT {symbol}$", exports, re.MULTILINE):
            raise ValueError(f"Renderer does not export required Android entry: {symbol}")
    print(f"Verified {len(libraries)} Android ELF libraries: {abi}, SDL/JNI entries, 16 KiB alignment, dependencies")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--abi", choices=("arm64-v8a", "x86_64"), required=True)
    parser.add_argument("--tools", type=Path, required=True)
    args = parser.parse_args()
    try:
        check(args.directory, args.abi, args.tools)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
