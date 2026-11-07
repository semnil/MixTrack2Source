# Use Case 5 — Parallel Reverb (Singing Stream, Live on/off)

> 日本語版は [uc5-parallel-reverb.ja.md](uc5-parallel-reverb.ja.md) を参照してください.

Read the microphone track with two Mix Track Source instances — one carrying the dry signal
unmodified, the other carrying a wet-only reverb signal. The karaoke BGM is routed directly
to the stream track from a separate track, keeping it free of any reverb processing. Muting
MT Source B removes the vocal reverb instantly, making this well suited to singing streams
where reverb needs to be toggled per song.

## Track Assignment

| Track | Content |
|---|---|
| Track 2 | Microphone (vocal) only |
| Track 3 | Karaoke BGM only |
| Track 6 | Stream output (vocal dry + wet sum + BGM) |

## Routing

```mermaid
sequenceDiagram
    participant M   as OBS Mixer
    participant MA  as MT Source A (dry)
    participant MB  as MT Source B (wet)
    participant BG  as BGM Source
    participant ST  as Live Stream<br/>(Track 6)

    M  ->> MA : input: Track 2 (mic)
    M  ->> MB : input: Track 2 (mic)
    Note over MA,MB : two instances read the same track in parallel

    MA ->> ST : no filter (dry signal)

    activate MB
    Note over MB : Reverb VST<br/>(wet 100% / dry 0%)
    MB ->> ST : wet signal
    deactivate MB

    Note over ST : dry and wet signals are summed

    activate BG
    Note over BG : Delay Compensation<br/>(built-in filter)
    BG ->> ST : BGM (Track 3 → Track 6 direct)
    deactivate BG
```

## on/off Toggle

| Action | Result |
|---|---|
| **Mute** MT Source B | Dry vocal only (reverb off) |
| **Unmute** MT Source B | Dry + wet vocal (reverb on) |

The mute button is accessible at any time in the Audio Mixer panel. Keeping MT Source A active
at all times ensures the dry vocal is never interrupted when toggling reverb off. The BGM is
unaffected in either state.

## BGM Delay Compensation

A reverb VST introduces processing latency of a few to several tens of milliseconds due to its
internal buffering. MT Source A (dry) and MT Source B (wet) share the same input bus and remain
in sync with each other, but the BGM source bypasses the reverb entirely and therefore arrives
at the stream earlier by the VST's processing delay. Without compensation, the vocal onset will
sound late relative to the BGM.

Add a built-in **Delay** filter to the BGM source and set the delay value (ms) to match the
VST's processing latency.

**Measuring the delay:**

1. Feed a single test impulse (e.g. a click) through the microphone.
2. Monitor or record the output and observe the timing offset between the dry vocal signal and
   the BGM.
3. Set that difference (ms) as the delay value on the BGM source's Delay filter.

Some reverb plug-ins publish their processing latency in the documentation — consult the
plug-in's manual as a starting point before fine-tuning by ear.

## OBS Settings Checklist

| Location | Setting |
|---|---|
| Add Source | Add **Mix Track** twice — name them *MT Source A* and *MT Source B* |
| MT Source A | Input = **Track 2** |
| MT Source B | Input = **Track 2** |
| MT Source A › Filters | None |
| MT Source B › Filters | Add a **Reverb VST** → set dry = **0%**, wet = **100%** |
| BGM Source › Filters | Add **Delay** → set delay (ms) = VST processing latency |
| Advanced Audio Properties (MT Source A) | Track 2 **☐**, Track 6 **☑** |
| Advanced Audio Properties (MT Source B) | Track 2 **☐**, Track 6 **☑** |
| Advanced Audio Properties (microphone source) | Track 2 **☑**, Track 6 **☐** |
| Advanced Audio Properties (BGM source) | Track 3 **☑**, Track 6 **☑** |
| Advanced Audio Properties (all other sources) | Track 2 **☐**, Track 3 **☐**, Track 6 **☐** |
| MT Source A volume fader | Adjust dry vocal level |
| MT Source B volume fader | Adjust wet level (reverb send amount) |
| Settings › Output › Streaming | Audio Track = **6** |

## Notes

- Enabling Track 6 directly on the BGM source routes it to the stream without passing through
  any reverb processing, keeping the backing track clean.
- The dry/wet balance is controlled independently by each source's volume fader. Set the wet
  level in advance for each song and simply mute/unmute to toggle during the stream.
- This use case requires a reverb VST that can output wet signal only (wet 100% / dry 0%).
  A plug-in that cannot isolate the wet output will double the dry signal and is not suitable.
- To also capture the blend in the recording, enable Track 5 on MT Source A, MT Source B, and
  the BGM source in Advanced Audio Properties, and enable Track 5 in Settings › Output ›
  Recording.
