# Antigravity XIAO ESP32S3 Sensor Platform

XIAO ESP32S3を用いた複合センサ環境プラットフォームのプロジェクトです。

## アーキテクチャ実装状況

現在、アーキテクチャの段階的実装を進めています。

### [Step 1] HAL・共通基盤の整備（完了）
- `pins.h`: センサとI2Cのピン定義を管理
- `i2c_bus`: I2C初期化とスキャン機能をモジュール化
- `logger`: `services::Logger` によるログレベル管理とシリアル出力のフォーマット化

### [Step 2] 状態管理とディスプレイ基盤（完了）
- `core/sensor_types.h`, `core/sensor_snapshot.h`: センサの状態や測定値を保持する共通データ型の整備
- `SensorManager`: 各センサの統合管理クラスの骨格実装
- `DisplayManager`: `Adafruit SSD1306` ライブラリを用いたOLED描画機能の統合

### [Step 3] SHT45 (温湿度センサ) の統合（完了）
- `sensor_interface.h`: 全センサ共通の基底クラスを定義
- `Sht45Sensor`: `Sensirion I2C SHT4x` を用いたSHT45のドライバを実装
- `SensorManager` にてSHT45の実データを読み出し、OLEDに反映するよう統合

## 使用ライブラリ
- PlatformIO/Arduino標準ライブラリ（`Wire` 等）
- `Adafruit SSD1306`
- `Adafruit GFX Library`
- `Sensirion I2C SHT4x`
- `Sensirion Core`
