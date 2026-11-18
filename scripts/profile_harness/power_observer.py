"""Validation for explicitly declared read-only Intel power observations.

Polling measurably affects this host. It is never enabled by default and does
not establish exclusive GPU activity, CPU/GPU clock alignment or a root cause.
"""
import math


def validate_csv(text, minimum_span_seconds):
    if len(text) > 1024 * 1024 or not math.isfinite(minimum_span_seconds) or minimum_span_seconds <= 0:
        raise ValueError('invalid observer coverage bound')
    ticks = []
    for line in text.splitlines():
        if not line.startswith('[') or '] ' not in line:
            raise ValueError('missing observer timestamp')
        stamp, values = line[1:].split('] ', 1)
        fields = values.split(', ')
        if fields[0] == 'power.draw (W)':
            continue
        if len(fields) != 5:
            raise ValueError('incomplete observer row')
        hour, minute, second = map(float, stamp.split(':'))
        if not (hour.is_integer() and minute.is_integer() and 0 <= hour < 24 and 0 <= minute < 60 and 0 <= second < 60):
            raise ValueError('invalid observer timestamp')
        numbers = [float(fields[i]) for i in (0, 1, 2, 4)]
        if not all(math.isfinite(v) and v >= 0 for v in numbers):
            raise ValueError('unavailable/invalid observer metric')
        if not fields[3]:
            raise ValueError('missing throttle availability')
        ticks.append(hour * 3600 + minute * 60 + second)
    if len(ticks) < 100:
        raise ValueError('insufficient observer samples')
    gaps = [(b - a) % 86400 for a, b in zip(ticks, ticks[1:])]
    if not all(0 < delta < 5 for delta in gaps) or sum(gaps) < minimum_span_seconds:
        raise ValueError('observer gaps or incomplete coverage')
    return {'rows': len(ticks), 'span_seconds': sum(gaps), 'maximum_sample_gap_seconds': max(gaps)}
