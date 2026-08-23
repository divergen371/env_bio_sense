# SCD41 校正ロジック見直し 作業指示書

## 1. 目的

`env_bio_sense` の SCD41 校正・補償ロジックを見直し、以下を達成すること。

- SCD41 の通常運用を **ASC（Automatic Self-Calibration）中心**へ戻す。
- FRC（Forced Recalibration）を「定期メンテナンス」ではなく、**外部の既知 CO₂ 参照値がある場合だけ実行する保守機能**として扱う。
- 「SCD41 自身が表示した値を、そのまま FRC の参照値として返す」運用を禁止する。
- FRC 実行時の測定状態、気圧補償、補正量、失敗理由を追跡可能にする。
- BMP581 から SCD41 へ渡している気圧補償経路を明示し、単位・鮮度・意味を保証する。
- 今回観測した以下のような異常を再発させない。

  - 外気で約 390 ppm を表示
  - 390 ppm を FRC 参照値として入力
  - FRC 後に約 320 ppm まで低下
  - 400 ppm で再度 FRC 後、外気で約 371 ppm
  - 室内へ移動すると約 575 ppmへ上昇

今回の現象は、CO₂変化への追従自体よりも **絶対値の基準・校正経路**に問題がある可能性が高い。センサー故障を前提にせず、まずソフトウェア側の校正状態管理を整理すること。

---

## 2. 対象リポジトリ

- Repository: `divergen371/env_bio_sense`
- Branch: 作業開始時の `main` 最新を基準にすること。

主な確認対象:

- `src/drivers/sensors/scd41_sensor.cpp`
- `include/drivers/sensors/scd41_sensor.h`
- `src/services/sensor_manager.cpp`
- `include/services/sensor_manager.h`
- `src/services/web_server_service.cpp`
- SCD41 の最終校正日時を保存している Storage 関連コード
- 必要に応じて `src/drivers/sensors/bmp5_sensor_base.cpp`
- 関連テスト

---

## 3. 現行実装で確認済みの問題

### 3.1 ASC が起動時に無条件で無効化されている

現行 `Scd41Sensor::begin()` では次の処理が行われている。

```cpp
scd4x_.setAutomaticSelfCalibration(0);
```

このため、通常運用が事実上「ASC 無効 + 手動 FRC 依存」になっている。

今回の見直しでは、**通常運用の既定値を ASC 有効へ変更すること**。

ただし、ASC target の `400 ppm` は Sensirion の工場既定値であり、「現在の屋外大気は必ず 400 ppm」という意味ではない。ASC target は将来的に設定可能なパラメータとして扱い、アプリ側で「外気 = 400 ppm」と決め打ちしないこと。

---

### 3.2 「7日ごとに手動校正推奨」という設計を廃止する

現行 `SensorManager::isScd41CalibrationRecommended()` は、最終手動校正から 7 日経過すると FRC を推奨する。

これは FRC の用途と合っていない。

FRC は **externally obtained CO₂ reference value（外部から得た CO₂ 参照値）**を使用する機能であり、「1週間経ったから外気を 400 ppm と仮定して補正する」用途ではない。

以下を実施すること。

- 7日周期の FRC 推奨ロジックを削除する。
- Web UI の `SCD41 Manual Calibration is recommended!` バッジを削除する。
- 最終 FRC 時刻を保存する場合は、単なる診断情報として残す。
- 「一定期間経過 = FRC 必要」という判定には使用しない。

---

### 3.3 Web UI が FRC の誤用を誘導している

現行 UI:

```text
SCD41 Manual Calibration:
Expose sensor to fresh air (>3 mins) before calibrating.
Target CO2 (ppm): 400
```

問題点:

1. 「fresh air」と「既知濃度の外部参照」が混同されている。
2. 入力欄の既定値が `400` のため、現在の屋外大気を無条件に 400 ppm とみなす操作を誘発する。
3. SCD41 自身の表示値を見て、その値を FRC に入力することを防げない。

修正後:

- FRC を `Advanced / External Reference Calibration` 相当の扱いにする。
- target ppm の入力欄は **空欄**を既定とする。
- `400` を自動入力しない。
- 説明文を以下の意味へ変更する。

  > 外部の信頼できる CO₂ 基準器または既知濃度の校正環境がある場合のみ使用する。SCD41 自身の表示値や「外気だから 400 ppm」という推定値を入力してはならない。

- FRC 実行前に明示的な確認を要求する。
- API 側でも入力値と状態を検証し、UI だけに安全性を依存させない。

