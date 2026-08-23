# SCD41 校正ロジック見直し 実装計画

## 目的
SCD41の通常運用をASC (Automatic Self-Calibration) 中心に戻し、FRC (Forced Recalibration) を外部の既知のCO2参照値がある場合のみ実行する保守機能として再設計します。Web UIからの誤用を防ぎ、FRC実行時の安全性、排他制御、気圧補償の厳格化を行います。

## ユーザーへの確認事項 (User Review Required)
- **ASCのデフォルト有効化:** 起動時にASCを無効化していた処理を削除し、工場出荷時の動作（ASC有効）へ戻します。
- **FRCの7日周期推奨の廃止:** 定期的なFRC推奨を廃止します。Web UI上のバッジ等も削除されます。

## Open Questions
- 特になし。指示書 `docs/SCD41_calibration_logic_review_instructions.md` の要件をすべて網羅するように設計しました。

## 提案する変更 (Proposed Changes)

### SCD41 ドライバ (`src/drivers/sensors/scd41_sensor.cpp`, `include/drivers/sensors/scd41_sensor.h`)
- **ASCの有効化:** `begin()` における `scd4x_.setAutomaticSelfCalibration(0)` を削除し、必要に応じて明示的に有効化します。
- **FRC APIの改修:**
  - `Scd41FrcResult` 構造体を導入し、生値(`rawWord`)と補正値(`correctionPpm` = `rawWord - 0x8000`)を明確に分離します。`0xFFFF` をエラーとして処理します。
  - FRC実行前に、定期測定(periodic measurement)が3分以上稼働しているか(`measurementStartMs_`などを用いて)検証します。
  - 排他制御: FRC実行中(`calibrationInProgress_ = true`)は、`update()` 処理内での I2C コマンド送信(ポーリングやデータ読み出し)をスキップします。
  - 測定停止コマンド(`stopPeriodicMeasurement()`)失敗時はFRCを強行せず中止します。
- **気圧補償:** `setAmbientPressure` の引数が hPa単位(SCD41では Pa/100)であることを明確化し、気圧データの鮮度(`lastAmbientPressureSetMs_`)を追跡します。FRC前に気圧が古すぎる場合（例：10秒以上前）は警告または拒否します。
- **Factory Reset & Recovery:** 新規に `factoryResetAndReconfigure()` メソッドを追加し、リセットと設定(ASC、TemperatureOffset等)の再適用および測定再開をサポートします。

### センサーマネージャ (`src/services/sensor_manager.cpp`, `include/services/sensor_manager.h`)
- **FRC推奨ロジック削除:** `isScd41CalibrationRecommended()` などのメソッドと、それを呼び出している処理を削除します。
- **FRC API連携:** SCD41ドライバの新しいFRC APIを呼び出すように修正し、結果の情報を上位に返せるようにします。

### Web サーバー及び UI (`src/services/web_server_service.cpp`)
- **APIの変更:** `POST /api/scd41/calibrate` において、`reference_ppm` と `confirm_external_reference: true` を要求する形に変更します。
- **Web UIの変更:**
  - "SCD41 Manual Calibration is recommended!" のバッジを削除します。
  - Target CO2入力欄のデフォルト値 `400` を空欄にし、「外部参照がある場合のみ使用」の警告文を追加します。
  - 校正ボタン押下前に外部参照値であるか確認するチェックボックスを追加します。
  - FRC成功時に `signed ppm` での補正値を表示します。

## 検証計画 (Verification Plan)
### 自動テスト
- `Scd41FrcResult` の補正値デコードロジックに対するユニットテストを追加/更新。
- FRCの事前条件（uptime 3分以上など）のロジックテストを追加。
- FRCコマンドの順序検証テストを追加。

### 手動検証
- Web UIを開き、推奨バッジが消えていること、SCD41校正のデフォルト値が空欄であることを確認。
- （外部参照がない場合）FRCを実行せず、ログでASCが有効化され、BMP581の気圧がSCD41に反映されていることを確認。
