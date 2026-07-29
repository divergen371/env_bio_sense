# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

- **[Step 1] HAL・共通基盤の整備（✅ 完了）**
- **[Step 2] 状態管理とディスプレイ基盤（✅ 完了）**
- **[Step 3] SHT45 (温湿度センサ) の統合（✅ 完了）**
- **[Step 4] BMP585 (気圧センサ) の統合（✅ 完了）**
- **[Step 5] SCD41 (CO2センサ) の統合（✅ 完了）**
- **[Step 6] MAX30102 (脈波・血中酸素センサ) の統合（✅ 完了）**
- **[Step 7] MAX30102 信号処理（✅ 完了）**
- **[Step 8] システム全体の最適化・安定化（← 現在のフェーズ）**
  - **自動キャリブレーション（Auto-LED Current）の実装**
  - 指数平滑移動平均(EMA)フィルタの導入
  - UIへのステータス（指なし、キャリブレーション中など）反映
- **[Step 9] 高度なDSPアルゴリズムの探求 (Future Work)**
  - アナログ・デバイセズ技術記事「RAQ Issue 230」に基づく、離散周期変換（DPT: Discrete Period Transform）を用いた耐モーションアーティファクトアルゴリズムの研究と実装

---

## Proposed Changes (Step 8: オートキャリブレーションと最適化)

### 1. 状態定義の追加
#### [MODIFY] [include/core/sensor_types.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/core/sensor_types.h)
- 脈波センサの内部状態を示す `PpgState` Enum を追加します。
  - `NoFinger`: 指が置かれていない状態
  - `Calibrating`: LED電流の自動調整中
  - `Measuring`: 測定および計算実行中
- `PpgData` 構造体に `PpgState state` を追加します。

### 2. オートキャリブレーション・ステートマシンの実装
#### [MODIFY] [include/drivers/sensors/max30102_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/max30102_sensor.h)
#### [MODIFY] [src/drivers/sensors/max30102_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/max30102_sensor.cpp)
- `update()` メソッド内で以下のステートマシンを稼働させます。
  1. **[NoFinger]**: IR値が一定の閾値（例: 20,000）を下回る場合は「指なし」と判定し、LED電流を最小（省電力）にして待機します。計算結果はリセットします。
  2. **[Calibrating]**: 指が検出されたら、LED電流を低め（例: 10/255）から開始し、IR値が適正範囲（80,000 〜 120,000）に収まるように1サンプルごとにLED電流（Red/IR共に）を増減させます。
  3. **[Measuring]**: 適正範囲に収まったらその電流値で固定し、スライディングウィンドウによるHR/SpO2計算を開始します。
  4. **[Fallback]**: 5秒以上キャリブレーションが終わらない場合は、測定を強行します。

### 3. スムージング（移動平均）と外れ値除外
#### [MODIFY] [src/drivers/sensors/max30102_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/max30102_sensor.cpp)
- 算出されたHR/SpO2に対し、**指数平滑移動平均（EMA）** フィルタをかけて急激なブレを抑え込みます。（例: `新値 = 0.2*実測 + 0.8*前回値`）
- `40未満` や `200超` の非現実的なHRが算出された場合は、計算をスキップして前回の正常値を保持します。

### 4. UI へのステータスフィードバック
#### [MODIFY] [src/services/display_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/display_manager.cpp)
#### [MODIFY] [src/main.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/main.cpp)
- OLED表示を分岐し、現在の `PpgState` に応じたメッセージを表示します。
  - `NoFinger` → `HR: No Finger`
  - `Calibrating` → `HR: Calibrating...`
  - `Measuring` → `HR: 90.5 bpm`

## Verification Plan
- ビルド・実行後、指を乗せるとOLEDが `Calibrating...` になり、その後安定したHRが表示されることを確認します。
- 指を離すと速やかに `No Finger` に戻ることを確認します。
