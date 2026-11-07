# 使用例 3 — 配信と録画でコンプレッサー設定を分岐

> For the English version, see [uc3-dual-instance-comp.md](uc3-dual-instance-comp.md).

同一のミックストラックを 2 つの Mix Track Source インスタンスで読み込み、それぞれに異なるフィルターチェーンを持たせることで、配信と録画で別々の音声処理を実現する。

## ルーティング

```mermaid
sequenceDiagram
    participant M   as OBS ミキサー
    participant MA  as MT Source A
    participant MB  as MT Source B
    participant ST  as 配信ストリーム<br>(トラック 6)
    participant RC  as 録画ファイル<br>(トラック 5)

    M  ->> MA : トラック 1 を入力
    M  ->> MB : トラック 1 を入力
    Note over MA,MB : 2 インスタンスが同一トラックを並列で読み込む

    activate MA
    Note over MA : コンプレッサー（強）<br>+ ラウドネス補正
    MA ->> ST : 
    deactivate MA

    activate MB
    Note over MB : コンプレッサー（軽）
    MB ->> RC : 
    deactivate MB
```

## OBS 設定チェックリスト

| 場所 | 設定 |
|---|---|
| ソースを追加 | **ミックストラック** を 2 回追加 → それぞれ *MT Source A*、*MT Source B* と命名 |
| MT Source A | 入力 = **トラック 1** |
| MT Source B | 入力 = **トラック 1** |
| MT Source A › フィルタ | コンプレッサー（高レシオ）+ ラウドネス系フィルター |
| MT Source B › フィルタ | コンプレッサー（低レシオ）またはフィルターなし |
| オーディオの詳細プロパティ（MT Source A） | トラック 1 **☐**、トラック 5 **☐**、トラック 6 **☑** |
| オーディオの詳細プロパティ（MT Source B） | トラック 1 **☐**、トラック 5 **☑**、トラック 6 **☐** |
| オーディオの詳細プロパティ（その他すべてのソース） | トラック 1 **☑**、トラック 5 **☐**、トラック 6 **☐** |
| 設定 › 出力 › 配信 | 音声トラック = **6** |
| 設定 › 出力 › 録画 | トラック 5 **☑** |

## 補足

- MT Source A と MT Source B は、トラック 1 へ音声を送る他のソースのトラック 6 とトラック 5 を自動で外す (行うタイミングは [README.ja.md](../README.ja.md) を参照)。そのため、チェックリストのそれらのソースのトラック 5 とトラック 6 は手で外さなくてよい。
- 生音のアーカイブも残したい場合は、録画タブで トラック 1 も ☑ にし、その他のソースが トラック 1 への出力を維持していることを確認する。
- CPU 負荷はインスタンス数に比例するが、軽量なフィルターチェーン 2 本であれば通常は問題ない。