---

### 3.4 FRC correction の解釈が不十分

Sensirion SCD4x の仕様では FRC response word は次で解釈する。

```text
correction_ppm = response_word - 0x8000
```

例:

```text
0x7FCE -> -50 ppm
0x8000 ->   0 ppm
0x8032 -> +50 ppm
0xFFFF -> FRC failed
```

現行実装では `uint16_t frcCorrection` をそのまま保持し、

```cpp
Logger::info(..., "Correction: 0x%04X", frcCorrection);
```

Web UI でも hexadecimal の生値として表示している。

これを修正すること。

### 必須変更

FRC 結果は raw word と signed correction を分離する。

例:

```cpp
struct Scd41FrcResult {
    bool success;
    uint16_t rawWord;
    int16_t correctionPpm;
    uint16_t targetPpm;
    uint16_t preCalibrationCo2Ppm;
    uint16_t ambientPressureHpa;
    uint32_t timestampMs;
};
```

`0xFFFF` は signed correction へ変換する前に失敗として扱うこと。

ログ/UI/API は最低限以下を返すこと。

```json
{
  "status": "ok",
  "target_ppm": 430,
  "pre_co2_ppm": 478,
  "correction_ppm": -48,
  "raw_word": 32720,
  "pressure_hpa": 1008
}
```

16進値は診断補助として残してよいが、ユーザー向けの主表示は **signed ppm** にする。

---

## 4. FRC 実行シーケンスの再設計

Sensirion の仕様に沿って、FRC を以下のステートマシンとして扱うこと。

```text
Periodic Measurement
        |
        | >= 3 min continuous operation
        v
Validate Preconditions
        |
        v
Apply / confirm latest ambient pressure
        |
        v
Stop Periodic Measurement
        |
        | wait >= 500 ms
        v
performForcedRecalibration(reference_ppm)
        |
        | command completion >= 400 ms
        v
Decode correction
        |
        v
Restart Periodic Measurement
        |
        v
Post-FRC observation
```

### 4.1 最低限チェックする条件

FRC を実行する前に以下を検証すること。

- SCD41 が Ready である。
- periodic measurement を連続して 3 分以上実行している。
- 有効な CO₂ 測定値が存在する。
- target ppm がアプリケーションで許容する範囲内である。
  - 初期案: `400 <= target_ppm <= 5000`
  - 将来変更できる定数にする。
- BMP581 気圧補償を使用する場合、直近の有効な ambient pressure が存在する。
- センサーが別の maintenance/calibration 処理中でない。
- FRC 実行中は通常 `update()` が SCD41 にコマンドを発行しない。

### 4.2 3分条件をコメントだけにしない

現行ヘッダには

```cpp
// The sensor must have been operating in periodic measurement mode for >3 mins.
```

とあるが、実装上は強制されていない。

以下のような状態を保持すること。

```cpp
uint32_t measurementStartMs_;
bool calibrationInProgress_;
uint32_t lastMeasurementMs_;
```

FRC API 内で実時間を検証する。

---

## 5. 排他制御をシーケンス単位で見直す

現行コードは I2C コマンド単位で `I2cLockGuard` を取得している。

しかし FRC は

```text
stop
wait 500ms
FRC
restart
```

という複数コマンドの状態遷移である。

Web API が非同期タスクから呼ばれる場合、`stopPeriodicMeasurement()` 後の 500 ms の間に通常の `Scd41Sensor::update()` が走り、`getDataReadyFlag()` などを発行する可能性を排除すること。

### 要件

- `calibrationInProgress_` または専用 mutex/state を追加する。
- FRC 中の `update()` は SCD41 I2C 操作を行わない。
- FRC 中の `setAmbientPressure()` も不用意に割り込ませない。
- I2C バス全体を 1 秒近く占有する設計にはしない。
  - SCD41 内部の maintenance 状態で排他する。
  - 他センサーの I2C 利用は可能な限り妨げない。

---

## 6. `stopPeriodicMeasurement()` 失敗時の扱いを厳格化する

現行 FRC 実装では stop 失敗時に警告だけ出して処理を継続している。

```cpp
if (error) {
    Logger::warn(...);
}
```

FRC 実行時は、stop に失敗した状態で FRC を続行しないこと。

### 要件

- `stopPeriodicMeasurement()` が失敗した場合は FRC を中止。
- measurement の再開が必要なら recovery path を実行。
- 「already stopped だから問題ない」という扱いをする場合は、センサー状態を明示的に管理し、曖昧なエラー握り潰しをしない。
- 失敗理由を API とログへ返す。

