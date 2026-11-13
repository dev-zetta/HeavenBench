#!/usr/bin/env python3
"""Analyze completed GPU-lab windows without mixing hardware or workloads."""
import argparse
import json
import math
from pathlib import Path
import statistics


def analyze(captures):
    signature = None
    grouped = {}
    for capture in captures:
        if (capture.get('schema') != 1 or capture.get('benchmark') != 'gpu-matrix-lab'
                or capture.get('scored') is not False or capture.get('quick_check') is not False):
            raise ValueError('Expected a completed, unscored performance capture, not a quick check')
        keys = ('adapter', 'workload', 'timing', 'implementation_sha256', 'compiler', 'build_type', 'application_version')
        current = {key: capture[key] for key in keys}
        if signature is None:
            signature = current
        elif current != signature:
            raise ValueError('GPU, driver, implementation, compiler, version or workload mismatch')
        validation = capture['validation']
        if validation['enabled'] or validation['errors'] or validation['warnings']:
            raise ValueError('Performance captures must have validation disabled and no reported diagnostics')
        if capture['build_type'] != 'Release':
            raise ValueError('Performance captures require a Release build')
        workload = capture['workload']
        precision = (workload['input'], workload['accumulation_output'])
        integer = precision == ('INT8', 'INT32')
        if precision != ('FP16', 'FP32') and not integer:
            raise ValueError('Unsupported arithmetic contract')
        size, batches = workload['matrix_size'], workload['batches']
        if type(size) is not int or type(batches) is not int or not 1 <= size <= 4096 or not 1 <= batches <= 256:
            raise ValueError('Invalid workload dimensions')
        if integer and (workload.get('signedness') != 'signed' or workload.get('input_range') != [-128, 127]
                        or workload.get('operations_per_dispatch') != 2 * size ** 3 * batches
                        or workload.get('maximum_accumulator_magnitude') != size * 128 * 128):
            raise ValueError('Incomplete signed INT8 arithmetic contract')
        kernels = ('shared-tiled-int8-int32', 'packed-dot4-int8-int32', 'cooperative-int8-int32') if integer else (
            'shared-tiled-simd-fp32', 'cooperative-fp16-fp32')
        seen = set()
        for result in capture['results']:
            if result['kernel'] not in kernels or result['kernel'] in seen:
                raise ValueError('Unknown or duplicate kernel result')
            seen.add(result['kernel'])
            if result['status'] == 'unavailable':
                raise ValueError('Requested comparison contains an unavailable kernel')
            if (result['status'] != 'passed' or not result['measurement_window_complete']
                    or not math.isfinite(result['wall_seconds']) or not math.isfinite(result['warmup_seconds'])
                    or result['wall_seconds'] < 60 or result['warmup_seconds'] < 2
                    or result.get('exact_integer_cpu_oracle') is not True
                    or result['correct_output_elements_before_and_after'] != size * size * batches):
                raise ValueError('Incomplete timing window or correctness coverage')
            if integer:
                if 'arithmetic_gflops' in result:
                    raise ValueError('Integer operations must not be reported as FLOPs')
                enabled = result.get('enabled_features', {})
                capabilities = capture['adapter'].get('capabilities', {})
                if result['kernel'] == 'packed-dot4-int8-int32' and (
                        enabled.get('integer_dot_product') is not True
                        or capabilities.get('packed_signed_int8_dot_accelerated') is not True):
                    raise ValueError('Signed packed INT8 acceleration or logical-device enablement unavailable')
                if result['kernel'] == 'cooperative-int8-int32':
                    if any(enabled.get(key) is not True for key in ('cooperative_matrix', 'shader_int8',
                            'storage_buffer_8bit', 'vulkan_memory_model', 'maintenance4')):
                        raise ValueError('INT8 cooperative logical-device feature enablement unavailable')
                    shape = result.get('enabled_matrix_shape', {})
                    if (shape not in capture['adapter'].get('matrix_shapes', [])
                            or shape.get('a_type') != 3 or shape.get('b_type') != 3
                            or shape.get('accumulator_type') != 5 or shape.get('result_type') != 5
                            or shape.get('scope') != 3 or shape.get('saturating') is not False
                            or any(type(shape.get(key)) is not int or shape[key] <= 0 or size % shape[key]
                                   for key in ('m', 'n', 'k'))):
                        raise ValueError('Enabled signed INT8/INT32 cooperative shape unavailable or incompatible')
            samples = result['gpu_batch_ms']
            repeats = result['dispatches_per_sample']
            if (type(repeats) is not int or not 1 <= repeats <= 4096 or not samples
                    or any(isinstance(value, bool) or not isinstance(value, (float, int))
                           or not math.isfinite(value) or value <= 0 for value in samples)):
                raise ValueError('Invalid raw GPU intervals')
            mean_ms = statistics.fmean(samples) / repeats
            operations = 2 * size ** 3 * batches
            row = {'mean_gpu_ms': mean_ms,
                   'p50_gpu_ms': sorted(samples)[math.ceil(len(samples) * 0.5) - 1] / repeats,
                   'p95_gpu_ms': sorted(samples)[math.ceil(len(samples) * 0.95) - 1] / repeats,
                   ('integer_gops' if integer else 'arithmetic_gflops'): operations / (mean_ms * 1e6),
                   'samples': len(samples)}
            grouped.setdefault(result['kernel'], []).append(row)
        if not seen:
            raise ValueError('No kernel results supplied')
    if signature is None:
        raise ValueError('No captures supplied')
    summary = {kernel: {'runs': len(rows), 'mean_gpu_ms': statistics.fmean(r['mean_gpu_ms'] for r in rows),
                        'min_run_gpu_ms': min(r['mean_gpu_ms'] for r in rows),
                        'max_run_gpu_ms': max(r['mean_gpu_ms'] for r in rows), 'per_run': rows}
               for kernel, rows in grouped.items()}
    integer = signature['workload']['input'] == 'INT8'
    reference, matrix = ('shared-tiled-int8-int32', 'cooperative-int8-int32') if integer else (
        'shared-tiled-simd-fp32', 'cooperative-fp16-fp32')
    ratio = summary[reference]['mean_gpu_ms'] / summary[matrix]['mean_gpu_ms'] if reference in summary and matrix in summary else None
    output = {'status': 'passed', 'identity': signature, 'kernels': summary,
            'reference_to_cooperative_time_ratio': ratio,
            'scope': 'Observed compute-kernel implementations with matching arithmetic and inputs, including tiling, memory access and dispatch/barrier overhead; ratios do not isolate instruction throughput. Not peak hardware throughput, XeSS performance or a Heaven scene FPS gain. GPU contention and thermal conditions require separate review.'}
    if integer:
        dot = 'packed-dot4-int8-int32'
        output['reference_to_dp4a_time_ratio'] = summary[reference]['mean_gpu_ms'] / summary[dot]['mean_gpu_ms'] if reference in summary and dot in summary else None
        output['dp4a_to_cooperative_time_ratio'] = summary[dot]['mean_gpu_ms'] / summary[matrix]['mean_gpu_ms'] if dot in summary and matrix in summary else None
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', type=Path, nargs='+')
    args = parser.parse_args()
    try:
        print(json.dumps(analyze([json.loads(path.read_text()) for path in args.captures]), indent=2))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, 'GPU lab analysis: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
