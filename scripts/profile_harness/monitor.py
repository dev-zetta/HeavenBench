"""Read-only Linux observations; desktop sampling is not GPU isolation."""
import hashlib
import ctypes
import mmap
import os
from pathlib import Path
import platform
import re
import subprocess
import time

BLOCKERS = {"ninja", "make", "gmake", "cc1", "cc1plus", "clang", "clang++",
            "gcc", "g++", "rustc", "glslangValidator", "matc", "heaven_benchmark"}
FAULT = re.compile(r"GPU HANG|GPU fault|device.?lost|timed out.*job|reset.*engine|wedged|CAT error", re.I)


def file_identity(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular input file: {path}")
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    if (before.st_ino, before.st_dev, before.st_size, before.st_mtime_ns,
        before.st_ctime_ns) != (after.st_ino, after.st_dev, after.st_size,
                               after.st_mtime_ns, after.st_ctime_ns):
        raise ValueError(f"input changed while hashing: {path}")
    return {"path": str(path.absolute()), "sha256": digest.hexdigest(), "bytes": after.st_size}


def processes(exclude=()):
    blockers, media, unreadable = [], [], 0
    for path in Path("/proc").iterdir():
        if not path.name.isdigit() or int(path.name) in exclude:
            continue
        try:
            comm = (path / "comm").read_text().strip()
            stat = (path / "stat").read_text().rsplit(")", 1)[1].split()
            if stat[0] == "Z":
                continue
            item = {"pid": int(path.name), "comm": comm, "state": stat[0],
                    "start_ticks": int(stat[19])}
            truncated = {name[:15] for name in BLOCKERS}
            if comm in truncated or comm.startswith(("heaven_bench", "clang-tidy", "run-clang-tidy")) or re.fullmatch(r"clang(?:\+\+)?-\d+", comm):
                blockers.append(item)
            if comm in {"mpv", "vlc", "ffmpeg", "obs", "obs-studio"}:
                media.append(item)
        except (OSError, ValueError, IndexError):
            unreadable += 1
    return {"blockers": blockers, "media_processes_pause_state_unknown": media,
            "unreadable_or_exited_processes": unreadable,
            "scope": "Readable proc entries; ordinary desktop clients remain. No exclusive GPU isolation claimed."}


def gpu_observation(pci):
    device = Path("/sys/bus/pci/devices") / pci
    values = {}
    patterns = ("tile*/gt*/freq*/*freq", "hwmon/hwmon*/temp*_input",
                "hwmon/hwmon*/temp*_label", "hwmon/hwmon*/power*_average",
                "hwmon/hwmon*/power*_input", "hwmon/hwmon*/power*_cap",
                "tile*/gt*/throttle*")
    for pattern in patterns:
        for path in sorted(device.glob(pattern)):
            if path.is_file():
                try:
                    value = path.read_text().strip()
                    values[str(path.relative_to(device))] = int(value) if value.isdigit() else value
                except OSError as exc:
                    values[str(path.relative_to(device))] = {"unavailable": str(exc)}
    return {"monotonic_ns": time.monotonic_ns(), "values": values,
            "unavailable": ["No throttle/power value inferred when its sysfs attribute is absent."]}


def host_identity(pci, driver_path):
    device = Path("/sys/bus/pci/devices") / pci
    return {"kernel": platform.release(), "machine": platform.machine(), "pci": pci,
            "vendor": (device / "vendor").read_text().strip(),
            "device": (device / "device").read_text().strip(),
            "kernel_driver": (device / "driver").resolve(strict=True).name,
            "vulkan_driver": {**file_identity(driver_path), "mapping": mapped_file_identity(driver_path)},
            "pid_namespace": os.readlink("/proc/self/ns/pid"),
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}


def mapped_file_identity(path):
    """Read proc's identity for a private data mapping; execute no library code.

    Overlay filesystems can report a different st_dev from /proc/maps. Comparing
    two actual mappings retains an exact device/inode check on such hosts.
    """
    with Path(path).open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_COPY) as mapped:
        anchor = ctypes.c_ubyte.from_buffer(mapped)
        address = ctypes.addressof(anchor)
        del anchor
        for line in Path("/proc/self/maps").read_text().splitlines():
            fields = line.split(None, 5)
            low, high = (int(v, 16) for v in fields[0].split("-"))
            if low <= address < high:
                if len(fields) != 6 or fields[5] != str(Path(path).resolve()):
                    raise ValueError("expected file mapping path differs")
                major, minor = (int(v, 16) for v in fields[3].split(":"))
                return {"device_major": major, "device_minor": minor, "inode": int(fields[4])}
    raise ValueError("expected file mapping not observed")


def kernel_interval(since, until):
    argv = ["journalctl", "-k", "-b", "--utc", "--since", since, "--until", until,
            "--no-pager", "-o", "short-iso-precise"]
    result = subprocess.run(argv, capture_output=True, text=True, timeout=8)
    return {"argv": argv, "returncode": result.returncode, "stdout": result.stdout,
            "stderr": result.stderr, "fault_lines": [line for line in result.stdout.splitlines() if FAULT.search(line)],
            "available": result.returncode == 0 and "No journal files" not in result.stderr}