---

## 7. BMP581 気圧補償の扱い

現行コードでは BMP581 の `pressureHpa` を整数 hPa にして SCD41 へ渡している。

```cpp
uint16_t pressInt = static_cast<uint16_t>(envTmp.pressureHpa);
scd41_.setAmbientPressure(pressInt);
```

SCD4x の仕様は

```text
ambient P [Pa] / 100
```

なので、SCD41 API が hPa 相当の整数を受け取る現行単位は妥当。

### この作業で保証すること

- SCD41 へ渡すのは **現地気圧（station/ambient pressure）**である。
- 海面更正気圧（sea-level pressure）を渡さない。
- 変数名に単位を明記する。

例:

```cpp
uint16_t ambientPressureHpa;
float pressurePa;
float pressureHpa;
```

- FRC 実行直前に、SCD41 が使用している最新 pressure をログへ残す。
- pressure の freshness を追跡する。

例:

```cpp
uint16_t lastAmbientPressureHpa_;
uint32_t lastAmbientPressureSetMs_;
bool hasAmbientPressure_;
```

- FRC 実行前に最新 pressure が古すぎる場合は、FRC を拒否または明示的に警告する。
  - freshness 閾値は定数化すること。
  - 初期案は 5〜10 秒程度。

### BMP581 の pressure offset について

`bmp5_sensor_base.cpp` では SCD41 に流れる `EnvironmentData.pressureHpa` は現在の raw station pressure 系であり、海面更正気圧ではない。

このチケットでは、BMP581 の高度校正から導かれた `pressureOffsetHpa_` を SCD41 補償へ無条件に流用しないこと。

理由:

- 高度校正用 offset には、BMP581 固有誤差以外に海面更正気圧・高度基準・温度モデル等の誤差が混入し得る。
- SCD41 が必要としているのは現地の実気圧。

必要なら別 Issue として「SCD41 に渡す station pressure の絶対精度校正」を検討する。

---

## 8. ASC の通常運用ポリシー

### 8.1 既定動作

通常起動時:

```text
SCD41 initialization
    -> configuration
    -> ASC enabled
    -> periodic measurement
    -> BMP581 pressure compensation
```

とする。

### 8.2 ASC target

SCD4x は ASC baseline target を設定可能で、工場既定値は 400 ppm。

ただしアプリ側では以下を守ること。

- `400 ppm` を「現在の屋外大気濃度」と説明しない。
- ASC target をハードコードされた「外気値」として扱わない。
- 必要なら config 化する。
- target を変更する場合は、その deployment で定期的に曝露される **既知の最低背景濃度**に基づくこと。
- 今回の修正では、外部基準なしに勝手に新しい target 値を推定しない。

### 8.3 persist_settings

ASC enabled/disabled や ASC target を power cycle 後も維持する必要がある場合のみ `persist_settings` を使用する。

注意:

- EEPROM 書き込みを起動ごとに行わない。
- 設定変更時だけ persist する。
- FRC/ASC の校正履歴は通常設定とは別にセンサー内部へ保存されるため、アプリ側で二重適用しない。

---

## 9. Factory Reset / Recovery 機能

今回すでに複数回 FRC を行っているため、検証用 recovery path を追加する。

SCD4x の factory reset は以下を消去する。

- EEPROM configuration
- FRC history
- ASC algorithm history

### 要件

- `Scd41Sensor::factoryResetAndReconfigure()` 相当を追加する。
- 通常 UI の目立つ位置には置かない。
- Advanced / Maintenance 操作にする。
- 誤操作防止の確認を要求する。
- reset 後は既存アプリ設定を必要に応じて再適用する。
  - temperature offset
  - ASC enabled
  - ASC target（明示設定している場合）
- periodic measurement を正常に再開する。
- reset 前後の設定値をログへ記録する。

### 注意

現行コードの SCD41 temperature offset `2.0 °C` は、このチケットでは値そのものの妥当性評価対象外とする。

ただし factory reset 後に工場既定値へ戻るため、現在の 2.0 °C を維持するなら再設定が必要。

---

## 10. 診断ログを拡充する

通常の 5 秒測定ログとは別に、校正イベントは一件の構造化イベントとして追跡できるようにする。

最低限:

