# Audio regression tests

[日本語](README.ja.md)

The harness runs synthetic audio through the plugin and a real libobs instance.
It creates temporary configuration and an offscreen graphics context without
launching the OBS application or opening microphones or speakers.

On macOS with Xcode Command Line Tools and Python 3.9 or later, run from the repository root:

```sh
mkdir -p work
bash tests/prepare-audio-deps.sh work/audio-deps
python3 tests/run-audio-mix.py --deps work/audio-deps --results work/audio-results --sanitize --check-mutex
```

Dependency preparation downloads OBS 32.1.0, its matching source headers, and SIMDe 0.8.2.
Use a new directory for each result set. The runner records input hashes, process
statuses, audio samples, timestamps, notification arrivals, and JSON assessments.

The checks compare pulse counts, amplitudes, source timestamps, and independently
aligned PCM sample positions. Each serial routing stage must add exactly one
1,024-sample block within a one-sample measurement tolerance. Feedback loops must
produce silence at participating instances and resume after the loop is broken.
The registry-contention case holds the instance list busy and verifies that audio
callbacks keep delivering timestamped silence instead of waiting. The enumeration
case changes the input track while OBS holds its source-list lock.
Monitor-only cases check source capture callbacks and final track output, including
switches back to monitoring with output, three-instance routes, and branched routes.
An unavailable monitor device prevents speaker playback; physical monitoring is not tested.
The `late-source` case delays one producer by 20 ms and uses 64 ms of fixed OBS
buffering so its input timestamps can be checked strictly. Other cases use the
default dynamic buffering. `--buffer-ms 64` selects the same fixed buffer for all
cases; CI uses this explicit configuration. Pulse counts, amplitudes, input
timestamps, and per-stage delay keep the same acceptance limits in both modes.
These are test configurations, not plugin settings.

Notification arrival differences are reported separately. They have no upper
acceptance limit in the routing regression test. To evaluate the unfulfilled
16 ms target from issue #7 explicitly:

```sh
python3 tests/check-notification.py work/audio-results --limit-ms 16
```

That command fails when the target is exceeded; passing the routing regression
does not mean issue #7 is resolved. `--source PATH` tests another plugin revision
with the same harness. `--cases` selects named scenarios, and `--rate` selects
44.1, 48, or 96 kHz (48 kHz by default).

Maintainers run the harness when changing audio routing. The `Audio regression`
GitHub Actions workflow runs it on macOS for pull requests and default-branch
pushes affecting `src`, `tests`, or the workflow itself.
