# env_bio_sense (XIAO ESP32S3 Sensor Platform)

Seeed Studio XIAO ESP32S3 を用いた、複合環境センサおよび生体光センサ（PPG）統合プラットフォームプロジェクトです。

---

## 主な機能・特徴

- **マルチセンサ統合管理 (`SensorManager`)**:
  - 環境センサ（温湿度、気圧、CO2濃度）および生体脈波センサを同一バス（I2C）上で一括管理。
  - 各センサの更新頻度最適化（環境系1000ms、生体系リアルタイム）とエラー検知・自己復帰機能を搭載。
- **自作脈波DSPエンジン (`PulseAnalyzer`) & 自動AGC**:
  - MAX30102向け独自時間領域分析アルゴリズムおよび動的LED輝度調整機能。
  - **動的移動平均・ヒステリシス閾値**: ノイズやアーティファクトによる永久ロックアップを防止し、常に最適なピーク検出閾値を維持。
  - **タイムアウト減衰リカバリー**: 脈を見失った際に閾値を急激に下げることで、0.5秒以内に確実な復帰を実現。
  - **Maxim公式仕様準拠の自動AGC**: ADCフルスケールの1/4〜3/4に信号を収める高速な自動キャリブレーション（LED電流制御）を実装。
  - **Maxim公式二次多項式キャリブレーション**: MAX30102（660nm / 880nm）に最適化された計算式でSpO2を算出。
- **OLEDディスプレイ描画 (`DisplayManager`)**:
  - 128x64 SSD1306 ディスプレイへセンサデータ・心拍数・SpO2・警告メッセージをリアルタイム表示。
- **ネイティブ単体テスト基盤 (`pio test -e native`)**:
  - Unityフレームワークおよび100Hz実効サンプリングレートに適合させた自動単体テスト環境。

---

## ハードウェア構成・センサ一覧

| センサ / デバイス | 接続インターフェース | 役割・取得データ |
|---|---|---|
| **Seeed Studio XIAO ESP32S3** | - | メインMCU |
| **SSD1306 OLED (128x64)** | I2C (0x3C) | リアルタイム情報表示 |
| **SHT45** | I2C (0x44) | 高精度 温度・湿度計測 |
| **BMP585** | I2C (0x47) | 高精度 気圧・温度計測 |
| **SCD41** | I2C (0x62) | NDIR方式 CO2濃度・温度・湿度計測 / 密閉加熱警告 |
| **MAX30102** | I2C (0x57) | 赤外・赤色LED PPG光センサ（心拍数・SpO2計測） |

---

## アーキテクチャ実装状況

### [Step 1] HAL・共通基盤の整備
- `hal/pins.h`: センサ・I2C・割込みピン定義の一元管理
- `hal/i2c_bus.h`: I2Cバス初期化および自動アドレススキャン機能
- `services/logger.h`: ログレベル管理（INFO/WARN/ERROR）およびシリアル出力フォーマット化

### [Step 2] 状態管理とディスプレイ基盤
- `core/sensor_types.h`, `core/sensor_snapshot.h`: スナップショット形式のデータモデル定義
- `services/sensor_manager.h`: 全センサのライフサイクル・Polling同期管理
- `services/display_manager.h`: OLED描画エンジンの統合

### [Step 3] SHT45 (温湿度センサ) の統合
- `drivers/sensors/sht45_sensor.cpp`: SHT45ドライバ実装と `SensorManager` への統合

### [Step 4] BMP585 (気圧センサ) の統合
- `drivers/sensors/bmp585_sensor.cpp`: BMP585ドライバ実装（Bosch SensorAPI利用）

### [Step 5] SCD41 (CO2センサ) の統合
- `drivers/sensors/scd41_sensor.cpp`: NDIR CO2センサの統合およびエンクロージャー密閉加熱 (`HeatTrapped`) 警告ロジック実装

### [Step 6] MAX30102 (PPGセンサ) & 自作DSPアルゴリズム (`PulseAnalyzer`) の統合
- **ハードウェア制御ライブラリの採用意図**:
  `SparkFun MAX3010x Pulse and Proximity Sensor Library` は、MAX30102のレジスタ設定、LED電流制御、FIFOバッファ取得などの**低レイヤーI2Cハードウェアドライバとしてのみ利用**しています。ライブラリ内蔵の脈波解析アルゴリズムは不応期制御や急激なベースライン変動への適応に課題があったため使用せず、取得した生データ（IR / Red FIFO）の信号処理・脈波解析はすべて自作DSP（`PulseAnalyzer`）へ引き渡す構成を採用しています。
- `drivers/sensors/max30102_sensor.cpp`: MAX30102のI2C通信・FIFOデータ取得・LED輝度自動キャリブレーション制御
- `utils/pulse_analyzer.cpp`: 自作脈波解析エンジン（DC除去、ローパス、動的移動平均ヒステリシスピーク検出、Maxim公式SpO2多項式）
- `test/test_pulse_analyzer/test_pulse_analyzer.cpp`: 25Hz環境対応ネイティブ単体テスト環境

---

## コマンド一覧

### ビルドと書き込み
```bash
# ファームウェアのビルド
pio run

# 実機 (XIAO ESP32S3) への書き込み
pio run -t upload

# シリアルモニター表示
pio device monitor
```

### 単体テストの実行
```bash
# PCローカル環境でのネイティブ単体テスト実行
pio test -e native
```

---

## 使用ライブラリ

- `Adafruit SSD1306` @ ^2.5.13
- `Adafruit GFX Library` @ ^1.11.11
- `Sensirion I2C SHT4x` @ ^1.1.0
- `Sensirion Core` @ ^0.7.1
- `Sensirion I2C SCD4x` @ ^0.4.0
- `BMP5_SensorAPI` (Bosch公式)
- `SparkFun MAX3010x Pulse and Proximity Sensor Library` @ ^1.1.2（低レイヤーI2C・レジスタ制御およびFIFO取得のみに使用）
- `Unity` (Native Test Framework)
