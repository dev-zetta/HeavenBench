#!/usr/bin/env python3
"""GPU-free source-aware integrity/key checks for the pinned SDK verifier."""
import hashlib
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys
import tempfile

sys.dont_write_bytecode = True
import verify_fsr31_source as verifier


def require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def reject(sdk, lock, reason):
    try:
        verifier.verify_source(sdk, lock)
    except SystemExit as error:
        require(reason in str(error), f"Unexpected rejection: {error}")
    else:
        raise AssertionError(f"Verifier accepted {reason}")


def main():
    if sys.argv[1:] == ["--negative-native-separators"]:
        verifier.manifest_key = str
    else:
        require(len(sys.argv) == 1, "Unknown verifier-test argument")

    # Exercise the exact production helper with both native path conventions,
    # independently of the operating system running this CPU test.
    for path_type in (PureWindowsPath, PurePosixPath):
        path = path_type("sdk/include/FidelityFX/host/ffx_types.h")
        key = verifier.manifest_key(path)
        require(key == "sdk/include/FidelityFX/host/ffx_types.h",
            f"Windows manifest key retained native separators: {key!r}")
        require(verifier.manifest_key(path.parent) == "sdk/include/FidelityFX/host",
            "Manifest parent directory is not portable")
        require(verifier.manifest_key(path_type("sdk/src/backends/vk")) == "sdk/src/backends/vk",
            "Backend-only directory exemption is not portable")
        require(verifier.manifest_key(path_type("LICENSE.txt").parent) == ".",
            "Root-only directory exemption changed")

    with tempfile.TemporaryDirectory(prefix="heaven-fsr-source-") as temporary:
        root = Path(temporary)
        sdk = root / "source"
        originals = {"LICENSE.txt": b"fixture notice\n",
            "sdk/src/backends/vk/ffx_vk.cpp": b"fixture reviewed backend\n",
            "sdk/include/fixture/header.h": b"fixture reviewed header\n"}
        for relative, content in originals.items():
            path = sdk / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        # Match the production exception for the one locked backend file:
        # unrelated upstream files in this directory remain outside the build.
        (sdk / "sdk/src/backends/vk/uncompiled.cpp").write_bytes(b"not a build input\n")
        lock = root / "sources.sha256"
        lock.write_text("".join(f"{hashlib.sha256(content).hexdigest()}  {relative}\n"
            for relative, content in originals.items()))
        require(verifier.verify_source(sdk, lock) == len(originals),
            "Reviewed source allowlist did not pass")
        extra = sdk / "sdk/include/fixture/injected.h"
        extra.write_bytes(b"unreviewed injected header\n")
        reject(sdk, lock, "Unexpected source")
        extra.unlink()
        header = sdk / "sdk/include/fixture/header.h"
        header.write_bytes(b"modified reviewed source\n")
        reject(sdk, lock, "modified")
        header.unlink()
        reject(sdk, lock, "missing/replaced")
        header.write_bytes(originals["sdk/include/fixture/header.h"])
        require(verifier.verify_source(sdk, lock) == len(originals),
            "Restored reviewed source did not pass")
    print("PASS: actual verifier Windows/POSIX keys, exemptions, reviewed hashes, injected/modified/missing rejection and restoration")


if __name__ == "__main__":
    main()
