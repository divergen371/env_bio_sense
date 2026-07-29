# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリング計画です。
システムの将来的な拡張（通信、記録、画面拡張など）に備え、ベタ書きされている現在の実装を段階的にレイヤアーキテクチャへ移行します。

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

- **[Step 1] HAL・共通基盤の整備（✅ 完了）**
- **[Step 2] 状態管理とディスプレイ基盤（✅ 完了）**
- **[Step 3] SHT45 (温湿度センサ) の統合（✅ 完了）**
- **[Step 4] BMP585 (気圧センサ) の統合（✅ 完了）**
- **[Step 5] SCD41 (CO2センサ) の統合（✅ 完了）**
- **[Step 6] MAX30102 (脈波・血中酸素センサ) の統合（← 現在のフェーズ）**
  - `Max30102Sensor` ドライバの実装
  - OLED への `HR:` / `SpO2:` (ダミーまたは生値) の反映
- **[Step 7] MAX30102 信号処理**
- **[Step 8] システム全体の完成**

---

## User Review Required
> [!IMPORTANT]
> **Step 6 (MAX30102 脈波・血中酸素センサの統合)** に進みます。
> MAX30102の制御には、Arduino界隈で最も実績があり安定している **`SparkFun MAX3010x Pulse and Proximity Sensor Library`** を採用する計画です。
> このステップではまず「センサとの通信確立」「赤色(Red)と赤外線(IR)の生データの安定取得」までを行い、心拍数(HR)やSpO2の具体的なアルゴリズム適用は次のStep 7で行う段階的なアプローチを取ります。
> 以下の変更内容で実装を進めてよろしいかご確認ください。

## Proposed Changes (Step 6: MAX30102 の統合)

### Application (アプリケーション層)
#### [MODIFY] [platformio.ini](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/platformio.ini)
- `sparkfun/SparkFun MAX3010x Pulse and Proximity Sensor Library @ ^1.1.2`（または最新互換版）を追加します。

### Drivers (ドライバ層)
#### [NEW] [include/drivers/sensors/max30102_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/max30102_sensor.h)
#### [NEW] [src/drivers/sensors/max30102_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/max30102_sensor.cpp)
- `IBioSensor` (または `ISensor`) を継承した `Max30102Sensor` クラスを作成します。
- `begin()` でI2C通信を確立し、SpO2モード(RedとIRの両方を発光)でのサンプリング設定を行います。
- `update()` でセンサのFIFOバッファから最新の生データ(Red / IR)を読み出します。

### Services (サービス層)
#### [MODIFY] [include/services/sensor_manager.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/sensor_manager.h)
#### [MODIFY] [src/services/sensor_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/sensor_manager.cpp)
- `Max30102Sensor` インスタンスをメンバとして追加し管理させます。
- `readPpg()` などのメソッドを経由して、Red/IRの生データや、後続のStepで計算されるHR/SpO2を `SensorSnapshot` の `ppg` フィールドに反映させます。

#### [MODIFY] [src/services/display_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/display_manager.cpp)
- `SensorSnapshot` に有効なデータがあるかを確認し、センサが正しく認識されたことを確認するための表示（例: `HR : wait..` など）を追加します。

## Verification Plan (Step 6)

### Automated Tests
- `pio run` でビルドが成功することを確認します。

### Manual Verification
- 実機に書き込んだ後、シリアルモニタ上でMAX30102の初期化が成功し、指を乗せるとRed/IRの生データが変化している様子（ログ出力）を確認します。
