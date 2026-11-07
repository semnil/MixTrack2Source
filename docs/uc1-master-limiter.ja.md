# 使用例 1 — 配信用マスタリミッター

> For the English version, see [uc1-master-limiter.md](uc1-master-limiter.md).

配信に流す前の全体ミックスにリミッターを適用しつつ、ローカル録画には未処理のミックスをそのまま残す。

## ルーティング

```mermaid
sequenceDiagram
    participant M  as OBS ミキサー
    participant MT as Mix Track Source
    participant ST as 配信ストリーム<br>(トラック 6)
    participant RC as 録画ファイル<br>(トラック 1)

    M  ->> MT : トラック 1 を入力
    activate MT
    Note over MT : リミッター<br>(内蔵フィルター)
    MT ->> ST : 
    deactivate MT

    M  -->> RC : 他ソースは トラック 1 のまま録画へ
```

## OBS 設定チェックリスト

| 場所 | 設定 |
|---|---|
| ソースを追加 | **ミックストラック** を追加 → 入力 = **トラック 1** |
| ソース › フィルタ | Mix Track Source に **リミッター** を追加 |
| オーディオの詳細プロパティ（Mix Track Source） | トラック 1 **☐**、トラック 6 **☑** |
| オーディオの詳細プロパティ（その他すべてのソース） | トラック 1 **☑**、トラック 6 **☐** |
| 設定 › 出力 › 配信 | 音声トラック = **6** |
| 設定 › 出力 › 録画 | トラック 1 **☑** |

## 補足

- 配信トラックのみリミットがかかるため、録画と配信で音量特性を独立して管理できる。
- 最小遅延は **1,024 サンプル**（48 kHz で約 21 ms）。許容できない場合は 設定 › 音声 › 詳細設定 の **低遅延音声バッファリングモード (Decklink/NDI 出力用)** を有効にする。
- 録画トラックには未処理のミックスが収録されるため、後からリマスタリングが可能。
