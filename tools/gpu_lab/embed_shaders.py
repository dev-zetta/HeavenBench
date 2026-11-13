"""Embed validated diagnostic modules; no runtime compiler or data dependency."""
from pathlib import Path
import hashlib
import struct
import sys

*modules, destination = map(Path, sys.argv[1:])
parts = ['#ifndef HEAVEN_GPU_LAB_SHADERS_H_', '#define HEAVEN_GPU_LAB_SHADERS_H_',
         '#include <cstdint>', 'namespace heaven::gpu_lab {']
identity = hashlib.sha256()
for path in (*modules, *sorted(Path(__file__).parent.glob('*.comp')), Path(__file__),
             Path(__file__).with_name('main.cpp'), Path(__file__).with_name('int8_contract.h'),
             Path(__file__).with_name('CMakeLists.txt')):
    data = path.read_bytes()
    identity.update(len(data).to_bytes(8, 'little'))
    identity.update(data)
parts.append('inline constexpr char implementation_sha256[] = "' + identity.hexdigest() + '";')
for source in modules:
    name = source.stem + '_spirv'
    data = source.read_bytes()
    if len(data) % 4 or data[:4] != b'\x03\x02\x23\x07':
        raise SystemExit('Invalid SPIR-V module: ' + str(source))
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    parts.append('inline constexpr std::uint32_t ' + name + '[] = {')
    parts.extend(','.join(hex(word) for word in words[i:i + 8]) + ','
                 for i in range(0, len(words), 8))
    parts.append('};')
parts.extend(['}', '#endif', ''])
destination.write_text('\n'.join(parts))
