# Documentation Decisions

> 日本語版は [documentation.ja.md](documentation.ja.md) を参照してください.

This file records closed decisions about the documents of this repository: what was decided, why, and how to apply it.

## The repository has no CLAUDE.md, and the issue templates keep both languages

- **Decision** (2026-10-20): This repository does not have a `CLAUDE.md`, and the headings of the issue templates (`.github/ISSUE_TEMPLATE/bug_report.md` and `.github/ISSUE_TEMPLATE/feature_request.md`) keep English and Japanese side by side
  - Two changes were committed to [#50](https://github.com/semnil/MixTrack2Source/pull/50) and taken out of it before the merge: a `CLAUDE.md` holding one line about the language of the documents (with `!CLAUDE.md` in `.gitignore`), and issue templates rewritten in English only, with a note that Japanese is welcome
- **Reason**: The maintainer had the two changes taken out. No reason is on record
- **How to apply**: Do not add a `CLAUDE.md`, and do not rewrite the headings of the issue templates. Where a general convention asks for either, leave it undone and report this decision
