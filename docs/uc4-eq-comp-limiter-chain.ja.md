# 使用例 4 — EQ・コンプレッサー・リミッターのフィルターチェーンと 2 トラック同時録画

> For the English version, see [uc4-eq-comp-limiter-chain.md](uc4-eq-comp-limiter-chain.md).

単一の Mix Track Source にフィルターを直列に積み、EQ → コンプレッサー → リミッターの順で処理する。生音（Track 4）と処理済み音（Track 5）を同時に録画し、編集時にどちらでも使えるようにする。

## ルーティング

```mermaid
sequenceDiagram
    participant M   as OBS ミキサー
    participant MT  as Mix Track Source
    participant RC  as 録画ファイル<br/>(Track 4, Track 5)

    M  -->> RC : 他ソースは Track 4 のまま録画へ（生音）
    M  ->>  MT : Track 4（ゲーム音＋SE）を入力
    activate MT
    Note over MT : EQ<br/>(VST または内蔵)
    deactivate MT
    activate MT
    Note over MT : コンプレッサー<br/>(VST または内蔵)
    deactivate MT
    activate MT
    Note over MT : リミッター<br/>(内蔵)
    MT ->> RC : 処理済み出力
    deactivate MT
```

## フィルターチェーンの順序

OBS のフィルターダイアログではリストの上から下の順に処理される。

| スロット | フィルター | 役割 |
|---|---|---|
| 1 | **EQ**（VST または内蔵） | 周波数バランスを整える |
| 2 | **コンプレッサー**（VST または内蔵） | ダイナミクスを制御する |
| 3 | **リミッター**（内蔵） | クリッピングを防ぐ安全弁として最後に配置 |

## OBS 設定チェックリスト

| 場所 | 設定 |
|---|---|
| ソースを追加 | **Mix Track** を追加 → 入力 = **Track 4** |
| ソース › フィルター | EQ → コンプレッサー → リミッターの順で追加 |
| 詳細音声プロパティ（Mix Track Source） | Track 4 **☐**、Track 5 **☑** |
| 詳細音声プロパティ（ゲーム音ソース） | Track 4 **☑**、Track 5 **☐** |
| 詳細音声プロパティ（その他すべてのソース） | Track 4 **☐**、Track 5 **☐** |
| 設定 › 出力 › 録画 | Track 4 **☑**、Track 5 **☑** |

## 補足

- リミッターを最後に置くことで、EQ やコンプレッサーの調整がどれだけ積極的であってもクリッピングが発生しない。
- Track 4（生音）はポストプロダクションでの再処理に適しており、Track 5（処理済み）はそのまま動画編集に使用できる。
