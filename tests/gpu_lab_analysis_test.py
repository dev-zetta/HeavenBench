"""Independent interval and invalid-comparison controls; no GPU required."""
import copy
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'scripts/analyze_gpu_lab.py'
spec = importlib.util.spec_from_file_location('gpu_lab_analysis', path)
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def fixture():
    return {'schema': 1, 'benchmark': 'gpu-matrix-lab', 'scored': False, 'quick_check': False,
            'adapter': {'device_uuid': [1], 'driver_version': 'test'}, 'implementation_sha256': 'fixed',
            'timing': {'timestamp_valid_bits': 64, 'timestamp_period_ns': 1},
            'compiler': 'test', 'application_version': 'test', 'build_type': 'Release',
            'workload': {'matrix_size': 256, 'batches': 4, 'input': 'FP16', 'accumulation_output': 'FP32'},
            'validation': {'enabled': False, 'errors': 0, 'warnings': 0},
            'results': [{'kernel': 'shared-tiled-simd-fp32', 'status': 'passed',
                         'measurement_window_complete': True, 'wall_seconds': 60, 'warmup_seconds': 2,
                         'exact_integer_cpu_oracle': True, 'correct_output_elements_before_and_after': 262144,
                         'dispatches_per_sample': 2, 'gpu_batch_ms': [2, 4, 6, 8]}]}


def int8_fixture():
    capture = fixture()
    capture['workload'].update(input='INT8', accumulation_output='INT32', signedness='signed',
                               input_range=[-128, 127], operations_per_dispatch=134217728,
                               maximum_accumulator_magnitude=4194304)
    shape = {'m': 8, 'n': 16, 'k': 32, 'a_type': 3, 'b_type': 3, 'accumulator_type': 5,
             'result_type': 5, 'scope': 3, 'saturating': False}
    capture['adapter']['matrix_shapes'] = [shape]
    capture['adapter']['capabilities'] = {'packed_signed_int8_dot_accelerated': True}
    capture['results'][0].update(kernel='shared-tiled-int8-int32', enabled_features={
        'cooperative_matrix': True, 'shader_int8': True, 'storage_buffer_8bit': True,
        'vulkan_memory_model': True, 'maintenance4': True, 'integer_dot_product': True})
    return capture


