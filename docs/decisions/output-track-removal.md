# Output Track Removal Decisions

> 日本語版は [output-track-removal.ja.md](output-track-removal.ja.md) を参照してください.

This file records closed decisions about removing the audio source's output tracks from the other audio sources: what was decided, why, what was rejected, and what would reopen the decision.
The behavior itself is in [README.md](../../README.md) and in the comments of `src/plugin-main.c`.

## Waiting tracks are dropped on the frontend's scene collection events

When the last audio source that needs a track removed lets go of it while the source it was removed from is gone, the track waits for a source with the same uuid, which is what undo brings back. This section is about when such a waiting track is dropped. [#63](https://github.com/semnil/MixTrack2Source/pull/63) added it.

- **Decision** (2026-10-25): Waiting tracks are dropped when the frontend cleans up a scene collection (`OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP`). No track starts waiting until the next collection has loaded (`OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED`). A waiting track applies once
- **Reason**: An earlier version dropped the waiting tracks when output channel 0 became empty. The tests of the auto-configuration wizard also empty channels 0 to 5 and then put the same sources back, so that version dropped the tracks that were waiting for an undo
- **Rejected options and history**:
  - Tell the two cases apart by whether the source of channel 0 comes back as the same one. This relies on the frontend creating a new transition each time it loads a collection, and it does not tie the drop to a notification that the collection is ending
  - Link obs-frontend-api. At the time of the decision this was rejected, because a libobs host without the OBS frontend could then not load the plugin. The plugin looked up `obs_frontend_add_event_callback` and `obs_frontend_remove_event_callback` once at module load instead. [#68](https://github.com/semnil/MixTrack2Source/pull/68) added the update check, which needs obs-frontend-api, and turned `ENABLE_FRONTEND_API` on in the CMake presets. Builds from the presets now link obs-frontend-api, and the lookup remains for builds without `ENABLE_FRONTEND_API`
- **Conditions for revisiting**: Builds without `ENABLE_FRONTEND_API` use the event numbers written in `src/plugin-main.c` (`SCENE_COLLECTION_CHANGED` 13, `EXIT` 17, `SCENE_COLLECTION_CLEANUP` 25). When a version of OBS changes `enum obs_frontend_event` in `obs-frontend-api.h`, compare the numbers with it again
