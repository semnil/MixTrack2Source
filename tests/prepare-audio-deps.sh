#!/usr/bin/env bash
# Extract test dependencies without installing or launching OBS.
set -euo pipefail

destination="${1:?Usage: prepare-audio-deps.sh NEW_DIRECTORY}"
mkdir "$destination"
destination="$(cd "$destination" && pwd)"
scratch="$(mktemp -d)"
mounted=false
cleanup() {
  if "$mounted"; then
    hdiutil detach "$scratch/mount"
  fi
  rm -rf "$scratch"
}
trap cleanup EXIT

case "$(uname -m)" in
  arm64)
    architecture=Apple
    digest=a5dc3507be161becef5cad8112c4acd168cbc37c80fc186b66ed453b7e9f4fc5
    ;;
  x86_64)
    architecture=Intel
    digest=a7dfd507bb04ace769e7a813a97d9f994fd79cdc5507b794b5bf1f32bdcd502e
    ;;
  *) echo 'Only macOS arm64 and x86_64 are supported' >&2; exit 1 ;;
esac

curl --fail --location --max-time 300 \
  "https://github.com/obsproject/obs-studio/releases/download/32.1.0/OBS-Studio-32.1.0-macOS-${architecture}.dmg" \
  --output "$scratch/obs.dmg"
printf '%s  %s\n' "$digest" "$scratch/obs.dmg" | shasum -a 256 --check
hdiutil attach -readonly -nobrowse -mountpoint "$scratch/mount" "$scratch/obs.dmg"
mounted=true
cp -R "$scratch/mount/OBS.app/Contents/Frameworks" "$destination/Frameworks"
hdiutil detach "$scratch/mount"
mounted=false

curl --fail --location --max-time 120 \
  https://github.com/obsproject/obs-studio/archive/5533a277e4400203b50114c993f89878d50a27b9.tar.gz \
  --output "$scratch/obs-source.tar.gz"
mkdir "$destination/obs-studio-32.1.0-sources"
tar -xf "$scratch/obs-source.tar.gz" -C "$destination/obs-studio-32.1.0-sources" --strip-components=1
curl --fail --location --max-time 120 \
  https://github.com/simd-everywhere/simde/archive/refs/tags/v0.8.2.tar.gz \
  --output "$scratch/simde.tar.gz"
tar -xf "$scratch/simde.tar.gz" -C "$destination"
cat > "$destination/obsconfig.h" <<'HEADER'
#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
HEADER
