# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリング計画です。
システムの将来的な拡張（通信、記録、画面拡張など）に備え、ベタ書きされている現在の実装を段階的にレイヤアーキテクチャへ移行します。

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

- **[Step 1] HAL・共通基盤の整備（✅ 完了）**
- **[Step 2] 状態管理とディスプレイ基盤（✅ 完了）**
- **[Step 3] SHT45 (温湿度センサ) の統合（✅ 完了）**
- **[Step 4] BMP585 (気圧センサ) の統合（✅ 完了）**
- **[Step 5] SCD41 (CO2センサ) の統合（← 現在のフェーズ）**
  - `Scd41Sensor` ドライバの実装
  - CO2濃度のOLEDへの統合
- **[Step 6] MAX30102 (脈波・血中酸素センサ) 基礎**
- **[Step 7] MAX30102 信号処理**
- **[Step 8] システム全体の完成**

---

## User Review Required
> [!IMPORTANT]
> **Step 5 (SCD41 CO2センサの統合)** に進みます。
> 温湿度センサSHT45の時と同様に、SCD41の製造元であるSensirion社が提供している公式Arduinoライブラリ（`Sensirion I2C SCD4x`）を使用する計画です。
> 以下の変更内容で実装を進めてよろしいかご確認ください。

## Proposed Changes (Step 5: SCD41 の統合)

### Application (アプリケーション層)
#### [MODIFY] [platformio.ini](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/platformio.ini)
- `sensirion/Sensirion I2C SCD4x @ ^0.4.0`（または最新版）を追加します。

### Drivers (ドライバ層)
#### [NEW] [include/drivers/sensors/scd41_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/scd41_sensor.h)
#### [NEW] [src/drivers/sensors/scd41_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/scd41_sensor.cpp)
- `IEnvironmentSensor` を継承した `Scd41Sensor` クラスを作成します。
- `begin()` で初期化と periodic measurement (定期測定) の開始を行います。
- `update()` でデータレディ(Data Ready)状態をポーリングし、新しいデータがあれば読み出します。

### Services (サービス層)
#### [MODIFY] [include/services/sensor_manager.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/sensor_manager.h)
#### [MODIFY] [src/services/sensor_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/sensor_manager.cpp)
- `Scd41Sensor` インスタンスをメンバとして追加し、`initialize()` と `update()` で管理させます。
- 取得した CO2濃度 (ppm) を `SensorSnapshot` の `co2Ppm` に反映させます。

#### [MODIFY] [src/services/display_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/display_manager.cpp)
- `SensorSnapshot` に有効な `co2Ppm` データがある場合、OLEDの適切な位置（例えば `C: 400 ppm` など）にCO2濃度を描画するように修正します。

## Verification Plan (Step 5)

### Automated Tests
- `pio run` でビルドが成功することを確認します。

### Manual Verification
- 実機に書き込んだ後、OLEDディスプレイに `C: XXXX ppm` と現在のCO2濃度が表示され、数秒ごとに値が更新されることを確認します（SCD41の測定間隔は約5秒です）。