```text
event=SCD41_FRC_BEGIN
target_ppm=...
pre_co2_ppm=...
pre_co2_age_ms=...
ambient_pressure_hpa=...
pressure_age_ms=...
asc_enabled=...
measurement_uptime_ms=...

event=SCD41_FRC_RESULT
success=true/false
raw_word=0x....
correction_ppm=...
error=...
restart_success=true/false
```

FRC 後は最初の数サンプルについて、

```text
event=SCD41_POST_FRC
elapsed_ms=...
co2_ppm=...
pressure_hpa=...
```

をログ可能にする。

常時 verbose にする必要はない。debug/diagnostic build flag でもよい。

---

## 11. 表示値・生センサー値・校正値を混同しない

今回のデバッグでは OLED/UI が約 5 秒更新であるため、「ユーザーが見た値」と「FRC 実行直前にセンサーが保持している最新値」が一致する保証がない。

### 要件

FRC 実行時に以下を明確に区別する。

- `displayed_co2_ppm`
- `latest_sensor_co2_ppm`
- `frc_reference_ppm`
- `frc_correction_ppm`

FRC API は表示キャッシュ値を暗黙に calibration reference として使用してはならない。

UI の表示値を FRC target へ自動コピーする機能も作らない。

---

## 12. API の変更案

現行:

```http
POST /api/scd41/calibrate

{
  "target_ppm": 400
}
```

変更後も endpoint は維持してよいが、意味を明確にする。

### Request

```json
{
  "reference_ppm": 428,
  "confirm_external_reference": true
}
```

### Success response

```json
{
  "status": "ok",
  "reference_ppm": 428,
  "pre_co2_ppm": 476,
  "correction_ppm": -48,
  "raw_word": 32720,
  "ambient_pressure_hpa": 1008,
  "measurement_uptime_ms": 845000
}
```

### Validation failure example

```json
{
  "status": "rejected",
  "error_code": "EXTERNAL_REFERENCE_NOT_CONFIRMED",
  "message": "FRC requires an externally obtained CO2 reference value."
}
```

その他:

- `SENSOR_NOT_READY`
- `MEASUREMENT_UPTIME_TOO_SHORT`
- `PRESSURE_STALE`
- `REFERENCE_OUT_OF_RANGE`
- `STOP_MEASUREMENT_FAILED`
- `FRC_FAILED`
- `RESTART_MEASUREMENT_FAILED`

などを定義する。

---

## 13. テスト

### 13.1 Unit test: FRC correction decode

最低限:

```text
raw 0x7FCE -> -50 ppm
raw 0x8000 ->   0 ppm
raw 0x8032 -> +50 ppm
raw 0xFFFF -> failure
```

### 13.2 Unit test: FRC preconditions

- measurement uptime < 3 min -> reject
- no valid CO₂ -> reject
- invalid target -> reject
- stale pressure -> reject（pressure compensation policy が有効な場合）
- calibration already running -> reject
- valid state -> proceed

### 13.3 Integration test: command order

fake/mock SCD4x driver を使用できるなら、最低限次を検証する。

```text
set/confirm ambient pressure
stopPeriodicMeasurement
wait >= 500 ms
performForcedRecalibration
decode result
startPeriodicMeasurement
```

stop 失敗時に FRC を呼ばないこと。

FRC 失敗時にも measurement recovery を試みること。

### 13.4 Concurrency test

FRC 実行中に `update()` が呼ばれても SCD41 I2C command が割り込まないこと。

### 13.5 UI/API test

- target/reference の既定値が 400 になっていない。
- external reference confirmation がない場合は reject。
- correction が hexadecimal のみで表示されない。
- 7日周期の manual calibration recommendation が存在しない。

---

## 14. 実機検証手順

### Phase A: 校正履歴リセット

1. 現在の firmware で最終ログを保存。
2. 修正版 firmware を書き込む。
3. SCD41 factory reset を 1 回実施。
4. アプリ設定を再適用。
5. ASC 有効を read-back で確認。
6. FRC は実行しない。

### Phase B: Factory calibration + ASC 状態で観察

1. 屋外の直射日光・呼気直撃・排気源を避けた場所に設置。
2. 10〜30 分以上ログ取得。
3. BMP581 ambient pressure が SCD41 へ継続投入されていることを確認。
4. CO₂ 値、温度、湿度、気圧を保存。
5. 室内へ移動して CO₂ 上昇への応答を確認。

この段階では「屋外値を○○ ppm に合わせる」処理をしない。

### Phase C: FRC 検証

**外部の信頼できる CO₂ 基準値が用意できた場合だけ実施する。**

