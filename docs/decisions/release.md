# Release Decisions

> 日本語版は [release.ja.md](release.ja.md) を参照してください.

This file records closed decisions about the release procedure: what was decided, why, what was rejected, and what would reopen the decision.
The procedure itself is in the "Release" section of [README.md](../../README.md).

## Prereleases are not part of the release procedure

- **Decision** (2026-10-20): The release procedure covers only versions of the form `X.Y.Z`. Prereleases are not part of it
  - The code that treats a tag pushed by hand, such as `1.0.3-rc1`, as a prerelease stays (`.github/scripts/release-tag-shape` and the `create-release` job of `.github/workflows/push.yaml`). The README does not describe it as a procedure
- **Reason**: `cmake/common/bootstrap.cmake` reads the version from `buildspec.json`, and `CMakeLists.txt` passes it to `project(VERSION)`, which accepts only numbers. A pull request that changes the version to `1.0.3-rc1` fails its own build
- **Rejected options**:
  - Let `buildspec.json` hold a prerelease version, and pass only its numeric part to CMake. This changes `cmake/common/bootstrap.cmake`, the versions of the pkg, the installer and CPack, and the artifact paths in `.github/workflows/build-project.yaml`, which makes it a large change
  - Describe a tag `X.Y.Z-rcN` pushed by hand in the README, with restrictions. The packages are named after the version in `buildspec.json`. Before the version is changed, that is the previous version. After it is changed, the release tag of that version already exists. Neither order gives a consistent procedure
- **Conditions for revisiting**:
  - If prereleases are asked for again, the first rejected option is the only consistent one. List every place that consumes the version (CMake, the pkg, the installer, CPack and the artifact paths) before estimating it
  - When the release path changes, trace where the version goes, down to every place that consumes it, before designing the checks
- **Record of what went wrong**:
  - The checks of [#48](https://github.com/semnil/MixTrack2Source/pull/48) (fixture tests and mutations) covered only the behavior of the shell steps. They did not cover a contract that crosses layers: the version reaches `project(VERSION)`. A review just before the merge found this. The README of #48 said that a prerelease version was accepted, which was wrong, and [#49](https://github.com/semnil/MixTrack2Source/pull/49) corrected it
  - Since #48, merging a version change tags the commit and starts the release build, so the state "the version is changed but not released yet" no longer exists. The first description of the options was written without checking this, and the options had to be chosen again
