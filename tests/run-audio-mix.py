#!/usr/bin/env python3
"""Build and run the real-libobs audio regression harness on macOS.

DEPS contains the extracted OBS 32.1.0 Frameworks directory, its matching
obs-studio-32.1.0-sources, simde-0.8.2, and a generated obsconfig.h.
Example: python3 tests/run-audio-mix.py --deps /path/to/deps --results /tmp/results
Use --source /path/to/plugin-main.c to test a baseline or an isolated mutation.
This writes no OBS user profile and opens no microphone or speaker.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CASES = "single multi jitter stalled-source parallel switch chain cycle cycle-live cycle-three cycle-branch monitor-only monitor-own monitor-live monitor-three monitor-branch registry-busy enumeration-update hide mute filter offset input-switch scene-volume global global-first reverse shared-leaf sync-offset fade fade-power opaque opaque-changing late-source controls hybrid-switch hybrid-edge".split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "src/plugin-main.c")
    parser.add_argument("--rate", type=int, choices=(44100, 48000, 96000), default=48000)
    parser.add_argument("--buffer-ms", type=int, choices=(0, 64, 128, 256), default=0,
                        help="Fixed OBS buffering for every case; 0 uses dynamic buffering except late-source")
    parser.add_argument("--cases", nargs="+", choices=CASES, default=CASES)
    parser.add_argument("--check-mutex", action="store_true", help="Check plugin mutex ownership with error-checking mutexes")
    parser.add_argument("--sanitize", action="store_true", help="Enable UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    deps, results = args.deps.resolve(), args.results.resolve()
    results.mkdir(parents=True, exist_ok=False)
    if sys.platform != "darwin":
        parser.error("The checked-in runner currently supports the macOS Frameworks bundle only")
    env = dict(os.environ, LC_ALL="C", LANG="C")
    failures = []
    executions = []
    with tempfile.TemporaryDirectory(prefix="mt2s-audio-") as temporary:
        scratch = Path(temporary)
        executable = scratch / "audio-test"
        command = [
            "/usr/bin/clang", "-std=c11", "-g", "-Wno-deprecated-declarations", "-Werror=format",
            f'-DMT2S_TEST_SOURCE="{args.source.resolve()}"',
            "-I", str(ROOT / "src"), "-I", str(deps),
            "-I", str(deps / "obs-studio-32.1.0-sources/libobs"),
            "-I", str(deps / "simde-0.8.2"),
            "-F", str(deps / "Frameworks"), "-framework", "libobs",
            "-Wl,-rpath," + str(deps / "Frameworks"),
            str(ROOT / "tests/audio-mix.c"), "-o", str(executable),
        ]
        if args.check_mutex:
            command += ["-DMT2S_CHECK_MUTEX"]
        if args.sanitize:
            command += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
        source_files = [args.source.resolve(), Path(__file__).resolve(), ROOT / "tests/audio-mix.c",
                        ROOT / "tests/check-audio-mix.py", ROOT / "tests/check-notification.py"]
        manifest = {"rate": args.rate, "cases": args.cases, "ubsan": args.sanitize, "check_mutex": args.check_mutex,
                    "buffer_ms_by_case": {case: 64 if case == "late-source" and not args.buffer_ms else args.buffer_ms for case in args.cases},
                    "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files}}
        (results / "run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        subprocess.run(command, check=True, timeout=60, env=env)
        (scratch / "work/config").mkdir(parents=True)
        for case in args.cases:
            log = results / f"{case}-{args.rate}.log"
            with log.open("w") as output:
                try:
                    run = subprocess.run(
                        [str(executable), case, str(args.rate), str(deps), str(results), str(args.buffer_ms)],
                        cwd=scratch, env=env, stdout=output, stderr=subprocess.STDOUT,
                        timeout=60 if case == "controls" else 15,
                    )
                    status = run.returncode
                except subprocess.TimeoutExpired:
                    status = "timeout"
            print(case, status, flush=True)
            executions.append({"case": case, "status": status})
            (results / "executions.json").write_text(json.dumps(executions, indent=2) + "\n")
            if status != 0:
                failures.append((case, status))
                break
        if failures:
            print("Execution failed; no overall success verdict:", failures, flush=True)
            return 1
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests/check-audio-mix.py"), str(results), "--rate", str(args.rate)],
            env=env, timeout=30,
        )
        notifications = subprocess.run(
            [sys.executable, str(ROOT / "tests/check-notification.py"), str(results), "--rate", str(args.rate)],
            env=env, timeout=30,
        )
        return 1 if result.returncode or notifications.returncode else 0


if __name__ == "__main__":
    raise SystemExit(main())
