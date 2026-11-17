#!/usr/bin/env python3
"""Check PCM and callback timestamps from audio-mix.c (no third-party packages).

Usage: python3 tests/check-audio-mix.py RESULTS_DIRECTORY --rate 48000
Require the documented 1,024-sample delay for each serial routing stage.
Notification arrival times are measured separately by check-notification.py.
"""

import argparse
import array
import json
import math
import re
from pathlib import Path


def read_track(directory, case, rate, track):
    stem = directory / f"{case}-{rate}-{track}"
    samples = array.array("f")
    samples.frombytes(stem.with_suffix(".f32").read_bytes())
    timestamps = list(map(int, stem.with_suffix(".timestamps").read_text().splitlines()))
    assert len(samples) == len(timestamps) * 1024
    assert all(math.isfinite(value) for value in samples), "Non-finite audio samples"
    pulses = [
        (timestamps[i // 1024] + round((i % 1024) * 1e9 / rate), value)
        for i, value in enumerate(samples)
        if abs(value) > 0.001
    ]
    positions = [i for i, value in enumerate(samples) if abs(value) > 0.001]
    return pulses, timestamps, positions


def check(directory, case, rate):
    recordings = [read_track(directory, case, rate, n) for n in range(3)]
    tracks = [row[0] for row in recordings]
    expected = [[0.2] * 12, [0.2] * 12, []]
    pairs = [(0, 1)]
    if case in ("multi", "jitter", "late-source", "stalled-source"):
        expected[:2] = [[0.5] * 12] * 2
    elif case in ("chain", "parallel"):
        expected[2] = [0.2] * 12
        pairs.append((0, 2))
    elif case in ("cycle", "cycle-three", "cycle-branch"):
        expected[1:] = [[0.2] * 7] * 2
        pairs.append((0, 2) if case == "cycle-branch" else (1, 2))
    elif case == "cycle-live":
        expected[1:] = [[0.2] * 8] * 2
        pairs.append((1, 2))
    elif case == "switch":
        expected[:2] = [[0.2] * 5 + [0.3] * 7] * 2
    elif case == "registry-busy":
        expected[1] = [0.2] * 8
    elif case in ("hide", "mute"):
        expected[:2] = [[0.2] * 8] * 2
    elif case == "filter":
        expected[:2] = [[0.08] * 12, [0.04] * 12]
    elif case in ("offset", "sync-offset"):
        expected[:2] = [[0.2, 0.3] * 12] * 2
    elif case == "input-switch":
        expected = [[0.2] * 12, [0.3] * 12, [0.2] * 5 + [0.3] * 7]
        pairs = [(0, 2)]
    elif case == "opaque-changing":
        expected[:2] = [[0.05] * 6 + [0.15] * 6] * 2
    elif case == "opaque":
        expected[:2] = [[0.05] * 12] * 2
    elif case == "hybrid-switch":
        expected[:2] = [[0.2] * 5 + [0.05] * 4 + [0.2] * 3] * 2
    elif case == "scene-volume":
        expected[:2] = [[0.1] * 12, [0.05] * 12]

    failures = []
    for n, (actual, want) in enumerate(zip(tracks, expected)):
        if len(actual) != len(want):
            failures.append(f"track {n + 1}: expected {len(want)} pulses, got {len(actual)}")
            continue
        if case == "hybrid-edge" and n < 2:
            # Visibility actions and active-tree changes can overlap at the
            # two edge pulses. Compare the routed value with native output below.
            valid = all(abs(v-0.2) < 1e-5 for _,v in actual[:5] + actual[-2:])
            valid &= all(abs(v-0.05) < 1e-5 for _,v in actual[6:9])
            valid &= all(min(abs(actual[i][1]-v) for v in (0.05,0.2,0.25)) < 1e-5 for i in (5,9))
        elif case in ("fade", "fade-power") and n < 2:
            # Check the native envelope and destination, then compare every
            # routed sample below rather than estimating transition progress.
            upper = math.hypot(0.2, 0.3) if case == "fade-power" else 0.3
            valid = all(0.2 - 1e-5 <= v <= upper + 1e-5 for _, v in actual)
            valid &= all(abs(v - 0.3) < 1e-5 for _, v in actual[-3:])
        else:
            valid = all(abs(v - w) < 1e-5 for (_, v), w in zip(actual, want))
        if not valid:
            failures.append(f"track {n + 1}: amplitude mismatch")

    if case in ("fade", "fade-power", "hybrid-edge", "hybrid-switch") and len(tracks[0]) == len(tracks[1]):
        if any(abs(a[1] - b[1]) > 1e-5 for a, b in zip(tracks[0], tracks[1])):
            failures.append("track 2: processed samples differ from native amplitudes")

    # Reference the generated source timestamps as well as the other output
    # track: equal queue slips on both tracks must not look like a pass.
    native_input_lag_ms = []
    source_log = directory / f"{case}-{rate}.log"
    input_times = []
    for line in source_log.read_text().splitlines():
        if not line.startswith("ARRIVAL source=input-"):
            continue
        fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
        offset = 5_000_000 if case == "sync-offset" and fields["source"] == "input-b" else 0
        input_times.append(int(fields["timestamp"]) + round(int(fields["frame"]) * 1e9 / rate) + offset)
    for timestamp, _ in tracks[0]:
        closest = min(input_times, key=lambda value: abs(timestamp - value))
        native_input_lag_ms.append((timestamp - closest) / 1e6)
    if native_input_lag_ms and max(map(abs, native_input_lag_ms)) > 1000 / rate:
        failures.append("track 1: native PCM moved from the input sample timeline")

    waveform_failures = list(failures)
    lags = {}
    measurement_errors = {}
    for a, b in pairs:
        left, right = tracks[a], tracks[b]
        selection = None
        if case in ("cycle", "cycle-three", "cycle-branch") and a == 0:
            selection = list(range(5, 12))
        elif case in ("cycle-live", "registry-busy") and a == 0:
            selection = list(range(5)) + list(range(9, 12))
        if selection is not None and len(left) == 12:
            left = [left[i] for i in selection]
        if len(left) != len(right) or not right:
            failures.append(f"track {a + 1}->{b + 1}: missing matching pulses")
            continue
        delays = [(y[0] - x[0]) / 1e6 for x, y in zip(left, right)]
        # Independently count PCM sample positions after aligning the callback
        # streams at a shared output timestamp. Registration can differ by a block.
        _, times_a, positions_a = recordings[a]
        _, times_b, positions_b = recordings[b]
        common = sorted(set(times_a) & set(times_b))
        assert common, "No shared output block for PCM alignment"
        origin_a, origin_b = times_a.index(common[0]) * 1024, times_b.index(common[0]) * 1024
        if selection is not None and len(positions_a) == 12:
            positions_a = [positions_a[i] for i in selection]
        pcm_delays = [
            ((y - origin_b) - (x - origin_a)) * 1000 / rate
            for x, y in zip(positions_a, positions_b)
        ]
        error = max(abs(x - y) for x, y in zip(delays, pcm_delays))
        measurement_errors[f"{a + 1}->{b + 1}"] = error
        if error > 1000 / rate:
            failures.append(f"track {a + 1}->{b + 1}: PCM and timestamp measurements disagree")
        lags[f"{a + 1}->{b + 1}"] = [min(delays), max(delays)]
        if case in ("hybrid-switch", "hybrid-edge") and max(delays)-min(delays) > 1000/rate:
            message = "route timing changes across the processing-path switch"
            failures.append(message)
            waveform_failures.append(message)
        stages = 2 if case == "chain" and (a, b) == (0, 2) else 1
        expected_ms = stages * 1024 * 1000 / rate
        if any(abs(delay - expected_ms) > 1000 / rate for delay in delays):
            failures.append(f"track {a + 1}->{b + 1}: expected {stages * 1024} samples of delay")

    return {
        "case": case,
        "rate": rate,
        "pulses": [len(row) for row in tracks],
        "native_input_lag_ms": native_input_lag_ms,
        "amplitudes": [[round(v, 7) for _, v in row] for row in tracks],
        "lag_ms": lags,
        "pcm_timestamp_disagreement_ms": measurement_errors,
        "waveform_pass": not waveform_failures,
        "waveform_failures": waveform_failures,
        "failures": failures,
        "pass": not failures,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--rate", type=int, default=48000)
    args = parser.parse_args()
    cases = sorted(p.name.removesuffix(f"-{args.rate}-0.f32") for p in args.directory.glob(f"*-{args.rate}-0.f32"))
    assert cases, "No recordings found"
    results = [check(args.directory, case, args.rate) for case in cases]
    (args.directory / f"results-{args.rate}.json").write_text(json.dumps(results, indent=2) + "\n")
    for row in results:
        print(("PASS" if row["pass"] else "FAIL"), row["case"],
              "waveform=" + ("PASS" if row["waveform_pass"] else "FAIL"),
              row["lag_ms"], "; ".join(row["failures"]))
    raise SystemExit(0 if all(row["pass"] for row in results) else 1)
