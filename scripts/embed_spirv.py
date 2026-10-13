#!/usr/bin/env python3
"""Embed a compiled, word-aligned SPIR-V module deterministically."""
import pathlib
import struct
import sys

source, destination = map(pathlib.Path, sys.argv[1:])
data = source.read_bytes()
if len(data) % 4 or data[:4] != b'\x03\x02\x23\x07':
    raise SystemExit('Invalid SPIR-V module')
words = struct.unpack('<' + 'I' * (len(data) // 4), data)
destination.write_text('#pragma once\n#include <cstdint>\ninline constexpr uint32_t kFgUiSpirv[] = {\n' + '\n'.join(','.join(hex(w) for w in words[i:i+8]) + ',' for i in range(0, len(words), 8)) + '\n};\n')
