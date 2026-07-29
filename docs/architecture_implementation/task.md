# XIAO ESP32S3 アーキテクチャ実装 (Step 8: システム全体の最適化・安定化) タスクリスト

- `[x]` `include/core/sensor_types.h` に `PpgState` Enum を追加し、`PpgData` に `state` フィールドを追加する
- `[x]` `include/drivers/sensors/max30102_sensor.h` にオートキャリブレーション用変数とEMAフィルタ用変数を追加する
- `[x]` `src/drivers/sensors/max30102_sensor.cpp` に指検出、キャリブレーション、および異常値のカット＋EMA処理を実装する
- `[x]` `src/services/display_manager.cpp` のOLED描画ロジックを `PpgState` に応じて分岐させる (`No Finger`, `Calibrating...`, 測定値)
- `[x]` `src/main.cpp` のログ出力を `PpgState` に応じて見やすく修正する
- `[ ]` ビルドと実機確認を依頼する
