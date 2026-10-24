# Mix Track to Source

> 日本語版は [README.ja.md](README.ja.md) を参照してください.

```mermaid
flowchart TB
    subgraph OBS Mix tracks
        T1["Any Track
        from 1 to 6"]
        T2@{ shape: procs, label: "Output Tracks" }
    end

    subgraph Mix Track to Source Plugin
        subgraph P1["mt2s_callback()"]
            P2["obs_source_output_audio()"]
        end

        subgraph OBS Audio Mixer
            S["Source"]
        end
        P2-->S
    end
    T1-->P1
    S-->T2
    S-.-x|"To avoid loopback,
    the input track
    cannot be output"|T1
```

- Select an input mix track to create an audio source
- The audio source uses the selected mix track as input and allows various filters to be applied
- Allows selection of mix tracks as output, excluding the mix track selected as input
- Removes the audio source's output tracks from the other audio sources that send audio to its input track, so their audio is not output twice (other Mix Track sources are left alone)
  - The removed tracks are turned back on when the audio source is removed or its input or output tracks change, unless another Mix Track source still needs them removed
  - A track the user turns back on is left as it is from then on
  - The record of removed tracks is saved with the scene collection
- Buffering causes a minimum delay of 1024 samples (21 ms at 48 kHz)
  - If even greater delay is unacceptable, enable **Low Latency Audio Buffering Mode (For Decklink/NDI outputs)** in OBS Settings > Audio > Advanced

## Install

Please download the archive file from the [Releases](https://github.com/semnil/MixTrack2Source/releases) page.

### Windows

Run the `mix-track-to-source-<version>-windows-x64-Installer.exe` file to install it.

Alternatively, extract the archive file and place the `mix-track-to-source` folder in the following location:

```
C:\ProgramData\obs-studio\plugins
```


### macOS

Run the `mix-track-to-source-<version>-macos-universal.pkg` file to install it.


### Ubuntu

Run the following command:

```
sudo dpkg -i mix-track-to-source-<version>-x86_64-linux-gnu.deb
```


## Usage

- `Add Source` > `Mix Track` to add an audio source
- Select a mix track to use for input from `Track 1` to `Track 6`
- Select outputs for added audio track in the Advanced Audio Properties window
  - Cannot select the same track for both input and output
  - The output tracks are removed automatically from the other audio sources that send audio to the input track
    - A source that starts sending audio to the input track later keeps them until the input or output tracks of this audio source change or the scene collection is loaded again; deselect them by hand if needed

### Usage example

After applying a limiter to the input on Track 1, the settings for outputting the audio to Track 6 for distribution are as follows:

![](img/Properties_for_Mix_Track.png)
![](img/Filters_for_Mix_Track.png)
![](img/Advanced_Audio_Properties.png)
![](img/Settings_Output.png)

- When adding an audio source, set the input to Track 1
- Add a limiter to the filter of that audio source
- Enable the output of Track 6 for that audio source; the output to Track 6 is then disabled automatically for the other audio sources that send audio to Track 1
- Select Track 6 in Settings > Output > Streaming > Streaming Settings > Audio Track


## Information for development

Please refer to the template repository information.  
https://github.com/obsproject/obs-plugintemplate

### Release

The version is defined only by `"version"` in `buildspec.json`, and the release tag is that version as it is (for example `1.0.3`, with no leading `v`).
To release, merge a pull request that changes nothing but that version:

1. When the `Push` workflow finishes successfully for the merge commit on `master`, the `Tag Release` workflow tags that commit with the version and runs the `Push` workflow on the tag
2. The run on the tag builds the release packages and creates a draft release
3. Review the draft on the [Releases](https://github.com/semnil/MixTrack2Source/releases) page and publish it

- The version takes the form `X.Y.Z` (for example `1.0.3`). With any other form, either the build fails (CMake accepts only numbers in the version, so `1.0.3-rc1` is rejected) or `Tag Release` fails without pushing a tag
- A push to `master` that does not change the version does nothing. The commit compared with its first parent is the merge commit, or the last commit of a rebase merge
- A tag that already exists on the remote is never moved. If it names another commit, `Tag Release` fails
- A tag pushed by hand runs the same release path
