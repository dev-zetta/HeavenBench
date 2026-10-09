#!/usr/bin/env python3
"""Regression tests for row-vector scene hierarchy and malformed meshes."""

import importlib.util
import pathlib
import struct
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location('assemble_scene', pathlib.Path(__file__).resolve().parents[1] / 'tools/extraction/assemble_scene.py')
scene = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scene)
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


class AssemblyTests(unittest.TestCase):
    def test_local_translation_follows_parent_rotation(self):
        parent = [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        local = IDENTITY.copy()
        local[12] = 2
        with mock.patch.object(scene, 'extract_mesh_transforms', return_value=[('mesh', local)]), \
             mock.patch.object(scene, 'extract_node_refs_with_transforms', return_value=[]), \
             mock.patch.object(scene, 'resolve_path', return_value='mesh'):
            entries = scene.process_node_file('node', [], parent)
        self.assertEqual(scene.transform_point(entries[0][1], 0, 0, 0), (0, 2, 0))

    def test_nested_reference_translation_follows_parent_rotation(self):
        parent = [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        local = IDENTITY.copy()
        local[12] = 2
        with mock.patch.object(scene, 'extract_mesh_transforms', side_effect=[[], [('mesh', IDENTITY)]]), \
             mock.patch.object(scene, 'extract_node_refs_with_transforms', side_effect=[[('child', local)], []]), \
             mock.patch.object(scene, 'resolve_path', side_effect=lambda name, _: name):
            entries = scene.process_node_file('parent', [], parent)
        self.assertEqual(scene.transform_point(entries[0][1], 0, 0, 0), (0, 2, 0))

    def test_normal_stays_perpendicular_with_nonuniform_scale(self):
        transform = IDENTITY.copy()
        transform[0] = 2
        normal = scene.transform_normal(transform, 1, 1, 0)
        self.assertAlmostEqual(2 * normal[0] - normal[1], 0)
        self.assertAlmostEqual(sum(value * value for value in normal), 1)

    def test_normal_with_rotation_and_reflection(self):
        rotation = [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        self.assertEqual(scene.transform_normal(rotation, 1, 0, 0), (0, 1, 0))
        reflection = IDENTITY.copy()
        reflection[0] = -1
        self.assertEqual(scene.transform_normal(reflection, 1, 0, 0), (-1, 0, 0))

    def test_reference_cycle_is_rejected(self):
        with mock.patch.object(scene, 'extract_mesh_transforms', return_value=[]), \
             mock.patch.object(scene, 'extract_node_refs_with_transforms', return_value=[('node', IDENTITY)]), \
             mock.patch.object(scene, 'resolve_path', return_value='node'):
            with self.assertRaisesRegex(ValueError, 'Cyclic'):
                scene.process_node_file('node', [])

    def test_repeated_reference_is_allowed(self):
        with mock.patch.object(scene, 'extract_mesh_transforms', side_effect=[[], [('mesh', IDENTITY)], [('mesh', IDENTITY)]]), \
             mock.patch.object(scene, 'extract_node_refs_with_transforms', side_effect=[[('child', IDENTITY), ('child', IDENTITY)], [], []]), \
             mock.patch.object(scene, 'resolve_path', side_effect=lambda name, _: name):
            self.assertEqual(len(scene.process_node_file('parent', [])), 2)

    def test_negative_and_truncated_mesh_counts(self):
        header = b'mi08' + bytes(40) + struct.pack('<i', 1)
        header += struct.pack('<i', 4) + b'abc\0' + bytes(40)
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / 'bad.mesh'
            for count in (-1, 10_000_000):
                source.write_bytes(header + struct.pack('<i', count) + bytes(12))
                with self.subTest(count=count), self.assertRaises(ValueError):
                    scene.read_mesh(source)


if __name__ == '__main__':
    unittest.main()
