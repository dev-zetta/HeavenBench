#!/usr/bin/env python3
"""GPU-free golden policy tests for the offline hi-res texture generator."""

from __future__ import annotations

import math
import unittest

import numpy as np

import hires_texture_generator as generator


def source(role: str, wrap: str = "repeat") -> generator.SourceTexture:
    return generator.SourceTexture(
        "heaven/test.dds", b"source", 64, 64, "BC3", 4096,
        role, wrap, True,
    )


class HiresTextureGeneratorTests(unittest.TestCase):
    def test_scaling_policy(self) -> None:
        self.assertEqual(generator.target_scale(16, 16), 4)
        self.assertEqual(generator.target_scale(1024, 512), 4)
        self.assertEqual(generator.target_scale(2048, 1024), 2)
        self.assertEqual(generator.target_scale(4096, 2048), 1)
        with self.assertRaises(ValueError):
            generator.target_scale(0, 0)

    def test_content_address_includes_semantic_and_wrap(self) -> None:
        digest = generator.sha256_bytes(b"source")
        diffuse = generator.cache_key(source("diffuse"), digest)
        self.assertEqual(diffuse, generator.cache_key(source("diffuse"), digest))
        self.assertNotEqual(diffuse, generator.cache_key(source("normal"), digest))
        self.assertNotEqual(
            diffuse, generator.cache_key(source("diffuse", "clamp"), digest)
        )
        cpu = generator.TorchRuntime("cpu", "2.1.2", "", None, "CPU", "host")
        rocm = generator.TorchRuntime(
            "rocm", "2.9.1+rocm7.2.1", "7.2.1", 0, "AMD GPU", "gfx1201"
        )
        self.assertNotEqual(
            generator.cache_key(source("diffuse"), digest, cpu),
            generator.cache_key(source("diffuse"), digest, rocm),
        )

    def test_bc7_dx10_header_and_mip_selection(self) -> None:
        header = generator.compressed_header(8, 8, 4, "BC7", True)
        payload = bytes(64 + 16 + 16 + 16)
        info = generator.dds_info(header + payload)
        self.assertEqual(info.format, "BC7_SRGB")
        self.assertEqual((info.width, info.height, info.mip_count), (8, 8, 4))
        self.assertEqual(generator.tier_bytes(info, 4), 48)

    def test_alpha_coverage_and_dilation(self) -> None:
        y, x = np.mgrid[:64, :64]
        alpha = (((x - 31.5) ** 2 + (y - 31.5) ** 2) < 22.0 ** 2).astype(
            np.float32
        )
        rgb = np.zeros((64, 64, 3), np.float32)
        rgb[alpha > 0.5] = (0.8, 0.3, 0.1)
        dilated = generator.dilate_foreground(rgb, alpha, "repeat")
        self.assertGreater(float(dilated[alpha < 0.5].max()), 0.0)
        top_alpha = generator.resize_channels(
            alpha, 256, 256, linear_rgb=False, wrap="repeat"
        )[:, :, 0]
        top_alpha = generator.preserve_alpha_coverage(top_alpha, alpha)
        top = np.concatenate((np.repeat(top_alpha[:, :, None], 3, axis=2),
                              top_alpha[:, :, None]), axis=2)
        original = np.concatenate((rgb, alpha[:, :, None]), axis=2)
        mips = generator.build_mips(source("diffuse"), top, original)
        base = float((alpha >= 0.5).mean())
        for mip in mips:
            if mip.shape[0] * mip.shape[1] < 200:
                continue
            drift = abs(float((mip[:, :, 3] >= 0.5).mean()) - base)
            self.assertLessEqual(drift, 0.005)

    def test_legacy_quality_failures_are_resumable_warnings(self) -> None:
        entry = {
            "role": "diffuse",
            "validation_failures": ["SSIM below 0.95", "PSNR below 30 dB"],
        }
        failures, warnings = generator.reclassify_cached_validation(entry)
        self.assertEqual(failures, [])
        self.assertEqual(warnings, ["SSIM below 0.95", "PSNR below 30 dB"])

        operational = {
            "role": "diffuse",
            "validation_failures": ["Compressonator exited with status 1"],
        }
        failures, warnings = generator.reclassify_cached_validation(operational)
        self.assertEqual(failures, ["Compressonator exited with status 1"])
        self.assertEqual(warnings, [])

    def test_packed_data_alpha_is_not_cutout_coverage(self) -> None:
        entry = {
            "role": "specular",
            "validation_failures": [
                "alpha coverage drift above 0.5 percentage points"
            ],
        }
        failures, warnings = generator.reclassify_cached_validation(entry)
        self.assertEqual(failures, [])
        self.assertEqual(warnings, [])

        original = np.zeros((64, 64, 4), np.float32)
        original[:, :, 0] = np.indices((64, 64)).sum(axis=0) % 2
        original[:, :, 3] = original[:, :, 0]
        top = np.zeros((256, 256, 4), np.float32)
        mips = generator.build_mips(source("specular", "clamp"), top, original)
        metrics, failures, warnings = generator.validation_metrics(
            source("specular", "clamp"), original, top, mips
        )
        self.assertNotIn("max_alpha_coverage_drift", metrics)
        self.assertEqual(failures, [])
        self.assertEqual(warnings, [])

    def test_learned_color_round_trip_thresholds_are_warnings(self) -> None:
        y, x = np.mgrid[:64, :64]
        checker = ((x + y) % 2).astype(np.float32)
        original = np.stack((checker, 1.0 - checker, checker,
                             np.ones_like(checker)), axis=2)
        top = np.zeros((256, 256, 4), np.float32)
        top[:, :, 3] = 1.0
        mips = generator.build_mips(source("diffuse", "clamp"), top, original)
        _, failures, warnings = generator.validation_metrics(
            source("diffuse", "clamp"), original, top, mips
        )
        self.assertEqual(failures, [])
        self.assertIn("PSNR below 30 dB", warnings)
        self.assertIn("SSIM below 0.95", warnings)

    def test_vector_normal_filter_renormalizes(self) -> None:
        y, x = np.mgrid[:64, :64]
        nx = 0.35 * np.sin(2.0 * math.pi * x / 64.0)
        ny = 0.35 * np.cos(2.0 * math.pi * y / 64.0)
        nz = np.sqrt(np.maximum(0.0, 1.0 - nx * nx - ny * ny))
        encoded = generator.encode_normal(np.stack((nx, ny, nz), axis=2))
        up = generator.resize_normal(encoded, 256, 256, "repeat")
        vectors = generator.decode_normal(up)
        lengths = np.linalg.norm(vectors, axis=2)
        self.assertLess(float(np.abs(lengths - 1.0).max()), 1e-5)
        down = generator.resize_normal(up, 64, 64, "repeat")
        dots = np.clip(np.sum(generator.decode_normal(encoded) *
                              generator.decode_normal(down), axis=2), -1.0, 1.0)
        error = float(np.degrees(np.arccos(dots)).mean())
        self.assertLessEqual(error, 2.0)

    def test_scalar_bounds_and_repeat_edge(self) -> None:
        y, x = np.mgrid[:64, :64]
        scalar = (0.5 + 0.25 * np.sin(2.0 * math.pi * x / 64.0) *
                  np.cos(2.0 * math.pi * y / 64.0)).astype(np.float32)
        rgba = np.repeat(scalar[:, :, None], 4, axis=2)
        rgba[:, :, 3] = 1.0
        top = generator.generate_top(source("height"), rgba, 4, None)
        self.assertGreaterEqual(float(top[:, :, 0].min()), float(scalar.min()))
        self.assertLessEqual(float(top[:, :, 0].max()), float(scalar.max()))
        down = generator.resize_array(
            top[:, :, 0], 64, 64, generator.import_image_stack()[0].INTER_AREA,
            "repeat",
        )
        self.assertLessEqual(float(np.abs(down - scalar).mean()), 1.0 / 255.0)
        source_edge = max(
            float(np.abs(rgba[:, 0, :3] - rgba[:, -1, :3]).mean()),
            float(np.abs(rgba[0, :, :3] - rgba[-1, :, :3]).mean()),
        )
        output_edge = max(
            float(np.abs(top[:, 0, :3] - top[:, -1, :3]).mean()),
            float(np.abs(top[0, :, :3] - top[-1, :, :3]).mean()),
        )
        self.assertLessEqual(output_edge, source_edge + 2.0 / 255.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
