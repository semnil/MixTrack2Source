# OBS Forum Resource Listing

> 日本語版は [forum-listing.ja.md](forum-listing.ja.md) を参照してください.

This file holds the parts of the [Mix Track to Source](https://obsproject.com/forum/resources/mix-track-to-source.2419/) resource page on the OBS forum that are not in [README.md](../README.md).
Change this file first, then enter the same content in the forum editor.

## Fields

| Field | Value |
| --- | --- |
| Title | Mix Track to Source |
| Tags | audio, source |
| Supported Bit Versions | 64-bit |
| Source Code URL | https://github.com/semnil/MixTrack2Source |
| Minimum OBS Studio Version | 31.1.1 |
| Supported Platforms | Windows, macOS, Linux |

## Description

The description on the forum is made of these parts, in this order:

1. The first section, "Install", and "Usage" of [README.md](../README.md) for the released version. The forum shows the diagram as a PNG image rendered from the Mermaid block, and the screenshots as attachments of the files in [img](../img). When a release changes those sections, update the forum page after the release is published
2. The sections below

### AI usage disclosure

All of the plugin code (src/plugin-main.c and the locale files) was designed and written by me without AI tools, up to and including version 1.0.1. The plugin code has not changed since then.

Starting with version 1.0.2, I have used AI tools as assistants for the following. None of these changes the plugin code.

Version 1.0.2, with Claude Code (Anthropic):

- Build and packaging: the Windows installer and fixes to the build scripts
- Documentation: the Windows installation steps in the README

Changes after 1.0.2 (for the next release), with Claude Code (Anthropic), reviewed with OpenAI Codex:

- Build and CI: the GitHub Actions workflows, the release process, and a fix to the build scripts
- Documentation: the release procedure in the README and a Japanese translation of the README

OpenAI Codex was used only to review these changes. It did not write any of them.

I reviewed all AI-assisted changes to the build, packaging, and CI myself and tested the resulting builds in OBS Studio before releasing them. I reviewed all AI-assisted documentation changes myself.

### Reporting bugs

Please report bugs on GitHub Issues: https://github.com/semnil/MixTrack2Source/issues
You can also post in the discussion thread of this resource.
