#!/usr/bin/env python3
"""Small generated archives exercise extraction without proprietary assets."""

import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


UNIGINEX = pathlib.Path(sys.argv.pop(1)).resolve()
MESH2OBJ = pathlib.Path(sys.argv.pop(1)).resolve()
SEED = 0xA13CDBDE


def advance(seed):
    return (seed * 0x6B982 & 0xFFFFFFFF) % 0x7FFFFFAB


def rotate(value, amount, left):
    amount &= 7
    if not left:
        amount = -amount & 7
    return ((value << amount) | (value >> ((8 - amount) & 7))) & 255


class Encoder:
    def __init__(self, seed=SEED):
        self.seed = seed

    def integer(self, value, bits=32):
        data = bytearray(value.to_bytes(bits // 8, 'little'))
        if bits == 32:
            order = (3, 2, 0, 1)
            rotations = ((1, False), (3, True), (2, False), (0, True))
            xor_order = (3, 0, 2, 1)
        else:
            order = (7, 6, 4, 5, 3, 2, 0, 1)
            rotations = ((5, False), (7, True), (4, False), (6, True),
                         (1, False), (3, True), (2, False), (0, True))
            xor_order = (7, 6, 4, 5, 3, 0, 2, 1)
        rotation_seeds = []
        for _ in rotations:
            rotation_seeds.append(self.seed)
            self.seed = advance(self.seed)
        for index in xor_order:
            data[index] ^= self.seed & 255
            self.seed = advance(self.seed)
        for (index, left), seed in zip(rotations, rotation_seeds):
            data[index] = rotate(data[index], seed, not left)
        return bytes(data[index] for index in order)

    def string(self, value):
        encoded = bytearray()
        previous = 0
        for index, value in enumerate(value + b'\0'):
            seed = self.seed
            self.seed = advance(self.seed)
            cipher = value ^ ((self.seed + previous) & 255)
            self.seed = advance(self.seed)
            encoded.append(rotate(cipher, seed + previous, not ((index + previous) & 1)))
            previous = value
        return bytes(encoded)

    def data(self, value):
        encoded = bytearray()
        previous = 0
        for index, value in enumerate(value):
            seed = self.seed
            self.seed = advance(self.seed)
            cipher = value ^ ((self.seed - previous) & 255)
            self.seed = advance(self.seed)
            left = bool((index ^ previous) & 1)
            amount = seed + previous if left else seed - previous
            encoded.append(rotate(cipher, amount, not left))
            previous = value
        return bytes(encoded)


def archive(entries, compression=0, data_offset_override=None, size_override=None):
    data = bytearray(b'ar04' + struct.pack('<I', compression))
    metadata = bytearray()
    index_encoder = Encoder()
    data_seed = SEED
    for name, payload in entries:
        offset = len(data)
        if compression == 2:
            compressed = zlib.compress(payload)
        elif compression == 1:
            # A short literal-only LZ4 block needs no third-party test module.
            assert len(payload) < 15
            compressed = bytes([len(payload) << 4]) + payload
        else:
            compressed = payload
        encoder = Encoder(data_seed)
        data.extend(encoder.integer(len(compressed)))
        data.extend(encoder.integer(0))
        data.extend(encoder.integer(0))
        data.extend(encoder.data(compressed))
        data_seed = advance(data_seed)
        name = name.encode()
        metadata.extend(index_encoder.integer(zlib.crc32(name)))
        metadata.extend(index_encoder.string(name))
        metadata.extend(index_encoder.integer(len(payload) if size_override is None else size_override))
        metadata.extend(index_encoder.integer(len(compressed)))
        metadata.extend(index_encoder.integer(offset if data_offset_override is None else data_offset_override, 64))
    return data + metadata + struct.pack('<Q', len(data))


def archive_v3(entries):
    data = bytearray(b'ar03')
    info_seed = SEED
    for name, payload in entries:
        encoder = Encoder(info_seed)
        name = name.encode()
        compressed = zlib.compress(payload)
        data.extend(encoder.integer(zlib.crc32(name)))
        data.extend(encoder.string(name))
        data.extend(encoder.integer(len(payload)))
        info_seed = encoder.seed
        data.extend(encoder.integer(len(compressed)))
        data.extend(encoder.integer(len(compressed)))
        data.extend(encoder.integer(zlib.crc32(payload)))
        data.extend(encoder.data(compressed))
    return data


def mesh(uv_count=3, vertex_count=3):
    data = b'mi08' + bytes(40) + struct.pack('<i', 1)
    data += struct.pack('<i', 4) + b'abc\0' + bytes(40)
    data += struct.pack('<i', vertex_count)
    if vertex_count != 3:
        return data
    for vertex in ((0, 0, 0), (1, 0, 0), (0, 1, 0)):
        data += struct.pack('<3f3H', *vertex, 32768, 32768, 65535)
    data += struct.pack('<i', uv_count) + struct.pack('<2f', 0, 0) * uv_count
    data += struct.pack('<ii3H', 0, 1, 0, 1, 2)
    return data


class ExtractionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.out = self.root / 'out'

    def extract(self, data, *options):
        source = self.root / 'test.ung'
        source.write_bytes(data)
        return subprocess.run([UNIGINEX, '-o', *options, source, self.out],
                              capture_output=True, text=True, timeout=5)

    def assert_rejected(self, result):
        self.assertGreater(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(result.returncode, 128, result.stderr)
        self.assertIn('Error:', result.stderr)
        self.assertNotIn('Sanitizer', result.stderr)
        self.assertNotIn('runtime error:', result.stderr)

    def test_cipher_known_answer(self):
        # First filename metadata in the original Heaven ar03 archive. This
        # independently fixes the cipher arithmetic rather than testing only
        # an encoder and decoder that could share the same mistaken PRNG.
        encoder = Encoder()
        metadata = encoder.integer(0xC4CBCFF2) + encoder.string(b'heaven/gui/font_ch.ttf')
        self.assertEqual(metadata.hex(), 'ecb6376df45d59228b4566016f1dbddad0d733abf1f9e9735636e2')

    def test_version3_encrypted_archive(self):
        entries = [('a.txt', b'first'), ('folder/b.txt', b'second')]
        result = self.extract(archive_v3(entries))
        self.assertEqual(result.returncode, 0, result.stderr)
        for name, payload in entries:
            self.assertEqual((self.out / name).read_bytes(), payload)

    def test_encrypted_archive(self):
        entries = [('a.txt', b'first'), ('folder/b.txt', b'second')]
        for compression in (0, 1, 2):
            with self.subTest(compression=compression):
                result = self.extract(archive(entries, compression))
                self.assertEqual(result.returncode, 0, result.stderr)
                for name, payload in entries:
                    self.assertEqual((self.out / name).read_bytes(), payload)

    def test_filter_preserves_data_seed(self):
        result = self.extract(archive([('a.txt', b'first'), ('b.txt', b'second')]), '-f', 'b.txt')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.out / 'a.txt').exists())
        self.assertEqual((self.out / 'b.txt').read_bytes(), b'second')

    def test_invalid_index_offsets(self):
        for offset in (0, 7, 17, 2**64 - 1):
            with self.subTest(offset=offset):
                data = b'ar04' + bytes(4) + struct.pack('<Q', offset)
                self.assert_rejected(self.extract(data))

    def test_invalid_data_offset(self):
        self.assert_rejected(self.extract(archive([('a', b'x')], data_offset_override=2**63)))

    def test_truncated_data_block(self):
        data = archive([('a', b'x')], data_offset_override=8 + 12)
        self.assert_rejected(self.extract(data))

    def test_uncompressed_size_mismatch(self):
        self.assert_rejected(self.extract(archive([('a', b'x')], size_override=2)))

    def test_existing_directory_symlink(self):
        outside = self.root / 'outside'
        outside.mkdir()
        self.out.mkdir()
        (self.out / 'linked').symlink_to(outside, target_is_directory=True)
        result = self.extract(archive([('linked/escape.txt', b'overwrite')]))
        self.assert_rejected(result)
        self.assertFalse((outside / 'escape.txt').exists())

    def test_existing_file_symlink(self):
        outside = self.root / 'outside.txt'
        outside.write_bytes(b'original')
        self.out.mkdir()
        (self.out / 'linked').symlink_to(outside)
        self.assert_rejected(self.extract(archive([('linked', b'overwrite')])))
        self.assertEqual(outside.read_bytes(), b'original')

    def test_mesh_texture_indices(self):
        for uv_count in (0, 1, 3):
            with self.subTest(uv_count=uv_count):
                source = self.root / f'mesh{uv_count}.mesh'
                source.write_bytes(mesh(uv_count))
                result = subprocess.run([MESH2OBJ, source], capture_output=True, text=True, timeout=5)
                if uv_count == 1:
                    self.assert_rejected(result)
                    self.assertFalse(source.with_suffix('.obj').exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('f 1', source.with_suffix('.obj').read_text())


if __name__ == '__main__':
    unittest.main()
