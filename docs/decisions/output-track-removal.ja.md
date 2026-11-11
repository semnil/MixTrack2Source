# 出力トラックを外す機能の決定

> For the English version, see [output-track-removal.md](output-track-removal.md).

音声ソースの出力トラックを、他の音声ソースから外す機能について閉じた判断の記録。何を決めたか、理由、却下した案、決定を開け直す条件を書く。
挙動そのものは [README.ja.md](../../README.ja.md) と、`src/plugin-main.c` のコメントにある。

## 復元待ちのトラックは frontend のシーンコレクションのイベントで捨てる

あるトラックを外しておく必要のある最後の音声ソースがそれを手放したときに、外された側のソースが無くなっていると、そのトラックは同じ uuid のソースを待つ。元に戻す操作 (undo) が連れ戻すのが、その同じ uuid のソースである。この節は、その待っているトラックをいつ捨てるかについて書く。この仕組みは [#63](https://github.com/semnil/MixTrack2Source/pull/63) で入った。

- **決定** (2026-10-25): 待っているトラックは、frontend がシーンコレクションを片付けるとき (`OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP`) に捨てる。次のコレクションが読み込まれる (`OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED`) まで、新しい待ちは作らない。待っているトラックが効くのは 1 回だけ
- **理由**: 以前の版は、出力チャンネル 0 が空になったときに、待っているトラックを捨てていた。自動構成ウィザードのテストも、チャンネル 0 から 5 を空にして、同じソースを戻す。そのため以前の版は、undo を待っているトラックを捨てていた
- **却下した案と経緯**:
  - チャンネル 0 のソースが同じものに戻るかどうかで、2 つの場合を見分ける。frontend がコレクションを読み込むたびに新しいトランジションを作ることに頼っていて、捨てる契機を「コレクションが終わる」という通知に結び付けていない
  - obs-frontend-api をリンクする。決定の時点では却下した。OBS の frontend を持たない libobs のホストが、プラグインを読み込めなくなるため。代わりに、プラグインはモジュールの読み込みのときに 1 回だけ、`obs_frontend_add_event_callback` と `obs_frontend_remove_event_callback` を探していた。[#68](https://github.com/semnil/MixTrack2Source/pull/68) が、obs-frontend-api を必要とする更新の確認を足し、CMake のプリセットで `ENABLE_FRONTEND_API` をオンにした。今は、プリセットからのビルドは obs-frontend-api をリンクし、探す経路は `ENABLE_FRONTEND_API` の無いビルドのために残っている
- **開け直す条件**: `ENABLE_FRONTEND_API` の無いビルドは、`src/plugin-main.c` に書いたイベントの番号 (`SCENE_COLLECTION_CHANGED` 13、`EXIT` 17、`SCENE_COLLECTION_CLEANUP` 25) を使う。OBS の版が `obs-frontend-api.h` の `enum obs_frontend_event` を変えたら、番号を照らし合わせ直す
