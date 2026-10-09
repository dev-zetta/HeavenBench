#!/usr/bin/env python3
"""Compatibility entry point; implementation lives in tools/hires_textures."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.hires_textures.hires_texture_generator import *  # noqa: F403,E402
from tools.hires_textures.hires_texture_generator import main

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