class AnalysisTest(unittest.TestCase):
    def test_exact_int8_operations_are_not_flops(self):
        capture = int8_fixture()
        result = analysis.analyze([capture])['kernels']['shared-tiled-int8-int32']['per_run'][0]
        self.assertAlmostEqual(result['integer_gops'], 53.6870912)
        self.assertNotIn('arithmetic_gflops', result)

    def test_int8_matched_three_way_comparison(self):
        reference = int8_fixture()
        dot, matrix = copy.deepcopy(reference), copy.deepcopy(reference)
        dot['results'][0].update(kernel='packed-dot4-int8-int32', gpu_batch_ms=[1, 2, 3, 4])
        matrix['results'][0].update(kernel='cooperative-int8-int32', gpu_batch_ms=[0.5, 1, 1.5, 2],
                                    enabled_matrix_shape=matrix['adapter']['matrix_shapes'][0])
        output = analysis.analyze([reference, dot, matrix])
        self.assertEqual(output['reference_to_cooperative_time_ratio'], 4)
        self.assertEqual(output['reference_to_dp4a_time_ratio'], 2)
        self.assertEqual(output['dp4a_to_cooperative_time_ratio'], 2)

    def test_int8_capability_enablement_and_shape_negatives(self):
        capture = int8_fixture()
        capture['results'][0]['kernel'] = 'packed-dot4-int8-int32'
        for field in ('integer_dot_product',):
            wrong = copy.deepcopy(capture)
            wrong['results'][0]['enabled_features'][field] = False
            with self.assertRaisesRegex(ValueError, 'enablement'):
                analysis.analyze([wrong])
        wrong = copy.deepcopy(capture)
        wrong['adapter']['capabilities']['packed_signed_int8_dot_accelerated'] = False
        with self.assertRaisesRegex(ValueError, 'acceleration'):
            analysis.analyze([wrong])
        capture['results'][0].update(kernel='cooperative-int8-int32',
                                     enabled_matrix_shape=capture['adapter']['matrix_shapes'][0])
        for field in ('cooperative_matrix', 'shader_int8', 'storage_buffer_8bit', 'vulkan_memory_model', 'maintenance4'):
            wrong = copy.deepcopy(capture)
            wrong['results'][0]['enabled_features'][field] = False
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'enablement'):
                analysis.analyze([wrong])
        for field, value in [('a_type', 6), ('result_type', 6), ('scope', 2), ('saturating', True), ('k', 30)]:
            wrong = copy.deepcopy(capture)
            wrong['results'][0]['enabled_matrix_shape'][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'shape'):
                analysis.analyze([wrong])

    def test_int8_precision_signedness_and_units_negatives(self):
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            analysis.analyze([fixture(), int8_fixture()])
        for field, value in [('signedness', 'unsigned'), ('input_range', [0, 255]),
                             ('operations_per_dispatch', 1), ('maximum_accumulator_magnitude', 0)]:
            capture = int8_fixture()
            capture['workload'][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'contract'):
                analysis.analyze([capture])
        capture = int8_fixture()
        capture['results'][0]['arithmetic_gflops'] = 999
        with self.assertRaisesRegex(ValueError, 'FLOPs'):
            analysis.analyze([capture])
        for field, value in [('quick_check', True), ('validation', {'enabled': True, 'errors': 0, 'warnings': 0})]:
            capture = int8_fixture()
            capture[field] = value
            with self.assertRaises(ValueError):
                analysis.analyze([capture])

    def test_uses_raw_intervals_and_independent_arithmetic(self):
        capture = fixture()
        capture['results'][0]['gpu_ms_per_dispatch_mean'] = 99999
        result = analysis.analyze([capture])['kernels']['shared-tiled-simd-fp32']
        self.assertEqual(result['mean_gpu_ms'], 2.5)
        self.assertEqual(result['per_run'][0]['p50_gpu_ms'], 2)
        self.assertEqual(result['per_run'][0]['p95_gpu_ms'], 4)
        self.assertAlmostEqual(result['per_run'][0]['arithmetic_gflops'], 53.6870912)

    def test_repeated_kernel_comparison(self):
        reference = fixture()
        matrix = copy.deepcopy(reference)
        matrix['results'][0]['kernel'] = 'cooperative-fp16-fp32'
        matrix['results'][0]['gpu_batch_ms'] = [1, 2, 3, 4]
        result = analysis.analyze([reference, matrix, matrix, reference])
        self.assertEqual(result['reference_to_cooperative_time_ratio'], 2)
        self.assertEqual(result['kernels']['cooperative-fp16-fp32']['runs'], 2)

    def test_mismatched_hardware_workload_and_source_refused(self):
        for key, value in [('adapter', {'device_uuid': [2]}), ('implementation_sha256', 'other'),
                           ('compiler', 'other'), ('workload', {'matrix_size': 128})]:
            changed = fixture()
            changed[key] = value
            with self.assertRaisesRegex(ValueError, 'mismatch'):
                analysis.analyze([fixture(), changed])

    def test_incomplete_or_invalid_evidence_refused(self):
        changes = [('wall_seconds', 59), ('wall_seconds', float('nan')), ('warmup_seconds', 1.9), ('measurement_window_complete', False),
                   ('exact_integer_cpu_oracle', False), ('correct_output_elements_before_and_after', 1),
                   ('gpu_batch_ms', []), ('gpu_batch_ms', [float('nan')]), ('gpu_batch_ms', [-1]),
                   ('dispatches_per_sample', 0), ('dispatches_per_sample', True), ('status', 'unavailable')]
        for key, value in changes:
            capture = fixture()
            capture['results'][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                analysis.analyze([capture])
        capture = fixture()
        capture['results'].append(copy.deepcopy(capture['results'][0]))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            analysis.analyze([capture])
        for key, value in [('quick_check', True), ('scored', True), ('build_type', 'Debug'),
                           ('validation', {'enabled': True, 'errors': 0, 'warnings': 0})]:
            capture = fixture()
            capture[key] = value
            with self.assertRaises(ValueError):
                analysis.analyze([capture])


if __name__ == '__main__':
    unittest.main()
