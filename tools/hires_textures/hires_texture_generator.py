#!/usr/bin/env python3
"""Deterministic, semantic-aware HeavenBench 4K texture baker.

Heavy inference and compression dependencies are deliberately loaded only by
this opt-in host tool. The renderer and normal asset packer remain C++-only.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import html
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Iterable


PIPELINE_VERSION = "1.2.1"
LEGACY_PIPELINE_VERSIONS = ("1.2.0",)
HAT_REV = "1638a9a822581657811867bf670717f8371fc3e5"
BASICSR_REV = "5668ba75eb8a77e8d2dd46746a36fee0fbb0fdcd"
COMPRESSONATOR_REV = "80284f3544a9ebb2a47b49691213be77929a0727"
MODEL_SHA256 = "f5b1e3bbbb05147ca2beefcc715279cb647d7976cbda67d62ea7e6e20d5ffcc7"
HAT_ARCHIVE_SHA256 = "e18c17922ca3c12d181cc364ee0f5e25daf7fcf166ae2761ef77beeecde8dad3"
BASICSR_ARCHIVE_SHA256 = "79822f3537d6ea7ff90484ed303893ff1b80d24251ef3f8d227a4f634fd3e9bf"
COMPRESSONATOR_ARCHIVE_SHA256 = "b1ec393a86e001c4e81549ca375ce59ed3f6b737efd52d10b344c48391aaeb98"
COMPRESSONATOR_CLI_SHA256 = "92b9e4bc5388b4c839200535a88402009fb3e8c3b97645999ec54e510f798671"
COMPRESSONATOR_CLI_URL = (
    "https://github.com/GPUOpen-Tools/compressonator/releases/download/"
    "V4.4.19/compressonatorcli-4.4.19-Linux.tar.gz"
)
BASICSR_PATCH_SHA256 = "da33b2cdfa111798addf0bc348dc15b951aef2931be09b52117f4250d0adfd35"
MODEL_URL = ("https://huggingface.co/AEmotionStudio/ai-upscale-models/resolve/"
             "main/Real_HAT_GAN_SRx4.pth?download=true")
SKY_FILES = ("sky/moon_disk.png", "sky/moon_normal.png", "sky/milkyway.png")
CONTACT_PATTERNS = (
    "island", "airship", "ruins", "leaf", "foliage", "grass",
    "particle", "smoke", "fire", "sky/",
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(path: str) -> bool:
    p = PurePosixPath(path)
    return bool(path) and not p.is_absolute() and "\\" not in path and all(
        part not in ("", ".", "..") for part in p.parts
    )


def run(command: list[str], *, cwd: Path | None = None) -> None:
    shown = " ".join(command)
    print(f"[hires] {shown}", flush=True)
    subprocess.run(command, cwd=cwd, check=True)


@dataclasses.dataclass(frozen=True)
class DdsInfo:
    width: int
    height: int
    mip_count: int
    format: str
    payload_offset: int
    mip_sizes: tuple[int, ...]

    @property
    def gpu_bytes(self) -> int:
        return sum(self.mip_sizes)


def dds_info(data: bytes) -> DdsInfo:
    if len(data) < 128 or data[:4] != b"DDS " or struct.unpack_from("<I", data, 4)[0] != 124:
        raise ValueError("invalid DDS header")
    height = struct.unpack_from("<I", data, 12)[0]
    width = struct.unpack_from("<I", data, 16)[0]
    mip_count = struct.unpack_from("<I", data, 28)[0]
    mip_count = max(1, mip_count)
    pf_flags, fourcc, bits = struct.unpack_from("<I4sI", data, 80)
    payload = 128
    if fourcc == b"DX10":
        if len(data) < 148:
            raise ValueError("truncated DDS DX10 header")
        dxgi, _, _, array_size, _ = struct.unpack_from("<IIIII", data, 128)
        if array_size != 1 or dxgi not in (98, 99):
            raise ValueError(f"unsupported DDS DXGI format {dxgi}")
        fmt = "BC7_SRGB" if dxgi == 99 else "BC7_UNORM"
        payload = 148
    elif fourcc in (b"DXT1",):
        fmt = "BC1"
    elif fourcc in (b"DXT5",):
        fmt = "BC3"
    elif fourcc in (b"ATI1", b"BC4U"):
        fmt = "BC4"
    elif fourcc in (b"ATI2", b"BC5U"):
        fmt = "BC5"
    elif not (pf_flags & 0x4) and bits in (24, 32):
        fmt = "RGB8" if bits == 24 else "RGBA8"
    else:
        raise ValueError(f"unsupported DDS FourCC {fourcc!r}")
    sizes: list[int] = []
    for level in range(mip_count):
        w, h = max(1, width >> level), max(1, height >> level)
        if fmt == "BC1":
            size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
        elif fmt in ("BC3", "BC5", "BC7_UNORM", "BC7_SRGB"):
            size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
        elif fmt == "BC4":
            size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
        else:
            size = w * h * (bits // 8)
        sizes.append(size)
    if payload + sum(sizes) > len(data):
        raise ValueError("truncated DDS mip payload")
    return DdsInfo(width, height, mip_count, fmt, payload, tuple(sizes))


def tier_bytes(info: DdsInfo, max_edge: int) -> int:
    level = 0
    edge = max(info.width, info.height)
    while edge > max_edge and level + 1 < info.mip_count:
        level += 1
        edge = max(1, edge >> 1)
    return sum(info.mip_sizes[level:])


def target_scale(width: int, height: int) -> int:
    """Return a power-of-two scale up to 4x without exceeding a 4096 edge."""
    edge = max(width, height)
    if edge <= 0:
        raise ValueError("texture dimensions must be positive")
    if edge <= 1024:
        return 4
    if edge <= 2048:
        return 2
    return 1


@dataclasses.dataclass
class SourceTexture:
    logical_path: str
    data: bytes
    width: int
    height: int
    source_format: str
    source_gpu_bytes: int
    role: str
    wrap: str
    has_alpha_hint: bool


@dataclasses.dataclass(frozen=True)
class TorchRuntime:
    backend: str
    torch_build: str
    accelerator_version: str
    device_index: int | None
    device_name: str
    architecture: str

    @property
    def torch_device(self) -> str:
        return "cpu" if self.device_index is None else f"cuda:{self.device_index}"

    def manifest(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


class SourceReader:
    def __init__(self, data_path: Path, repo: Path):
        self.data_path = data_path
        self.repo = repo
        self.data_prefix = "heaven"
        self.inventory: dict[str, dict[str, Any]] = {}
        self.derived_textures: set[str] = set()
        self._zip: zipfile.ZipFile | None = None
        if data_path.is_file():
            self._zip = zipfile.ZipFile(data_path, "r")
            try:
                manifest = json.loads(self._zip.read("heavenbench-assets.json"))
                self.data_prefix = manifest["data_root_prefix"]
                self.inventory = manifest.get("texture_inventory", {}).get(
                    "textures", {}
                )
                self.derived_textures = {
                    f"{self.data_prefix}/{item['output_path']}"
                    for item in manifest.get("derived_assets", [])
                    if isinstance(item, dict) and item.get("output_path")
                }
            except (KeyError, ValueError) as exc:
                raise RuntimeError("--data ZIP lacks a valid asset manifest") from exc
        elif data_path.is_dir():
            self.data_prefix = data_path.resolve().name
        else:
            raise RuntimeError(f"--data is neither a directory nor ZIP: {data_path}")

    def close(self) -> None:
        if self._zip:
            self._zip.close()

    def _read_loose(self, logical: str) -> bytes:
        prefix = self.data_prefix + "/"
        if logical.startswith(prefix):
            relative = logical[len(prefix):]
            if relative in SKY_FILES:
                return (self.repo / "benchmark/assets" / relative).read_bytes()
            return (self.data_path / relative).read_bytes()
        if logical.startswith("core/"):
            return (self.data_path.parent / logical).read_bytes()
        raise FileNotFoundError(logical)

    def read(self, logical: str) -> bytes:
        if self._zip:
            return self._zip.read(logical)
        return self._read_loose(logical)

    def names(self) -> list[str]:
        if self._zip:
            candidates = [
                n for n in self._zip.namelist()
                if not n.endswith("/") and "/texture_sets/" not in n and
                n not in self.derived_textures
            ]
        else:
            candidates = []
            for root in (self.data_path, self.data_path.parent / "core"):
                if not root.is_dir():
                    continue
                base = root.parent
                candidates.extend(
                    p.relative_to(base).as_posix() for p in root.rglob("*.dds")
                )
            candidates.extend(f"{self.data_prefix}/{name}" for name in SKY_FILES)
        wanted = [n for n in candidates if n.lower().endswith(".dds")]
        wanted.extend(
            f"{self.data_prefix}/{name}" for name in SKY_FILES
            if f"{self.data_prefix}/{name}" in candidates
        )
        return sorted(set(wanted))


def classify(path: str, source_format: str) -> tuple[str, str]:
    lower = path.lower()
    stem = PurePosixPath(lower).stem
    if lower.endswith("/sky/moon_normal.png"):
        return "normal", "clamp"
    if "/sky/" in lower:
        return "color", "repeat" if "milkyway" in lower else "clamp"
    if source_format == "BC4" or stem.endswith(("_h", "_t")):
        return ("tessellation" if stem.endswith("_t") else "height"), "repeat"
    if source_format == "BC5" or stem.endswith(("_n", "_dn")) or "normal" in stem:
        return "normal", "repeat"
    if stem.endswith(("_d", "_dd", "_e")):
        return ("emissive" if stem.endswith("_e") else "diffuse"), "repeat"
    if stem.endswith(("_s", "_ds")):
        return "specular", "repeat"
    return "data", "repeat"


def png_dimensions(data: bytes) -> tuple[int, int]:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("invalid PNG")
    return struct.unpack_from(">II", data, 16)


def build_inventory(reader: SourceReader) -> list[SourceTexture]:
    result: list[SourceTexture] = []
    for logical in reader.names():
        if not safe_relative(logical):
            raise RuntimeError(f"unsafe source path: {logical}")
        data = reader.read(logical)
        if logical.lower().endswith(".dds"):
            info = dds_info(data)
            width, height = info.width, info.height
            source_format = info.format
            gpu_bytes = info.gpu_bytes
            alpha_hint = info.format in ("BC1", "BC3", "RGBA8")
        else:
            width, height = png_dimensions(data)
            source_format = "PNG_RGBA8"
            gpu_bytes = sum(
                max(1, width >> m) * max(1, height >> m) * 4
                for m in range(int(math.log2(max(width, height))) + 1)
            )
            alpha_hint = True
        role, wrap = classify(logical, source_format)
        authored = reader.inventory.get(logical)
        if authored is not None:
            expected = {
                "width": width,
                "height": height,
                "source_format": source_format,
                "source_sha256": sha256_bytes(data),
            }
            for field, value in expected.items():
                if authored.get(field) != value:
                    raise RuntimeError(
                        f"stale texture inventory for {logical}: {field}"
                    )
            role = str(authored["role"])
            wrap = str(authored["wrap"])
            alpha_hint = bool(authored["has_alpha"])
        if role not in {
            "diffuse", "emissive", "color", "normal", "specular",
            "height", "tessellation", "data",
        }:
            raise RuntimeError(f"unsupported texture role for {logical}: {role}")
        if wrap != "repeat" and not wrap.startswith("clamp"):
            raise RuntimeError(f"unsupported address mode for {logical}: {wrap}")
        result.append(SourceTexture(
            logical, data, width, height, source_format, gpu_bytes,
            role, wrap, alpha_hint,
        ))
    return result


def import_image_stack() -> tuple[Any, Any, Any]:
    import cv2
    import numpy as np
    from PIL import Image
    return cv2, np, Image


def select_torch_runtime(requested: str) -> TorchRuntime:
    import torch
    installed = os.environ.get("HEAVEN_HIRES_TORCH", requested)
    if installed != requested:
        raise RuntimeError(
            f"{requested} requested with the {installed} tool environment"
        )
    build = str(torch.__version__)
    if requested == "cpu":
        return TorchRuntime("cpu", build, "", None, "CPU", "host")
    if requested == "rocm":
        if not torch.version.hip:
            raise RuntimeError("ROCm requested, but this is not a HIP PyTorch build")
        accelerator = str(torch.version.hip)
    else:
        if not torch.version.cuda or torch.version.hip:
            raise RuntimeError("CUDA requested, but this is not a CUDA PyTorch build")
        accelerator = str(torch.version.cuda)
    if not torch.cuda.is_available() or torch.cuda.device_count() == 0:
        raise RuntimeError(f"{requested} requested, but no compute GPU is available")
    forced = os.environ.get("HEAVEN_HIRES_GPU_INDEX")
    if forced is not None:
        try:
            index = int(forced)
        except ValueError as exc:
            raise RuntimeError("HEAVEN_HIRES_GPU_INDEX must be an integer") from exc
        if index < 0 or index >= torch.cuda.device_count():
            raise RuntimeError("HEAVEN_HIRES_GPU_INDEX is outside the device list")
    else:
        index = max(
            range(torch.cuda.device_count()),
            key=lambda candidate: torch.cuda.get_device_properties(candidate).total_memory,
        )
    torch.cuda.set_device(index)
    properties = torch.cuda.get_device_properties(index)
    architecture = str(getattr(properties, "gcnArchName", ""))
    if not architecture:
        architecture = f"compute-{properties.major}.{properties.minor}"
    runtime = TorchRuntime(
        requested, build, accelerator, index,
        torch.cuda.get_device_name(index), architecture,
    )
    print(
        f"[hires] accelerator: {runtime.backend} {runtime.device_name} "
        f"({runtime.architecture}), PyTorch {runtime.torch_build}, "
        f"runtime {runtime.accelerator_version}"
    )
    return runtime


def decode_source(source: SourceTexture, destination: Path, compressor: Path) -> Any:
    cv2, np, _ = import_image_stack()
    if source.logical_path.lower().endswith(".png"):
        image = cv2.imdecode(np.frombuffer(source.data, np.uint8), cv2.IMREAD_UNCHANGED)
    else:
        input_dds = destination.with_suffix(".source.dds")
        output_png = destination.with_suffix(".source.png")
        input_dds.write_bytes(source.data)
        run([str(compressor), str(input_dds), str(output_png)])
        image = cv2.imread(str(output_png), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise RuntimeError(f"failed to decode {source.logical_path}")
    if image.ndim == 2:
        image = np.repeat(image[:, :, None], 4, axis=2)
        image[:, :, 3] = 255
    elif image.shape[2] == 3:
        image = cv2.cvtColor(image, cv2.COLOR_BGR2RGBA)
    else:
        image = cv2.cvtColor(image, cv2.COLOR_BGRA2RGBA)
    if image.shape[1] != source.width or image.shape[0] != source.height:
        raise RuntimeError(f"decoder dimensions changed for {source.logical_path}")
    return image.astype(np.float32) / 255.0


def srgb_to_linear(v: Any) -> Any:
    _, np, _ = import_image_stack()
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(v: Any) -> Any:
    _, np, _ = import_image_stack()
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.maximum(v, 0.0) ** (1.0 / 2.4) - 0.055)


def resize_array(image: Any, width: int, height: int,
                 interpolation: int, wrap: str) -> Any:
    cv2, np, _ = import_image_stack()
    if wrap == "repeat":
        repeats = (3, 3) if image.ndim == 2 else (3, 3, 1)
        tiled = np.tile(image, repeats)
        resized = cv2.resize(
            tiled, (width * 3, height * 3), interpolation=interpolation
        )
        return resized[height:height * 2, width:width * 2]
    return cv2.resize(image, (width, height), interpolation=interpolation)


def resize_channels(image: Any, width: int, height: int, *,
                    linear_rgb: bool, wrap: str = "clamp") -> Any:
    cv2, np, _ = import_image_stack()
    value = srgb_to_linear(image) if linear_rgb else image
    resized = resize_array(value, width, height, cv2.INTER_LANCZOS4, wrap)
    if resized.ndim == 2:
        resized = resized[:, :, None]
    if linear_rgb:
        resized = linear_to_srgb(resized)
    return np.clip(resized, 0.0, 1.0)


def dilate_foreground(rgb: Any, alpha: Any, wrap: str) -> Any:
    cv2, np, _ = import_image_stack()
    out = rgb.copy()
    valid = alpha > (0.5 / 255.0)
    kernel = np.ones((3, 3), np.uint8)

    def dilate(values: Any) -> Any:
        if wrap != "repeat":
            return cv2.dilate(
                values, kernel, borderType=cv2.BORDER_REPLICATE
            )
        # OpenCV morphology does not support BORDER_WRAP. Rolling all eight
        # neighbours is the exact 3x3 periodic maximum required by a repeating
        # texture and avoids a seam at the authored UV boundary.
        return np.maximum.reduce([
            np.roll(values, shift=(dy, dx), axis=(0, 1))
            for dy in (-1, 0, 1) for dx in (-1, 0, 1)
        ])

    for _ in range(16):
        if bool(valid.all()):
            break
        expanded = dilate(valid.astype(np.uint8)).astype(bool)
        fill = expanded & ~valid
        if not bool(fill.any()):
            break
        for channel in range(3):
            neighbor = dilate(out[:, :, channel])
            out[:, :, channel][fill] = neighbor[fill]
        valid = expanded
    return out


def preserve_alpha_coverage(alpha: Any, reference: Any) -> Any:
    _, np, _ = import_image_stack()
    target = float((reference >= 0.5).mean())
    if target <= 0.0 or target >= 1.0:
        return np.clip(alpha, 0.0, 1.0)
    lo, hi = -1.0, 1.0
    for _ in range(18):
        shift = (lo + hi) * 0.5
        coverage = float((np.clip(alpha + shift, 0.0, 1.0) >= 0.5).mean())
        if coverage < target:
            lo = shift
        else:
            hi = shift
    adjusted = np.clip(alpha + (lo + hi) * 0.5, 0.0, 1.0)

    # A global threshold shift can still miss the nearest representable pixel
    # count when many texels share exactly the same filtered value. Resolve
    # only that boundary tie deterministically; all other alpha values retain
    # their filtered gradients.
    desired = int(round(target * adjusted.size))
    flat = adjusted.reshape(-1).copy()
    covered = flat >= 0.5
    difference = desired - int(covered.sum())
    if difference > 0:
        candidates = np.flatnonzero(~covered)
        selected = candidates[np.argsort(flat[candidates], kind="stable")[-difference:]]
        flat[selected] = np.float32(0.5)
    elif difference < 0:
        candidates = np.flatnonzero(covered)
        selected = candidates[np.argsort(flat[candidates], kind="stable")[:-difference]]
        flat[selected] = np.nextafter(np.float32(0.5), np.float32(0.0))
    return flat.reshape(adjusted.shape)


def decode_normal(image: Any) -> Any:
    _, np, _ = import_image_stack()
    x = image[:, :, 0] * 2.0 - 1.0
    y = image[:, :, 1] * 2.0 - 1.0
    z = np.sqrt(np.maximum(0.0, 1.0 - x * x - y * y))
    n = np.stack((x, y, z), axis=2)
    return n / np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-8)


def encode_normal(normal: Any, alpha: Any | None = None) -> Any:
    _, np, _ = import_image_stack()
    n = normal / np.maximum(np.linalg.norm(normal, axis=2, keepdims=True), 1e-8)
    a = np.ones(n.shape[:2] + (1,), dtype=np.float32) if alpha is None else alpha[:, :, None]
    return np.concatenate((n * 0.5 + 0.5, a), axis=2)


def resize_normal(image: Any, width: int, height: int,
                  wrap: str = "clamp") -> Any:
    cv2, np, _ = import_image_stack()
    n = decode_normal(image)
    resized = resize_array(n, width, height, cv2.INTER_LANCZOS4, wrap)
    if resized.ndim == 2:
        resized = resized[:, :, None]
    resized /= np.maximum(np.linalg.norm(resized, axis=2, keepdims=True), 1e-8)
    return encode_normal(resized)


class HatUpscaler:
    def __init__(self, runtime: TorchRuntime, hat_root: Path, model_path: Path):
        import torch
        sys.path.insert(0, str(hat_root))
        from hat.archs.hat_arch import HAT
        if sha256_file(model_path) != MODEL_SHA256:
            raise RuntimeError("HAT model SHA-256 mismatch")
        self.device = torch.device(runtime.torch_device)
        torch.manual_seed(0)
        if torch.cuda.is_available():
            torch.cuda.manual_seed_all(0)
        torch.use_deterministic_algorithms(True)
        if hasattr(torch.backends, "cuda"):
            torch.backends.cuda.matmul.allow_tf32 = False
        if hasattr(torch.backends, "cudnn"):
            torch.backends.cudnn.benchmark = False
            torch.backends.cudnn.allow_tf32 = False
        torch.set_float32_matmul_precision("highest")
        self.torch = torch
        self.model = HAT(
            upscale=4, in_chans=3, img_size=64, window_size=16,
            compress_ratio=3, squeeze_factor=30, conv_scale=0.01,
            overlap_ratio=0.5, img_range=1.0, depths=[6] * 6,
            embed_dim=180, num_heads=[6] * 6, mlp_ratio=2,
            upsampler="pixelshuffle", resi_connection="1conv",
        )
        checkpoint = torch.load(model_path, map_location="cpu", weights_only=True)
        self.model.load_state_dict(checkpoint["params_ema"], strict=True)
        self.model.eval().to(self.device, dtype=torch.float32)

    def upscale(self, rgb: Any, scale: int, wrap: str) -> Any:
        cv2, np, _ = import_image_stack()
        torch = self.torch
        height, width = rgb.shape[:2]
        output = np.empty((height * scale, width * scale, 3), np.float32)
        core, context = 256, 32
        mode_wrap = wrap == "repeat"
        with torch.inference_mode():
            for y in range(0, height, core):
                valid_h = min(core, height - y)
                ys = np.arange(y - context, y + core + context)
                ys = ys % height if mode_wrap else np.clip(ys, 0, height - 1)
                for x in range(0, width, core):
                    valid_w = min(core, width - x)
                    xs = np.arange(x - context, x + core + context)
                    xs = xs % width if mode_wrap else np.clip(xs, 0, width - 1)
                    tile = rgb[ys[:, None], xs[None, :], :]
                    tensor = torch.from_numpy(tile.transpose(2, 0, 1).copy())
                    tensor = tensor.unsqueeze(0).to(self.device, dtype=torch.float32)
                    result = self.model(tensor).clamp_(0.0, 1.0)[0]
                    result = result.permute(1, 2, 0).cpu().numpy()
                    crop = result[
                        context * 4:(context + valid_h) * 4,
                        context * 4:(context + valid_w) * 4,
                    ]
                    if scale != 4:
                        crop = cv2.resize(
                            crop, (valid_w * scale, valid_h * scale),
                            interpolation=cv2.INTER_LANCZOS4,
                        )
                    output[y * scale:(y + valid_h) * scale,
                           x * scale:(x + valid_w) * scale] = crop
        return np.clip(output, 0.0, 1.0)


def generate_top(source: SourceTexture, image: Any, scale: int,
                 hat: HatUpscaler | None) -> Any:
    _, np, _ = import_image_stack()
    width, height = source.width * scale, source.height * scale
    if source.role in ("diffuse", "emissive", "color"):
        if hat is None:
            raise RuntimeError("HAT model was not initialized")
        rgb = dilate_foreground(image[:, :, :3], image[:, :, 3], source.wrap)
        top_rgb = hat.upscale(rgb, scale, source.wrap)
        alpha = resize_channels(
            image[:, :, 3], width, height, linear_rgb=False, wrap=source.wrap
        )[:, :, 0]
        alpha = preserve_alpha_coverage(alpha, image[:, :, 3])
        return np.concatenate((top_rgb, alpha[:, :, None]), axis=2)
    if source.role == "normal":
        return resize_normal(image, width, height, source.wrap)
    top = resize_channels(
        image, width, height, linear_rgb=False, wrap=source.wrap
    )
    if source.role in ("height", "tessellation"):
        low, high = float(image[:, :, 0].min()), float(image[:, :, 0].max())
        top = np.clip(top, low, high)
    return top


def build_mips(source: SourceTexture, top: Any,
               original: Any | None = None) -> list[Any]:
    cv2, np, _ = import_image_stack()
    mips = [np.clip(top, 0.0, 1.0).astype(np.float32)]
    alpha_reference = None
    if source.role in ("diffuse", "emissive", "color"):
        alpha_reference = (
            original[:, :, 3]
            if original is not None and original.shape[2] >= 4
            else mips[0][:, :, 3]
        )
    while mips[-1].shape[0] > 1 or mips[-1].shape[1] > 1:
        previous = mips[-1]
        width = max(1, previous.shape[1] // 2)
        height = max(1, previous.shape[0] // 2)
        if source.role in ("diffuse", "emissive", "color"):
            rgb = resize_channels(
                previous[:, :, :3], width, height,
                linear_rgb=True, wrap=source.wrap
            )
            alpha = resize_array(
                previous[:, :, 3], width, height, cv2.INTER_AREA, source.wrap
            )
            # Preserve the authored cutout coverage at every level. Using the
            # previous mip as the reference compounds rounding drift over a
            # complete chain, especially for sparse foliage and particles.
            alpha = preserve_alpha_coverage(alpha, alpha_reference)
            next_mip = np.concatenate((rgb, alpha[:, :, None]), axis=2)
        elif source.role == "normal":
            next_mip = resize_normal(previous, width, height, source.wrap)
        else:
            next_mip = resize_array(
                previous, width, height, cv2.INTER_AREA, source.wrap
            )
            if next_mip.ndim == 2:
                next_mip = next_mip[:, :, None]
            if source.role in ("height", "tessellation"):
                next_mip = np.clip(
                    next_mip, float(previous.min()), float(previous.max())
                )
        mips.append(np.clip(next_mip, 0.0, 1.0).astype(np.float32))
    return mips


def dds_header_rgba(width: int, height: int) -> bytes:
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    header = struct.pack(
        "<7I11I", 124, flags, height, width, width * 4, 0, 1, *([0] * 11)
    )
    pixel_format = struct.pack(
        "<II4sIIIII", 32, 0x1 | 0x40, b"\0\0\0\0", 32,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000,
    )
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    return b"DDS " + header + pixel_format + caps


def write_uncompressed_dds(path: Path, image: Any) -> None:
    _, np, _ = import_image_stack()
    rgba = np.clip(np.rint(image * 255.0), 0, 255).astype(np.uint8)
    if rgba.shape[2] == 1:
        rgba = np.repeat(rgba, 4, axis=2)
        rgba[:, :, 3] = 255
    elif rgba.shape[2] == 3:
        rgba = np.concatenate(
            (rgba, np.full(rgba.shape[:2] + (1,), 255, np.uint8)), axis=2
        )
    bgra = rgba[:, :, [2, 1, 0, 3]]
    path.write_bytes(dds_header_rgba(rgba.shape[1], rgba.shape[0]) + bgra.tobytes())


def compressed_header(width: int, height: int, mip_count: int,
                      fmt: str, srgb: bool) -> bytes:
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    caps = 0x1000
    if mip_count > 1:
        flags |= 0x20000
        caps |= 0x8 | 0x400000
    block = 8 if fmt == "BC4" else 16
    linear_size = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * block
    header = struct.pack(
        "<7I11I", 124, flags, height, width, linear_size, 0, mip_count,
        *([0] * 11),
    )
    fourcc = b"DX10" if fmt == "BC7" else (b"ATI2" if fmt == "BC5" else b"ATI1")
    pixel_format = struct.pack(
        "<II4sIIIII", 32, 0x4, fourcc, 0, 0, 0, 0, 0
    )
    result = b"DDS " + header + pixel_format + struct.pack("<5I", caps, 0, 0, 0, 0)
    if fmt == "BC7":
        result += struct.pack("<IIIII", 99 if srgb else 98, 3, 0, 1, 0)
    return result


def encode_mips(mips: list[Any], role: str, output: Path,
                compressor: Path, temp: Path) -> tuple[str, DdsInfo]:
    fmt = "BC5" if role == "normal" else (
        "BC4" if role in ("height", "tessellation") else "BC7"
    )
    srgb = role in ("diffuse", "emissive", "color")
    payloads: list[bytes] = []
    for level, mip in enumerate(mips):
        raw = temp / f"mip-{level}.rgba.dds"
        compressed = temp / f"mip-{level}.{fmt.lower()}.dds"
        write_uncompressed_dds(raw, mip)
        command = [str(compressor), "-fd", fmt, "-Quality", "1.0"]
        command += [str(raw), str(compressed)]
        run(command)
        encoded = compressed.read_bytes()
        info = dds_info(encoded)
        expected = max(1, (mip.shape[1] + 3) // 4) * max(1, (mip.shape[0] + 3) // 4)
        expected *= 8 if fmt == "BC4" else 16
        payload = encoded[info.payload_offset:info.payload_offset + info.mip_sizes[0]]
        if len(payload) != expected:
            raise RuntimeError(f"Compressonator emitted an invalid {fmt} block payload")
        payloads.append(payload)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(
        compressed_header(mips[0].shape[1], mips[0].shape[0], len(mips), fmt, srgb)
        + b"".join(payloads)
    )
    info = dds_info(output.read_bytes())
    return ("BC7_SRGB" if fmt == "BC7" and srgb else fmt), info


def validation_metrics(
    source: SourceTexture, original: Any, top: Any, mips: list[Any]
) -> tuple[dict[str, float], list[str], list[str]]:
    cv2, np, _ = import_image_stack()
    from skimage.metrics import peak_signal_noise_ratio, structural_similarity
    metrics: dict[str, float] = {}
    failures: list[str] = []
    warnings: list[str] = []
    down = resize_array(top, source.width, source.height,
                        cv2.INTER_AREA, source.wrap)
    if source.role in ("diffuse", "emissive", "color", "specular", "data"):
        left = srgb_to_linear(original[:, :, :3]) if source.role in ("diffuse", "emissive", "color") else original[:, :, :3]
        right = srgb_to_linear(down[:, :, :3]) if source.role in ("diffuse", "emissive", "color") else down[:, :, :3]
        metrics["psnr_db"] = float(peak_signal_noise_ratio(left, right, data_range=1.0))
        metrics["ssim"] = float(structural_similarity(left, right, channel_axis=2, data_range=1.0))
        if source.role in ("diffuse", "emissive", "color"):
            # HAT is a learned reconstruction model. These round-trip metrics
            # are valuable review signals, but a detailed reconstruction can
            # legitimately score below a strict pixel-wise threshold after it
            # is reduced to the authored resolution. They do not indicate a
            # missing, corrupt, or stale release asset.
            if metrics["psnr_db"] < 30.0: warnings.append("PSNR below 30 dB")
            if metrics["ssim"] < 0.95: warnings.append("SSIM below 0.95")
    if source.role == "normal":
        a, b = decode_normal(original), decode_normal(down)
        dots = np.clip(np.sum(a * b, axis=2), -1.0, 1.0)
        metrics["mean_normal_error_degrees"] = float(np.degrees(np.arccos(dots)).mean())
        if metrics["mean_normal_error_degrees"] > 2.0:
            warnings.append("mean normal angular error above 2 degrees")
    if source.role in ("height", "tessellation"):
        metrics["scalar_mae"] = float(np.abs(original[:, :, 0] - down[:, :, 0]).mean())
        metrics["scalar_overshoot"] = float(max(
            0.0, float(top[:, :, 0].max() - original[:, :, 0].max()),
            float(original[:, :, 0].min() - top[:, :, 0].min()),
        ))
        if metrics["scalar_mae"] > 1.0 / 255.0: warnings.append("scalar MAE above 1/255")
        if metrics["scalar_overshoot"] > 1e-7: failures.append("scalar range overshoot")
    if (source.role in ("diffuse", "emissive", "color") and
            original.shape[2] >= 4 and float(original[:, :, 3].min()) < 0.999):
        base = float((original[:, :, 3] >= 0.5).mean())
        measurable = [m for m in mips if m.shape[0] * m.shape[1] >= 200]
        drift = max(
            (abs(float((m[:, :, 3] >= 0.5).mean()) - base)
             for m in measurable),
            default=0.0,
        )
        metrics["max_alpha_coverage_drift"] = drift
        if drift > 0.005:
            warnings.append("alpha coverage drift above 0.5 percentage points")
    if source.wrap == "repeat":
        def discontinuity(image: Any) -> float:
            return float(max(
                np.abs(image[:, 0, :3] - image[:, -1, :3]).mean(),
                np.abs(image[0, :, :3] - image[-1, :, :3]).mean(),
            ))
        source_edge = discontinuity(original)
        output_edge = discontinuity(top)
        metrics["source_repeat_edge_discontinuity"] = source_edge
        metrics["output_repeat_edge_discontinuity"] = output_edge
        if output_edge > source_edge + 2.0 / 255.0:
            failures.append("repeat-edge discontinuity regressed by more than 2/255")
    return metrics, failures, warnings


QUALITY_WARNING_MESSAGES = frozenset({
    "PSNR below 30 dB",
    "SSIM below 0.95",
    "mean normal angular error above 2 degrees",
    "scalar MAE above 1/255",
    "alpha coverage drift above 0.5 percentage points",
})


def reclassify_cached_validation(entry: dict[str, Any]) -> tuple[list[str], list[str]]:
    """Split legacy validation findings without decoding or recompressing DDS.

    Quality thresholds remain visible in reports. Only invariant violations or
    operational errors keep a resumable bake incomplete.
    """
    failures: list[str] = []
    warnings = list(entry.get("validation_warnings", []))
    for finding in entry.get("validation_failures", []):
        if finding == "alpha coverage drift above 0.5 percentage points" and \
                entry.get("role") not in ("diffuse", "emissive", "color"):
            continue
        if finding in QUALITY_WARNING_MESSAGES:
            if finding not in warnings:
                warnings.append(finding)
        else:
            failures.append(finding)
    return failures, warnings


def cache_key(source: SourceTexture, source_hash: str,
              runtime: TorchRuntime | None = None, *,
              pipeline_version: str = PIPELINE_VERSION) -> str:
    payload = {
        "pipeline": pipeline_version,
        "source_sha256": source_hash,
        "logical_path": source.logical_path,
        "role": source.role,
        "wrap": source.wrap,
        "hat_revision": HAT_REV,
        "basicsr_revision": BASICSR_REV,
        "basicsr_patch_sha256": BASICSR_PATCH_SHA256,
        "compressonator_revision": COMPRESSONATOR_REV,
        "compressonator_cli_sha256": COMPRESSONATOR_CLI_SHA256,
        "model_sha256": MODEL_SHA256,
        "tile": 256,
        "context": 32,
        "fp32": True,
        "max_edge": 4096,
        "runtime": runtime.manifest() if runtime else {"backend": "policy-test"},
    }
    return sha256_bytes(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode())


def make_thumbnail(image: Any, label: str) -> Any:
    _, np, Image = import_image_stack()
    rgb = np.clip(np.rint(image[:, :, :3] * 255.0), 0, 255).astype(np.uint8)
    out = Image.fromarray(rgb, "RGB")
    out.thumbnail((256, 256), Image.Resampling.LANCZOS)
    return out.copy()


def write_contact_sheet(path: Path, images: list[tuple[str, Any]]) -> None:
    _, _, Image = import_image_stack()
    if not images:
        return
    cell_w, cell_h = 272, 292
    columns = 4
    rows = math.ceil(len(images) / columns)
    sheet = Image.new("RGB", (columns * cell_w, rows * cell_h), (24, 24, 24))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for index, (label, thumb) in enumerate(images):
        x, y = (index % columns) * cell_w, (index // columns) * cell_h
        sheet.paste(thumb, (x + 8, y + 8))
        draw.text((x + 8, y + 268), label[:42], fill=(230, 230, 230))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path, optimize=True)


def copy_provenance(output: Path, runtime: TorchRuntime) -> list[Path]:
    provenance_dir = output / "provenance"
    license_dir = provenance_dir / "licenses"
    license_dir.mkdir(parents=True, exist_ok=True)
    provenance = provenance_dir / "PROVENANCE.md"
    provenance.write_text(
        "# HeavenBench Hi-Res Texture Set\n\n"
        f"- Generator: {PIPELINE_VERSION}\n"
        f"- HAT: `{HAT_REV}`\n"
        f"  - source archive SHA-256: `{HAT_ARCHIVE_SHA256}`\n"
        f"- BasicSR: `{BASICSR_REV}`\n"
        f"  - source archive SHA-256: `{BASICSR_ARCHIVE_SHA256}`\n"
        f"  - torchvision compatibility patch SHA-256: `{BASICSR_PATCH_SHA256}`\n"
        f"- Real_HAT_GAN_SRx4 SHA-256: `{MODEL_SHA256}`\n"
        f"  - fetched from: `{MODEL_URL}`\n"
        f"- Compressonator: `{COMPRESSONATOR_REV}`\n"
        f"  - source archive SHA-256: `{COMPRESSONATOR_ARCHIVE_SHA256}`\n"
        f"  - official Linux CLI: `{COMPRESSONATOR_CLI_URL}`\n"
        f"  - Linux CLI SHA-256: `{COMPRESSONATOR_CLI_SHA256}`\n"
        "- Inference: deterministic FP32, 256 px core tiles, 32 px context\n"
        "- Maximum output edge: 4096 px\n"
        f"- Runtime: `{runtime.backend}` / PyTorch `{runtime.torch_build}` / "
        f"accelerator `{runtime.accelerator_version}`\n"
        f"- Device: `{runtime.device_name}` (`{runtime.architecture}`)\n",
        encoding="utf-8",
    )
    candidates = (
        (os.environ.get("HEAVEN_HIRES_HAT"), ("LICENSE", "LICENSE.txt"), "HAT-LICENSE.txt"),
        (os.environ.get("HEAVEN_HIRES_BASICSR"),
         ("LICENSE", "LICENSE.txt", "LICENSE/LICENSE"),
         "BasicSR-LICENSE.txt"),
        (os.environ.get("HEAVEN_HIRES_COMPRESSONATOR_SRC"),
         ("LICENSE", "LICENSE.txt", "license/license.txt"),
         "Compressonator-LICENSE.txt"),
    )
    artifacts = [provenance]
    for root, names, target in candidates:
        if not root:
            raise RuntimeError(f"source root is unavailable for {target}")
        copied = False
        for name in names:
            source = Path(root) / name
            if source.is_file():
                destination = license_dir / target
                shutil.copyfile(source, destination)
                artifacts.append(destination)
                copied = True
                break
        if not copied:
            raise RuntimeError(f"license file is unavailable for {target}")
    return artifacts


def artifact_record(output: Path, path: Path, data_prefix: str) -> dict[str, Any]:
    relative = path.relative_to(output).as_posix()
    return {
        "file": relative,
        "archive_path": f"{data_prefix}/texture_sets/metadata/{relative}",
        "sha256": sha256_file(path),
    }


def write_reports(output: Path, entries: list[dict[str, Any]], complete: bool,
                  elapsed: float, runtime: TorchRuntime) -> tuple[Path, Path]:
    report = {
        "format": "heavenbench-hires-texture-report",
        "schema_version": 1,
        "complete": complete,
        "pipeline_version": PIPELINE_VERSION,
        "runtime": runtime.manifest(),
        "elapsed_seconds": elapsed,
        "counts": {
            "total": len(entries),
            "generated": sum(e["status"] == "generated" for e in entries),
            "aliases": sum(e["status"] == "alias" for e in entries),
            "failed": sum(e["status"] == "failed" for e in entries),
            "with_warnings": sum(bool(e.get("validation_warnings")) for e in entries),
            "warnings": sum(len(e.get("validation_warnings", [])) for e in entries),
        },
        "entries": entries,
    }
    json_path = output / "reports/texture-quality-report.json"
    html_path = output / "reports/texture-quality-report.html"
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    rows = []
    for e in entries:
        metrics = "<br>".join(
            f"{html.escape(k)}: {float(v):.5g}" for k, v in e.get("metrics", {}).items()
        )
        rows.append(
            "<tr>"
            f"<td>{html.escape(e['original_path'])}</td>"
            f"<td>{html.escape(e['role'])}</td>"
            f"<td>{html.escape(e['status'])}</td>"
            f"<td>{e.get('source', {}).get('width', '')}x{e.get('source', {}).get('height', '')}</td>"
            f"<td>{e.get('variant', {}).get('width', '')}x{e.get('variant', {}).get('height', '')}</td>"
            f"<td>{metrics}</td>"
            f"<td>{html.escape('; '.join(e.get('validation_failures', [])))}</td>"
            f"<td>{html.escape('; '.join(e.get('validation_warnings', [])))}</td>"
            "</tr>"
        )
    html_path.write_text(
        "<!doctype html><meta charset='utf-8'><title>HeavenBench texture report</title>"
        "<style>body{font:14px sans-serif;background:#17191d;color:#eee;padding:24px}"
        "table{border-collapse:collapse;width:100%}th,td{border:1px solid #555;padding:6px;vertical-align:top}"
        "th{background:#292d34}tr:nth-child(even){background:#20242a}</style>"
        f"<h1>Hi-Res texture bake: {'COMPLETE' if complete else 'INCOMPLETE'}</h1>"
        f"<p>{len(entries)} textures; "
        f"{sum(bool(e.get('validation_warnings')) for e in entries)} with warnings; "
        f"elapsed {elapsed:.1f} seconds.</p>"
        "<table><thead><tr><th>Texture</th><th>Role</th><th>Status</th><th>Source</th>"
        "<th>Variant</th><th>Metrics</th><th>Failures</th><th>Warnings</th>"
        "</tr></thead><tbody>"
        + "".join(rows) + "</tbody></table>",
        encoding="utf-8",
    )
    return json_path, html_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--device", required=True, choices=("cpu", "cuda", "rocm"))
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--resume", action="store_true", default=True)
    parser.add_argument(
        "--revalidate", action="store_true",
        help="verify and reclassify existing outputs without initializing a GPU",
    )
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.revalidate and args.force:
        raise RuntimeError("--revalidate and --force cannot be used together")
    repo = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    old_manifest = output / "texture-set.json"
    old_document: dict[str, Any] = {}
    if old_manifest.is_file() and not args.force:
        try:
            old_document = json.loads(old_manifest.read_text())
        except (ValueError, TypeError):
            old_document = {}
    if args.revalidate and not old_document:
        raise RuntimeError("--revalidate requires an existing texture-set.json")

    compressor = Path(os.environ.get("HEAVEN_HIRES_COMPRESSONATOR", ""))
    hat_root = Path(os.environ.get("HEAVEN_HIRES_HAT", ""))
    model = Path(os.environ.get("HEAVEN_HIRES_MODEL", ""))
    if not compressor.is_file() or not os.access(compressor, os.X_OK):
        raise RuntimeError("HEAVEN_HIRES_COMPRESSONATOR is unavailable")
    if not hat_root.is_dir() or not model.is_file():
        raise RuntimeError("pinned HAT source/model is unavailable")
    if args.revalidate:
        try:
            runtime = TorchRuntime(**old_document["dependencies"]["runtime"])
        except (KeyError, TypeError) as exc:
            raise RuntimeError(
                "existing texture-set.json lacks a valid runtime identity"
            ) from exc
        if runtime.backend != args.device:
            raise RuntimeError(
                f"existing set used {runtime.backend}, not requested {args.device}"
            )
        print(f"[hires] revalidating recorded {runtime.backend} outputs (GPU-free)")
    else:
        runtime = select_torch_runtime(args.device)

    started = time.monotonic()
    reader = SourceReader(args.data.resolve(), repo)
    try:
        inventory = build_inventory(reader)
    finally:
        reader.close()
    if args.limit > 0:
        inventory = inventory[:args.limit]
    print(f"[hires] inventory: {len(inventory)} render textures")

    old_entries: dict[str, dict[str, Any]] = {}
    if old_document:
        try:
            old_entries = {
                e["original_path"]: e for e in old_document["entries"]
            }
        except (KeyError, TypeError):
            old_entries = {}

    hat: HatUpscaler | None = None
    entries: list[dict[str, Any]] = []
    contact: dict[str, list[tuple[str, Any]]] = {"source": [], "2k": [], "4k": []}
    contact_counts = {pattern: 0 for pattern in CONTACT_PATTERNS}
    reader = SourceReader(args.data.resolve(), repo)
    try:
        for index, source in enumerate(inventory, 1):
            source_hash = sha256_bytes(source.data)
            key = cache_key(source, source_hash, runtime)
            print(f"[hires] [{index}/{len(inventory)}] {source.logical_path} ({source.role})")
            alias = (
                source.logical_path.startswith("core/") and
                source.width == 16 and source.height == 16
            )
            source_meta = {
                "format": source.source_format,
                "width": source.width,
                "height": source.height,
                "gpu_bytes": source.source_gpu_bytes,
                "has_alpha": source.has_alpha_hint,
            }
            if alias:
                entries.append({
                    "original_path": source.logical_path,
                    "source_sha256": source_hash,
                    "source": source_meta,
                    "role": source.role,
                    "wrap": source.wrap,
                    "cache_key": key,
                    "status": "alias",
                    "variant": {
                        "archive_path": source.logical_path,
                        "sha256": source_hash,
                        "format": source.source_format,
                        "width": source.width,
                        "height": source.height,
                        "gpu_bytes_2k": source.source_gpu_bytes,
                        "gpu_bytes_4k": source.source_gpu_bytes,
                    },
                    "metrics": {},
                    "validation_failures": [],
                    "validation_warnings": [],
                })
                continue

            variant_relative = (
                Path("variants") / PurePosixPath(source.logical_path).with_suffix(".dds")
            )
            variant_path = output / variant_relative
            logical_variant = PurePosixPath(source.logical_path).with_suffix(".dds")
            if logical_variant.parts[0] == reader.data_prefix:
                logical_variant = PurePosixPath(*logical_variant.parts[1:])
            archive_path = str(
                PurePosixPath(reader.data_prefix) / "texture_sets" / "4k" /
                logical_variant
            )
            previous = old_entries.get(source.logical_path)
            accepted_keys = {key, *(
                cache_key(source, source_hash, runtime, pipeline_version=version)
                for version in LEGACY_PIPELINE_VERSIONS
            )}
            if (previous and previous.get("status") in ("generated", "failed") and
                    previous.get("cache_key") in accepted_keys and variant_path.is_file()):
                variant = previous.get("variant", {})
                if sha256_file(variant_path) == variant.get("sha256"):
                    failures, warnings = reclassify_cached_validation(previous)
                    if not failures:
                        resumed = dict(previous)
                        resumed["status"] = "generated"
                        resumed["validation_failures"] = []
                        resumed["validation_warnings"] = warnings
                        resumed["validated_by"] = PIPELINE_VERSION
                        entries.append(resumed)
                        suffix = " (quality warnings retained)" if warnings else ""
                        print(f"[hires]   verified cache hit{suffix}")
                        continue

            if args.revalidate:
                entries.append({
                    "original_path": source.logical_path,
                    "source_sha256": source_hash,
                    "source": source_meta,
                    "role": source.role,
                    "wrap": source.wrap,
                    "cache_key": key,
                    "status": "failed",
                    "variant": {},
                    "metrics": {},
                    "validation_failures": [
                        "revalidation cache miss; rerun without --revalidate"
                    ],
                    "validation_warnings": [],
                })
                print("[hires]   FAILED: revalidation cache miss", file=sys.stderr)
                continue

            try:
                with tempfile.TemporaryDirectory(prefix="heaven-hires-", dir=output) as tmp_name:
                    temp = Path(tmp_name)
                    image = decode_source(source, temp / "decoded", compressor)
                    scale = target_scale(source.width, source.height)
                    if source.role in ("diffuse", "emissive", "color") and hat is None:
                        hat = HatUpscaler(runtime, hat_root, model)
                    top = generate_top(source, image, scale, hat)
                    mips = build_mips(source, top, image)
                    encoded_format, info = encode_mips(
                        mips, source.role, variant_path, compressor, temp
                    )
                    metrics, failures, warnings = validation_metrics(
                        source, image, top, mips
                    )
                    output_hash = sha256_file(variant_path)
                    entry = {
                        "original_path": source.logical_path,
                        "source_sha256": source_hash,
                        "source": source_meta,
                        "role": source.role,
                        "wrap": source.wrap,
                        "cache_key": key,
                        "status": "generated" if not failures else "failed",
                        "variant": {
                            "file": variant_relative.as_posix(),
                            "archive_path": archive_path,
                            "sha256": output_hash,
                            "format": encoded_format,
                            "width": info.width,
                            "height": info.height,
                            "gpu_bytes_2k": tier_bytes(info, 2048),
                            "gpu_bytes_4k": tier_bytes(info, 4096),
                        },
                        "metrics": metrics,
                        "validation_failures": failures,
                        "validation_warnings": warnings,
                    }
                    entries.append(entry)
                    contact_group = next((
                        pattern for pattern in CONTACT_PATTERNS
                        if pattern in source.logical_path.lower() and
                        contact_counts[pattern] < 2
                    ), None)
                    if contact_group is not None and len(contact["source"]) < 24:
                        contact_counts[contact_group] += 1
                        label = PurePosixPath(source.logical_path).name
                        contact["source"].append((label, make_thumbnail(image, label)))
                        contact["4k"].append((label, make_thumbnail(top, label)))
                        selected_2k = mips[0]
                        for mip in mips:
                            if max(mip.shape[0], mip.shape[1]) <= 2048:
                                selected_2k = mip
                                break
                        contact["2k"].append((label, make_thumbnail(selected_2k, label)))
            except Exception as exc:  # fail closed, retain diagnostics and continue
                entries.append({
                    "original_path": source.logical_path,
                    "source_sha256": source_hash,
                    "source": source_meta,
                    "role": source.role,
                    "wrap": source.wrap,
                    "cache_key": key,
                    "status": "failed",
                    "variant": {},
                    "metrics": {},
                    "validation_failures": [str(exc)],
                    "validation_warnings": [],
                })
                print(f"[hires]   FAILED: {exc}", file=sys.stderr)
    finally:
        reader.close()

    contact_paths: list[Path] = []
    for tier, images in contact.items():
        path = output / f"reports/contact-sheet-{tier}.png"
        write_contact_sheet(path, images)
        if path.is_file():
            contact_paths.append(path)

    complete = (
        args.limit == 0 and bool(inventory) and len(entries) == len(inventory) and
        all(e["status"] in ("generated", "alias") for e in entries) and
        len(contact_paths) == 3
    )
    elapsed = time.monotonic() - started
    report_paths = write_reports(output, entries, complete, elapsed, runtime)
    provenance_paths = copy_provenance(output, runtime)
    artifacts = [
        artifact_record(output, path, reader.data_prefix)
        for path in (*report_paths, *contact_paths, *provenance_paths)
    ]
    manifest = {
        "format": "heavenbench-texture-set",
        "schema_version": 1,
        "complete": complete,
        "generator": {
            "id": "heavenbench-faithful-hires",
            "version": PIPELINE_VERSION,
        },
        "dependencies": {
            "hat": {"revision": HAT_REV,
                    "archive_sha256": HAT_ARCHIVE_SHA256},
            "basicsr": {"revision": BASICSR_REV,
                        "archive_sha256": BASICSR_ARCHIVE_SHA256,
                        "patch_sha256": BASICSR_PATCH_SHA256},
            "real_hat_gan_srx4": {"url": MODEL_URL,
                                  "sha256": MODEL_SHA256},
            "compressonator": {
                "revision": COMPRESSONATOR_REV,
                "archive_sha256": COMPRESSONATOR_ARCHIVE_SHA256,
                "cli_url": COMPRESSONATOR_CLI_URL,
                "cli_sha256": COMPRESSONATOR_CLI_SHA256,
            },
            "runtime": runtime.manifest(),
        },
        "policy": {
            "maximum_edge": 4096,
            "upscale": 4,
            "color_model": "Real_HAT_GAN_SRx4",
            "deterministic_fp32": True,
            "tile_size": 256,
            "context_overlap": 32,
            "alpha_ai": False,
            "normal_filter": "vector-renormalized-lanczos",
            "data_filter": "linear-lanczos",
            "encoder": "Compressonator quality 1.0",
            "quality_thresholds": "reported as review warnings",
        },
        "entries": entries,
        "artifacts": artifacts,
    }
    old_manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"[hires] texture set: {old_manifest}")
    print(f"[hires] status: {'COMPLETE' if complete else 'INCOMPLETE'}")
    return 0 if complete else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
