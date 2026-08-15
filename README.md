# env_bio_sense (XIAO ESP32S3 Sensor Platform)

Seeed Studio XIAO ESP32S3 を用いた、複合環境センサおよび生体光センサ（PPG）統合プラットフォームプロジェクトです。

---

## 主な機能・特徴

- **マルチセンサ統合管理 (`SensorManager`)**:
  - 環境センサ（温湿度、気圧、CO2濃度）および生体脈波センサを同一バス（I2C）上で一括管理。
  - 各センサの更新頻度最適化（環境系1000ms、生体系リアルタイム）とエラー検知・自己復帰機能を搭載。
- **気象庁AMeDAS連携と絶対高度算出**:
  - 起動時や定期的に気象庁AMeDAS APIから最新の海面気圧データを取得し、逆距離加重法 (IDW) で現在地の補間海面気圧を自動計算。
    (※詳細な処理フローやメモリ節約型ストリーム解析については [気象庁AMeDAS連携とIDW補間アルゴリズム解説](docs/jma_amedas_integration_guide.md) を参照)
  - BMP581で取得した気圧データと組み合わせることで、精度の高い絶対高度を算出します。
- **自律的メンテナンス・校正管理**:
  - **SCD41**: FRC（手動校正）機能の実装と、長期間校正が行われていない場合のUIへの校正推奨通知。
  - **SHT45**: 結露防止機能（95%RH以上でヒーターを自動作動、クールダウン期間のデータ補正マスク）。
  - **SGP41**: ベースライン状態のFRAM永続化による、再起動後の高速な空気質指数の復旧。
- **自作脈波DSPエンジン (`PulseAnalyzer`) & 自動AGC**:
  - MAX30102向け独自時間領域分析アルゴリズムおよび動的LED輝度調整機能。
  - **動的移動平均・ヒステリシス閾値**: ノイズやアーティファクトによる永久ロックアップを防止し、常に最適なピーク検出閾値を維持。
  - **タイムアウト減衰リカバリー**: 脈を見失った際に閾値を急激に下げることで、0.5秒以内に確実な復帰を実現。
  - **Maxim公式仕様準拠の自動AGC**: ADCフルスケールの1/4〜3/4に信号を収める高速な自動キャリブレーション（LED電流制御）を実装。
  - **Maxim公式二次多項式キャリブレーション**: MAX30102（660nm / 880nm）に最適化された計算式でSpO2を算出。
- **OLEDディスプレイ描画 (`DisplayManager`)**:
  - 128x64 SSD1306 ディスプレイへセンサデータ・心拍数・SpO2・警告メッセージ・高度などをリアルタイム表示。
- **高信頼性二層ストレージ (FRAM + SD)**:
  - 測定データをまずFRAM（MB85RC256V）のリングバッファにジャーナリングし、その後SDカードへフラッシュする二層設計。
  - センサの校正状態や補正値、AMeDAS海面気圧情報などもFRAM上に安全に永続化されます。
  - SDカード未挿入時・書き込みエラー時にもデータはFRAM上に保持され、SD復旧時に一括リカバリします。
- **オンデマンドWi-Fi APモード（ファイルリモートアクセス）**:
  - 稼働中に `BOOT` ボタンを3秒間長押しすることで、ESP32自身がアクセスポイントとなり、スマホやPCからWebサーバーへアクセス可能。
  - SDカード内のCSVファイルの閲覧、ダウンロード、ストレージ空き容量の確認、Chart.jsによるデータのインタラクティブな可視化、手動気圧校正などをWeb UIから実行できます。

---

## ハードウェア構成・センサ一覧

| センサ / デバイス | 接続インターフェース | 役割・取得データ |
|---|---|---|
| **Seeed Studio XIAO ESP32S3** | - | メインMCU |
| **SSD1306 OLED (128x64)** | I2C (0x3C) | リアルタイム情報表示 |
| **SHT45** | I2C (0x44) | 高精度 温度・湿度計測（結露防止ヒーター制御付） |
| **BMP581** | I2C (0x47) | 高精度 気圧・絶対高度計測（AMeDAS連携対応） |
| **SCD41** | I2C (0x62) | NDIR方式 CO2濃度計測（FRC手動校正対応） |
| **SGP41** | I2C (0x59) | MOx方式 VOC および NOx インデックス計測（ベースライン永続化対応） |
| **MAX30102** | I2C (0x57) | 赤外・赤色LED PPG光センサ（心拍数・SpO2計測） |

---

## アーキテクチャ実装状況

### [Step 1] HAL・共通基盤の整備
- `hal/pins.h`: センサ・I2C・割込みピン定義の一元管理
- `hal/i2c_bus.h`: I2Cバス初期化、ロック制御、自動アドレススキャン機能
- `services/logger.h`: ログレベル管理およびシリアル出力フォーマット化

### [Step 2] 状態管理とディスプレイ基盤
- `core/sensor_types.h`: スナップショット形式のデータモデル定義
- `services/sensor_manager.h`: 全センサのライフサイクル同期、外部サービス（Time/Weather）との連携統合
- `services/display_manager.h`: OLED描画エンジンの統合

### [Step 3] 環境センサの統合 (SHT45 / BMP581)
- `drivers/sensors/sht45_sensor.cpp`: SHT45の高湿度結露防止ヒーター制御とデータマスク処理
- `drivers/sensors/bmp5_sensor_base.cpp`: BMP581のIIRフィルタ適用と、気温補正付き絶対高度算出、自動・手動ベース気圧校正

### [Step 4] 空気質・ガスセンサの統合 (SCD41 / SGP41)
- `drivers/sensors/scd41_sensor.cpp`: NDIR CO2センサのFRC(強制再校正)機能と校正推奨UIの統合
- `drivers/sensors/sgp41_sensor.cpp`: VOC/NOxインデックス算出ロジックおよびベースラインFRAM保存・復帰機能

### [Step 5] PPGセンサの統合 (MAX30102) & 自作DSPアルゴリズム
- `drivers/sensors/max30102_sensor.cpp`: MAX30102のI2C通信・FIFOデータ取得・LED輝度自動AGC制御
- `utils/pulse_analyzer.cpp`: 自作脈波解析エンジン（DC除去、ローパス、動的移動平均ヒステリシスピーク検出）
- `test/test_pulse_analyzer/test_pulse_analyzer.cpp`: 25Hz環境対応ネイティブ単体テスト環境

### [Step 6] ストレージ管理・Webインフラ (FRAM + SD + Wi-Fi)
- `storage/fram_storage.cpp` & `storage_manager.cpp`: MB85RC256Vを用いた不揮発リングバッファ、SD一括リカバリ、設定永続化(FRAM Superblock v3)
- `services/time_manager.cpp`: NTPサーバー時刻同期
- `services/weather_service.cpp`: 気象庁AMeDAS APIから海面気圧を自動取得・IDW(逆距離加重法)補間
- `services/web_server_service.cpp`: SPIFFSホスティング、SDファイルエクスプローラ、Chart.js可視化、手動IDW校正UI

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
- `SparkFun MAX3010x Pulse and Proximity Sensor Library` @ ^1.1.2
- `Unity` (Native Test Framework)
