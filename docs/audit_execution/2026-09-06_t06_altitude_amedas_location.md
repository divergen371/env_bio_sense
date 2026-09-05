# T06 BMP581高度・AMeDAS・現在地 実装記録

日付: 2026-09-06  
対象: P1-07、P1-08、P1-09、P2-07

## 結論

高度式そのものを変更する必要がある障害ではなく、式へ渡す海面更正気圧、位置、温度、校正値の品質・鮮度・所有権と、表示値の扱いが主因だった。これらを明示的な状態と出典を持つデータとして扱うよう修正した。コードとnative試験は完了し、実機・実データによるGateを残す。

## 実装内容

- `LocationService`を追加し、位置をGNSS Live、LastKnown、明示設定したfallback、Unavailableの順で選択する。
- GNSS Live採用条件をage 5秒以内、衛星4個以上、HDOP 5以下、100 m以内の3連続fixとした。位置喪失または100 m超の飛び後は再確認する。
- LastKnownの有効期間を24時間とし、設定座標fallbackは`LOCATION_FALLBACK_ENABLED=1`の場合だけ使用する。GPS導入前の固定座標が暗黙に復活する経路を除去した。
- 選択位置が1 km以上移動した場合、AMeDAS観測所を再選択する。
- AMeDASは観測時刻15分以内、品質値0、3地点以上を必須とし、実距離kmの二乗逆距離加重で補間する。
- 気圧場をValid、LastKnown、StaticFallback、Invalidで管理する。15分超でLastKnown、60分超でInvalidへ降格し、Invalid時の高度を有効扱いしない。
- HTTPSは`setInsecure()`を廃止し、GlobalSign Root R46を固定して証明書検証を有効にした。端末時刻が未確定の間は取得しない。
- AMeDASの採否、観測時刻、位置源、各観測所の距離・気圧・品質・採用有無を`/amedas_pressure_v1.csv`へ保存し、追記後に再openして内容を照合する。
- GNSS高度から海面更正気圧を連続逆算する経路を削除した。P0の所有者をWeatherServiceへ集約した。
- 高度計算温度は有効なSHT45値を優先し、利用不能時だけBMP581内部温度へfallbackする。APIとログへ温度源を出す。
- raw高度とdisplay高度を分離し、P0、P0状態、校正offsetの変更時に表示ヒステリシスを初期化する。P0へのlow-passは行わない。
- BMP581校正は固定標高13.6 m、60秒settle、5分間10 Hz、80%以上の有効sample、両端5%除外、offset絶対値5 hPa以下を条件にした。
- 候補offset適用後の平均を13.6 m±0.3 m、標準偏差を0.5 m以下とし、FRAMへ保存・読戻し一致した場合だけ適用する。失敗時は従来offsetを保持する。
- 校正の入力・統計・成否・永続化結果を`/bmp581_calibration_v1.csv`へ保存し、成功・失敗要約をFRAM compact eventにも残す。
- `/api/location`、`/api/weather`、`/api/altitude`とWeb表示へ、位置源、P0状態・age、観測所、raw/display高度、計算温度源、校正状態を公開した。

## 自動検証

- 高度・AMeDAS・校正native試験: 11件PASS
- 位置選択native試験: 7件PASS
- 合計: 18件PASS
- firmware build: PASS
- RAM: 93,056 / 327,680 bytes（28.4%）
- Flash: 1,279,785 / 3,342,336 bytes（38.3%）
- `git diff --check`: PASS
- `setInsecure()`、旧固定座標、GNSS P0連続逆算、`offset != 0`による成功推定が実行コードから除去されていることを確認した。

## 実機Gate

1. `/api/location`で3連続fix後にLiveへ遷移し、fix喪失後はLastKnownへ遷移することを確認する。
2. 1 km以上の移動でAMeDAS観測所が再選択されることを確認する。
3. 実JMA接続でTLS検証が成功し、`/api/weather`と`/amedas_pressure_v1.csv`の観測時刻・品質・使用地点・補間値が一致することを確認する。
4. 古い観測値または取得停止でValidからLastKnown、Invalidへ降格し、Invalid時は高度validがfalseになることを確認する。
5. 既知標高13.6 mで約6分の校正を行い、`/api/altitude`、FRAM event、`/bmp581_calibration_v1.csv`の結果を照合する。
6. 再起動後に同じoffsetが読戻されること、および失敗校正が以前のoffsetを破壊しないことを確認する。
7. 校正後5分のraw高度が平均13.6±0.3 m、標準偏差0.5 m以下であることを確認する。
8. 別途30分の実環境記録で平均13.6±3 m、かつ入力変更で説明できない5秒ジャンプがないことを確認する。

## 残余リスク

- 詳細AMeDAS・校正ログはSD unavailable時に保存できない。要約eventと校正offsetはFRAMへ残るが、T09でSD障害時の観測可能性を確認する。
- 証明書rootは期限・JMA側chain変更時にfirmware更新が必要になる。接続失敗をP0の品質低下として扱う動作をT09で確認する。
- Webに残る旧手動海面気圧処理とソース内HTML/CSS/JSはT08で整理する。
