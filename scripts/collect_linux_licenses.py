#!/usr/bin/env python3
"""Retain Debian/Ubuntu copyright files for an AppDir's bundled libraries."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main() -> None:
    appdir = Path(sys.argv[1]).resolve()
    destination = appdir / "usr/share/licenses/HeavenBench/system"
    destination.mkdir(parents=True, exist_ok=True)
    manifest = {}

    def notices_for(pattern):
        query = subprocess.run(
            ["dpkg-query", "--search", pattern],
            text=True, capture_output=True, check=False, timeout=30,
        )
        if query.returncode not in (0, 1):
            raise SystemExit(f"Cannot query distribution licenses: {query.stderr}")
        packages = set()
        for line in query.stdout.splitlines():
            if ": " in line:
                packages.update(line.rsplit(": ", 1)[0].split(", "))
        notices = []
        for package in sorted(packages):
            name = package.split(":", 1)[0]
            copyright_file = Path("/usr/share/doc") / name / "copyright"
            if copyright_file.is_file():
                target = destination / name / "copyright"
                target.parent.mkdir(exist_ok=True)
                shutil.copyfile(copyright_file, target)
                notices.append(str(target.relative_to(appdir)))
                # Debian copyright files often refer to shared full license
                # texts. Those paths will not exist inside a relocated bundle.
                text = copyright_file.read_text(encoding="utf-8")
                for license_name in sorted(set(re.findall(
                    r"/usr/share/common-licenses/([A-Za-z0-9][A-Za-z0-9.+-]*)", text
                ))):
                    license_name = license_name.rstrip(".")
                    source = Path("/usr/share/common-licenses") / license_name
                    if not source.is_file():
                        # Some package notices spell GPL versions with .0,
                        # while Debian's shared filenames omit that suffix.
                        canonical = {"GPL-2.0": "GPL-2", "GPL-3.0": "GPL-3",
                                     "LGPL-2.0": "LGPL-2", "LGPL-3.0": "LGPL-3"}
                        source = source.with_name(canonical.get(license_name, license_name))
                    if not source.is_file():
                        raise SystemExit(f"Referenced license text missing: {source}")
                    full_text = destination / "common-licenses" / license_name
                    full_text.parent.mkdir(exist_ok=True)
                    shutil.copyfile(source, full_text)
                    notices.append(str(full_text.relative_to(appdir)))
        return sorted(set(notices))

    for library in sorted((appdir / "usr/lib").glob("*.so*")):
        # linuxdeploy preserves the library basename. Query both merged-/usr
        # spellings; dpkg can record either on upgraded hosts.
        notices = notices_for(f"*/{library.name}")
        if not notices:
            raise SystemExit(f"No distribution copyright found for {library.name}")
        manifest[library.name] = notices

    # These components can be absent from ldd because they are compiled into
    # the executable. Resolve installed development files rather than silently
    # assuming a particular LLVM package version exists.
    for name, pattern in (("GLM", "*/glm/glm.hpp"),
                          ("LLVM libc++", "*/libc++.a"),
                          ("LLVM libc++abi", "*/libc++abi.a")):
        notices = notices_for(pattern)
        if not notices:
            raise SystemExit(f"No distribution copyright found for static dependency {name}")
        manifest[f"static:{name}"] = notices
    (destination / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
