#!/usr/bin/env python3
"""Compatibility entry point for the offline texture generator tests."""
from pathlib import Path
import runpy
import sys

directory = Path(__file__).resolve().parents[1] / "tools" / "hires_textures"
sys.path.insert(0, str(directory))
runpy.run_path(str(directory / "test_hires_texture_generator.py"), run_name="__main__")
