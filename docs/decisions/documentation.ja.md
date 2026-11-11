# ドキュメントの決定

> For the English version, see [documentation.md](documentation.md).

このリポジトリの文書について閉じた判断の記録。何を決めたか、理由、どう当てるかを書く。

## このリポジトリは CLAUDE.md を持たず、issue テンプレートは 2 つの言語のままにする

- **決定** (2026-10-20): このリポジトリは `CLAUDE.md` を持たない。issue テンプレート (`.github/ISSUE_TEMPLATE/bug_report.md` と `.github/ISSUE_TEMPLATE/feature_request.md`) の見出しは、英語と日本語を並べたままにする
  - 2 つの変更を [#50](https://github.com/semnil/MixTrack2Source/pull/50) にコミットし、マージの前に取り除いた。1 つは、文書の言語について 1 行だけを書いた `CLAUDE.md` (`.gitignore` の `!CLAUDE.md` つき)。もう 1 つは、issue テンプレートを英語だけに書き換えて、日本語でも構わないという注記を置く変更
- **理由**: メンテナーが、この 2 つの変更を取り除かせた。理由は記録に無い
- **当て方**: `CLAUDE.md` を足さない。issue テンプレートの見出しを書き換えない。一般の規約がどちらかを求める場面では、行わずに、この決定があることを報告する
