# Antigravity XIAO ESP32S3 Sensor Platform

XIAO ESP32S3を用いた複合センサ環境プラットフォームのプロジェクトです。

## アーキテクチャ実装状況

現在、アーキテクチャの段階的実装を進めています。

### [Step 1] HAL・共通基盤の整備（完了）
- `pins.h`: センサとI2Cのピン定義を管理
- `i2c_bus`: I2C初期化とスキャン機能をモジュール化
- `logger`: `services::Logger` によるログレベル管理とシリアル出力のフォーマット化

## 使用ライブラリ
現在のところ、PlatformIO/Arduinoの標準ライブラリ（`Wire`等）のみを使用しています。