1. SCD41 と参照器を同一空間へ置く。
2. 空気を均一化する。
3. SCD41 を通常 measurement mode で 3 分以上動作。
4. 参照器の値を `reference_ppm` として入力。
5. FRC correction を signed ppm で記録。
6. FRC 前後 10 分程度の値を保存。
7. correction が参照器との差と整合するか確認。

外部基準器がない場合、Phase C は実施しない。

---

## 15. 受入条件

以下をすべて満たしたら完了。

- [ ] 起動時に ASC を無条件で disable しない。
- [ ] 通常運用の既定が ASC enabled になっている。
- [ ] 7日周期の manual FRC 推奨ロジックが削除されている。
- [ ] Web UI に `400 ppm` の FRC 既定値が存在しない。
- [ ] FRC は外部 reference が必要であることが UI/API 上明示されている。
- [ ] SCD41 自身の表示値を calibration reference として自動利用しない。
- [ ] FRC correction を `word - 0x8000` で signed ppm に変換する。
- [ ] `0xFFFF` を失敗として処理する。
- [ ] FRC 前の 3 分以上連続 measurement 条件をコードで検証する。
- [ ] FRC 実行中に通常 update が割り込まない。
- [ ] stop measurement 失敗後に FRC を強行しない。
- [ ] BMP581 -> SCD41 の pressure 単位が hPa（Pa/100）であることがコード上明確。
- [ ] SCD41 へ sea-level pressure を渡していない。
- [ ] FRC 時の pressure 値と freshness がログに残る。
- [ ] factory reset + reconfigure の recovery path がある。
- [ ] FRC raw response と signed correction の unit test がある。
- [ ] FRC の command order を検証するテストがある。
- [ ] PlatformIO build が通る。
- [ ] 既存の SHT45 / SGP41 / BMP581 / GNSS / logger 等に回帰がない。

---

## 16. 実装上の優先順位

### P0: 事故防止

1. 7日周期 FRC 推奨を削除。
2. UI の default `400 ppm` を削除。
3. ASC を通常運用へ戻す。
4. FRC correction の signed decode を修正。
5. FRC sequence の排他と precondition を実装。

### P1: 診断性

6. FRC 前後の構造化ログ。
7. pressure freshness 管理。
8. factory reset / reconfigure。
9. API error code 整理。

### P2: 将来拡張

10. ASC target の設定化。
11. external reference device との比較ログ。
12. calibration event history の保存・可視化。

---

## 17. 作業時の禁止事項

- 「外気 = 400 ppm」を新しいコードにもハードコードしない。
- SCD41 の現在表示値を、そのまま FRC reference として使用しない。
- 原因不明の固定 offset をアプリ側で加減して帳尻を合わせない。
- FRC と ASC の両方が何をしているか不明なまま独自補正を重ねない。
- BMP581 の sea-level pressure を SCD41 ambient pressure に渡さない。
- factory reset を通常起動時に自動実行しない。
- `persist_settings` を毎起動・毎ループで呼ばない。
- 実機に外部 CO₂ reference がない状態で、FRC の精度検証を「外気の推定値」で代用しない。

---

## 18. 参考仕様

Sensirion SCD4x Datasheet, Version 1.7, April 2025:

- Field Calibration / ASC / FRC
- `perform_forced_recalibration`
- `set_automatic_self_calibration_enabled`
- `set_automatic_self_calibration_target`
- `set_ambient_pressure`
- `persist_settings`
- `perform_factory_reset`

Official datasheet:

https://sensirion.com/media/documents/48C4B7FB/67FE0194/CD_DS_SCD4x_Datasheet_D1.pdf

Sensirion SCD41 product/download page:

https://sensirion.com/jp/products/catalog/SCD41

---

## 19. エージェントへの最終指示

実装前に対象コードを読み、上記の現行実装確認事項が `main` 最新でも成立しているか再確認すること。

仕様とコードが食い違う場合は、Sensirion 公式データシートを優先し、独自解釈で補正動作を追加しないこと。

変更後は、以下を報告すること。

1. 変更ファイル一覧
2. 変更理由
3. FRC/ASC の新しい状態遷移
4. FRC correction の decode 方法
5. BMP581 pressure compensation のデータフロー
6. 追加テストと結果
7. PlatformIO build 結果
8. 実機確認が必要な項目
9. 既知の未解決事項

実機が必要な検証については、コードだけで「精度改善済み」と断定しないこと。
