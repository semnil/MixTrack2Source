#!/usr/bin/env python3
"""Compare source-capture arrival times, separately from final PCM alignment."""
import argparse
import json
import re
import statistics
from pathlib import Path


def check(directory, waveform, rate, limit_ms=None):
    case = waveform['case']
    events = []
    for line in (directory / f'{case}-{rate}.log').read_text().splitlines():
        if not line.startswith('ARRIVAL '):
            continue
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        events.append(dict(source=fields['source'], wall=int(fields['wall_ns']),
                           time=int(fields['timestamp']) + round(int(fields['frame']) * 1e9 / rate),
                           value=float(fields['value'])))
    sources = {name: [e for e in events if e['source'] == name]
               for name in ('input-a', 'input-b', 'route-a', 'route-b')}
    errors = []
    for name in ('input-a', 'input-b'):
        if len(sources[name]) != 12:
            errors.append(f'{name}: expected 12 stimulus notifications, got {len(sources[name])}')
    expected_a = 7 if case in ('cycle', 'cycle-three', 'cycle-branch') else 8 if case in ('cycle-live', 'hide', 'mute', 'registry-busy') else 24 if case in ('offset', 'sync-offset') else 12
    expected_b = expected_a if case in ('chain', 'parallel', 'cycle', 'cycle-live', 'cycle-three', 'cycle-branch') else 0
    for name, count in [('route-a', expected_a), ('route-b', expected_b)]:
        if len(sources[name]) != count:
            errors.append(f'{name}: expected {count} notification pulses, got {len(sources[name])}')
    measured = {}
    for target in ('route-a', 'route-b'):
        for event in sources[target]:
            candidates = sources['input-a']
            if not candidates:
                continue
            ordinal = min(range(len(candidates)), key=lambda n: abs(candidates[n]['time'] - event['time']))
            names = ['input-a']
            if case in ('multi', 'jitter', 'late-source', 'stalled-source'):
                names = ['input-a', 'input-b']
            elif case in ('offset', 'sync-offset'):
                names = ['input-a' if abs(event['value'] - 0.2) < 0.01 else 'input-b']
            elif case in ('switch', 'input-switch'):
                names = ['input-a' if ordinal < 5 else 'input-b']
            elif case in ('fade', 'fade-power'):
                names = ['input-a'] if ordinal < 5 else ['input-b'] if ordinal >= 9 else ['input-a', 'input-b']
            for name in names:
                if ordinal >= len(sources[name]):
                    continue
                original = sources[name][ordinal]
                # Pulses are 250 ms apart. Require the unique nearest pulse,
                # within half that spacing; do not match solely by wall time.
                if abs(original['time'] - event['time']) >= 125_000_000:
                    errors.append(f'{name}->{target}: no matching stimulus timestamp')
                    continue
                delta = (event['wall'] - original['wall']) / 1e6
                measured.setdefault(f'{name}->{target}', []).append(delta)
                if delta < 0 or (limit_ms is not None and delta > limit_ms):
                    errors.append(f'{name}->{target}: notification arrival outside the requested limit')
    if not sources['route-a']:
        errors.append('route-a: no notification pulses')
    if case in ('chain', 'parallel', 'cycle', 'cycle-live', 'cycle-three', 'cycle-branch') and not sources['route-b']:
        errors.append('route-b: no notification pulses')
    ranges = {name: {'count': len(v), 'min_ms': min(v), 'median_ms': statistics.median(v), 'max_ms': max(v)}
              for name, v in measured.items()}
    return dict(case=case, notification_counts={n: len(v) for n, v in sources.items()},
                notification_ranges=ranges, notification_failures=sorted(set(errors)),
                waveform_pass=waveform['waveform_pass'], waveform_failures=waveform['waveform_failures'],
                final_track_lag_ms=waveform['lag_ms'],
                pass_=not errors and waveform['pass'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--rate', type=int, default=48000)
    parser.add_argument('--limit-ms', type=float, help='Optional notification limit; use 16 to reproduce issue #7')
    args = parser.parse_args()
    waves = json.loads((args.directory / f'results-{args.rate}.json').read_text())
    results = [check(args.directory, w, args.rate, args.limit_ms) for w in waves]
    (args.directory / f'notifications-{args.rate}.json').write_text(json.dumps(results, indent=2) + '\n')
    for r in results:
        print('PASS' if r['pass_'] else 'FAIL', r['case'], r['notification_ranges'],
              '; '.join(r['notification_failures'] + r['waveform_failures']))
    raise SystemExit(0 if all(r['pass_'] for r in results) else 1)
