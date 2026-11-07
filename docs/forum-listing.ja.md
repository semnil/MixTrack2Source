# OBS フォーラムのリソース掲載内容

> For the English version, see [forum-listing.md](forum-listing.md).

OBS フォーラムの [Mix Track to Source](https://obsproject.com/forum/resources/mix-track-to-source.2419/) リソースページのうち、[README.ja.md](../README.ja.md) に無い部分の日本語訳。
フォーラムには英語版 ([forum-listing.md](forum-listing.md)) を掲載する。

## 項目

| 項目 | 値 |
| --- | --- |
| タイトル | Mix Track to Source |
| タグ | audio, source |
| 対応ビット数 | 64-bit |
| ソースコードの URL | https://github.com/semnil/MixTrack2Source |
| OBS Studio の最小バージョン | 31.1.1 |
| 対応プラットフォーム | Windows, macOS, Linux |

## 説明文

フォーラムの説明文は、次の部分をこの順に並べたもの:

1. リリース済みの版についての [README.md](../README.md) の最初の節、「Install」、「Usage」。フォーラムでは、構成図は Mermaid のブロックを PNG 画像にしたものを、スクリーンショットは [img](../img) のファイルを添付したものを表示する。リリースでこれらの節が変わったら、リリースを公開した後にフォーラムのページを更新する
2. 以下の節

### AI 利用の開示

プラグインのコード (src/plugin-main.c とロケールファイル) は、バージョン 1.0.1 まで全て AI ツールを使わずに私が設計し書いた。プラグインのコードはそれ以降変わっていない。

バージョン 1.0.2 からは、次の作業に AI ツールを補助として使っている。いずれもプラグインのコードは変えていない。

バージョン 1.0.2 では Claude Code (Anthropic) を使った:

- ビルドとパッケージ: Windows インストーラーと、ビルドスクリプトの修正
- ドキュメント: README の Windows でのインストール手順

1.0.2 より後の変更 (次のリリースに向けたもの) では Claude Code (Anthropic) を使い、OpenAI Codex でレビューしている:

- ビルドと CI: GitHub Actions のワークフロー、リリース手順、ビルドスクリプトの修正
- ドキュメント: README のリリース手順と、README の日本語訳

OpenAI Codex はこれらの変更のレビューにだけ使った。どの変更も Codex は書いていない。

AI の補助を受けたビルド・パッケージ・CI の変更は全て私自身がレビューし、リリース前にそのビルドを OBS Studio で動作確認した。AI の補助を受けたドキュメントの変更も全て私自身がレビューした。

### 不具合の報告

不具合は GitHub Issues へ報告する: https://github.com/semnil/MixTrack2Source/issues
このリソースの議論スレッドへの投稿でも受け付ける。
